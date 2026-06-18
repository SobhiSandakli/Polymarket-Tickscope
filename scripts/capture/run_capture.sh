#!/usr/bin/env bash
# =============================================================================
# run_capture.sh — one-command joint capture: one-or-more matches + BTC, same clock
#
# Launches collectors that all stamp the SAME host clock, so every feed joins on
# ts_ms in DuckDB later:
#
#   1. polymarket_harvester  — Polymarket CLOB ticks for EVERY match market passed
#                              AND the BTC up/down markets, in ONE process. The
#                              harvester takes a comma-separated OR filter, so a
#                              single subscription covers every market class:
#                                  "Canada vs. Qatar,Mexico vs. Korea,Bitcoin Up or Down"
#                              → data/polymarket_YYYYMMDD_HHMM.bin (+ market_metadata.csv)
#
#   2. binance_harvester     — Binance BTCUSDT top-of-book, the BTC reference feed.
#                              Runs continuously across all matches.
#                              → data/btc_YYYYMMDD_HHMM.bin
#
#   3. espn_collector.py     — ONE per match (ESPN soccer score feed, 1s poll).
#                              Each exits on its own at that match's full time; the
#                              capture keeps running until ALL of them have exited.
#                              Goal timestamps are the lag-measurement events.
#                              → data/espn_<sport>_<event>.csv
#
# Capturing several matches in one session is deliberate: a 0-0 group game yields
# no goals (no lag datapoint), so we capture back-to-back fixtures and keep the
# ones that score. BTC capture runs the whole time regardless.
#
# Usage
# ─────
#   Each match is one positional arg of the form  "<polymarket filter>@<espn event id>".
#
#   ./scripts/capture/run_capture.sh \
#       "Canada vs. Qatar@<event_id_1>" \
#       "Mexico vs. Korea@<event_id_2>"
#
#   # fixed window then auto stop+decode (seconds), instead of waiting for full time:
#   DURATION=21600 ./scripts/capture/run_capture.sh "Canada vs. Qatar@123" "Mexico vs. Korea@456"
#
# How to find the two values per match (see docs/CAPTURE_RUNBOOK.md §B):
#   - ESPN event id:
#       curl -s "https://site.api.espn.com/apis/site/v2/sports/soccer/fifa.world/scoreboard" \
#         | python3 -c "import json,sys; [print(e['id'], e['name'], e['date']) for e in json.load(sys.stdin)['events']]"
#   - Polymarket filter: dry-run the harvester and confirm it resolves to ~2-12 tokens:
#       POLYMARKET_MARKET_FILTER="Canada vs. Qatar" ./build/src/harvester/polymarket_harvester
#       # look for: [market-discovery] filter '...' → N/10000 tokens   (Ctrl-C after)
#
# Optional env (defaults shown):
#   ESPN_SPORT="soccer/fifa.world"   ESPN sport path (same for all matches).
#   BTC_FILTER="Bitcoin Up or Down"  Polymarket filter for the BTC binary markets.
#   DATA_DIR="<repo>/data"           Where journals + CSV are written.
#   DURATION=""                      Seconds to run then auto-stop. Empty = until all
#                                    matches reach full time (or Ctrl-C).
#   REDISCOVER_MINS=5                Polymarket re-discovery interval. The BTC up/down
#                                    markets roll over every few minutes, so keep this
#                                    short so new condition IDs get subscribed.
# =============================================================================
set -euo pipefail

# ── Resolve repo root (script lives in <repo>/scripts/capture/) ──────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ── Config ───────────────────────────────────────────────────────────────────
ESPN_SPORT="${ESPN_SPORT:-soccer/fifa.world}"
BTC_FILTER="${BTC_FILTER:-Bitcoin Up or Down}"
DATA_DIR="${DATA_DIR:-$REPO_ROOT/data}"
DURATION="${DURATION:-}"
REDISCOVER_MINS="${REDISCOVER_MINS:-5}"
PYTHON="${PYTHON:-python3}"

POLY_BIN="$REPO_ROOT/build/src/harvester/polymarket_harvester"
BNB_BIN="$REPO_ROOT/build/src/binance/binance_harvester"
ESPN_PY="$REPO_ROOT/scripts/collectors/espn_collector.py"

