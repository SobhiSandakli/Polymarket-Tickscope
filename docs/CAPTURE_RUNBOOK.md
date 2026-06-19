# Capture Runbook

Step-by-step procedures for the two capture campaigns that feed `data/samples/`:

- **A. Crypto** — Polymarket BTC 5-min up/down markets + Coinbase BTC-USD, same host, same clock (AWS)
- **B. World Cup 2026** — Polymarket match markets + ESPN soccer score feed

Both produce binary journals decoded offline to Parquet. Sample curation rules at the end.

---

## A. Crypto capture (AWS)

### Instance

- Ubuntu 24.04, x86-64. **Avoid T-series burstable instances** for long captures — CPU
  credit exhaustion was the original failure mode (see ADR-0003). A `c6i.large` /
  `c7i.large` is comfortable; the focused filter keeps load low.
- ≥20 GB disk. A focused BTC capture writes well under 1 GB/day; the 15-min rotation
  keeps individual files small.

### Build & install

```bash
sudo apt-get update && sudo apt-get install -y build-essential cmake git libssl-dev libz-dev
sudo git clone <repo-url> /opt/polymarket && cd /opt/polymarket
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target polymarket_harvester coinbase_harvester -j"$(nproc)"
```

### Configure + run both harvesters via systemd

```bash
# Market filter — BTC 5-min up/down markets
echo 'POLYMARKET_MARKET_FILTER="Bitcoin Up or Down"' | sudo tee /etc/default/polymarket-harvester

sudo cp deploy/harvester/polymarket-harvester.service /etc/systemd/system/
sudo cp deploy/coinbase/coinbase-harvester.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now polymarket-harvester coinbase-harvester
```

Verify within a minute or two:

```bash
journalctl -u polymarket-harvester -n 20   # token count should be small (filtered), WS connected
journalctl -u coinbase-harvester -n 20      # Coinbase WS connected
ls -lh /opt/polymarket/data/                # polymarket_*.bin and btc_*.bin growing;
                                            # market_metadata.csv written at startup
```

Notes:

- The 5-min up/down markets **roll over** — new condition IDs appear every few minutes.
  The harvester re-runs discovery periodically and re-subscribes; confirm in the journal
  that the token count refreshes over time rather than decaying to dead markets.
- Capture target: **≥48h** (one full weekend is ideal). Longer is better for the
  backtest notebooks; the lag analysis only needs hours.

### Stop, decode, pull down

```bash
sudo systemctl stop polymarket-harvester coinbase-harvester

cd /opt/polymarket
python3 scripts/harvester/log_to_parquet.py data/polymarket_*.bin
for f in data/btc_*.bin; do python3 scripts/harvester/btc_to_parquet.py "$f"; done

# From your machine:
scp 'ubuntu@<host>:/opt/polymarket/data/*.parquet' data/
scp 'ubuntu@<host>:/opt/polymarket/data/market_metadata.csv' data/
```

(Or use `scripts/harvester/hourly_flush.sh` for continuous S3 upload during the capture.)

---

## B. World Cup capture

Runs anywhere (laptop is fine) — no AWS required. Tournament starts **June 11, 2026**.

### 1. Pick the match and get the ESPN event ID

```bash
curl -s "https://site.api.espn.com/apis/site/v2/sports/soccer/fifa.world/scoreboard" \
  | python3 -c "import json,sys; [print(e['id'], e['name'], e['date']) for e in json.load(sys.stdin)['events']]"
```

### 2. Find the Polymarket market filter

Polymarket match markets are usually titled `"<Team A> vs. <Team B>"`. Dry-run the
harvester and check the subscribed token count in the log — it should be ~2–12 tokens
(winner market + side markets), not 0 and not thousands:

```bash
POLYMARKET_MARKET_FILTER="<Team A> vs. <Team B>" ./build/src/harvester/polymarket_harvester
# [market-discovery] filter '...' → N/10000 tokens   ← sanity-check N, then Ctrl-C
```

If N is 0, search the Gamma API for the right question string:

```bash
curl -s "https://gamma-api.polymarket.com/markets?ascending=false&limit=100&active=true" \
  | python3 -c "import json,sys; [print(m['question']) for m in json.load(sys.stdin) if 'World Cup' in m.get('question','') or 'vs.' in m.get('question','')]"
```

### 3. Run both collectors (start ≥15 min before kickoff)

```bash
# Terminal 1 — Polymarket ticks
POLYMARKET_MARKET_FILTER="<Team A> vs. <Team B>" ./build/src/harvester/polymarket_harvester

# Terminal 2 — ESPN score events (exits on its own at full time)
python3 scripts/collectors/espn_collector.py \
  --sport soccer/fifa.world --event <event_id> \
  --out data/espn_wc_<event_id>.csv
```

Or with Docker (both services together):

```bash
POLYMARKET_MARKET_FILTER="<Team A> vs. <Team B>" ESPN_EVENT_ID=<event_id> docker compose up
```

Stop the harvester after the final whistle, then decode:

```bash
python3 scripts/harvester/log_to_parquet.py data/polymarket_*.bin
```

Goals are the measurable events — a 0–0 group-stage match produces no lag datapoints,
so prefer capturing several matches and keeping the ones with goals.

### One command for both feeds (and several matches)

`scripts/capture/run_capture.sh` orchestrates the whole session: it runs the Polymarket
harvester (subscribed to every match market **plus** the BTC up/down markets via one
comma-separated OR filter), the Coinbase reference harvester (continuously), and one ESPN
collector per match. It stops when every match reaches full time — or on Ctrl-C — and
then decodes all journals to Parquet. Capturing back-to-back fixtures in one session is
the way to handle 0–0 risk: BTC capture runs the whole time, and you keep whichever match
scored.

```bash
# One match
./scripts/capture/run_capture.sh "Canada vs. Qatar@<espn_event_id>"

# Two back-to-back matches + BTC, one session:
./scripts/capture/run_capture.sh \
    "Canada vs. Qatar@<espn_event_id_1>" \
    "Mexico vs. Korea@<espn_event_id_2>"
```

Each argument is `"<polymarket filter>@<espn event id>"`. Confirm each filter resolves to
a sane token count (§B.2) and each event id (§B.1) before kickoff. Override `DATA_DIR`,
`BTC_FILTER`, `ESPN_SPORT`, or `DURATION` via environment.

---

## Curating `data/samples/`

Rules for what gets committed (everything else in `data/` stays gitignored):

1. **Small.** Trim to the analysis window. Target ≤20 MB per market class. For crypto:
   a contiguous 2–4h window where both feeds overlap, filtered to ticks with
   `best_bid > 0`. For sports: the game window only.
2. **Joint.** Crypto samples must include both the Polymarket parquet *and* the Coinbase
   parquet for the **same window** — the whole point is the same-clock join.
3. **Metadata included.** Trim `market_metadata.csv` to the asset IDs present in the
   sample (`market_metadata_btc.csv` / keep token→question mapping for sports).
4. **Documented.** Add the dataset to `data/samples/README.md`: date, filter used,
   duration, what notebook it powers.
5. **Pipeline-pure.** Only files produced by this repo's tools — no hand edits.

Example trim (DuckDB):

```sql
COPY (
  SELECT * FROM read_parquet('data/polymarket_*.parquet')
  WHERE timestamp BETWEEN <t0> AND <t1>
) TO 'data/samples/crypto/polymarket_btc_<date>.parquet' (FORMAT PARQUET, COMPRESSION ZSTD);
```
