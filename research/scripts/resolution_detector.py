#!/usr/bin/env python3
"""Resolution Detector — determines TRUE win rate for ConvergenceNo.

Streams through parquet files keeping only per-condition aggregates:
  - Last NO book update (for resolution detection)
  - First YES cross below threshold (for entry signals)

Usage:
    python research/scripts/resolution_detector.py
"""

import sys, pathlib, time
import numpy as np
import pandas as pd
import duckdb
import requests

# ── Find data directory ──────────────────────────────────────────────────
for _base in [pathlib.Path.cwd(), pathlib.Path.cwd().parent, pathlib.Path.cwd().parent.parent]:
    if (_base / 'data' / 'parquet').exists():
        PQ_DIR = _base / 'data' / 'parquet'
        META_CSV = str(_base / 'data' / 'market_metadata.csv')
        break
else:
    sys.exit("data/parquet not found — run from the repo root with captured data present.")

THRESHOLD = 0.40

# ── Load metadata ────────────────────────────────────────────────────────
print("Loading metadata...")
con = duckdb.connect()
meta = con.execute(f"""
    SELECT asset_id, condition_id, outcome, question, end_date,
           COALESCE(taker_fee_bps, 0)::INT AS fee_bps
    FROM read_csv('{META_CSV}', types={{'asset_id':'VARCHAR','outcome':'VARCHAR'}})
    WHERE question ILIKE '%up or down%'
      AND COALESCE(taker_fee_bps, 0) > 0
""").fetchdf()
con.close()

asset_to_cid = dict(zip(meta['asset_id'], meta['condition_id']))
asset_to_outcome = dict(zip(meta['asset_id'], meta['outcome']))
cid_to_question = dict(zip(meta['condition_id'], meta['question']))
cid_to_end = dict(zip(meta['condition_id'], meta['end_date']))

# Build sets for fast lookup
target_assets = set(meta['asset_id'].tolist())
no_assets_set = set(meta[meta['outcome'] == 'NO']['asset_id'].tolist())
yes_assets_set = set(meta[meta['outcome'] == 'YES']['asset_id'].tolist())

# Map condition_id -> NO asset_id
cid_to_no_asset = {}
for _, row in meta[meta['outcome'] == 'NO'].iterrows():
    cid_to_no_asset[row['condition_id']] = row['asset_id']

print(f"  Conditions: {meta['condition_id'].nunique()}")
print(f"  YES tokens: {len(yes_assets_set)}, NO tokens: {len(no_assets_set)}")

# ── Streaming aggregation ────────────────────────────────────────────────
# We track per-condition:
#   last_no_book: {cid: (ts, bid, ask, mid, trade_price)}  -- last observed NO book
#   first_yes_cross: {cid: (ts, yes_mid)}  -- first YES mid <= threshold
#   no_near_cross: {cid: (ts, bid, ask)}  -- NO book nearest to first_yes_cross time

last_no_book = {}     # cid -> (ts, bid, ask)
last_no_trade = {}    # cid -> (ts, price)
first_yes_cross = {}  # cid -> ts_ms

# For matching NO book near cross time, collect per-condition
# We'll do a second pass for just the conditions that crossed
no_book_at_cross = {}  # cid -> (ts, bid, ask, time_diff)

pq_files = sorted(PQ_DIR.glob("polymarket_*.parquet"))
print(f"\nStreaming {len(pq_files)} parquet files...")