# ── Parse matches (each arg: "<filter>@<espn event id>") ─────────────────────
[[ $# -ge 1 ]] || { echo "ERROR: pass at least one match as \"<filter>@<espn event id>\"" >&2
    echo "  e.g. $0 \"Canada vs. Qatar@726143\" \"Mexico vs. Korea@726145\"" >&2; exit 1; }

match_filters=()   # polymarket filter per match
match_events=()    # espn event id per match
for arg in "$@"; do
    [[ "$arg" == *"@"* ]] || { echo "ERROR: match '$arg' must be \"<filter>@<espn event id>\"" >&2; exit 1; }
    match_filters+=("${arg%@*}")   # everything before the last @
    match_events+=("${arg##*@}")   # everything after the last @
done

# Combined OR filter: every match market + BTC up/down markets in one harvester.
COMBINED_FILTER="$(IFS=,; echo "${match_filters[*]}"),${BTC_FILTER}"

log() { echo "[run_capture $(date -u '+%H:%M:%SZ')] $*"; }

# ── Preflight ────────────────────────────────────────────────────────────────
for f in "$POLY_BIN" "$BNB_BIN"; do
    [[ -x "$f" ]] || { echo "ERROR: missing binary $f — build first:" >&2
        echo "  cmake --build build --target polymarket_harvester binance_harvester -j\"\$(nproc)\"" >&2
        exit 1; }
done
[[ -f "$ESPN_PY" ]] || { echo "ERROR: missing $ESPN_PY" >&2; exit 1; }
mkdir -p "$DATA_DIR"

log "data dir       : $DATA_DIR"
log "polymarket flt : $COMBINED_FILTER"
log "matches        : ${#match_events[@]} (sport $ESPN_SPORT)"
[[ -n "$DURATION" ]] && log "duration       : ${DURATION}s (auto-stop)" || log "duration       : until all matches reach full time / Ctrl-C"

# ── Launch the two continuous C++ harvesters ─────────────────────────────────
POLYMARKET_DATA_DIR="$DATA_DIR" \
POLYMARKET_MARKET_FILTER="$COMBINED_FILTER" \
POLYMARKET_REDISCOVER_MINS="$REDISCOVER_MINS" \
    "$POLY_BIN" >"$DATA_DIR/polymarket_harvester.log" 2>&1 &
POLY_PID=$!
log "polymarket_harvester started (pid $POLY_PID) → polymarket_harvester.log"

BINANCE_DATA_DIR="$DATA_DIR" \
    "$BNB_BIN" >"$DATA_DIR/binance_harvester.log" 2>&1 &
BNB_PID=$!
log "binance_harvester started    (pid $BNB_PID) → binance_harvester.log"

# ── Launch one ESPN collector per match ──────────────────────────────────────
sport_slug="$(echo "$ESPN_SPORT" | tr '/' '_')"
espn_pids=()
for i in "${!match_events[@]}"; do
    ev="${match_events[$i]}"
    out="$DATA_DIR/espn_${sport_slug}_${ev}.csv"
    "$PYTHON" "$ESPN_PY" --sport "$ESPN_SPORT" --event "$ev" \
        --out "$out" --interval 1.0 >"$DATA_DIR/espn_${ev}.log" 2>&1 &
    pid=$!
    espn_pids+=("$pid")
    log "espn_collector started       (pid $pid) match='${match_filters[$i]}' event=$ev → $(basename "$out")"
done

# ── Shutdown + decode ────────────────────────────────────────────────────────
decoded=false
decode() {
    $decoded && return; decoded=true
    log "decoding journals → Parquet"
    shopt -s nullglob
    local poly_bins=("$DATA_DIR"/polymarket_*.bin)
    local btc_bins=("$DATA_DIR"/btc_*.bin)
    if (( ${#poly_bins[@]} )); then
        "$PYTHON" "$REPO_ROOT/scripts/harvester/log_to_parquet.py" "${poly_bins[@]}" || \
            log "WARNING: polymarket decode reported an error"
    else
        log "WARNING: no polymarket_*.bin produced — check polymarket_harvester.log"
    fi
    for f in "${btc_bins[@]}"; do
        "$PYTHON" "$REPO_ROOT/scripts/harvester/btc_to_parquet.py" "$f" || \
            log "WARNING: btc decode failed for $f"
    done
    log "done. Parquet + espn_*.csv + market_metadata.csv are in $DATA_DIR"
}

stopping=false
shutdown() {
    $stopping && return; stopping=true
    log "stopping — SIGINT to collectors so the harvesters flush + seal journals"
    kill -INT "$POLY_PID" "$BNB_PID" "${espn_pids[@]}" 2>/dev/null || true
    wait "$POLY_PID" "$BNB_PID" "${espn_pids[@]}" 2>/dev/null || true
    decode
    exit 0
}
trap shutdown INT TERM

log "capturing. Watch a match:  tail -f $DATA_DIR/espn_${match_events[0]}.log"

# ── Wait loop: stop when all matches finish (or DURATION, or a harvester dies) ─
SECONDS=0
while true; do
    kill -0 "$POLY_PID" 2>/dev/null || { log "polymarket_harvester died — stopping (see polymarket_harvester.log)"; break; }
    kill -0 "$BNB_PID"  2>/dev/null || { log "binance_harvester died — stopping (see binance_harvester.log)"; break; }

    if [[ -n "$DURATION" ]] && (( SECONDS >= DURATION )); then
        log "duration reached"; break
    fi

    alive=0
    for p in "${espn_pids[@]}"; do kill -0 "$p" 2>/dev/null && alive=$((alive+1)); done
    if (( alive == 0 )) && [[ -z "$DURATION" ]]; then
        log "all matches reached full time"; break
    fi

    sleep 5
done

shutdown
