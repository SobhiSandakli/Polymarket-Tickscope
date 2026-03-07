#!/usr/bin/env bash
# =============================================================================
# hourly_flush.sh — polymarket data pipeline flush script
#
# Runs every hour via cron.  For each CLOSED .bin journal file (i.e. any file
# the Tickerplant is no longer writing to):
#   1. Convert .bin → .parquet  via log_to_parquet.py  (~89 MB peak RAM)
#   2. Upload .parquet + market_metadata.csv to S3
#   3. Delete local .bin and .parquet to keep the 20 GB EBS volume free
#
# A file is considered "closed" if it has not been modified in the last
# 20 minutes.  The Tickerplant rotates every 15 minutes, so any file older
# than 20 minutes is guaranteed to be sealed and safe to process.
#
# Usage
# ─────
#   Export the following variables (or set defaults below):
#
#   Cron entry (runs at :05 past every hour, avoids the :00 rotation boundary):
#     5 * * * * ubuntu /opt/polymarket/scripts/hourly_flush.sh >> /var/log/polymarket_flush.log 2>&1
#
# Exit codes
# ──────────
#   0 — all conversions and uploads succeeded (or nothing to do)
#   1 — one or more conversions failed  (local files NOT deleted on failure)
#   2 — S3 upload failed after successful conversion  (local parquets retained)
# =============================================================================
set -euo pipefail

# ── Configuration (override via environment or edit defaults here) ────────────
DATA_DIR="${POLYMARKET_DATA_DIR:-/opt/polymarket/data}"
PARQUET_DIR="${POLYMARKET_PARQUET_DIR:-/opt/polymarket/parquet}"
BINANCE_DIR="${POLYMARKET_BINANCE_DIR:-/opt/polymarket/binance}"
S3_URI="${POLYMARKET_S3_URI:-s3://CHANGE_ME/polymarket}"
PYTHON="${POLYMARKET_PYTHON:-/usr/bin/python3}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_PFX="[hourly_flush $(date -u '+%Y-%m-%dT%H:%M:%SZ')]"

# Minimum age in minutes before a .bin file is considered closed.
# Tickerplant rotates every 15 min; 20 min gives a safe margin.
MIN_AGE_MINUTES=20

# ── Sanity checks ─────────────────────────────────────────────────────────────
if [[ "$S3_URI" == *"CHANGE_ME"* ]]; then
    echo "$LOG_PFX ERROR: POLYMARKET_S3_URI is not set. Export it or edit this script." >&2
    exit 1
fi

if [[ ! -d "$DATA_DIR" ]]; then
    echo "$LOG_PFX ERROR: DATA_DIR '$DATA_DIR' does not exist." >&2
    exit 1
fi

mkdir -p "$PARQUET_DIR" "$BINANCE_DIR"

# ── Find closed .bin files ────────────────────────────────────────────────────
# -mmin +N  →  files whose mtime is MORE than N minutes in the past.
# sort ensures deterministic processing order (oldest first).
mapfile -t closed_bins < <(
    find "$DATA_DIR" -maxdepth 1 -name "polymarket_*.bin" \
         -mmin +"$MIN_AGE_MINUTES" -type f | sort
)

