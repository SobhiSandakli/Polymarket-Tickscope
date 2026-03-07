#!/usr/bin/env bash
set -euo pipefail

# bot_daily_s3.sh
#
# Creates daily bot artifacts from paper_trades.log + paper_status.log and uploads them to S3.
# Intended schedule: once per day shortly after 00:00 UTC.
#
# Output artifacts:
#   bot_trades_YYYY-MM-DD.log.gz
#   bot_status_YYYY-MM-DD.log.gz
#
# Env vars (override as needed):
#   BOT_LOG_PATH        default: /opt/polymarket/paper_trades.log
#   BOT_STATUS_LOG_PATH default: /opt/polymarket/paper_status.log
#   BOT_ARCHIVE_DIR     default: /opt/polymarket/bot_archive
#   BOT_S3_URI          default: s3://CHANGE_ME/polymarket-bot
#   TARGET_DATE_UTC     default: yesterday in UTC (YYYY-MM-DD)

BOT_LOG_PATH="${BOT_LOG_PATH:-/opt/polymarket/paper_trades.log}"
BOT_STATUS_LOG_PATH="${BOT_STATUS_LOG_PATH:-/opt/polymarket/paper_status.log}"
BOT_ARCHIVE_DIR="${BOT_ARCHIVE_DIR:-/opt/polymarket/bot_archive}"
BOT_S3_URI="${BOT_S3_URI:-s3://CHANGE_ME/polymarket-bot}"
TARGET_DATE_UTC="${TARGET_DATE_UTC:-$(date -u -d 'yesterday' +%F)}"

if [[ "$BOT_S3_URI" == *"CHANGE_ME"* ]]; then
  echo "ERROR: set BOT_S3_URI to your real S3 path (e.g. s3://my-bucket/polymarket-bot)" >&2
  exit 1
fi

if [[ ! -f "$BOT_LOG_PATH" ]]; then
  echo "ERROR: bot trades log file not found at $BOT_LOG_PATH" >&2
  exit 1
fi

mkdir -p "$BOT_ARCHIVE_DIR"

out_base="bot_trades_${TARGET_DATE_UTC}.log"
out_log="${BOT_ARCHIVE_DIR}/${out_base}"
out_gz="${out_log}.gz"

# Log line format starts with:
#   YYYY-MM-DD HH:MM:SS UTC | ...
grep -E "^${TARGET_DATE_UTC} " "$BOT_LOG_PATH" > "$out_log" || true

if [[ ! -s "$out_log" ]]; then
  echo "WARNING: no bot trades found for ${TARGET_DATE_UTC}; creating empty marker file"
  echo "no trades for ${TARGET_DATE_UTC}" > "$out_log"
fi

gzip -f "$out_log"

s3_dest="${BOT_S3_URI}/daily/date=${TARGET_DATE_UTC}/$(basename "$out_gz")"
aws s3 cp "$out_gz" "$s3_dest" --no-progress --only-show-errors

echo "Uploaded: $out_gz -> $s3_dest"

status_base="bot_status_${TARGET_DATE_UTC}.log"
status_log="${BOT_ARCHIVE_DIR}/${status_base}"
status_gz="${status_log}.gz"

if [[ -f "$BOT_STATUS_LOG_PATH" ]]; then
  grep -E "^${TARGET_DATE_UTC} " "$BOT_STATUS_LOG_PATH" > "$status_log" || true
  if [[ ! -s "$status_log" ]]; then
    echo "no status snapshots for ${TARGET_DATE_UTC}" > "$status_log"
  fi
  gzip -f "$status_log"

  status_s3_dest="${BOT_S3_URI}/daily/date=${TARGET_DATE_UTC}/$(basename "$status_gz")"
  aws s3 cp "$status_gz" "$status_s3_dest" --no-progress --only-show-errors
  echo "Uploaded: $status_gz -> $status_s3_dest"
else
  echo "WARNING: status log file not found at $BOT_STATUS_LOG_PATH (status artifact skipped)"
fi
