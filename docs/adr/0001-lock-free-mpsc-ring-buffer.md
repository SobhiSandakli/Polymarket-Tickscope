# ADR-0001: Lock-free MPSC ring buffer between WebSocket and Tickerplant

**Status:** Accepted  
**Date:** 2026-01

## Context

The harvester runs two hot threads: a WebSocket I/O thread (receives and parses ticks)
and a Tickerplant thread (writes ticks to the binary journal). These threads must
communicate without blocking either one — a stalled journal write must not drop ticks,
and a tick burst must not stall the journal.

## Decision

Use a statically-allocated, lock-free MPSC (multi-producer single-consumer) ring buffer
(`include/polymarket/memory/RingBuffer.hpp`, capacity 65536 slots) as the queue between
the two threads.

- Static allocation: the buffer lives in BSS, never heap-allocated, never freed.
- Lock-free: producers (WS threads) and the consumer (Tickerplant) coordinate purely
  via `std::atomic` with explicit memory ordering — no mutexes, no condition variables.
- Fixed capacity: if the consumer falls behind, the ring fills and producers spin-wait
  (backpressure). This is preferable to unbounded queuing which leads to OOM.

## Consequences

- Zero heap allocation in the hot path — cache-friendly, no allocator contention.
- Ring size (65536) must be tuned to the expected burst size. At ~10k subscriptions and
  peak tick rates of ~1k/s, 65536 slots gives ~65s of headroom before backpressure.
- At focused subscription sizes (2–10 markets), the ring is effectively never full.