BATCH_SIZE = 10
for batch_start in range(0, len(pq_files), BATCH_SIZE):
    batch_files = pq_files[batch_start:batch_start + BATCH_SIZE]
    batch_list = [str(f) for f in batch_files]

    con = duckdb.connect()
    con.execute("SET threads=2")

    # Read only relevant columns, filter by asset_id
    try:
        df = con.execute(f"""
            SELECT
                epoch_ms(t.timestamp) AS ts_ms,
                t.price, t.best_bid, t.best_ask,
                t.event_type, t.asset_id
            FROM read_parquet({batch_list}) t
            WHERE t.asset_id IN ({','.join("'" + a + "'" for a in target_assets)})
        """).fetchdf()
    except Exception as e:
        # Fallback: if IN list too large, use a join approach
        con.register('target_view', pd.DataFrame({'asset_id': list(target_assets)}))
        df = con.execute(f"""
            SELECT epoch_ms(t.timestamp) AS ts_ms,
                   t.price, t.best_bid, t.best_ask,
                   t.event_type, t.asset_id
            FROM read_parquet({batch_list}) t
            SEMI JOIN target_view tv ON t.asset_id = tv.asset_id
        """).fetchdf()
        con.unregister('target_view')
    con.close()

    if len(df) == 0:
        continue

    # Enrich with condition_id and outcome
    df['condition_id'] = df['asset_id'].map(asset_to_cid)
    df['outcome'] = df['asset_id'].map(asset_to_outcome)

    # Process NO book updates
    no_book = df[(df['outcome'] == 'NO') &
                 (df['event_type'] == 'PRICE_CHANGE') &
                 (df['best_bid'] > 0) &
                 (df['best_ask'] > df['best_bid'])]

    for _, r in no_book.iterrows():
        cid = r['condition_id']
        ts = r['ts_ms']
        if cid not in last_no_book or ts > last_no_book[cid][0]:
            last_no_book[cid] = (ts, r['best_bid'], r['best_ask'])

    # Process NO trades
    no_trades = df[(df['outcome'] == 'NO') & (df['event_type'] == 'LAST_TRADE')]
    for _, r in no_trades.iterrows():
        cid = r['condition_id']
        ts = r['ts_ms']
        if cid not in last_no_trade or ts > last_no_trade[cid][0]:
            last_no_trade[cid] = (ts, r['price'])

    # Process YES book updates for threshold crossing
    yes_book = df[(df['outcome'] == 'YES') &
                  (df['event_type'] == 'PRICE_CHANGE') &
                  (df['best_bid'] > 0) &
                  (df['best_ask'] > df['best_bid'])]
    yes_book = yes_book.copy()
    yes_book['mid'] = (yes_book['best_bid'] + yes_book['best_ask']) / 2.0
    yes_crosses = yes_book[yes_book['mid'] <= THRESHOLD]

    for _, r in yes_crosses.iterrows():
        cid = r['condition_id']
        ts = r['ts_ms']
        if cid not in first_yes_cross or ts < first_yes_cross[cid]:
            first_yes_cross[cid] = ts

    # Track NO book updates near cross times (within 5s window)
    # We check all NO book updates against known cross times
    for _, r in no_book.iterrows():
        cid = r['condition_id']
        if cid not in first_yes_cross:
            continue
        cross_ts = first_yes_cross[cid]
        ts = r['ts_ms']
        if abs(ts - cross_ts) <= 5000:
            diff = abs(ts - cross_ts)
            if cid not in no_book_at_cross or diff < no_book_at_cross[cid][3]:
                no_book_at_cross[cid] = (ts, r['best_bid'], r['best_ask'], diff)

    pct = min(100, (batch_start + BATCH_SIZE) / len(pq_files) * 100)
    sys.stdout.write(f"\r  {min(batch_start + BATCH_SIZE, len(pq_files))}/{len(pq_files)} files | "
                     f"{len(first_yes_cross)} crosses | {len(last_no_book)} NO books")
    sys.stdout.flush()

print(f"\n\nDone streaming.")
print(f"  Conditions with YES cross <= {THRESHOLD}: {len(first_yes_cross)}")
print(f"  Conditions with last NO book: {len(last_no_book)}")
print(f"  Conditions with NO book near cross: {len(no_book_at_cross)}")

# ── Build resolution table ───────────────────────────────────────────────
print("\nDetecting resolutions...")
resolutions = {}
for cid, (ts, bid, ask) in last_no_book.items():
    mid = (bid + ask) / 2.0
    trade_price = last_no_trade.get(cid, (0, 0))[1]

    if mid >= 0.95 or trade_price >= 0.95:
        resolutions[cid] = ('NO_WINS', mid, trade_price)
    elif mid <= 0.05 or trade_price <= 0.05:
        resolutions[cid] = ('YES_WINS', mid, trade_price)
    else:
        resolutions[cid] = ('UNRESOLVED', mid, trade_price)

res_counts = {}
for cid, (res, _, _) in resolutions.items():
    res_counts[res] = res_counts.get(res, 0) + 1

print(f"  All markets resolution:")
for k, v in sorted(res_counts.items()):
    print(f"    {k:15s}: {v:>5d}")

# ── Build entry signals table ────────────────────────────────────────────
print(f"\nBuilding entry signals (threshold={THRESHOLD})...")
entries = []
for cid, cross_ts in first_yes_cross.items():
    if cid not in no_book_at_cross:
        continue  # no NO book data near the cross
    if cid not in cid_to_no_asset:
        continue  # no NO asset mapping

    _, no_bid, no_ask, _ = no_book_at_cross[cid]
    no_mid = (no_bid + no_ask) / 2.0

    res_info = resolutions.get(cid, ('UNRESOLVED', no_mid, 0))
    last_mid = res_info[1]

    entries.append({
        'condition_id': cid,
        'entry_ts': cross_ts,
        'no_entry_ask': no_ask,
        'no_entry_bid': no_bid,
        'no_entry_mid': no_mid,
        'resolution': res_info[0],
        'last_no_mid': last_mid,
        'question': cid_to_question.get(cid, ''),
        'end_date': cid_to_end.get(cid, None),
    })

