"""Core data structures for the backtesting framework."""

from dataclasses import dataclass, field
from enum import IntEnum
from typing import Optional

import numpy as np


class Side(IntEnum):
    BID = 0   # buying
    ASK = 1   # selling


class OrderType(IntEnum):
    MARKET = 0
    LIMIT = 1


@dataclass(slots=True)
class Signal:
    """A trading signal emitted by a strategy."""
    timestamp_ms: int
    asset_id: str
    side: Side
    size: float
    order_type: OrderType = OrderType.MARKET
    limit_price: Optional[float] = None
    metadata: Optional[dict] = None  # strategy-specific info (e.g., pair leg)


@dataclass(slots=True)
class Fill:
    """A simulated execution."""
    timestamp_ms: int
    asset_id: str
    side: Side
    size: float
    price: float
    fee: float
    slippage: float


@dataclass
class StrategyResult:
    """Complete output of a backtest run."""
    name: str
    signals: list[Signal]
    fills: list[Fill]
    pnl_curve: np.ndarray          # cumulative PnL at each fill
    positions: dict[str, float]    # asset_id -> net position at end
    metrics: dict[str, float]
    unrealized_pnl: float = 0.0    # MTM PnL from open positions
    peak_capital: float = 0.0      # max capital deployed at any point
