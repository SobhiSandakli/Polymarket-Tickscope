"""CLI runner for the Polymarket backtest optimizer.

Runs a coarse grid search over each configured strategy's parameters and writes
a per-strategy heatmap plus a combined summary CSV. ConvergenceNo is the one
reference strategy that ships with the repo (in-sample positive but OVERFIT —
see docs/FINDINGS.md); add your own Strategy subclasses to STRATEGY_GRIDS to
sweep them the same way.

Usage:
    python research/backtest/run_optimizer.py \\
        --parquet-dir data/parquet \\
        --metadata-csv data/market_metadata.csv \\
        --db-path /tmp/polymarket_backtest.duckdb \\
        --output /tmp/optimizer_results/ \\
        --capital 1000        # optional: capital gate (default 10000)

Output:
  {output}/{strategy_name}_heatmap.html
  {output}/optimizer_summary.csv
"""

from __future__ import annotations

import argparse
import os
import sys
import time

import pandas as pd

# Make sure we can import from the research package regardless of cwd
_repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _repo_root not in sys.path:
    sys.path.insert(0, _repo_root)

from research.backtest.data_loader import DataLoader
from research.backtest.optimizer import CoarseGridSearch, GridResult
from research.backtest.strategies import ConvergenceNo


# ---------------------------------------------------------------------------
# Parameter grids. ConvergenceNo is the only strategy shipped with the repo;
# add your own Strategy subclasses (from strategies/base.py) here to sweep them.
# ---------------------------------------------------------------------------

STRATEGY_GRIDS: list[dict] = [
    {
        "cls": ConvergenceNo,
        "param1": "threshold",
        "p1_vals": [0.25, 0.30, 0.35, 0.40, 0.45],
        "param2": "exit_threshold",
        "p2_vals": [0.75, 0.80, 0.85, 0.90, 0.95],
        "fixed": {"size": 10.0},
    },
]


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

def run_grid_search(loader: DataLoader, output_dir: str, capital: float) -> None:
    """Run CoarseGridSearch on every strategy in STRATEGY_GRIDS, save heatmaps."""
    grid_search = CoarseGridSearch()
    summary_rows: list[pd.DataFrame] = []

    for spec in STRATEGY_GRIDS:
        cls = spec["cls"]
        print(f"\n=== {cls.__name__} ===")
        t0 = time.perf_counter()

        result: GridResult = grid_search.run(
            strategy_cls=cls,
            param1=spec["param1"],
            p1_vals=spec["p1_vals"],
            param2=spec["param2"],
            p2_vals=spec["p2_vals"],
            fixed_kwargs=spec["fixed"],
            loader=loader,
            starting_capital=capital,
        )

        elapsed = time.perf_counter() - t0

        # Save heatmap HTML
        try:
            fig = grid_search.plot_heatmap(result, metric="total_pnl")
            html_path = os.path.join(output_dir, f"{cls.name}_heatmap.html")
            fig.write_html(html_path)
            print(f"  Heatmap → {html_path}")
        except ImportError:
            print("  plotly not installed — skipping heatmap. pip install plotly")

        # Tidy DataFrame for summary CSV
        tidy = result.to_dataframe()
        summary_rows.append(tidy)
        print(f"  Done ({elapsed:.1f}s total)")

    if summary_rows:
        summary = pd.concat(summary_rows, ignore_index=True)
        csv_path = os.path.join(output_dir, "optimizer_summary.csv")
        summary.to_csv(csv_path, index=False)
        print(f"\n  Summary → {csv_path}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Polymarket backtest optimizer — coarse grid search over strategy params"
    )
    p.add_argument(
        "--parquet-dir",
        default="data/parquet",
        help="Directory containing polymarket_*.parquet files (default: data/parquet)",
    )
    p.add_argument(
        "--metadata-csv",
        default="data/market_metadata.csv",
        help="Path to market_metadata.csv (default: data/market_metadata.csv)",
    )
    p.add_argument(
        "--db-path",
        default="/tmp/polymarket_backtest.duckdb",
        help="DuckDB cache file (default: /tmp/polymarket_backtest.duckdb)",
    )
    p.add_argument(
        "--output",
        default="/tmp/optimizer_results",
        help="Directory to save outputs (created if it doesn't exist)",
    )
    p.add_argument(
        "--capital",
        type=float,
        default=10_000.0,
        help="Max capital for the capital gate (default 10000)",
    )
    return p.parse_args()


def main() -> None:
    args = parse_args()

    # Ensure output directory exists
    os.makedirs(args.output, exist_ok=True)

    print(f"\nPolymarket Backtest Optimizer")
    print(f"  Parquet: {args.parquet_dir}")
    print(f"  Meta:    {args.metadata_csv}")
    print(f"  DB:      {args.db_path}")
    print(f"  Output:  {args.output}")
    print(f"  Capital: ${args.capital:,.0f}")

    t_start = time.perf_counter()

    # Trigger lazy load by accessing .con (no explicit .load() method)
    loader = DataLoader(
        parquet_dir=args.parquet_dir,
        metadata_csv=args.metadata_csv,
        db_path=args.db_path,
    )
    _ = loader.con  # force table build / staleness check now, before timing

    run_grid_search(loader, args.output, args.capital)

    total = time.perf_counter() - t_start
    print(f"\nAll done in {total/60:.1f} min. Results in {args.output}")


if __name__ == "__main__":
    main()
