"""Strategy: Convergence momentum — buy NO on crypto down-moves.

When a crypto "up or down" market's YES token drops below a threshold,
the DOWN outcome resolves ~70% of the time. Buy the NO token at the ask
and hold to resolution (or exit when NO mid >= exit_threshold).

Data-backed edge (36h, 179M ticks):
  YES<=0.35 trigger: 253 conditions, 173 wins / 70 losses (68% win rate)
  Avg PnL: +$0.030/share, Total: +$7.70/share before fees
  Avg NO entry price: $0.677, dynamic fee ~$0.008/share

Backtest engine results (with fill simulation + dynamic fees):
  threshold=0.35:       +$89.74  (241 fills, 5.48% ROC on $1,638 capital)
  threshold=0.40:       +$138.53 (255 fills, 8.62% ROC on $1,606 capital)
  threshold=0.35 limit: +$126.39 (229 fills, 8.47% ROC, +$3.80 maker rebate)

The YES side (buying YES at 0.70) is consistently NEGATIVE (-$12.29)
due to crypto up-moves being noisier/less decisive than down-moves.
"""

import pandas as pd

from .base import Strategy
from ..data_loader import DataLoader


class ConvergenceNo(Strategy):
    name = "convergence_no"

    def configure(
        self,
        threshold: float = 0.35,
        exit_threshold: float = 0.90,
        size: float = 10.0,
        use_limit: bool = False,
        **kw,
    ) -> None:
        self.threshold = threshold
        self.exit_threshold = exit_threshold
        self.size = size
        self.use_limit = use_limit

    def generate_signals(self, loader: DataLoader) -> pd.DataFrame:
        order_type = 1 if self.use_limit else 0

        return loader.query(f"""
            WITH yes_book AS (
                SELECT ts_ms, condition_id,
                       (best_bid + best_ask) / 2.0 AS mid
                FROM ticks
                WHERE event_type = 'PRICE_CHANGE'
                  AND fee_bps > 0
                  AND outcome = 'YES'
                  AND question ILIKE '%up or down%'
                  AND best_bid > 0 AND best_ask > best_bid
                  AND condition_id IS NOT NULL
            ),
            -- First cross below threshold per condition (single entry)
            first_cross AS (
                SELECT DISTINCT ON (condition_id)
                    condition_id, ts_ms
                FROM yes_book
                WHERE mid <= {self.threshold}
                ORDER BY condition_id, ts_ms
            ),
            -- Map condition_id → NO asset_id
            no_assets AS (
                SELECT condition_id,
                       MAX(CASE WHEN outcome = 'NO' THEN asset_id END) AS no_asset
                FROM ticks
                WHERE condition_id IS NOT NULL
                  AND question ILIKE '%up or down%'
                  AND fee_bps > 0
                GROUP BY condition_id
                HAVING no_asset IS NOT NULL
            ),
            -- Get the NO token's book near the cross time
            no_entry AS (
                SELECT DISTINCT ON (fc.condition_id)
                    fc.condition_id,
                    fc.ts_ms,
                    na.no_asset,
                    t.best_ask AS no_ask,
                    t.best_bid AS no_bid
                FROM first_cross fc
                JOIN no_assets na ON fc.condition_id = na.condition_id
                JOIN ticks t ON t.asset_id = na.no_asset
                    AND t.event_type = 'PRICE_CHANGE'
                    AND t.ts_ms BETWEEN fc.ts_ms - 5000 AND fc.ts_ms + 5000
                    AND t.best_bid > 0 AND t.best_ask > t.best_bid
                ORDER BY fc.condition_id, ABS(t.ts_ms - fc.ts_ms)
            ),
            -- EXIT: first time NO mid >= exit_threshold AFTER entry
            no_book AS (
                SELECT ts_ms, asset_id, condition_id,
                       (best_bid + best_ask) / 2.0 AS mid,
                       best_bid
                FROM ticks
                WHERE event_type = 'PRICE_CHANGE'
                  AND fee_bps > 0
                  AND outcome = 'NO'
                  AND question ILIKE '%up or down%'
                  AND best_bid > 0 AND best_ask > best_bid
                  AND condition_id IS NOT NULL
            ),
            exit_signals AS (
                SELECT DISTINCT ON (e.condition_id)
                    e.condition_id,
                    nb.ts_ms AS exit_ts,
                    e.no_asset,
                    nb.best_bid AS exit_bid
                FROM no_entry e
                JOIN no_book nb ON e.condition_id = nb.condition_id
                    AND nb.asset_id = e.no_asset
                    AND nb.ts_ms > e.ts_ms
                    AND nb.mid >= {self.exit_threshold}
                ORDER BY e.condition_id, nb.ts_ms
            ),
            -- BUY signals (entry)
            buy_signals AS (
                SELECT
                    ne.ts_ms,
                    ne.no_asset AS asset_id,
                    0 AS side,
                    {self.size} AS size,
                    {order_type} AS order_type,
                    CASE WHEN {order_type} = 1
                         THEN ne.no_bid + 0.01
                         ELSE NULL END ::DOUBLE AS limit_price
                FROM no_entry ne
            ),
            -- SELL signals (exit / take profit)
            sell_signals AS (
                SELECT
                    ex.exit_ts AS ts_ms,
                    ex.no_asset AS asset_id,
                    1 AS side,
                    {self.size} AS size,
                    0 AS order_type,
                    NULL::DOUBLE AS limit_price
                FROM exit_signals ex
            )
            SELECT * FROM buy_signals
            UNION ALL
            SELECT * FROM sell_signals
            ORDER BY ts_ms
        """)
