#!/usr/bin/env python3
"""
Betfair Exchange Stream collector.

Connects to the Betfair Exchange Stream API (real-time SSL socket, not HTTP poll),
subscribes to a target sports market, and writes a CSV row whenever the best
back/lay price changes for any runner (team).

Timestamps use epoch-milliseconds on the local system clock — the same clock
the C++ Polymarket harvester uses — so both datasets are directly joinable by
ts_ms in DuckDB.

Credentials (env vars — never pass on command line):
    BETFAIR_USERNAME   Betfair account username
    BETFAIR_PASSWORD   Betfair account password
    BETFAIR_APP_KEY    Application key from developer.betfair.com

Usage:
    BETFAIR_USERNAME=xxx BETFAIR_PASSWORD=yyy BETFAIR_APP_KEY=zzz \\
        python3 betfair_collector.py --sport basketball --search "NBA Finals" \\
        --out data/betfair_nba_finals.csv

    # List available markets (no --out needed):
    python3 betfair_collector.py --sport basketball --search "NBA" --list

Output CSV schema:
    ts_ms, market_id, runner_id, runner_name,
    best_back, best_lay, last_traded_price,
    event_type

event_type values: start, price_change, heartbeat
"""

import argparse
import csv
import json
import os
import socket
import ssl
import sys
import time
import urllib.request
import urllib.parse

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
LOGIN_URL  = "https://identitysso.betfair.com/api/login"
API_URL    = "https://api.betfair.com/exchange/betting/json-rpc/v1"
STREAM_HOST = "stream-api.betfair.com"
STREAM_PORT = 443

# Betfair event type IDs
EVENT_TYPE_IDS = {
    "basketball": "7522",
    "soccer":     "1",
    "tennis":     "2",
    "hockey":     "7524",
}


# ---------------------------------------------------------------------------
# Auth
# ---------------------------------------------------------------------------
def login(username: str, password: str, app_key: str) -> str:
    payload = urllib.parse.urlencode({"username": username, "password": password}).encode()
    req = urllib.request.Request(
        LOGIN_URL,
        data=payload,
        headers={
            "X-Application": app_key,
            "Content-Type": "application/x-www-form-urlencoded",
            "Accept": "application/json",
        },
    )
    with urllib.request.urlopen(req, timeout=10) as r:
        resp = json.loads(r.read())
    if resp.get("status") != "SUCCESS":
        raise RuntimeError(f"Betfair login failed: {resp.get('error', resp)}")
    token = resp["token"]
    print(f"[betfair] login OK — session token obtained", flush=True)
    return token


# ---------------------------------------------------------------------------
# REST API helper (for market discovery)
# ---------------------------------------------------------------------------
def api_call(method: str, params: dict, session: str, app_key: str) -> dict:
    body = json.dumps({"jsonrpc": "2.0", "method": f"SportsAPING/v1.0/{method}", "params": params, "id": 1})
    req = urllib.request.Request(
        API_URL,
        data=body.encode(),
        headers={
            "X-Authentication": session,
            "X-Application": app_key,
            "Content-Type": "application/json",
            "Accept": "application/json",
        },
    )
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read())


def find_markets(event_type_id: str, search: str, session: str, app_key: str) -> list[dict]:
    result = api_call(
        "listMarkets",
        {
            "filter": {
                "eventTypeIds": [event_type_id],
                "textQuery": search,
            },
            "marketProjection": ["RUNNER_DESCRIPTION", "EVENT", "COMPETITION"],
            "maxResults": 20,
            "sort": "FIRST_TO_START",
        },
        session,
        app_key,
    )
    markets = result.get("result", [])
    return markets


def get_runner_names(market_catalogue: dict) -> dict[int, str]:
    return {r["selectionId"]: r["runnerName"] for r in market_catalogue.get("runners", [])}


