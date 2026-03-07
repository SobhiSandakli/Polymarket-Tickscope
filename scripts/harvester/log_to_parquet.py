#!/usr/bin/env python3
"""
log_to_parquet.py — polymarket HFT journal decoder.

Reads the binary journal written by the Tickerplant (128-byte Tick records),
decodes every field, and writes a typed Parquet file suitable for analysis
and backtesting.

Binary layout (matches include/polymarket/core/Tick.hpp):
    Offset  Field        C type      Bytes   Notes
       0    timestamp    uint64_t      8     Unix epoch ms
       8    price        double         8     probability [0,1]
      16    size         double         8     order / trade quantity
      24    best_bid     double         8     0.0 for last_trade_price events
      32    best_ask     double         8     0.0 for last_trade_price events
      40    side         uint8_t        1     0=BID, 1=ASK
      41    event_type   uint8_t        1     0=price_change, 1=last_trade_price
      42    asset_id     char[80]      80     null-terminated token ID
     122    (padding)    —              6     cache-line alignment
                                     ────
                                     128    = sizeof(Tick)

Struct format: '<QddddBB80s6x'
  Q   = uint64_t  (8)   timestamp
  d   = double    (8)   price
  d   = double    (8)   size
  d   = double    (8)   best_bid
  d   = double    (8)   best_ask
  B   = uint8_t   (1)   side
  B   = uint8_t   (1)   event_type
  80s = char[80]  (80)  asset_id
  6x  = padding   (6)
Total = 128 bytes
"""

import argparse
import struct
import sys
import time
from pathlib import Path

import numpy as np
import pandas as pd

# ---------------------------------------------------------------------------
# Constants — single source of truth for the wire format
# ---------------------------------------------------------------------------
TICK_FMT  = struct.Struct('<QddddBB80s6x')
TICK_SIZE = TICK_FMT.size          # must be 128
assert TICK_SIZE == 128, f"TICK_SIZE={TICK_SIZE}, expected 128 — struct mismatch!"

SIDE_MAP       = {0: 'BID',          1: 'ASK'}
EVENT_TYPE_MAP = {0: 'PRICE_CHANGE', 1: 'LAST_TRADE'}


# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Decode a polymarket binary journal to Parquet.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        'input_path',
        nargs='?',
        type=Path,
        default=Path('/tmp/polymarket_journal.bin'),
        help='Path to the binary journal file.',
    )
    parser.add_argument(
        'output_path',
        nargs='?',
        type=Path,
        default=None,
        help='Path for the output Parquet file (default: input with .parquet extension).',
    )
    args = parser.parse_args()

    if args.output_path is None:
        args.output_path = args.input_path.with_suffix('.parquet')

    return args


# ---------------------------------------------------------------------------
# I/O helpers
# ---------------------------------------------------------------------------
def read_journal(path: Path) -> bytes:
    with open(path, 'rb') as f:
        return f.read()


def validate_size(data: bytes) -> tuple[int, int]:
    n_complete, remainder = divmod(len(data), TICK_SIZE)

    if remainder > 0:
        print(
            f"WARNING: {remainder} trailing bytes ignored "
            f"(truncated record — unclean shutdown?)"
        )

    if n_complete == 0:
        print(
            f"ERROR: journal is empty or too small to contain a single record "
            f"({len(data)} bytes < {TICK_SIZE}).",
            file=sys.stderr,
        )
        sys.exit(1)

    return n_complete, remainder


# ---------------------------------------------------------------------------
# Decoding
# ---------------------------------------------------------------------------
def decode_records(data: bytes, n: int) -> pd.DataFrame:
    """
    Unpack n Tick records from data and return a typed DataFrame.
    Pre-allocates NumPy arrays for all numeric fields.
    """
    timestamps   = np.empty(n, dtype=np.uint64)
    prices       = np.empty(n, dtype=np.float64)
    sizes        = np.empty(n, dtype=np.float64)
    best_bids    = np.empty(n, dtype=np.float64)
    best_asks    = np.empty(n, dtype=np.float64)
    sides_raw    = np.empty(n, dtype=np.uint8)
    event_types  = np.empty(n, dtype=np.uint8)
    asset_ids    = []

    for i, (ts, price, size, best_bid, best_ask,
            side, event_type, asset_bytes) in enumerate(
        TICK_FMT.iter_unpack(data[:n * TICK_SIZE])
    ):
        timestamps[i]  = ts
        prices[i]      = price
        sizes[i]       = size
        best_bids[i]   = best_bid
        best_asks[i]   = best_ask
        sides_raw[i]   = side
        event_types[i] = event_type
        asset_ids.append(asset_bytes.rstrip(b'\x00').decode('utf-8'))

    ts_dt = pd.to_datetime(timestamps, unit='ms', utc=True)

    df = pd.DataFrame({
        'timestamp':  ts_dt,
        'price':      prices,
        'size':       sizes,
        'best_bid':   best_bids,
        'best_ask':   best_asks,
        'side':       pd.Categorical(
                          [SIDE_MAP.get(int(s), 'UNKNOWN') for s in sides_raw],
                          categories=['BID', 'ASK'],
                      ),
        'event_type': pd.Categorical(
                          [EVENT_TYPE_MAP.get(int(e), 'UNKNOWN') for e in event_types],
                          categories=['PRICE_CHANGE', 'LAST_TRADE'],
                      ),
        'asset_id':   asset_ids,
    })

    return df


# ---------------------------------------------------------------------------
# Parquet output
# ---------------------------------------------------------------------------
def write_parquet(df: pd.DataFrame, path: Path) -> None:
    df.to_parquet(path, engine='pyarrow', compression='snappy', index=False)


# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
def print_summary(
    df: pd.DataFrame,
    input_path: Path,
    output_path: Path,
    elapsed_s: float,
) -> None:
    input_mb   = input_path.stat().st_size  / 1_048_576
    output_mb  = output_path.stat().st_size / 1_048_576
    throughput = input_mb / elapsed_s if elapsed_s > 0 else float('inf')

    t_min = df['timestamp'].min()
    t_max = df['timestamp'].max()

    unique_assets = df['asset_id'].unique().tolist()
    side_counts   = df['side'].value_counts()
    event_counts  = df['event_type'].value_counts()

    print()
    print(f"Input:   {input_path}  ({input_mb:.2f} MB, {len(df):,} records)")
    print(f"Output:  {output_path}  ({output_mb:.2f} MB)")
    print(f"Time range: {t_min}  →  {t_max}")
    print(f"Unique assets: {len(unique_assets)}  "
          f"({unique_assets[:5]}{'...' if len(unique_assets) > 5 else ''})")

    side_parts = '  '.join(
        f"{side}={side_counts.get(side, 0):,}" for side in ['BID', 'ASK']
    )
    print(f"Side counts: {side_parts}")

    event_parts = '  '.join(
        f"{et}={event_counts.get(et, 0):,}"
        for et in ['PRICE_CHANGE', 'LAST_TRADE']
    )
    print(f"Event types: {event_parts}")
    print(f"Throughput: {throughput:.1f} MB/s")
    print()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def main() -> None:
    args = parse_args()

    t0 = time.perf_counter()

    data = read_journal(args.input_path)
    n, _ = validate_size(data)
    df   = decode_records(data, n)
    write_parquet(df, args.output_path)

    elapsed = time.perf_counter() - t0
    print_summary(df, args.input_path, args.output_path, elapsed)


if __name__ == '__main__':
    main()
