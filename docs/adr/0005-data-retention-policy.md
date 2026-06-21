# ADR-0005: In-process data retention, and append-only metadata

**Status:** Accepted
**Date:** 2026-06

## Context

Two coupled problems surfaced for long-running (24/7) capture:

1. **The metadata CSV grew unbounded and was rewritten in full every cycle.**
   `merge_and_write_metadata_csv` read and rewrote the *entire* `market_metadata.csv`
   on every re-discovery (every `POLYMARKET_REDISCOVER_MINS`, default 5–15 min),
   preserving every token ever seen (active + delisted). At 100k+ rows this is
   O(all-history) work every few minutes to add ~10 genuinely-new tokens. The 5-min
   markets (BTC up/down) roll over constantly, so the file grows by thousands of tokens
   per day.

2. **Captured `.bin`/`.parquet` accumulate without limit.** Useful for backtesting, but
   the operator should decide how much history to keep.

Naively deleting old/inactive metadata rows is unsafe: the CSV is the **decode key**
(`token_id` → question/outcome) for the binary tick files. Deleting a token's metadata
makes any tick referencing it permanently un-decodable. So retention must delete data
*and* its metadata together, never metadata alone.

We also wanted this to be **natural in the code** — no zoo of scripts a cloner must wire
up — and **operator-controlled** (you own your storage).

## Decision

- **Boot keeps the full merge (once per launch).** `merge_and_write_metadata_csv` still
  runs at startup, producing a correct, deduplicated CSV with accurate `active` flags.
- **Re-discovery switches to append-only** (`append_new_metadata_csv`). A `metadata_known`
  set is seeded once at boot from the CSV; each cycle appends only token_ids not already
  in it. Per-cycle cost drops from O(all-history) to O(new-tokens) — no full rewrite. The
  `active` column then reflects active-at-first-discovery (a fresh boot merge repairs it);
  decoding only needs `token_id → question/outcome`, so this is sufficient.
- **`POLYMARKET_RETENTION_DAYS` env var** (unset / `0` = **unlimited**, the prior behavior,
  non-breaking). When set, the re-discovery thread, throttled to **~hourly**:
  - deletes **every file** in the data dir older than N days by **last-modified time**
    (`prune_old_data_files`). This is deliberately *generic* — it keys on mtime, not on any
    per-stream naming convention — so a Polymarket journal, a Coinbase journal, an ESPN CSV,
    or **any feed a user adds later** is covered by the same one knob, with no code change
    and no inconsistency between stream types. `directory_iterator` is non-recursive and
    only regular files are touched, so committed subdirs (`data/samples/`) are never
    entered. The live metadata dictionary is explicitly skipped (it's pruned by row, below).
  - prunes metadata rows for markets **resolved before** the cutoff
    (`prune_metadata_csv`: keep if `end_date` empty or `end_date >= cutoff`). Such markets
    have no retained ticks referencing them, so this is decode-safe. (Metadata can't be
    age-pruned as a file — it's appended to continuously — hence the per-row rule.)
  - The currently-open journal is being written, so its mtime is fresh and it is never
    deleted. Trade-off accepted: a journal decoded to Parquet much later is aged by decode
    time, not capture time — fine for continuous capture, where files are written live.
- **Cloud archive retention is delegated to the platform**: `.parquet` shipped to S3 is
  expired with an **S3 lifecycle rule** (native object TTL), not application code.

## Consequences

- The 5-minute full rewrite is gone; metadata writes are O(new) regardless of history.
- Operators get a single knob (`POLYMARKET_RETENTION_DAYS`) — no extra scripts, no cron;
  the existing re-discovery thread does the work. Default behavior is unchanged.
- Retention deletes `.parquet` too (the operator chose the window), so anything worth
  keeping should be shipped to S3 (where lifecycle rules govern it) before it ages out.
- The one inherent separate step is `.bin → .parquet` decode (a Python step) — that's
  data transformation, not retention.

## Alternatives considered

- **Time-partitioned metadata files** (`market_metadata_YYYYMMDD.csv`): also kills the
  rewrite and makes retention a file delete, but ripples into every reader
  (decode/analysis/upload all expect one `market_metadata.csv`). Append-only achieves the
  same per-cycle cost with zero ripple, so it was preferred.
- **External prune script + cron/systemd timer**: rejected — script proliferation; the
  goal was for retention to be natural within the running tool.
- **Embedded KV store (SQLite/Parquet keyed by token_id)**: overkill for this scope.
