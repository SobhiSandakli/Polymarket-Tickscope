# ADR-0004: Gamma API pagination must use ascending=false and PAGE_SIZE=100

**Status:** Accepted  
**Date:** 2026-06

## Context

The Gamma API (`gamma-api.polymarket.com/markets`) has two undocumented constraints
discovered through testing:

1. **Hard cap of 100 results per page** regardless of the `limit` parameter.
   The original `PAGE_SIZE=200` caused the loop to break after page 1 every time
   (`page_count(100) < PAGE_SIZE(200)` → break), so only the first 100 markets were
   ever fetched.

2. **Default sort order is ascending** (lowest volume first). Without `ascending=false`,
   the 100 markets returned are the *least* liquid ones — the opposite of what we want.
   High-volume game markets (e.g. NBA Finals at $3M/24h) never appeared.

## Decision

- Set `PAGE_SIZE = 100` to match the API's actual page cap and fix the pagination loop.
- Add `&ascending=false` to every page request so results are ordered highest-volume
  first, and the most liquid markets appear on page 1.
- `MAX_PAGES` increased from 25 to 50 (supports up to 5,000 markets with correct
  pagination).

## Consequences

- First page now reliably contains the ~100 most liquid markets — sufficient for most
  focused-mode use cases.
- Full discovery now paginates through the reachable head of the active market list
  (the top ~2,100 by 24h volume — see the offset cap below) rather than stopping after
  page 1.

## Offset cap, and why offset (not keyset)

Gamma also rejects *deep* offset pagination: `offset=2100` returns **HTTP 422**
(`"offset too large, use /markets/keyset for deeper pagination"`). The last reachable
page is `offset=2000`, so discovery tops out at ~2,100 markets (≈4,200 YES/NO tokens) —
the top ~2,100 by 24h volume.

We deliberately stay on offset pagination rather than switching to the keyset endpoint:

- **Offset can sort by live volume; keyset cannot.** Keyset pagination needs a *stable,
  monotonic* cursor key (e.g. `id` / `created_at`). `volume24hr` changes with every
  trade, so it can't be a keyset key. To get "top-N by volume" via keyset you'd have to
  walk the *entire* catalog by id and rank client-side — far more work for the same head
  of the distribution.
- **The cap never bites in practice.** The markets we capture (crypto up/down, sports,
  headline events) are always high-volume, hence always within the top ~2,100.
- Keyset would only be needed to archive the long tail (rank 2100+), which this project
  does not target. Adding it to `MarketDiscovery.cpp` is a known, unbuilt option.