merged = pd.DataFrame(entries)
print(f"  Entry signals: {len(merged)}")
print(f"  Avg NO entry ask: ${merged['no_entry_ask'].mean():.3f}")

print(f"\n  Entry resolution breakdown:")
for k, v in merged['resolution'].value_counts().items():
    print(f"    {k:15s}: {v:>5d} ({v/len(merged)*100:5.1f}%)")

resolved = merged[merged['resolution'] != 'UNRESOLVED']
if len(resolved) > 0:
    no_wins = (resolved['resolution'] == 'NO_WINS').sum()
    yes_wins = (resolved['resolution'] == 'YES_WINS').sum()
    print(f"\n  Tick-data win rate: {no_wins}/{no_wins+yes_wins} = {no_wins/(no_wins+yes_wins)*100:.1f}%")

# ── Query API for unresolved ─────────────────────────────────────────────
unresolved = merged[merged['resolution'] == 'UNRESOLVED']
print(f"\nQuerying Polymarket API for {len(unresolved)} unresolved entries...")

if len(unresolved) > 0:
    GAMMA_BASE = "https://gamma-api.polymarket.com"
    api_results = []
    unique_cids = unresolved['condition_id'].unique()
    print(f"  {len(unique_cids)} unique conditions...")

    for i, cid in enumerate(unique_cids):
        try:
            resp = requests.get(f"{GAMMA_BASE}/markets",
                                params={'condition_id': cid}, timeout=10)
            if resp.status_code == 200:
                data = resp.json()
                m = data[0] if isinstance(data, list) and len(data) > 0 else (data if isinstance(data, dict) else None)
                if m:
                    api_results.append({
                        'condition_id': cid,
                        'api_active': m.get('active'),
                        'api_closed': m.get('closed'),
                        'api_resolved': m.get('resolved'),
                        'api_outcome': m.get('outcome', ''),
                    })
                else:
                    api_results.append({'condition_id': cid, 'api_outcome': 'NOT_FOUND'})
            else:
                api_results.append({'condition_id': cid, 'api_outcome': f'HTTP_{resp.status_code}'})
        except requests.RequestException:
            api_results.append({'condition_id': cid, 'api_outcome': 'ERROR'})

        if (i + 1) % 5 == 0:
            time.sleep(1)
        if (i + 1) % 50 == 0:
            print(f"    ...{i+1}/{len(unique_cids)}")

    api_df = pd.DataFrame(api_results)
    print(f"\n  API results: {len(api_df)}")
    print(f"  Outcome breakdown:")
    for k, v in api_df['api_outcome'].value_counts().head(10).items():
        print(f"    {str(k):20s}: {v}")

    def map_api(row):
        o = str(row.get('api_outcome', '')).strip().upper()
        if o in ('NO', 'FALSE', '0'):
            return 'NO_WINS'
        elif o in ('YES', 'TRUE', '1'):
            return 'YES_WINS'
        elif row.get('api_closed') == True or row.get('api_resolved') == True:
            return 'RESOLVED_UNKNOWN'
        else:
            return 'STILL_OPEN'

    api_df['api_resolution'] = api_df.apply(map_api, axis=1)
    print(f"  API resolution:")
    for k, v in api_df['api_resolution'].value_counts().items():
        print(f"    {k:20s}: {v}")

    api_map = api_df.set_index('condition_id')['api_resolution'].to_dict()
    merged['final_resolution'] = merged.apply(
        lambda r: r['resolution'] if r['resolution'] != 'UNRESOLVED'
                  else api_map.get(r['condition_id'], 'UNKNOWN'),
        axis=1
    )
else:
    api_df = pd.DataFrame()
    merged['final_resolution'] = merged['resolution']

# ── Final resolution ─────────────────────────────────────────────────────
print("\n" + "=" * 60)
print("FINAL RESOLUTION (tick data + API)")
print("=" * 60)
for k, v in merged['final_resolution'].value_counts().items():
    print(f"  {k:20s}: {v:>5d} ({v/len(merged)*100:5.1f}%)")

