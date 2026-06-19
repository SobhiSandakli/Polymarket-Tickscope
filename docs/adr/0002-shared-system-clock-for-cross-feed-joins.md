# ADR-0002: Single system clock for all feeds to enable cross-feed time joins

**Status:** Accepted  
**Date:** 2026-01

## Context

The latency-arb thesis requires measuring the time delta between an event on a reference
feed (Coinbase BTC price, ESPN score) and the corresponding reprice on Polymarket. This
requires timestamps from both feeds to be directly comparable.

## Decision

All harvesters (Polymarket, Coinbase, and future reference feeds) timestamp ticks using
the same local system clock: `std::chrono::system_clock` in milliseconds since Unix
epoch, stored as `uint64_t ts_ms`. No per-feed clock offsets, no NTP correction logic.

Both binary record formats (`Tick` 128B and `BtcTick` 64B) share the same `ts_ms` field
at the same byte offset so DuckDB can join them directly:
```sql
JOIN btc_ticks b ON b.ts_ms BETWEEN t.ts_ms - 5 AND t.ts_ms + 5
```

## Consequences

- Cross-feed latency measurements are only as accurate as the local clock's stability
  (~1ms jitter typical on Linux, ~10ms on macOS). Sufficient for detecting lags of
  tens to hundreds of milliseconds; not sufficient for microsecond-level HFT.
- All harvesters must run on the same physical machine. Running them on separate hosts
  would require NTP synchronisation and would introduce measurement error.
- Simplicity: no distributed timestamping infrastructure required.
