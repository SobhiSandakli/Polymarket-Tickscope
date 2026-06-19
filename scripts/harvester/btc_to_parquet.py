#!/usr/bin/env python3
"""
btc_to_parquet.py — Coinbase BtcTick journal decoder.

Reads the binary journal written by the Coinbase harvester (64-byte BtcTick
records), decodes every field, and writes a typed Parquet file.

Binary layout (matches include/polymarket/core/BtcTick.hpp):
    Offset  Field        C type      Bytes   Notes
       0    timestamp    uint64_t      8     Unix epoch ms (local clock)
       8    bid          double        8     Coinbase BTCUSDT best bid
      16    ask          double        8     Coinbase BTCUSDT best ask
      24    mid          double        8     (bid+ask)/2 precomputed
      32    (padding)    —            32     cache-line alignment
                                    ────
                                      64    = sizeof(BtcTick)

Struct format: '<Qddd32x'
  Q   = uint64_t  (8)   timestamp
  d   = double    (8)   bid
  d   = double    (8)   ask
  d   = double    (8)   mid
  32x = padding   (32)
  Total = 64 bytes

Usage:
    python3 scripts/harvester/btc_to_parquet.py btc_20260307_0930.bin [output.parquet]
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
TICK_FMT  = struct.Struct('<Qddd32x')
TICK_SIZE = TICK_FMT.size
assert TICK_SIZE == 64, f"TICK_SIZE={TICK_SIZE}, expected 64 — struct mismatch!"


# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Decode a Coinbase BtcTick binary journal to Parquet.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        'input_path',
        type=Path,
        help='Path to the binary journal file (btc_YYYYMMDD_HHMM.bin).',
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
# Decoding
# ---------------------------------------------------------------------------
def decode_records(data: bytes, n: int) -> pd.DataFrame:
    timestamps = np.empty(n, dtype=np.uint64)
    bids       = np.empty(n, dtype=np.float64)
    asks       = np.empty(n, dtype=np.float64)
    mids       = np.empty(n, dtype=np.float64)

    for i, (ts, bid, ask, mid) in enumerate(
        TICK_FMT.iter_unpack(data[:n * TICK_SIZE])
    ):
        timestamps[i] = ts
        bids[i]       = bid
        asks[i]       = ask
        mids[i]       = mid

    ts_dt = pd.to_datetime(timestamps, unit='ms', utc=True)

    return pd.DataFrame({
        'timestamp': ts_dt,
        'bid':       bids,
        'ask':       asks,
        'mid':       mids,
    })


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main() -> None:
    args = parse_args()

    t0 = time.perf_counter()

    data = args.input_path.read_bytes()
    n, remainder = divmod(len(data), TICK_SIZE)

    if remainder > 0:
        print(f"WARNING: {remainder} trailing bytes ignored (truncated record?)")
    if n == 0:
        print(f"ERROR: journal is empty ({len(data)} bytes < {TICK_SIZE})",
              file=sys.stderr)
        sys.exit(1)

    df = decode_records(data, n)
    df.to_parquet(args.output_path, engine='pyarrow', compression='snappy', index=False)

    elapsed = time.perf_counter() - t0
    input_mb  = args.input_path.stat().st_size  / 1_048_576
    output_mb = args.output_path.stat().st_size / 1_048_576

    print(f"\nInput:      {args.input_path}  ({input_mb:.2f} MB, {n:,} records)")
    print(f"Output:     {args.output_path}  ({output_mb:.2f} MB)")
    print(f"Time range: {df['timestamp'].min()}  →  {df['timestamp'].max()}")
    print(f"BTC range:  ${df['mid'].min():,.2f} – ${df['mid'].max():,.2f}")
    print(f"Decoded in  {elapsed:.2f}s")


if __name__ == '__main__':
    main()