rmask = merged['final_resolution'].isin(['NO_WINS', 'YES_WINS'])
rf = merged[rmask]
if len(rf) > 0:
    nw = (rf['final_resolution'] == 'NO_WINS').sum()
    yw = (rf['final_resolution'] == 'YES_WINS').sum()
    print(f"\n  ╔══════════════════════════════════════════╗")
    print(f"  ║  TRUE WIN RATE: {nw}/{nw+yw} = {nw/(nw+yw)*100:.1f}%          ║")
    print(f"  ╚══════════════════════════════════════════╝")

# ── True PnL ─────────────────────────────────────────────────────────────
SIZE = 10.0
FEE_RATE = 0.25
FEE_EXPONENT = 2
SLIPPAGE = 0.0005

def calc_fee(price, shares, is_buy=True):
    fb = FEE_RATE * (price * (1 - price)) ** FEE_EXPONENT
    return shares * fb * price if is_buy else shares * fb

results = []
for _, row in merged.iterrows():
    ep = row['no_entry_ask'] + SLIPPAGE
    cost = ep * SIZE
    bf = calc_fee(ep, SIZE, True)
    res = row['final_resolution']

    if res == 'NO_WINS':
        pnl = 1.0 * SIZE - cost - bf - calc_fee(1.0, SIZE, False)
        status = 'WIN'
    elif res == 'YES_WINS':
        pnl = -cost - bf
        status = 'LOSS'
    else:
        lp = row.get('last_no_mid', ep)
        if pd.isna(lp): lp = ep
        pnl = lp * SIZE - cost - bf - calc_fee(lp, SIZE, False)
        status = 'OPEN'

    results.append(dict(condition_id=row['condition_id'], entry_ts=row['entry_ts'],
                        entry_price=ep, cost=cost, resolution=res, status=status, pnl=pnl))

pnl_df = pd.DataFrame(results)

print("\n" + "=" * 60)
print("TRUE PnL BREAKDOWN")
print("=" * 60)
print(f"{'Status':<12} {'Count':>6} {'Total PnL':>12} {'Avg PnL':>10}")
print("-" * 60)
for st in ['WIN', 'LOSS', 'OPEN']:
    sub = pnl_df[pnl_df['status'] == st]
    if len(sub) > 0:
        print(f"{st:<12} {len(sub):>6} {sub['pnl'].sum():>12.2f} {sub['pnl'].mean():>10.3f}")
print("-" * 60)
print(f"{'TOTAL':<12} {len(pnl_df):>6} {pnl_df['pnl'].sum():>12.2f} {pnl_df['pnl'].mean():>10.3f}")
print("=" * 60)

rp = pnl_df[pnl_df['status'].isin(['WIN', 'LOSS'])]
if len(rp) > 0:
    w = rp[rp['status']=='WIN']
    l = rp[rp['status']=='LOSS']
    print(f"\nResolved PnL: ${rp['pnl'].sum():.2f}  |  Win rate: {(rp['status']=='WIN').mean()*100:.1f}%")
    if len(w) > 0: print(f"Avg win:      ${w['pnl'].mean():.3f}")
    if len(l) > 0: print(f"Avg loss:     ${l['pnl'].mean():.3f}")

# ── Duration analysis ────────────────────────────────────────────────────
# We need tick spans from our streaming data
# Use first_yes_cross times and last_no_book times per condition
print("\n" + "=" * 60)
print("MARKET DURATION ANALYSIS")
print("=" * 60)

# Approximate duration from our data: time from first observation to last
# We tracked last_no_book and first_yes_cross — combine for span estimate
dur_data = []
for _, row in pnl_df.iterrows():
    cid = row['condition_id']
    entry_ts = row['entry_ts']
    last_ts = last_no_book.get(cid, (entry_ts, 0, 0))[0]
    span_min = max(0, (last_ts - entry_ts)) / 60000.0
    dur_data.append({'condition_id': cid, 'span_min': span_min, **row})

dur_df = pd.DataFrame(dur_data)

def bucket(mins):
    if pd.isna(mins) or mins <= 0: return 'unknown'
    elif mins <= 10: return '0-10min'
    elif mins <= 30: return '10-30min'
    elif mins <= 60: return '30-60min'
    elif mins <= 240: return '1-4h'
    elif mins <= 1440: return '4-24h'
    else: return '24h+'

dur_df['bucket'] = dur_df['span_min'].apply(bucket)

