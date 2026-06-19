#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# local_tester.sh — converts completed journal segments to Parquet (local dev)
#
# Watches DATA_DIR for closed .bin files (Polymarket + Coinbase) and converts
# them to Parquet for use in research notebooks.
#
# A file is "closed" when its mtime is older than the rotation interval plus
# a safety margin — the C++ process has moved on to a new file by then.
#
# Usage:
#   bash scripts/harvester/local_tester.sh              # foreground
#   bash scripts/harvester/local_tester.sh &            # background
#   DATA_DIR=/custom/path bash scripts/harvester/local_tester.sh
#
# Environment variables (all optional):
#   DATA_DIR          — directory containing .bin journals (default: data/)
#   INTERVAL_SECONDS  — must match C++ rotation interval (default: 900 = 15 min)
#   MARGIN_SECONDS    — safety margin before touching a file (default: 120)
#   ARCHIVE_TTL_HOURS — delete raw .bin from archive after N hours (default: 24)
# ---------------------------------------------------------------------------

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

DATA_DIR="${DATA_DIR:-$PROJECT_DIR/data}"
ARCHIVE_DIR="$DATA_DIR/archive"
POLY_SCRIPT="$SCRIPT_DIR/log_to_parquet.py"
BTC_SCRIPT="$SCRIPT_DIR/btc_to_parquet.py"
INTERVAL_SECONDS="${INTERVAL_SECONDS:-$((15 * 60))}"   # 15 min default
MARGIN_SECONDS="${MARGIN_SECONDS:-120}"                 # 2 min safety
ARCHIVE_TTL_HOURS="${ARCHIVE_TTL_HOURS:-24}"            # keep raw .bin for 24h

mkdir -p "$DATA_DIR" "$ARCHIVE_DIR"

# Portable mtime: macOS uses "stat -f %m", Linux uses "stat -c %Y"
if [[ "$OSTYPE" == "darwin"* ]]; then
    file_mtime() { stat -f %m "$1"; }
else
    file_mtime() { stat -c %Y "$1"; }
fi

echo "[local_tester] Watching $DATA_DIR for closed journal segments"
echo "[local_tester] Interval=${INTERVAL_SECONDS}s  Margin=${MARGIN_SECONDS}s"
echo "[local_tester] Archive → $ARCHIVE_DIR"
echo "[local_tester] Press Ctrl-C to stop."

while true; do
    now=$(date +%s)
    cutoff=$((INTERVAL_SECONDS + MARGIN_SECONDS))

    # ── Polymarket journals ──────────────────────────────────────────────────
    for bin_file in "$DATA_DIR"/polymarket_*.bin; do
        [ -f "$bin_file" ] || continue
        age=$((now - $(file_mtime "$bin_file")))
        if [ "$age" -gt "$cutoff" ]; then
            base="$(basename "$bin_file" .bin)"
            parquet_file="$DATA_DIR/${base}.parquet"
            echo "[$(date -u +%H:%M:%S)] Converting $(basename "$bin_file") ..."
            if python3 "$POLY_SCRIPT" "$bin_file" "$parquet_file"; then
                mv "$bin_file" "$ARCHIVE_DIR/"
                echo "[$(date -u +%H:%M:%S)] Done → $(basename "$parquet_file"), raw archived"
            else
                echo "[$(date -u +%H:%M:%S)] WARN: conversion failed for $(basename "$bin_file")"
            fi
        fi
    done

    # ── Coinbase BTC journals ─────────────────────────────────────────────────
    for bin_file in "$DATA_DIR"/btc_*.bin; do
        [ -f "$bin_file" ] || continue
        age=$((now - $(file_mtime "$bin_file")))
        if [ "$age" -gt "$cutoff" ]; then
            base="$(basename "$bin_file" .bin)"
            parquet_file="$DATA_DIR/${base}.parquet"
            echo "[$(date -u +%H:%M:%S)] Converting $(basename "$bin_file") ..."
            if python3 "$BTC_SCRIPT" "$bin_file" "$parquet_file"; then
                mv "$bin_file" "$ARCHIVE_DIR/"
                echo "[$(date -u +%H:%M:%S)] Done → $(basename "$parquet_file"), raw archived"
            else
                echo "[$(date -u +%H:%M:%S)] WARN: conversion failed for $(basename "$bin_file")"
            fi
        fi
    done

    # ── Purge archived .bin files older than TTL ─────────────────────────────
    archive_age_limit_s=$((ARCHIVE_TTL_HOURS * 3600))
    for old_bin in "$ARCHIVE_DIR"/*.bin; do
        [ -f "$old_bin" ] || continue
        old_age=$((now - $(file_mtime "$old_bin")))
        if [ "$old_age" -gt "$archive_age_limit_s" ]; then
            rm "$old_bin"
            echo "[$(date -u +%H:%M:%S)] Purged $(basename "$old_bin") (${old_age}s old)"
        fi
    done

    sleep 60
done
