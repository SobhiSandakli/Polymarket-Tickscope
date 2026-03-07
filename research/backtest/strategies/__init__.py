"""Strategy registry — only proven profitable strategies live here."""

from .convergence_no import ConvergenceNo
from .base import BaseStrategy

ALL_STRATEGIES = [ConvergenceNo]

__all__ = [
    "BaseStrategy",
    "ConvergenceNo",
    "ALL_STRATEGIES",
]
