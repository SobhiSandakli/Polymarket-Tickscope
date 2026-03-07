"""Strategy base class — all strategies inherit from this."""

from abc import ABC, abstractmethod

import pandas as pd

from ..data_loader import DataLoader


class Strategy(ABC):
    """Pure signal generator: data in, signals out as a DataFrame.

    No position tracking, no fill logic — the engine handles that.
    Strategies return a DataFrame with standardized columns:
        ts_ms, asset_id, side, size, order_type, limit_price
    """

    name: str = "unnamed"

    def __init__(self, **params):
        self._params = params
        self.configure(**params)

    def configure(self, **params) -> None:
        """Override to accept strategy-specific tunable parameters."""
        pass

    @abstractmethod
    def generate_signals(self, loader: DataLoader) -> pd.DataFrame:
        """Query the data and return signals as a DataFrame.

        Required columns: ts_ms, asset_id, side, size, order_type
        Optional columns: limit_price (defaults to NULL)
        """
        ...

    def __repr__(self) -> str:
        return f"{self.__class__.__name__}({self._params})"
