#!/usr/bin/env python3
"""
World Cup goal-lag analysis (2026-06-19 capture, 4 matches).

For each real goal, the Total-Goals Over/Under market whose line the goal crosses
(1-0 → O/U 0.5, 2-0 → O/U 1.5, 3-0 → O/U 2.5) is a near-deterministic signal: the
YES token snaps toward 1.0 the instant the goal makes that line certain. We measure
WHEN that token's mid-price repriced vs. WHEN ESPN's API reported the goal — same
epoch-ms clock, so it's a direct subtraction.

Negative lag  = Polymarket moved BEFORE ESPN detected the goal.
"""
import duckdb, pandas as pd, numpy as np

import os
# Runs from a fresh clone against the committed sample by default; point CAPTURE_DIR
# at a full capture (data/captures/<name>/) to analyze raw harvester output instead.
CAP = os.environ.get("CAPTURE_DIR", "data/samples/sports")
_sample = os.path.exists(f"{CAP}/market_metadata_worldcup.csv")   # committed trimmed sample
PARQUET = f"{CAP}/polymarket_worldcup_*.parquet" if _sample else f"{CAP}/polymarket_*.parquet"
MD = f"{CAP}/market_metadata_worldcup.csv" if _sample else f"{CAP}/market_metadata.csv"
CHART = "research/notebooks/lag_chart_worldcup_2026.png"

EVENT_TO_MATCH = {
    "760442": "United States vs. Australia",
    "760443": "Türkiye vs. Paraguay",
    "760444": "Brazil vs. Haiti",
    "760445": "Scotland vs. Morocco",
}

con = duckdb.connect()
cols = {c: "VARCHAR" for c in
        ["asset_id","condition_id","outcome","question","end_date","active","taker_fee_bps"]}
con.execute(f"CREATE TABLE md AS SELECT * FROM read_csv('{MD}', header=true, columns={cols})")

def ou_token(match, line):
    """YES asset_id for '<match>: O/U <line>' (exact question, full-match total goals)."""
    q = f"{match}: O/U {line}"
    r = con.execute("SELECT asset_id FROM md WHERE question = ? AND outcome='YES'", [q]).fetchall()
    return (r[0][0], q) if r else (None, q)

# ── 1. Collect real goals from the ESPN CSVs (drop the Brazil API glitch) ──────
goals = []  # (match, event, goal_ts_ms, total_after, clock, label)
for ev, match in EVENT_TO_MATCH.items():
    csv = f"{CAP}/espn_soccer_fifa.world_{ev}.csv"
    df = pd.read_csv(csv)
    sc = df[df.event_type == "score_change"].copy()
    # A real goal is exactly +1 to one side. The glitch rows are -3 / +3 → dropped.
    sc = sc[(sc.home_score_delta == 1) | (sc.away_score_delta == 1)]
    for _, r in sc.iterrows():
        total = int(r.home_score) + int(r.away_score)
        scorer = r.home_team if r.home_score_delta == 1 else r.away_team
        goals.append(dict(match=match, event=ev, goal_ts=int(r.ts_ms),
                          total=total, clock=r.display_clock, scorer=scorer,
                          score=f"{r.home_team} {r.home_score}-{r.away_score} {r.away_team}"))

# ── 2. Candidate goal-sensitive markets per goal ──────────────────────────────
# A goal moves several markets; we measure on whichever has the LARGEST, cleanest
# baseline→post move (best signal-to-noise). Candidates:
#   - Total Goals O/U at the line the goal crosses   (YES rises toward 1.0)
#   - Match draw market                              (YES falls — a goal kills the draw)
#   - O/U 0.5 "any goal"                             (YES rises; good for 1st goals)
def yes_token(question):
    r = con.execute("SELECT asset_id FROM md WHERE question = ? AND outcome='YES'",
                    [question]).fetchall()
    return r[0][0] if r else None

for g in goals:
    line = f"{g['total']-1}.5"          # 1-0 crosses 0.5, 2-0 crosses 1.5, ...
    cands = [
        (f"{g['match']}: O/U {line}",            +1, f"O/U {line}"),
        (f"Will {g['match']} end in a draw?",    -1, "draw"),
        (f"{g['match']}: O/U 0.5",               +1, "O/U 0.5"),
    ]
    g["candidates"] = [(yes_token(q), d, lbl, q) for q, d, lbl in cands]

tokens = sorted({t for g in goals for (t, _, _, _) in g["candidates"] if t})
print(f"{len(goals)} goals, {len(tokens)} distinct candidate tokens to load")

# ── 3. Pull price_change ticks for just those tokens (one scan) ───────────────
in_list = ",".join(f"'{t}'" for t in tokens)
ticks = con.execute(f"""
    SELECT epoch_ms(timestamp) AS ts_ms, asset_id,
           (best_bid + best_ask)/2.0 AS mid
    FROM read_parquet('{PARQUET}')
    WHERE event_type = 'PRICE_CHANGE' AND best_bid > 0 AND best_ask > 0
      AND asset_id IN ({in_list})
    ORDER BY ts_ms
""").df()
print(f"loaded {len(ticks):,} mid ticks across those tokens")

