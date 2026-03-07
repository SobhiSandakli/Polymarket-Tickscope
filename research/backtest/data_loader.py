"""DuckDB data access layer — wraps the notebook's parquet + metadata loading."""

from pathlib import Path

import duckdb
import pandas as pd


class DataLoader:
    """Loads tick data from parquet files into an in-memory DuckDB table.

    Mirrors the exact SQL from arbitrage_hunter.ipynb cell 1 so every
    strategy sees the same `ticks` schema.
    """

    def __init__(
        self,
        parquet_dir: str | Path = "data/parquet",
        metadata_csv: str | Path = "data/market_metadata.csv",
        memory_limit: str = "4GB",
        db_path: str = "/tmp/polymarket_backtest.duckdb",
        read_only: bool = False,
    ):
        self._parquet_dir = Path(parquet_dir)
        self._metadata_csv = Path(metadata_csv)
        self._memory_limit = memory_limit
        self._db_path = db_path
        self._read_only = read_only
        self._con: duckdb.DuckDBPyConnection | None = None
        self._loaded = False

    @property
    def con(self) -> duckdb.DuckDBPyConnection:
        if self._con is None:
            try:
                self._con = duckdb.connect(self._db_path, read_only=self._read_only)
            except duckdb.IOException:
                # Another process holds the lock — fall back to a unique path
                import os
                fallback = f"{self._db_path}.{os.getpid()}"
                print(f"[DataLoader] Lock conflict on {self._db_path}, "
                      f"falling back to {fallback}")
                self._db_path = fallback
                self._con = duckdb.connect(self._db_path, read_only=self._read_only)
            if not self._read_only:
                self._con.execute(f"SET memory_limit='{self._memory_limit}'")
            self._con.execute("SET temp_directory='/tmp/duckdb_bt_tmp'")
            self._con.execute("SET max_temp_directory_size='16GB'")
            self._con.execute("SET threads=2")
            self._con.execute("SET preserve_insertion_order=false")
        if not self._loaded:
            self._load()
        return self._con

    def _load(self) -> None:
        # Read-only: table must already exist from a prior write session.
        # Skip all write operations and staleness checks.
        if self._read_only:
            self._loaded = True
            return

        # Check if table already exists (file-backed DB persists)
        exists = self._con.execute(
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_name = 'ticks'"
        ).fetchone()[0]

        if exists:
            # Verify the cached table covers all parquet files on disk.
            # If new files were added, the row count from parquet will
            # exceed the cached table — drop and rebuild.
            parquet_glob = str(self._parquet_dir / "polymarket_*.parquet")
            parquet_count = self._con.execute(
                f"SELECT COUNT(*) FROM read_parquet('{parquet_glob}')"
            ).fetchone()[0]
            cached_count = self._con.execute(
                "SELECT COUNT(*) FROM ticks"
            ).fetchone()[0]

            if parquet_count <= cached_count:
                self._loaded = True
                return

            # New data on disk — drop stale table and rebuild
            self._con.execute("DROP TABLE ticks")

        self._create_ticks_table()
        self._loaded = True

    def reload(self) -> None:
        """Force-rebuild the ticks table from parquet files on disk."""
        con = self.con  # ensure connection is open
        exists = con.execute(
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_name = 'ticks'"
        ).fetchone()[0]
        if exists:
            con.execute("DROP TABLE ticks")
        self._loaded = False
        self._create_ticks_table()
        self._loaded = True

    def _create_ticks_table(self) -> None:
        parquet_glob = str(self._parquet_dir / "polymarket_*.parquet")
        metadata_path = str(self._metadata_csv)

        self._con.execute(f"""
            CREATE TABLE ticks AS
            SELECT
                epoch_ms(t.timestamp)  AS ts_ms,
                t.price,
                t.size,
                t.best_bid,
                t.best_ask,
                t.side,
                t.event_type,
                t.asset_id,
                m.condition_id,
                m.outcome,
                m.question,
                m.end_date,
                COALESCE(m.taker_fee_bps, 0)::INT AS fee_bps
            FROM read_parquet('{parquet_glob}') t
            LEFT JOIN read_csv('{metadata_path}',
                         types={{'asset_id':'VARCHAR','outcome':'VARCHAR'}}) m
              ON t.asset_id = m.asset_id
        """)

    def query(self, sql: str) -> pd.DataFrame:
        """Run arbitrary SQL against the ticks table."""
        return self.con.execute(sql).fetchdf()

    def query_raw(self, sql: str):
        """Run SQL, return raw DuckDB result (for scalar queries)."""
        return self.con.execute(sql)

    def get_trades(self) -> pd.DataFrame:
        return self.query(
            "SELECT * FROM ticks WHERE event_type = 'LAST_TRADE'"
        )

    def get_book_updates(self) -> pd.DataFrame:
        return self.query(
            "SELECT * FROM ticks WHERE event_type = 'PRICE_CHANGE'"
        )

    def get_yes_no_pairs(self) -> pd.DataFrame:
        """Return condition_id -> (yes_asset_id, no_asset_id) mapping."""
        return self.query("""
            SELECT
                condition_id,
                MAX(CASE WHEN outcome = 'YES' THEN asset_id END) AS yes_asset,
                MAX(CASE WHEN outcome = 'NO'  THEN asset_id END) AS no_asset
            FROM ticks
            WHERE condition_id IS NOT NULL
            GROUP BY condition_id
            HAVING yes_asset IS NOT NULL AND no_asset IS NOT NULL
        """)

    def summary(self) -> dict:
        row = self.con.execute("""
            SELECT
                COUNT(*)                              AS total_ticks,
                SUM((event_type='LAST_TRADE')::INT)   AS trades,
                COUNT(DISTINCT condition_id)           AS markets,
                COUNT(DISTINCT asset_id)               AS tokens,
                MIN(ts_ms)                             AS min_ts,
                MAX(ts_ms)                             AS max_ts
            FROM ticks
        """).fetchone()
        return dict(
            total_ticks=row[0], trades=row[1], markets=row[2],
            tokens=row[3], min_ts=row[4], max_ts=row[5],
            hours=round((row[5] - row[4]) / 3.6e6, 1),
        )
