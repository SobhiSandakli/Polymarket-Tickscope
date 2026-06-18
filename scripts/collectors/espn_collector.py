#!/usr/bin/env python3
"""
ESPN live score collector — sport-generic.

Polls the ESPN scoreboard API every ~1 second and writes a CSV row
whenever the score, period, or game clock changes. The --sport flag
selects the ESPN sport path; the schema is identical across sports
("period" = quarter/period/half depending on the sport).

Timestamps use epoch-milliseconds on the local system clock — the same
clock the C++ Polymarket harvester uses — so both datasets are directly
joinable by ts_ms in DuckDB.

Usage:
    # NBA
    python3 espn_collector.py --sport basketball/nba --event 401859965 \
        --out data/espn_nba_401859965.csv
    # NHL (the committed Stanley Cup sample was captured this way)
    python3 espn_collector.py --sport hockey/nhl --event 401874173 \
        --out data/espn_nhl_401874173.csv
    # 2026 World Cup
    python3 espn_collector.py --sport soccer/fifa.world --event <event_id> \
        --out data/espn_wc_<event_id>.csv

    # Find the event ID: list today's games for a sport
    curl -s "https://site.api.espn.com/apis/site/v2/sports/soccer/fifa.world/scoreboard" \
        | python3 -c "import json,sys; [print(e['id'], e['name']) for e in json.load(sys.stdin)['events']]"

Output CSV schema:
    ts_ms, event_id, period, display_clock, home_team, home_score,
    away_team, away_score, home_score_delta, away_score_delta,
    game_state, event_type
"""

import argparse
import csv
import os
import sys
import time
import urllib.request
import json


SCOREBOARD_BASE = "https://site.api.espn.com/apis/site/v2/sports"


def now_ms() -> int:
    return int(time.time() * 1000)


def fetch_json(url: str) -> dict:
    req = urllib.request.Request(url, headers={"User-Agent": "polymarket-collector/1.0"})
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read())


def find_game(data: dict, event_id: str) -> dict | None:
    for event in data.get("events", []):
        if event["id"] == event_id:
            return event
    return None


def parse_state(event: dict) -> dict:
    status = event["status"]
    comp = event["competitions"][0]

    home = next(c for c in comp["competitors"] if c["homeAway"] == "home")
    away = next(c for c in comp["competitors"] if c["homeAway"] == "away")

    return {
        "period":        status.get("period", 0),
        "display_clock": status.get("displayClock", ""),
        "game_state":    status["type"].get("state", ""),
        "home_team":     home["team"]["abbreviation"],
        "home_score":    int(home.get("score") or 0),
        "away_team":     away["team"]["abbreviation"],
        "away_score":    int(away.get("score") or 0),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--event", required=True, help="ESPN event ID")
    ap.add_argument("--out",   required=True, help="Output CSV path")
    ap.add_argument("--interval", type=float, default=1.0, help="Poll interval seconds")
    ap.add_argument("--sport", default="basketball/nba", help="ESPN sport path (e.g. hockey/nhl)")
    args = ap.parse_args()

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)

    fieldnames = [
        "ts_ms", "event_id", "period", "display_clock",
        "home_team", "home_score", "away_team", "away_score",
        "home_score_delta", "away_score_delta", "game_state", "event_type",
    ]

    file_exists = os.path.exists(args.out)
    outfile = open(args.out, "a", newline="", buffering=1)  # line-buffered
    writer = csv.DictWriter(outfile, fieldnames=fieldnames)
    if not file_exists:
        writer.writeheader()
        outfile.flush()

    prev = None
    consecutive_errors = 0
    scoreboard_url = f"{SCOREBOARD_BASE}/{args.sport}/scoreboard"
    print(f"[espn] collecting event={args.event} sport={args.sport} → {args.out}", flush=True)

    while True:
        try:
            data = fetch_json(scoreboard_url)
            event = find_game(data, args.event)
            consecutive_errors = 0
        except Exception as e:
            consecutive_errors += 1
            print(f"[espn] fetch error ({consecutive_errors}): {e}", flush=True)
            if consecutive_errors >= 10:
                print("[espn] 10 consecutive errors — exiting", flush=True)
                sys.exit(1)
            time.sleep(args.interval)
            continue

        if event is None:
            # Game not in scoreboard yet (pre-game) or already removed (final)
            ts = now_ms()
            print(f"[espn] {ts} game {args.event} not in scoreboard", flush=True)
            time.sleep(5)
            continue

        curr = parse_state(event)
        ts = now_ms()

        # Detect what changed
        if prev is None:
            event_type = "start"
        elif curr["game_state"] == "post" and prev["game_state"] != "post":
            event_type = "game_end"
        elif curr["period"] != prev["period"]:
            event_type = "period_change"
        elif curr["home_score"] != prev["home_score"] or curr["away_score"] != prev["away_score"]:
            event_type = "score_change"
        else:
            # No change — write a heartbeat every 30s so we have clock continuity
            # but skip writing every second to keep the file manageable
            if ts % 30000 < int(args.interval * 1000) + 100:
                event_type = "heartbeat"
            else:
                prev = curr
                time.sleep(args.interval)
                continue

        home_delta = curr["home_score"] - (prev["home_score"] if prev else 0)
        away_delta = curr["away_score"] - (prev["away_score"] if prev else 0)

        row = {
            "ts_ms":             ts,
            "event_id":          args.event,
            "period":            curr["period"],
            "display_clock":     curr["display_clock"],
            "home_team":         curr["home_team"],
            "home_score":        curr["home_score"],
            "away_team":         curr["away_team"],
            "away_score":        curr["away_score"],
            "home_score_delta":  home_delta,
            "away_score_delta":  away_delta,
            "game_state":        curr["game_state"],
            "event_type":        event_type,
        }
        writer.writerow(row)
        outfile.flush()

        print(
            f"[espn] {ts}  {curr['game_state']}  "
            f"Q{curr['period']} {curr['display_clock']}  "
            f"{curr['home_team']} {curr['home_score']} - "
            f"{curr['away_team']} {curr['away_score']}  [{event_type}]",
            flush=True,
        )

        prev = curr

        if curr["game_state"] == "post":
            print("[espn] game finished — exiting", flush=True)
            break

        time.sleep(args.interval)

    outfile.close()


if __name__ == "__main__":
    main()