# ── 4. Measure reprice timing per goal (direction-aware, best-signal market) ───
def measure_token(asset_id, direction, gt):
    """direction +1 = YES rises on goal, -1 = YES falls. Returns reprice metrics."""
    t = ticks[ticks.asset_id == asset_id]
    pre  = t[(t.ts_ms >= gt-120_000) & (t.ts_ms < gt-15_000)]
    post = t[(t.ts_ms >= gt+10_000) & (t.ts_ms <= gt+120_000)]
    if len(pre) < 3 or len(post) < 3:
        return None
    baseline = pre.mid.median()
    post_level = post.mid.median()
    jump = post_level - baseline
    if direction * jump <= 0:        # market didn't move in the expected direction
        return None
    thresh = baseline + 0.5*jump
    win = t[(t.ts_ms >= gt-180_000) & (t.ts_ms <= gt+90_000)].sort_values("ts_ms")
    crossed = win[(win.mid >= thresh)] if direction > 0 else win[(win.mid <= thresh)]
    if crossed.empty:
        return None
    reprice_ts = int(crossed.ts_ms.iloc[0])
    return dict(baseline=baseline, post=post_level, jump=jump,
                reprice_ts=reprice_ts, lag_s=(reprice_ts-gt)/1000.0)

def measure(g):
    """Pick the candidate market with the largest |jump| (best signal)."""
    best = None
    for asset_id, direction, lbl, q in g["candidates"]:
        if not asset_id:
            continue
        m = measure_token(asset_id, direction, g["goal_ts"])
        if m and (best is None or abs(m["jump"]) > abs(best["jump"])):
            best = {**m, "market": lbl, "asset_id": asset_id, "direction": direction}
    return best

rows = []
for g in goals:
    m = measure(g)
    g["m"] = m
    low = (m is not None) and abs(m["jump"]) < 0.04   # too small to trust
    rows.append({
        "match": g["match"], "clock": g["clock"], "score@goal": g["score"],
        "market": m["market"] if m else "—",
        "baseline": round(m["baseline"],3) if m else None,
        "post": round(m["post"],3) if m else None,
        "jump": round(abs(m["jump"]),3) if m else None,
        "lag_s": round(m["lag_s"],1) if m else None,
        "trust": ("low-signal" if low else "ok") if m else "no data",
    })

res = pd.DataFrame(rows)
pd.set_option("display.width", 200); pd.set_option("display.max_colwidth", 40)
print("\n================  WORLD CUP GOAL-LAG RESULTS  ================")
print(res.to_string(index=False))

ok = res[res.trust == "ok"]
if not ok.empty:
    print(f"\nTrusted measurements (jump ≥ 0.04): {len(ok)}/{len(res)} goals")
    print(f"Lag (Polymarket reprice − ESPN goal): median {ok.lag_s.median():+.1f}s, "
          f"mean {ok.lag_s.mean():+.1f}s, range [{ok.lag_s.min():+.1f}, {ok.lag_s.max():+.1f}]s")
    print("Negative = market repriced BEFORE ESPN's API reported the goal")
    print("(i.e. ESPN's public scoreboard API lags the live event by that much).")

# ── 5. Chart: the 4 trusted goals — market mid vs. ESPN's goal timestamp ──────
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

trusted = [g for g in goals if g["m"] and abs(g["m"]["jump"]) >= 0.04]
if trusted:
    n = len(trusted)
    fig, axes = plt.subplots((n+1)//2, 2, figsize=(12, 3.2*((n+1)//2)), squeeze=False)
    for ax, g in zip(axes.flat, trusted):
        m = g["m"]; gt = g["goal_ts"]
        t = ticks[(ticks.asset_id == m["asset_id"]) &
                  (ticks.ts_ms >= gt-150_000) & (ticks.ts_ms <= gt+90_000)].copy()
        t["rel"] = (t.ts_ms - gt)/1000.0
        ax.step(t.rel, t.mid, where="post", color="#1f77b4", lw=1.4)
        ax.axvline(0, color="#d62728", lw=1.6, label="ESPN detects goal")
        ax.axvline(m["lag_s"], color="#2ca02c", ls="--", lw=1.4, label="Polymarket repriced")
        ax.set_title(f"{g['match']}  {g['score']}  ({g['clock']})\n"
                     f"{m['market']}  ·  market led ESPN by {abs(m['lag_s']):.0f}s",
                     fontsize=9)
        ax.set_xlabel("seconds relative to ESPN goal detection")
        ax.set_ylabel("market-implied prob (mid)")
        ax.legend(fontsize=7, loc="best"); ax.grid(alpha=0.25)
    for ax in axes.flat[n:]:
        ax.set_visible(False)
    fig.suptitle("Polymarket repriced ~50–73s BEFORE ESPN's API detected each goal\n"
                 "2026 World Cup, 19 Jun 2026 — 4 matches", fontsize=12, y=1.02)
    fig.tight_layout()
    fig.savefig(CHART, dpi=110, bbox_inches="tight")
    print(f"\nchart written → {CHART}")
