"""Strategy registry. ConvergenceNo is the last strategy still implemented here;
it is in-sample positive but OVERFIT (went negative out-of-sample) — kept as the
reference implementation, not a live edge. See docs/FINDINGS.md."""

from .convergence_no import ConvergenceNo
from .base import BaseStrategy

ALL_STRATEGIES = [ConvergenceNo]

__all__ = [
    "BaseStrategy",
    "ConvergenceNo",
    "ALL_STRATEGIES",
]
