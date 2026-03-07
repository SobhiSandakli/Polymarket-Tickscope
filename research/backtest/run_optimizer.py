"""CLI runner for the Polymarket backtest optimizer.

Usage:
    python research/backtest/run_optimizer.py \\
        --db /tmp/polymarket_backtest.duckdb \\
        --bucket A            # A, B, or ALL
        --output /tmp/optimizer_results/
        --capital 1000        # optional: capital gate (default 10000)

Bucket A (LatencySweep):   ArbYesNo, GraphArb, BtcTierArb
Bucket B (CoarseGridSearch): MeanReversion, InformedFlow, ToxicFlow,
                              TimeDecay, ConvergenceNo

Output:
  Bucket A → {output}/bucket_a_{strategy_name}.csv
  Bucket B → {output}/bucket_b_{strategy_name}_heatmap.html
             {output}/bucket_b_summary.csv
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
from research.backtest.optimizer import (
    CoarseGridSearch,
    GridResult,
    LatencySweep,
    LATENCY_SWEEP_VALUES,
)
from research.backtest.strategies import (
    ArbYesNo,
    GraphArb,
    BtcTierArb,
    MeanReversion,
    InformedFlow,
    ToxicFlow,
    TimeDecay,
    ConvergenceNo,
)


# ---------------------------------------------------------------------------
# Parameter grids for Bucket B
# ---------------------------------------------------------------------------

BUCKET_B_GRIDS: list[dict] = [
    {
        "cls": ConvergenceNo,
        "param1": "threshold",
        "p1_vals": [0.25, 0.30, 0.35, 0.40, 0.45],
        "param2": "exit_threshold",
        "p2_vals": [0.75, 0.80, 0.85, 0.90, 0.95],
        "fixed": {"size": 10.0},
    },
    {
        "cls": MeanReversion,
        "param1": "window_s",
        "p1_vals": [60, 120, 300, 600, 1800],
        "param2": "z_threshold",
        "p2_vals": [1.0, 1.5, 2.0, 2.5, 3.0],
        "fixed": {"size": 10.0},
    },
    {
        "cls": InformedFlow,
        "param1": "min_trade_size",
        "p1_vals": [10, 25, 50, 100, 200],
        "param2": "window_ms",
        "p2_vals": [30_000, 60_000, 120_000, 300_000, 600_000],
        "fixed": {"size": 10.0},
    },
    {
        "cls": ToxicFlow,
        "param1": "toxicity_threshold",
        "p1_vals": [0.55, 0.60, 0.65, 0.70, 0.75],
        "param2": "lookback_ms",
        "p2_vals": [30_000, 60_000, 120_000, 300_000, 600_000],
        "fixed": {"size": 10.0},
    },
    {
        "cls": TimeDecay,
        "param1": "min_hours_to_res",
        "p1_vals": [1, 2, 4, 8, 24],
        "param2": "price_threshold",
        "p2_vals": [0.30, 0.35, 0.40, 0.45, 0.50],
        "fixed": {"size": 10.0},
    },
]


# ---------------------------------------------------------------------------
# Runners
# ---------------------------------------------------------------------------

def run_bucket_a(loader: DataLoader, output_dir: str, capital: float) -> None:
    """Run LatencySweep on all Bucket A strategies."""
    sweep = LatencySweep()
    bucket_a = [ArbYesNo, GraphArb, BtcTierArb]

    for cls in bucket_a:
        print(f"\n=== Bucket A | {cls.__name__} ===")
        t0 = time.perf_counter()

        df = sweep.run(cls, fixed_kwargs={}, loader=loader, starting_capital=capital)

        elapsed = time.perf_counter() - t0
        out_path = os.path.join(output_dir, f"bucket_a_{cls.name}.csv")
        df.to_csv(out_path, index=False)
        print(f"  Saved → {out_path}  (total {elapsed:.1f}s)")


def run_bucket_b(loader: DataLoader, output_dir: str, capital: float) -> None:
    """Run CoarseGridSearch on all Bucket B strategies, save HTML heatmaps."""
    grid_search = CoarseGridSearch()
    summary_rows: list[pd.DataFrame] = []

    for spec in BUCKET_B_GRIDS:
        cls = spec["cls"]
        print(f"\n=== Bucket B | {cls.__name__} ===")
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
            html_path = os.path.join(output_dir, f"bucket_b_{cls.name}_heatmap.html")
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
        csv_path = os.path.join(output_dir, "bucket_b_summary.csv")
        summary.to_csv(csv_path, index=False)
        print(f"\n  Bucket B summary → {csv_path}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Polymarket backtest optimizer — LatencySweep (A) and CoarseGridSearch (B)"
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
        "--bucket",
        choices=["A", "B", "ALL"],
        default="ALL",
        help="Which bucket to run: A (latency sweep), B (grid search), or ALL",
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
    print(f"  Bucket:  {args.bucket}")
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

    if args.bucket in ("A", "ALL"):
        run_bucket_a(loader, args.output, args.capital)

    if args.bucket in ("B", "ALL"):
        run_bucket_b(loader, args.output, args.capital)

    total = time.perf_counter() - t_start
    print(f"\nAll done in {total/60:.1f} min. Results in {args.output}")


if __name__ == "__main__":
    main()
