"""Performance analytics for backtested strategies."""

import numpy as np

from .types import Fill, Side


def compute_metrics(
    pnl_curve: np.ndarray,
    fills: list[Fill],
    n_signals: int,
    unrealized_pnl: float = 0.0,
    peak_capital: float = 0.0,
    round_trip_pnls: list[float] | None = None,
) -> dict[str, float]:
    """Compute standardized performance metrics.

    Win rate, avg_win, avg_loss, and Kelly are computed from round-trip
    trade PnLs (completed open→close cycles), NOT per-fill PnL diffs.
    """
    n_fills = len(fills)

    gross_fees = sum(f.fee for f in fills if f.fee > 0)      # taker fees paid
    maker_rebates = sum(-f.fee for f in fills if f.fee < 0)  # rebates earned
    net_fees = gross_fees - maker_rebates
    total_volume = sum(f.size * f.price for f in fills)
    avg_slippage = np.mean([f.slippage for f in fills]) if fills else 0.0

    # PnL metrics
    total_pnl = float(pnl_curve[-1]) if len(pnl_curve) > 0 else 0.0

    # Round-trip trade metrics (the correct way)
    rt = np.array(round_trip_pnls) if round_trip_pnls else np.array([])
    n_trades = len(rt)
    if n_trades > 0:
        wins_arr = rt[rt > 0]
        losses_arr = rt[rt < 0]
        n_wins = len(wins_arr)
        n_losses = len(losses_arr)
        win_rate = n_wins / n_trades
        avg_win = float(np.mean(wins_arr)) if n_wins > 0 else 0.0
        avg_loss = float(np.mean(np.abs(losses_arr))) if n_losses > 0 else 0.0
        gross_profit = float(np.sum(wins_arr))
        gross_loss = float(np.sum(np.abs(losses_arr)))
        profit_factor = gross_profit / gross_loss if gross_loss > 0 else float("inf")

        # Kelly criterion: f* = p - (1-p)/b where b = avg_win/avg_loss
        if avg_loss > 0 and avg_win > 0:
            b = avg_win / avg_loss
            kelly_f = win_rate - (1.0 - win_rate) / b
            half_kelly = max(kelly_f / 2.0, 0.0)
        else:
            kelly_f = 0.0
            half_kelly = 0.0
    else:
        n_wins = n_losses = 0
        win_rate = avg_win = avg_loss = 0.0
        gross_profit = gross_loss = 0.0
        profit_factor = 0.0
        kelly_f = half_kelly = 0.0

    # Sharpe from round-trip PnLs (annualized — grain of salt with small samples)
    if n_trades > 1 and np.std(rt) > 0:
        sharpe = float(np.mean(rt) / np.std(rt)) * np.sqrt(252)
    else:
        sharpe = 0.0

    # Max drawdown from PnL curve
    if len(pnl_curve) > 0:
        running_max = np.maximum.accumulate(pnl_curve)
        drawdowns = running_max - pnl_curve
        max_drawdown = float(np.max(drawdowns))
    else:
        max_drawdown = 0.0

    total_with_unrealized = total_pnl + unrealized_pnl
    return_on_capital = total_with_unrealized / peak_capital if peak_capital > 0 else 0.0

    return {
        "realized_pnl": round(total_pnl, 4),
        "unrealized_pnl": round(unrealized_pnl, 4),
        "total_pnl": round(total_with_unrealized, 4),
        "net_pnl": round(total_with_unrealized, 4),
        "gross_fees": round(gross_fees, 4),
        "maker_rebates": round(maker_rebates, 4),
        "net_fees": round(net_fees, 4),
        "peak_capital": round(peak_capital, 2),
        "return_on_capital": round(return_on_capital, 4),
        "sharpe_ratio": round(sharpe, 2),
        "max_drawdown": round(max_drawdown, 4),
        "n_signals": n_signals,
        "n_fills": n_fills,
        "n_trades": n_trades,
        "fill_rate": round(n_fills / n_signals, 3) if n_signals > 0 else 0.0,
        "avg_slippage_bps": round(avg_slippage * 10_000, 1),
        "win_rate": round(win_rate, 3),
        "n_wins": n_wins,
        "n_losses": n_losses,
        "avg_win": round(avg_win, 4),
        "avg_loss": round(avg_loss, 4),
        "profit_factor": round(profit_factor, 2),
        "kelly_f": round(kelly_f, 4),
        "half_kelly": round(half_kelly, 4),
        "total_volume": round(total_volume, 2),
    }
