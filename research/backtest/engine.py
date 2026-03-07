"""Backtest orchestrator — runs strategies through fill simulation and PnL computation.

Performance design:
  - Strategies return signal DataFrames (no iterrows)
  - Fill simulation uses batched numpy searchsorted (bounded memory)
  - Only the final PnL loop (hundreds/thousands of fills) runs in Python
"""

import numpy as np
import pandas as pd

from .data_loader import DataLoader
from .fills import FillModel
from .metrics import compute_metrics
from .types import Fill, Side, StrategyResult


class BacktestEngine:
    """Runs strategies, simulates fills, and computes PnL + metrics.

    Key design: signal generation stays in DuckDB. Fill simulation pulls
    data per-asset-batch and uses numpy searchsorted. Python only loops
    over the final fills (hundreds-thousands of rows).

    Args:
        max_capital: Maximum capital that can be deployed simultaneously.
            Entry fills are skipped once deployed capital would exceed this.
            Default 10_000 preserves historical behaviour (effectively unlimited
            for small strategies). Set to realistic capital for accurate ROC.
    """

    def __init__(
        self,
        loader: DataLoader,
        fill_model: FillModel | None = None,
        max_capital: float = 10_000.0,
    ):
        self.loader = loader
        self.fill_model = fill_model or FillModel()
        self.max_capital = max_capital

    def run(self, strategy) -> StrategyResult:
        """Run a single strategy end-to-end."""
        # Step 1: signals as DataFrame
        signals_df = strategy.generate_signals(self.loader)

        if signals_df.empty:
            empty = np.array([0.0])
            return StrategyResult(
                name=strategy.name, signals=[], fills=[],
                pnl_curve=empty, positions={},
                metrics={"strategy": strategy.name, **_empty_metrics()},
            )

        signals_df = _normalize_signals_df(signals_df)
        n_signals = len(signals_df)

        # Step 2: fill simulation (batched numpy — bounded memory)
        fills_df = self.fill_model.simulate(signals_df, self.loader.con)

        # Step 3: convert fills DF → Fill objects (small — hundreds/thousands)
        fills = _fills_from_df(fills_df)

        # Step 3b: capital gate — drop entry fills that exceed budget
        fills = _apply_capital_gate(fills, self.max_capital)

        # Step 4: PnL curve + peak capital + round-trip PnLs
        pnl_curve, positions, avg_costs, peak_capital, round_trip_pnls = _compute_pnl(fills)

        # Step 5: mark-to-market open positions
        unrealized_pnl = _compute_unrealized(
            positions, avg_costs, self.loader.con,
        )

        # Step 6: metrics (using round-trip PnLs for accurate win rate)
        metrics = compute_metrics(
            pnl_curve, fills, n_signals,
            unrealized_pnl=unrealized_pnl,
            peak_capital=peak_capital,
            round_trip_pnls=round_trip_pnls,
        )
        metrics["strategy"] = strategy.name

        return StrategyResult(
            name=strategy.name,
            signals=[],
            fills=fills,
            pnl_curve=pnl_curve,
            positions=positions,
            metrics=metrics,
            unrealized_pnl=unrealized_pnl,
            peak_capital=peak_capital,
        )

    def run_many(self, strategies: list) -> pd.DataFrame:
        """Run multiple strategies and return a comparison DataFrame."""
        results = []
        for strat in strategies:
            res = self.run(strat)
            results.append(res.metrics)
        df = pd.DataFrame(results)
        if "sharpe_ratio" in df.columns:
            df.sort_values("sharpe_ratio", ascending=False, inplace=True)
        return df.reset_index(drop=True)


def _normalize_signals_df(df: pd.DataFrame) -> pd.DataFrame:
    """Ensure signal DataFrame has correct types."""
    df = df.copy()
    df["ts_ms"] = df["ts_ms"].astype("int64")
    df["side"] = df["side"].astype("int32")
    df["order_type"] = df["order_type"].astype("int32")
    if "limit_price" not in df.columns:
        df["limit_price"] = None
    return df