print(f"{'Duration':<12} {'Count':>6} {'Wins':>5} {'Losses':>6} {'WinRate':>8} {'TotalPnL':>10} {'AvgPnL':>8}")
print("-" * 65)
for b in ['0-10min', '10-30min', '30-60min', '1-4h', '4-24h', '24h+', 'unknown']:
    sub = dur_df[dur_df['bucket'] == b]
    if len(sub) == 0: continue
    wins = (sub['status'] == 'WIN').sum()
    losses = (sub['status'] == 'LOSS').sum()
    r = wins + losses
    wr = f"{wins/r*100:.0f}%" if r > 0 else 'N/A'
    print(f"{b:<12} {len(sub):>6} {wins:>5} {losses:>6} {wr:>8} {sub['pnl'].sum():>10.2f} {sub['pnl'].mean():>8.3f}")

print("\n--- Capital efficiency ---")
for b in ['0-10min', '10-30min', '30-60min', '1-4h', '4-24h', '24h+']:
    sub = dur_df[dur_df['bucket'] == b]
    if len(sub) == 0: continue
    avg_h = sub['span_min'].mean() / 60
    cap_h = (sub['cost'] * sub['span_min'] / 60).sum()
    if cap_h > 0:
        print(f"  {b:<12}: avg {avg_h:.1f}h, PnL/cap-hour = ${sub['pnl'].sum()/cap_h:.4f}")

# ── Kelly criterion ──────────────────────────────────────────────────────
if len(rp) > 0:
    print("\n" + "=" * 60)
    print("KELLY CRITERION (TRUE DATA)")
    print("=" * 60)

    for st in ['WIN', 'LOSS']:
        sub = rp[rp['status'] == st]
        if len(sub) > 0:
            print(f"  {st}: n={len(sub)}, avg entry=${sub['entry_price'].mean():.3f}, "
                  f"range=[${sub['entry_price'].min():.3f}, ${sub['entry_price'].max():.3f}]")

    p = (rp['status'] == 'WIN').mean()
    avg_w = w['pnl'].mean() if len(w) > 0 else 0
    avg_l = abs(l['pnl'].mean()) if len(l) > 0 else 1

    if avg_l > 0:
        b_ratio = avg_w / avg_l
        kelly = p - (1 - p) / b_ratio
        print(f"\n  Win rate:       {p*100:.1f}%")
        print(f"  Avg win:        ${avg_w:.3f}")
        print(f"  Avg loss:       ${avg_l:.3f}")
        print(f"  Win/loss ratio: {b_ratio:.3f}")
        print(f"  Kelly f*:       {kelly:.4f}")
        print(f"  Half-Kelly:     {kelly/2:.4f}")
        if kelly <= 0:
            print(f"\n  *** NEGATIVE KELLY — STRATEGY HAS NO EDGE ***")
        else:
            print(f"\n  Optimal: {kelly*100:.1f}% of capital per trade")
            print(f"  On $1000: ${kelly*1000:.0f}/trade (half-Kelly: ${kelly/2*1000:.0f})")

# ── Final summary ────────────────────────────────────────────────────────
print("\n" + "=" * 60)
print("FINAL SUMMARY")
print("=" * 60)

total = len(pnl_df)
wins = (pnl_df['status'] == 'WIN').sum()
losses = (pnl_df['status'] == 'LOSS').sum()
opens = (pnl_df['status'] == 'OPEN').sum()
resolved = wins + losses

print(f"Total entry signals:   {total}")
print(f"Resolved (WIN+LOSS):   {resolved} ({resolved/total*100:.0f}%)")
print(f"  - Wins:              {wins}")
print(f"  - Losses:            {losses}")
print(f"Still open/unknown:    {opens}")

if resolved > 0:
    wr = wins / resolved
    print(f"\nTRUE WIN RATE:         {wr*100:.1f}%")
    print(f"Claimed (old memory):  74.9%")
    print(f"Delta:                 {(wr - 0.749)*100:+.1f}pp")

print(f"\nTotal PnL (all):       ${pnl_df['pnl'].sum():.2f}")
print(f"Resolved PnL:          ${pnl_df[pnl_df['status'].isin(['WIN','LOSS'])]['pnl'].sum():.2f}")
print(f"Unrealized (open):     ${pnl_df[pnl_df['status']=='OPEN']['pnl'].sum():.2f}")

if resolved > 0 and wins > 0:
    avg_ep = rp['entry_price'].mean()
    ev = wr * (1.0 - avg_ep) - (1 - wr) * avg_ep
    print(f"\nAvg NO entry price:    ${avg_ep:.3f}")
    print(f"EV per share:          ${ev:.4f}")
    if ev > 0:
        print(f"EV per $1 risked:      ${ev/avg_ep:.4f}")
        print(f"\nAt {SIZE:.0f} shares/trade:     ${ev*SIZE:.3f}/trade")
    else:
        print(f"\n  *** NEGATIVE EV — no edge at this threshold ***")

print(f"\n{'='*60}")
