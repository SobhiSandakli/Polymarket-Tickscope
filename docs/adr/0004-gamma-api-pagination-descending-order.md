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
- Full discovery (all pages) now correctly paginates through the entire active market
  list rather than stopping after page 1.
