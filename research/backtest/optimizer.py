"""Parameter optimizer for the Polymarket backtest framework.

Two sweep modes:
  LatencySweep    — Bucket A structural arbs. Locks strategy params, sweeps
                    network_latency_ms ∈ [1, 5, 10, 20, 50, 100]. Useful for
                    confirming an edge exists only at sub-millisecond speed.

  CoarseGridSearch — Bucket B directional takers. Locks latency at 50ms,
                     sweeps two strategy-specific parameters on a 5×5 grid.
                     Output: GridResult + Plotly heatmap with plateau markers.

Quarantined strategies raise QuarantineError immediately — no sweeps run.

Usage:
    from research.backtest.optimizer import LatencySweep, CoarseGridSearch
    from research.backtest.strategies import ArbYesNo, ConvergenceNo

    sweep = LatencySweep()
    df = sweep.run(ArbYesNo, fixed_kwargs={}, loader=loader)

    grid = CoarseGridSearch()
    result = grid.run(ConvergenceNo,
                      param1="threshold",       p1_vals=[0.25,0.30,0.35,0.40,0.45],
                      param2="exit_threshold",  p2_vals=[0.75,0.80,0.85,0.90,0.95],
                      fixed_kwargs={"size": 10.0},
                      loader=loader)
    fig = grid.plot_heatmap(result, metric="total_pnl")
    fig.write_html("/tmp/convergence_no_heatmap.html")
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field

import numpy as np
import pandas as pd
from tqdm.auto import tqdm

from .data_loader import DataLoader
from .engine import BacktestEngine
from .fills import FillModel


# ---------------------------------------------------------------------------
# Bucket membership constants
# ---------------------------------------------------------------------------

QUARANTINE: set[str] = {"MicroSpread", "FeeArb", "MarketMaking", "BookImbalance"}

LATENCY_SWEEP_VALUES: list[int] = [1, 5, 10, 20, 50, 100]  # ms
BUCKET_B_LATENCY_MS: int = 50


# ---------------------------------------------------------------------------
# Exceptions
# ---------------------------------------------------------------------------

class QuarantineError(Exception):
    """Raised when a quarantined strategy is passed to the optimizer."""


# ---------------------------------------------------------------------------
# Data containers
# ---------------------------------------------------------------------------

@dataclass
class GridResult:
    """Full output of a CoarseGridSearch run.

    Attributes:
        strategy_name:   e.g. "convergence_no"
        param1_name:     name of the first swept parameter
        param1_values:   list of p1 values (Y-axis of heatmap)
        param2_name:     name of the second swept parameter
        param2_values:   list of p2 values (X-axis of heatmap)
        pnl_matrix:      shape (len(p1_vals), len(p2_vals)), dtype float64
        metrics_matrix:  other scalar metrics on the same grid, keyed by name
    """
    strategy_name: str
    param1_name: str
    param1_values: list
    param2_name: str
    param2_values: list
    pnl_matrix: np.ndarray              # (n_p1, n_p2)
    metrics_matrix: dict[str, np.ndarray] = field(default_factory=dict)

    def to_dataframe(self) -> pd.DataFrame:
        """Flatten grid to tidy DataFrame for CSV export."""
        rows = []
        for i, p1 in enumerate(self.param1_values):
            for j, p2 in enumerate(self.param2_values):
                row = {
                    "strategy": self.strategy_name,
                    self.param1_name: p1,
                    self.param2_name: p2,
                    "total_pnl": self.pnl_matrix[i, j],
                }
                for metric, mat in self.metrics_matrix.items():
                    row[metric] = mat[i, j]
                rows.append(row)
        return pd.DataFrame(rows)


# ---------------------------------------------------------------------------
# LatencySweep
# ---------------------------------------------------------------------------

class LatencySweep:
    """Sweep network_latency_ms for a Bucket A structural arb strategy.

    All strategy parameters are fixed; only latency changes per run.
    Prints a formatted table and returns a DataFrame.
    """

    def run(
        self,
        strategy_cls,
        fixed_kwargs: dict,
        loader: DataLoader,
        latency_values: list[int] | None = None,
        starting_capital: float = 10_000.0,
    ) -> pd.DataFrame:
        """Run latency sweep and return results as a DataFrame.

        Args:
            strategy_cls:      Strategy class (not instance) from BUCKET_A.
            fixed_kwargs:      Strategy constructor kwargs (no latency param here).
            loader:            Shared DataLoader with open DuckDB connection.
            latency_values:    Override default [1, 5, 10, 20, 50, 100] ms.
            starting_capital:  Max capital for the capital gate.

        Returns:
            DataFrame with columns: latency_ms, total_pnl, n_signals, n_fills,
            fill_rate, roc, runtime_s
        """
        cls_name = strategy_cls.__name__
        if cls_name in QUARANTINE:
            raise QuarantineError(
                f"{cls_name} is quarantined — no parameter sweep will help. "
                "Remove it from the run or investigate the quarantine reason."
            )

        lats = latency_values if latency_values is not None else LATENCY_SWEEP_VALUES
        rows = []

        pbar = tqdm(
            lats,
            desc=f"LatencySweep: {cls_name}",
            unit="lat",
            leave=True,
        )

        for lat in pbar:
            t0 = time.perf_counter()

            fill_model = FillModel(network_latency_ms=lat)
            engine = BacktestEngine(loader, fill_model=fill_model, max_capital=starting_capital)
            strategy = strategy_cls(**fixed_kwargs)
            result = engine.run(strategy)
            m = result.metrics

            elapsed = time.perf_counter() - t0
            row = {
                "latency_ms": lat,
                "total_pnl": m.get("total_pnl", 0.0),
                "n_signals": m.get("n_signals", 0),
                "n_fills": m.get("n_fills", 0),
                "fill_rate": m.get("fill_rate", 0.0),
                "roc": m.get("return_on_capital", 0.0),
                "runtime_s": round(elapsed, 2),
            }
            rows.append(row)

            pbar.set_postfix(
                lat=f"{lat}ms",
                pnl=f"${row['total_pnl']:.2f}",
                fills=row['n_fills'],
            )

        pbar.close()
        return pd.DataFrame(rows)


# ---------------------------------------------------------------------------
# CoarseGridSearch
# ---------------------------------------------------------------------------

class CoarseGridSearch:
    """5×5 parameter sweep for Bucket B directional taker strategies.

    Latency is locked at BUCKET_B_LATENCY_MS (50ms). Two strategy parameters
    are swept independently. Results are collected in a pnl_matrix and can
    be visualised as a Plotly heatmap.
    """

    def run(
        self,
        strategy_cls,
        param1: str,
        p1_vals: list,
        param2: str,
        p2_vals: list,
        fixed_kwargs: dict,
        loader: DataLoader,
        latency_ms: int = BUCKET_B_LATENCY_MS,
        starting_capital: float = 10_000.0,
    ) -> GridResult:
        """Run a 5×5 parameter grid and return a GridResult.

        Args:
            strategy_cls:   Strategy class from BUCKET_B.
            param1:         First parameter name (Y-axis).
            p1_vals:        Values for param1 (len should be 5).
            param2:         Second parameter name (X-axis).
            p2_vals:        Values for param2 (len should be 5).
            fixed_kwargs:   Other strategy kwargs held constant.
            loader:         Shared DataLoader.
            latency_ms:     Network latency, default 50ms.
            starting_capital: Capital gate.

        Returns:
            GridResult with pnl_matrix and secondary metric matrices.
        """
        cls_name = strategy_cls.__name__
        if cls_name in QUARANTINE:
            raise QuarantineError(
                f"{cls_name} is quarantined — cannot run CoarseGridSearch."
            )

        n1, n2 = len(p1_vals), len(p2_vals)
        pnl_mat = np.full((n1, n2), np.nan)
        roc_mat = np.full((n1, n2), np.nan)
        fill_rate_mat = np.full((n1, n2), np.nan)
        n_signals_mat = np.full((n1, n2), np.nan)

        fill_model = FillModel(network_latency_ms=latency_ms)

        total_cells = n1 * n2

        pbar = tqdm(
            total=total_cells,
            desc=f"GridSearch: {cls_name}",
            unit="cell",
            leave=True,
        )

        for i, p1 in enumerate(p1_vals):
            for j, p2 in enumerate(p2_vals):
                t0 = time.perf_counter()

                kwargs = {**fixed_kwargs, param1: p1, param2: p2}
                strategy = strategy_cls(**kwargs)
                engine = BacktestEngine(loader, fill_model=fill_model, max_capital=starting_capital)
                result = engine.run(strategy)
                m = result.metrics

                pnl_mat[i, j] = m.get("total_pnl", 0.0)
                roc_mat[i, j] = m.get("return_on_capital", 0.0)
                fill_rate_mat[i, j] = m.get("fill_rate", 0.0)
                n_signals_mat[i, j] = m.get("n_signals", 0)

                elapsed = time.perf_counter() - t0
                pbar.update(1)
                pbar.set_postfix(
                    p1=p1, p2=p2,
                    pnl=f"${pnl_mat[i,j]:.2f}",
                    t=f"{elapsed:.1f}s",
                )

        pbar.close()

        return GridResult(
            strategy_name=strategy_cls.name,
            param1_name=param1,
            param1_values=p1_vals,
            param2_name=param2,
            param2_values=p2_vals,
            pnl_matrix=pnl_mat,
            metrics_matrix={
                "roc": roc_mat,
                "fill_rate": fill_rate_mat,
                "n_signals": n_signals_mat,
            },
        )

    def plot_heatmap(self, result: GridResult, metric: str = "total_pnl") -> "go.Figure":
        """Build a Plotly heatmap from a GridResult.

        Args:
            result:  GridResult from CoarseGridSearch.run()
            metric:  "total_pnl" (default), "roc", or "fill_rate"

        Returns:
            Plotly Figure. Save with fig.write_html("path.html").

        Heatmap design:
          - colorscale: RdYlGn (red=bad, white=zero, green=good)
          - midpoint at 0 so negative PnL is red, positive is green
          - plateau cells marked with ★ (star overlay)

        Plateau definition:
          A cell is a plateau if its value is within 10% of the global max AND
          it has ≥ 2 neighbours (4-connected) also within 10% of the global max.
          This filters out isolated spikes while highlighting robust optima.
        """
        try:
            import plotly.graph_objects as go
        except ImportError as e:
            raise ImportError("plotly is required for heatmaps: pip install plotly") from e

        if metric == "total_pnl":
            z = result.pnl_matrix
        elif metric in result.metrics_matrix:
            z = result.metrics_matrix[metric]
        else:
            raise ValueError(f"Unknown metric '{metric}'. Available: total_pnl, {list(result.metrics_matrix)}")

        z_display = np.where(np.isnan(z), 0.0, z)

        x_labels = [str(v) for v in result.param2_values]
        y_labels = [str(v) for v in result.param1_values]

        global_max = float(np.nanmax(z_display))
        plateau_mask = _find_plateaus(z_display, global_max, threshold=0.10)

        # Build annotation text: value + star if plateau
        text_matrix = []
        for i in range(z_display.shape[0]):
            row_text = []
            for j in range(z_display.shape[1]):
                val = z_display[i, j]
                star = " ★" if plateau_mask[i, j] else ""
                row_text.append(f"{val:.2f}{star}")
            text_matrix.append(row_text)

        # Zero-centred colorscale
        abs_max = max(abs(float(np.nanmin(z_display))), abs(global_max), 1e-6)

        heatmap = go.Heatmap(
            z=z_display,
            x=x_labels,
            y=y_labels,
            text=text_matrix,
            texttemplate="%{text}",
            colorscale="RdYlGn",
            zmid=0.0,
            zmin=-abs_max,
            zmax=abs_max,
            colorbar=dict(title=metric),
        )

        plateau_count = int(plateau_mask.sum())
        title = (
            f"{result.strategy_name} — {metric}<br>"
            f"<sup>X: {result.param2_name} | Y: {result.param1_name} | "
            f"★ = plateau ({plateau_count} cells)</sup>"
        )

        fig = go.Figure(heatmap)
        fig.update_layout(
            title=title,
            xaxis_title=result.param2_name,
            yaxis_title=result.param1_name,
            font=dict(size=12),
            width=700,
            height=600,
        )

        return fig


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _find_plateaus(z: np.ndarray, global_max: float, threshold: float = 0.10) -> np.ndarray:
    """Return boolean mask of plateau cells.

    A cell is a plateau if:
      1. Its value is within `threshold` (10%) of global_max.
      2. At least 2 of its 4-connected neighbours also meet criterion 1.

    This rejects isolated spikes (no neighbours) while marking robust regions.
    """
    if global_max <= 0:
        return np.zeros_like(z, dtype=bool)

    cutoff = global_max * (1.0 - threshold)
    near_max = z >= cutoff

    n_rows, n_cols = z.shape
    neighbour_count = np.zeros_like(z, dtype=int)

    # Count 4-connected neighbours that are also near max
    if n_rows > 1:
        neighbour_count[:-1, :] += near_max[1:, :]   # cell below
        neighbour_count[1:, :]  += near_max[:-1, :]  # cell above
    if n_cols > 1:
        neighbour_count[:, :-1] += near_max[:, 1:]   # cell right
        neighbour_count[:, 1:]  += near_max[:, :-1]  # cell left

    return near_max & (neighbour_count >= 2)
