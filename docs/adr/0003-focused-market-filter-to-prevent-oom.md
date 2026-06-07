# ADR-0003: POLYMARKET_MARKET_FILTER env var for focused subscriptions

**Status:** Accepted  
**Date:** 2026-06

## Context

The harvester originally subscribed to every active market (~5,000 markets, ~10,000
tokens). On a burstable AWS T-instance this caused two failure modes:

1. **CPU credit exhaustion:** parsing a 10k-token firehose pinned CPU at ~79%, draining
   burst credits. AWS then throttles to baseline, the Tickerplant consumer falls behind
   the ring, and the process OOMs.
2. **RDB overflow:** `OrderBook` has a hard cap of `MAX_TOKENS=4096`
   (`include/polymarket/rdb/OrderBook.hpp`). With ~10k subscriptions, tokens above the
   cap were silently dropped.

For the latency-arb study we only need 2–4 tokens (one game market, YES + NO sides).

## Decision

Add `POLYMARKET_MARKET_FILTER` environment variable: a comma-separated list of
case-insensitive substrings matched against the market `question` field.

- **CSV is always written from the full API response** — needed for DuckDB JOINs.
- **The filter only controls the WebSocket subscription** — the two concerns are
  deliberately separate.
- Unset or empty = full firehose (original behaviour, no regression).

Implemented in `src/gateway/MarketDiscovery.cpp` (`filter_by_question`) and wired in
`src/harvester/main.cpp` and the `rediscover_loop`.

## Consequences

- Focused mode eliminates OOM on small instances entirely.
- A mis-spelled filter silently produces 0 subscriptions. The harvester now warns
  explicitly when `subscribe_meta` is empty after filtering.
- The filter is substring-based, not exact-match, so partial terms work:
  `POLYMARKET_MARKET_FILTER="Spurs"` catches both "Spurs vs. Knicks" and any other
  Spurs market active at the time.