def _fills_from_df(fills_df: pd.DataFrame) -> list[Fill]:
    """Convert fills DataFrame to Fill objects for PnL computation."""
    if fills_df.empty:
        return []
    return [
        Fill(
            timestamp_ms=int(row.ts_ms),
            asset_id=row.asset_id,
            side=Side(int(row.side)),
            size=float(row.size),
            price=float(row.price),
            fee=float(row.fee),
            slippage=float(row.slippage),
        )
        for row in fills_df.itertuples(index=False)
    ]


def _compute_pnl(fills: list[Fill]) -> tuple[np.ndarray, dict[str, float], dict[str, float], float, list[float]]:
    """Compute cumulative PnL from fills using average-cost tracking.

    Returns: (pnl_curve, positions, avg_costs, peak_capital, round_trip_pnls)
    peak_capital = max(sum of |position * avg_cost|) across all fills.
    round_trip_pnls = realized PnL of each completed trade (open→close).
    """
    if not fills:
        return np.array([0.0]), {}, {}, 0.0, []

    positions: dict[str, float] = {}
    avg_costs: dict[str, float] = {}
    cumulative_pnl = 0.0
    pnl_points = []
    peak_capital = 0.0
    running_capital = 0.0  # incremental sum of |pos * avg_cost|
    round_trip_pnls: list[float] = []  # PnL per completed trade

    for fill in fills:
        asset = fill.asset_id
        pos = positions.get(asset, 0.0)
        avg_cost = avg_costs.get(asset, 0.0)

        # Remove old contribution before position update
        running_capital -= abs(pos) * avg_cost

        if fill.side == Side.BID:  # buying
            qty = fill.size
            cost = fill.price * qty + fill.fee

            if pos >= 0:
                total_cost = avg_cost * pos + cost
                new_pos = pos + qty
                avg_costs[asset] = total_cost / new_pos if new_pos > 0 else 0.0
                positions[asset] = new_pos
            else:
                cover_qty = min(qty, abs(pos))
                pnl = cover_qty * (avg_cost - fill.price) - fill.fee
                cumulative_pnl += pnl
                round_trip_pnls.append(pnl)
                remaining = qty - cover_qty
                if remaining > 0:
                    positions[asset] = remaining
                    avg_costs[asset] = fill.price
                else:
                    positions[asset] = pos + qty
                    if positions[asset] == 0:
                        avg_costs[asset] = 0.0

        else:  # selling
            qty = fill.size

            if pos <= 0:
                total_cost = avg_cost * abs(pos) + fill.price * qty - fill.fee
                new_pos = pos - qty
                avg_costs[asset] = total_cost / abs(new_pos) if new_pos != 0 else 0.0
                positions[asset] = new_pos
            else:
                close_qty = min(qty, pos)
                pnl = close_qty * (fill.price - avg_cost) - fill.fee
                cumulative_pnl += pnl
                round_trip_pnls.append(pnl)
                remaining = qty - close_qty
                if remaining > 0:
                    positions[asset] = -remaining
                    avg_costs[asset] = fill.price
                else:
                    positions[asset] = pos - qty
                    if positions[asset] == 0:
                        avg_costs[asset] = 0.0

        pnl_points.append(cumulative_pnl)

        # Add back new contribution after position update (O(1) per fill)
        running_capital += abs(positions[asset]) * avg_costs.get(asset, 0.0)
        if running_capital > peak_capital:
            peak_capital = running_capital

    return np.array(pnl_points), positions, avg_costs, peak_capital, round_trip_pnls