# ---------------------------------------------------------------------------
# Stream connection
# ---------------------------------------------------------------------------
class BetfairStream:
    def __init__(self, session: str, app_key: str):
        self._session = session
        self._app_key = app_key
        self._sock = None
        self._buf = b""

    def connect(self):
        ctx = ssl.create_default_context()
        raw = socket.create_connection((STREAM_HOST, STREAM_PORT), timeout=10)
        self._sock = ctx.wrap_socket(raw, server_hostname=STREAM_HOST)
        # read connection message
        conn_msg = self._read_one()
        print(f"[betfair] stream connected — {conn_msg.get('op')}", flush=True)

    def auth(self):
        self._send({"op": "authentication", "id": 1, "session": self._session, "appKey": self._app_key})
        resp = self._read_one()
        if resp.get("statusCode") != "SUCCESS":
            raise RuntimeError(f"Stream auth failed: {resp}")
        print(f"[betfair] stream authenticated", flush=True)

    def subscribe(self, market_ids: list[str]):
        self._send({
            "op": "marketSubscription",
            "id": 2,
            "marketFilter": {"marketIds": market_ids},
            "marketDataFilter": {"ladderLevels": 1},  # best price only
        })
        print(f"[betfair] subscribed to {market_ids}", flush=True)

    def messages(self):
        """Yield parsed JSON messages as they arrive."""
        self._sock.settimeout(60)
        while True:
            try:
                chunk = self._sock.recv(4096)
            except socket.timeout:
                yield {"op": "_timeout"}
                continue
            if not chunk:
                break
            self._buf += chunk
            while b"\r\n" in self._buf:
                line, self._buf = self._buf.split(b"\r\n", 1)
                if line:
                    yield json.loads(line)

    def heartbeat(self):
        self._send({"op": "heartbeat", "id": 99})

    def _send(self, msg: dict):
        self._sock.sendall((json.dumps(msg) + "\r\n").encode())

    def _read_one(self) -> dict:
        while True:
            chunk = self._sock.recv(4096)
            self._buf += chunk
            if b"\r\n" in self._buf:
                line, self._buf = self._buf.split(b"\r\n", 1)
                if line:
                    return json.loads(line)


# ---------------------------------------------------------------------------
# Order-book state (delta decompression)
# ---------------------------------------------------------------------------
class RunnerBook:
    """Tracks best back/lay and last traded price for one runner."""
    def __init__(self):
        self.best_back: float = 0.0  # best price to back (like best bid on Polymarket)
        self.best_lay:  float = 0.0  # best price to lay  (like best ask)
        self.ltp:       float = 0.0  # last traded price

    def apply(self, rc: dict) -> bool:
        """Apply a runner change delta. Returns True if a price changed."""
        changed = False
        if "batb" in rc:  # best available to back
            for entry in rc["batb"]:
                if entry[0] == 0:  # level 0 = best price
                    new = entry[1]
                    if new != self.best_back:
                        self.best_back = new
                        changed = True
        if "batl" in rc:  # best available to lay
            for entry in rc["batl"]:
                if entry[0] == 0:
                    new = entry[1]
                    if new != self.best_lay:
                        self.best_lay = new
                        changed = True
        if "ltp" in rc:
            new = rc["ltp"]
            if new != self.ltp:
                self.ltp = new
                changed = True
        return changed

    def implied_prob(self) -> float:
        """Convert best back price (decimal odds) to implied probability [0,1]."""
        if self.best_back > 1:
            return round(1.0 / self.best_back, 4)
        return 0.0


# ---------------------------------------------------------------------------
# CSV writing
# ---------------------------------------------------------------------------
FIELDNAMES = [
    "ts_ms", "market_id", "runner_id", "runner_name",
    "best_back", "best_lay", "last_traded_price", "implied_prob",
    "event_type",
]


