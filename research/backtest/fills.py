"""Polymarket fill simulation — dynamic fees, latency modeling, maker rebates.

Fee model per Polymarket docs (Jan 2026):
  - Dynamic taker fee: fee = C * feeRate * (p * (1-p))^exponent
  - Crypto markets: feeRate=0.25, exponent=2 (max ~1.56% at p=0.50)
  - Sports markets: feeRate=0.0175, exponent=1 (max ~0.44% at p=0.50)
  - Standard markets: no fee (fee_bps=0 in metadata)
  - Buy fees collected in shares → USD = shares * price
  - Sell fees collected in USDC → direct dollar amount
  - Maker rebate: limit orders receive 20% (crypto) / 25% (sports)
  - Fees rounded to 4 decimal places, min 0.0001 USDC

Latency model:
  - Market orders execute against book state at sig_ts + latency_ms
  - Simulates network delay; faster bots may have moved the book
  - If book moved or liquidity pulled in the delay window, trade
    either fills at a worse price or fails the cost filter

Fill mechanics:
  - Market orders: searchsorted to find book state at arrival time
  - Limit orders: window search for trades crossing limit price
  - Cost filter: reject if spread + round-trip dynamic fee > max_cost
  - Batched by 100 assets to bound peak memory at ~200MB
"""

from dataclasses import dataclass

import numpy as np
import duckdb
import pandas as pd


_ASSET_BATCH = 100  # assets per DuckDB query — keeps peak mem ~200 MB


def _empty_fills_df() -> pd.DataFrame:
    return pd.DataFrame({
        "ts_ms": pd.array([], dtype="int64"),
        "asset_id": pd.array([], dtype="object"),
        "side": pd.array([], dtype="int32"),
        "size": pd.array([], dtype="float64"),
        "price": pd.array([], dtype="float64"),
        "fee": pd.array([], dtype="float64"),
        "slippage": pd.array([], dtype="float64"),
    })


def _dynamic_fee_vec(
    price: np.ndarray,
    size: np.ndarray,
    is_buy: np.ndarray,
    fee_rate: float,
    exponent: int,
) -> np.ndarray:
    """Vectorized Polymarket dynamic taker fee.

    fee = C * feeRate * (p * (1-p))^exponent
      - Buy: fee denominated in shares, convert to USD by multiplying * price
      - Sell: fee denominated in USDC, no conversion needed

    This creates the buy/sell asymmetry documented by Polymarket:
    selling has a higher effective fee rate than buying at the same price.
    """
    base = fee_rate * (price * (1.0 - price)) ** exponent  # per-share fee
    buy_fee = size * base * price   # shares → USD
    sell_fee = size * base          # already USDC
    fee = np.where(is_buy, buy_fee, sell_fee)
    return np.round(fee, 4)  # Polymarket rounds to 4 decimals