def _compute_unrealized(
    positions: dict[str, float],
    avg_costs: dict[str, float],
    con,
) -> float:
    """Mark-to-market: compute unrealized PnL from open positions.

    Queries DuckDB for the last known mid-price of every open asset.
    unrealized = sum(pos * (last_mid - avg_cost)) for each position.
    """
    open_assets = [a for a, q in positions.items() if abs(q) > 1e-9]
    if not open_assets:
        return 0.0

    import pandas as pd

    # Query last mid-price per open asset in batches
    batch_size = 200
    all_mids: dict[str, float] = {}

    for i in range(0, len(open_assets), batch_size):
        batch = open_assets[i : i + batch_size]
        batch_tbl = pd.DataFrame({"asset_id": batch})
        con.register("_mtm_batch", batch_tbl)
        mids_df = con.execute("""
            SELECT sub.asset_id, (sub.best_bid + sub.best_ask) / 2.0 AS last_mid
            FROM (
                SELECT DISTINCT ON (t.asset_id) t.asset_id, t.best_bid, t.best_ask
                FROM ticks t
                SEMI JOIN _mtm_batch b ON t.asset_id = b.asset_id
                WHERE t.event_type = 'PRICE_CHANGE'
                  AND t.best_bid > 0 AND t.best_ask > t.best_bid
                ORDER BY t.asset_id, t.ts_ms DESC
            ) sub
        """).fetchdf()
        con.unregister("_mtm_batch")

        for row in mids_df.itertuples(index=False):
            all_mids[row.asset_id] = row.last_mid

    unrealized = 0.0
    for asset in open_assets:
        pos = positions[asset]
        avg_cost = avg_costs.get(asset, 0.0)
        mid = all_mids.get(asset)
        if mid is not None:
            unrealized += pos * (mid - avg_cost)

    return unrealized


def _apply_capital_gate(fills: list[Fill], max_capital: float) -> list[Fill]:
    """Drop entry fills that would push deployed capital above max_capital.

    Tracks deployed capital = sum(|position| * avg_cost) after each fill.
    Entry (BID) fills that would exceed max_capital are skipped.
    Exit (ASK) fills that close/reduce positions always pass through — they
    free capital rather than consuming it.

    This prevents the engine from simulating trades on money it doesn't have,
    giving accurate ROC figures at realistic capital levels.
    """
    if max_capital >= 1e9:  # sentinel: effectively unlimited
        return fills

    positions: dict[str, float] = {}
    avg_costs: dict[str, float] = {}
    kept: list[Fill] = []

    for fill in fills:
        asset = fill.asset_id
        pos = positions.get(asset, 0.0)
        cost = avg_costs.get(asset, 0.0)

        if fill.side == Side.BID:
            new_exposure = fill.size * fill.price
            current_deployed = sum(
                abs(p) * avg_costs.get(a, 0.0)
                for a, p in positions.items()
            )
            if current_deployed + new_exposure > max_capital:
                continue  # can't afford — skip

        kept.append(fill)

        # Update position tracking
        if fill.side == Side.BID:
            old_value = cost * pos + fill.price * fill.size
            new_pos = pos + fill.size
            avg_costs[asset] = old_value / new_pos if new_pos > 0 else 0.0
            positions[asset] = new_pos
        else:
            new_pos = pos - fill.size
            positions[asset] = new_pos
            if abs(new_pos) < 1e-9:
                avg_costs[asset] = 0.0

    return kept


def _empty_metrics() -> dict:
    return {
        "realized_pnl": 0.0, "unrealized_pnl": 0.0,
        "total_pnl": 0.0, "net_pnl": 0.0,
        "gross_fees": 0.0, "maker_rebates": 0.0, "net_fees": 0.0,
        "peak_capital": 0.0, "return_on_capital": 0.0,
        "sharpe_ratio": 0.0, "max_drawdown": 0.0,
        "n_signals": 0, "n_fills": 0, "fill_rate": 0.0,
        "avg_slippage_bps": 0.0, "win_rate": 0.0,
        "profit_factor": 0.0, "total_volume": 0.0,
    }