def now_ms() -> int:
    return int(time.time() * 1000)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sport",  default="basketball", choices=list(EVENT_TYPE_IDS), help="Sport name")
    ap.add_argument("--search", required=True, help="Text search for market (e.g. 'NBA Finals')")
    ap.add_argument("--out",    help="Output CSV path")
    ap.add_argument("--list",   action="store_true", help="List matching markets and exit")
    ap.add_argument("--heartbeat-secs", type=int, default=30, help="Heartbeat row interval")
    args = ap.parse_args()

    username = os.environ.get("BETFAIR_USERNAME", "")
    password = os.environ.get("BETFAIR_PASSWORD", "")
    app_key  = os.environ.get("BETFAIR_APP_KEY", "")
    if not all([username, password, app_key]):
        print("ERROR: set BETFAIR_USERNAME, BETFAIR_PASSWORD, BETFAIR_APP_KEY env vars", file=sys.stderr)
        sys.exit(1)

    session = login(username, password, app_key)
    event_type_id = EVENT_TYPE_IDS[args.sport]
    markets = find_markets(event_type_id, args.search, session, app_key)

    if not markets:
        print(f"[betfair] no markets found for sport={args.sport} search='{args.search}'", flush=True)
        sys.exit(1)

    print(f"\n[betfair] found {len(markets)} market(s):")
    for m in markets:
        event = m.get("event", {})
        comp  = m.get("competition", {})
        runners = [r["runnerName"] for r in m.get("runners", [])]
        print(f"  {m['marketId']}  {event.get('name','')}  [{comp.get('name','')}]  runners={runners}")

    if args.list:
        return

    if not args.out:
        print("ERROR: --out required when not using --list", file=sys.stderr)
        sys.exit(1)

    # Use the first (most imminent) market
    target = markets[0]
    market_id = target["marketId"]
    runner_names = get_runner_names(target)
    print(f"\n[betfair] streaming market {market_id}  runners={runner_names}", flush=True)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    file_exists = os.path.exists(args.out)
    outfile = open(args.out, "a", newline="", buffering=1)
    writer = csv.DictWriter(outfile, fieldnames=FIELDNAMES)
    if not file_exists:
        writer.writeheader()
        outfile.flush()

    # Runner state
    books: dict[int, RunnerBook] = {}
    last_heartbeat: dict[int, int] = {}

    def write_row(runner_id: int, event_type: str):
        book = books[runner_id]
        ts = now_ms()
        row = {
            "ts_ms":              ts,
            "market_id":          market_id,
            "runner_id":          runner_id,
            "runner_name":        runner_names.get(runner_id, str(runner_id)),
            "best_back":          book.best_back,
            "best_lay":           book.best_lay,
            "last_traded_price":  book.ltp,
            "implied_prob":       book.implied_prob(),
            "event_type":         event_type,
        }
        writer.writerow(row)
        outfile.flush()
        print(
            f"[betfair] {ts}  {runner_names.get(runner_id, runner_id):<25}"
            f"  back={book.best_back:.2f}  lay={book.best_lay:.2f}"
            f"  prob={book.implied_prob():.4f}  [{event_type}]",
            flush=True,
        )

    stream = BetfairStream(session, app_key)
    stream.connect()
    stream.auth()
    stream.subscribe([market_id])

    hb_interval_ms = args.heartbeat_secs * 1000

    for msg in stream.messages():
        op = msg.get("op")

        if op == "_timeout":
            stream.heartbeat()
            continue

        if op == "mcm":  # market change message
            for mc in msg.get("mc", []):
                for rc in mc.get("rc", []):
                    rid = rc["id"]
                    if rid not in books:
                        books[rid] = RunnerBook()
                        last_heartbeat[rid] = 0
                    changed = books[rid].apply(rc)
                    if changed:
                        write_row(rid, "price_change")
                        last_heartbeat[rid] = now_ms()

        # Periodic heartbeat rows (clock continuity, ~30s)
        ts_now = now_ms()
        for rid, book in books.items():
            if ts_now - last_heartbeat.get(rid, 0) >= hb_interval_ms:
                write_row(rid, "heartbeat")
                last_heartbeat[rid] = ts_now

        # Exit when market is closed
        for mc in msg.get("mc", []):
            if mc.get("marketDefinition", {}).get("status") == "CLOSED":
                print(f"[betfair] market {market_id} CLOSED — exiting", flush=True)
                outfile.close()
                return

    outfile.close()


if __name__ == "__main__":
    main()