@dataclass
class FillModel:
    """Fill simulator with Polymarket's dynamic fee curve and latency modeling.

    Default parameters match Polymarket's crypto market settings (Jan 2026).
    For sports markets, use fee_rate=0.0175, exponent=1, maker_rebate_pct=0.25.
    """
    fee_rate: float = 0.25          # feeRate in dynamic formula
    exponent: int = 2               # crypto=2, sports=1
    maker_rebate_pct: float = 0.20  # maker gets 20% of taker fee back
    network_latency_ms: int = 50    # delay before order reaches exchange
    slippage_bps: float = 5.0       # adverse selection on market orders
    max_fill_pct: float = 0.5       # kept for API compat
    limit_window_ms: int = 60_000   # window for limit fills
    max_cost: float = 0.10          # reject if spread + round-trip fees > this

    def simulate(
        self,
        signals_df: pd.DataFrame,
        con: duckdb.DuckDBPyConnection,
    ) -> pd.DataFrame:
        """Simulate fills for all signals.

        signals_df: columns (ts_ms, asset_id, side, size, order_type, limit_price)
        con: DuckDB connection with `ticks` table

        Returns DataFrame: (ts_ms, asset_id, side, size, price, fee, slippage)
        Fee is positive for taker orders, negative for maker rebates.
        """
        if signals_df.empty:
            return _empty_fills_df()

        slip_rate = self.slippage_bps / 10_000

        has_market = (signals_df["order_type"] == 0).any()
        has_limit = (signals_df["order_type"] == 1).any()

        parts = []

        if has_market:
            mf = self._fill_market_batched(
                signals_df[signals_df["order_type"] == 0], con, slip_rate,
            )
            if not mf.empty:
                parts.append(mf)

        if has_limit:
            lf = self._fill_limit_batched(
                signals_df[
                    (signals_df["order_type"] == 1) & signals_df["limit_price"].notna()
                ],
                con,
            )
            if not lf.empty:
                parts.append(lf)

        if not parts:
            return _empty_fills_df()

        result = pd.concat(parts, ignore_index=True)
        result.sort_values("ts_ms", inplace=True, ignore_index=True)
        return result

    # ------------------------------------------------------------------
    # Market orders — taker fills with latency delay and dynamic fees
    # ------------------------------------------------------------------

    def _fill_market_batched(self, sigs, con, slip_rate):
        sig_groups = sigs.groupby("asset_id")
        assets = list(sig_groups.groups.keys())
        all_fills: list[pd.DataFrame] = []

        for i in range(0, len(assets), _ASSET_BATCH):
            batch = assets[i : i + _ASSET_BATCH]

            # Pull book data + fee_bps for this batch
            batch_tbl = pd.DataFrame({"asset_id": batch})
            con.register("_batch", batch_tbl)
            book = con.execute("""
                SELECT t.ts_ms, t.asset_id, t.best_bid, t.best_ask, t.fee_bps
                FROM ticks t
                SEMI JOIN _batch b ON t.asset_id = b.asset_id
                WHERE t.event_type = 'PRICE_CHANGE'
                  AND t.best_bid > 0 AND t.best_ask > t.best_bid
                ORDER BY t.asset_id, t.ts_ms
            """).fetchdf()
            con.unregister("_batch")

            if book.empty:
                continue

            book_groups = book.groupby("asset_id")

            for asset_id in batch:
                if asset_id not in book_groups.groups:
                    continue

                ab = book_groups.get_group(asset_id)
                asset_sigs = sig_groups.get_group(asset_id)

                # fee_bps > 0 means this is a fee-enabled market (crypto/sports)
                has_fee = bool(ab["fee_bps"].iloc[0] > 0)

                f = self._searchsorted_market(
                    ab["ts_ms"].values,
                    ab["best_bid"].values,
                    ab["best_ask"].values,
                    asset_sigs["ts_ms"].values,
                    asset_sigs["side"].values.astype(np.int32),
                    asset_sigs["size"].values,
                    slip_rate, asset_id, has_fee,
                )
                if f is not None:
                    all_fills.append(f)

        return pd.concat(all_fills, ignore_index=True) if all_fills else _empty_fills_df()

    def _searchsorted_market(
        self, book_ts, best_bid, best_ask,
        sig_ts, sig_sides, sig_sizes,
        slip_rate, asset_id, has_fee,
    ):
        """Vectorized market fill with latency delay and dynamic fees.

        Instead of looking at the book at signal time, we look at
        sig_ts + network_latency_ms — the time the order actually arrives
        at the exchange. A faster HFT bot may have moved the book by then.
        """
        # Latency: order arrives at sig_ts + network_latency_ms
        arrival_ts = sig_ts + self.network_latency_ms

        # Book state at arrival time (not signal time)
        idx = np.searchsorted(book_ts, arrival_ts, side="right") - 1
        valid = idx >= 0
        if not valid.any():
            return None

        idx = idx[valid]
        sig_ts = sig_ts[valid]
        sig_sides, sig_sizes = sig_sides[valid], sig_sizes[valid]

        bb = best_bid[idx]
        ba = best_ask[idx]

        # Cost filter: spread + round-trip dynamic fee
        spread = ba - bb
        if has_fee:
            mid = (bb + ba) / 2.0
            base = self.fee_rate * (mid * (1.0 - mid)) ** self.exponent
            round_trip_fee = 2.0 * base  # approximate buy+sell per share
        else:
            round_trip_fee = 0.0

        cost_ok = (spread + round_trip_fee) <= self.max_cost

        if not cost_ok.any():
            return None

        sig_ts = sig_ts[cost_ok]
        sig_sides, sig_sizes = sig_sides[cost_ok], sig_sizes[cost_ok]
        bb, ba = bb[cost_ok], ba[cost_ok]

        # Fill prices with slippage
        is_buy = sig_sides == 0
        buy_price = np.minimum(ba * (1.0 + slip_rate), 1.0)
        sell_price = np.maximum(bb * (1.0 - slip_rate), 0.0)
        price = np.where(is_buy, buy_price, sell_price)

        # Dynamic taker fee (or zero for fee-free markets)
        if has_fee:
            fee = _dynamic_fee_vec(
                price, sig_sizes, is_buy,
                self.fee_rate, self.exponent,
            )
        else:
            fee = np.zeros(len(price))

        slippage = np.where(is_buy, ba * slip_rate, bb * slip_rate)

        return pd.DataFrame({
            "ts_ms": sig_ts,
            "asset_id": asset_id,
            "side": sig_sides,
            "size": sig_sizes,
            "price": price,
            "fee": fee,
            "slippage": slippage,
        })

    # ------------------------------------------------------------------
    # Limit orders — maker fills with rebate (negative fee)
    # ------------------------------------------------------------------

    def _fill_limit_batched(self, sigs, con):
        sig_groups = sigs.groupby("asset_id")
        assets = list(sig_groups.groups.keys())
        all_fills: list[pd.DataFrame] = []
        window_ms = self.limit_window_ms

        for i in range(0, len(assets), _ASSET_BATCH):
            batch = assets[i : i + _ASSET_BATCH]

            batch_tbl = pd.DataFrame({"asset_id": batch})
            con.register("_batch", batch_tbl)
            trades = con.execute("""
                SELECT t.ts_ms, t.asset_id, t.price, t.fee_bps
                FROM ticks t
                SEMI JOIN _batch b ON t.asset_id = b.asset_id
                WHERE t.event_type = 'LAST_TRADE'
                ORDER BY t.asset_id, t.ts_ms
            """).fetchdf()
            con.unregister("_batch")

            if trades.empty:
                continue

            trade_groups = trades.groupby("asset_id")

            for asset_id in batch:
                if asset_id not in trade_groups.groups:
                    continue

                at = trade_groups.get_group(asset_id)
                asset_sigs = sig_groups.get_group(asset_id)

                has_fee = bool(at["fee_bps"].iloc[0] > 0)

                f = self._window_limit(
                    at["ts_ms"].values,
                    at["price"].values,
                    asset_sigs["ts_ms"].values,
                    asset_sigs["side"].values.astype(np.int32),
                    asset_sigs["size"].values,
                    asset_sigs["limit_price"].values,
                    window_ms, asset_id, has_fee,
                )
                if f is not None:
                    all_fills.append(f)

        return pd.concat(all_fills, ignore_index=True) if all_fills else _empty_fills_df()

    def _window_limit(
        self, trade_ts, trade_prices,
        sig_ts, sig_sides, sig_sizes, sig_limits,
        window_ms, asset_id, has_fee,
    ):
        """Limit fills for one asset with maker rebate.

        Limit orders are maker orders — they add liquidity. On fee-enabled
        markets, makers receive a rebate (negative fee) equal to
        maker_rebate_pct of the taker fee that would have been charged.
        """
        n = len(sig_ts)

        # Vectorised: find the trade-window bounds for all signals at once
        starts = np.searchsorted(trade_ts, sig_ts, side="left")
        ends = np.searchsorted(trade_ts, sig_ts + window_ms, side="right")

        fill_ts = np.empty(n, dtype=np.int64)
        filled = np.zeros(n, dtype=np.bool_)

        for j in range(n):
            s, e = starts[j], ends[j]
            if s >= e:
                continue
            w = trade_prices[s:e]
            if sig_sides[j] == 0:  # BID — buy if someone sold at or below limit
                mask = w <= sig_limits[j]
            else:                  # ASK — sell if someone bought at or above limit
                mask = w >= sig_limits[j]
            if mask.any():
                fill_ts[j] = trade_ts[s + np.argmax(mask)]
                filled[j] = True

        if not filled.any():
            return None

        fill_prices = sig_limits[filled]
        fill_sizes = sig_sizes[filled]
        fill_sides = sig_sides[filled]
        is_buy = fill_sides == 0

        if has_fee:
            # Maker rebate: negative fee = profit for the maker
            # Rebate = maker_rebate_pct * what the taker fee would have been
            taker_fee = _dynamic_fee_vec(
                fill_prices, fill_sizes, is_buy,
                self.fee_rate, self.exponent,
            )
            fee = -self.maker_rebate_pct * taker_fee
        else:
            fee = np.zeros(filled.sum())

        return pd.DataFrame({
            "ts_ms": fill_ts[filled],
            "asset_id": asset_id,
            "side": fill_sides,
            "size": fill_sizes,
            "price": fill_prices,
            "fee": fee,
            "slippage": np.zeros(filled.sum()),
        })