if [[ ${#closed_bins[@]} -eq 0 ]]; then
    echo "$LOG_PFX No closed .bin files found (nothing to do)."
    exit 0
fi

echo "$LOG_PFX Found ${#closed_bins[@]} closed .bin file(s) to process."

# ── Convert each .bin → .parquet ─────────────────────────────────────────────
converted_bins=()    # .bin paths that converted successfully
converted_parquets=() # corresponding .parquet paths

failed=0

for bin_file in "${closed_bins[@]}"; do
    base="$(basename "$bin_file" .bin)"
    parquet_file="$PARQUET_DIR/${base}.parquet"

    echo "$LOG_PFX  convert: $bin_file → $parquet_file"

    if "$PYTHON" "$SCRIPT_DIR/log_to_parquet.py" "$bin_file" "$parquet_file"; then
        converted_bins+=("$bin_file")
        converted_parquets+=("$parquet_file")
        echo "$LOG_PFX  ok: $(du -sh "$parquet_file" | cut -f1) written"
    else
        echo "$LOG_PFX  ERROR: conversion failed for $bin_file — skipping." >&2
        (( failed++ )) || true
    fi
done

if [[ ${#converted_parquets[@]} -eq 0 ]]; then
    echo "$LOG_PFX No files converted successfully."
    exit 1
fi

# ── Upload .parquet files to S3 ───────────────────────────────────────────────
echo "$LOG_PFX Uploading ${#converted_parquets[@]} parquet file(s) to ${S3_URI}/parquet/"

# aws s3 cp each file individually so we can verify per-file success.
upload_ok=true
for parquet_file in "${converted_parquets[@]}"; do
    base="$(basename "$parquet_file")"
    s3_dest="${S3_URI}/parquet/${base}"
    echo "$LOG_PFX  upload: $parquet_file → $s3_dest"
    if ! aws s3 cp "$parquet_file" "$s3_dest" \
            --storage-class STANDARD \
            --no-progress \
            --only-show-errors; then
        echo "$LOG_PFX  ERROR: upload failed for $parquet_file" >&2
        upload_ok=false
    fi
done

# Upload market_metadata.csv (overwrite; it's regenerated at every C++ startup)
metadata_csv="$DATA_DIR/market_metadata.csv"
if [[ -f "$metadata_csv" ]]; then
    echo "$LOG_PFX  upload: market_metadata.csv → ${S3_URI}/market_metadata.csv"
    aws s3 cp "$metadata_csv" "${S3_URI}/market_metadata.csv" \
        --no-progress \
        --only-show-errors || echo "$LOG_PFX  WARNING: metadata CSV upload failed (non-fatal)" >&2
fi

if [[ "$upload_ok" != "true" ]]; then
    echo "$LOG_PFX ERROR: one or more S3 uploads failed. Local files retained for retry." >&2
    exit 2
fi

# ── Delete local files after confirmed S3 upload ─────────────────────────────
echo "$LOG_PFX S3 sync complete. Removing local files..."

for i in "${!converted_bins[@]}"; do
    bin_file="${converted_bins[$i]}"
    parquet_file="${converted_parquets[$i]}"
    rm -f "$bin_file"
    rm -f "$parquet_file"
    echo "$LOG_PFX  deleted: $(basename "$bin_file")  $(basename "$parquet_file")"
done

# ── Process Binance BTC journals (btc_*.bin) ────────────────────────────────
mapfile -t closed_btc < <(
    find "$DATA_DIR" -maxdepth 1 -name "btc_*.bin" \
         -mmin +"$MIN_AGE_MINUTES" -type f | sort
)

if [[ ${#closed_btc[@]} -gt 0 ]]; then
    echo "$LOG_PFX Found ${#closed_btc[@]} closed btc .bin file(s) to process."

    for bin_file in "${closed_btc[@]}"; do
        base="$(basename "$bin_file" .bin)"
        parquet_file="$BINANCE_DIR/${base}.parquet"

        echo "$LOG_PFX  convert: $bin_file → $parquet_file"

        if "$PYTHON" "$SCRIPT_DIR/btc_to_parquet.py" "$bin_file" "$parquet_file"; then
            s3_dest="${S3_URI}/binance/${base}.parquet"
            if aws s3 cp "$parquet_file" "$s3_dest" --no-progress --only-show-errors; then
                rm -f "$bin_file" "$parquet_file"
                echo "$LOG_PFX  ok: uploaded + cleaned ${base}"
            else
                echo "$LOG_PFX  ERROR: S3 upload failed for $parquet_file" >&2
            fi
        else
            echo "$LOG_PFX  ERROR: conversion failed for $bin_file" >&2
            (( failed++ )) || true
        fi
    done
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo "$LOG_PFX Done. Uploaded: ${#converted_parquets[@]} poly + ${#closed_btc[@]} btc, Failed: $failed"

# Exit 1 if any conversion failed (cron will log it), but we processed the rest.
[[ $failed -eq 0 ]]
