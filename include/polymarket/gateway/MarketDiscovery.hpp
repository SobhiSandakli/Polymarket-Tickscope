#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace polymarket::gateway
{

    // ---------------------------------------------------------------------------
    // TokenMeta — full metadata for one CLOB token (YES or NO side of a market).
    //
    // Populated once at startup from the Polymarket Gamma REST API.
    // All fields are std::string — startup-only, no hot-path allocation concern.
    // ---------------------------------------------------------------------------
    struct TokenMeta
    {
        std::string token_id;     // CLOB token ID  (== asset_id on the wire)
        std::string condition_id; // Groups the YES + NO token pair for one market
        std::string outcome;      // "YES" or "NO"
        std::string question;     // Human-readable market question
        std::string end_date;     // ISO 8601 expiry, e.g. "2026-03-01T00:00:00Z"
        bool active;              // Is the market currently open for trading?
        int taker_fee_bps;        // Taker fee in basis points (0 = free)
    };

    // ---------------------------------------------------------------------------
    // fetch_active_tokens
    //
    // BOOT-SEQUENCE ONLY — never called in the hot path.
    //
    // Paginates GET /markets?closed=false&active=true&order=volume24hr from the
    // Polymarket Gamma API and returns full metadata for every active CLOB token.
    //
    // Returns an empty vector on network or parse failure (caller must check).
    // ---------------------------------------------------------------------------
    [[nodiscard]] std::vector<TokenMeta> fetch_active_tokens();

    // ---------------------------------------------------------------------------
    // write_metadata_csv
    //
    // BOOT-SEQUENCE ONLY.
    //
    // Writes a single CSV sidecar file containing the full TokenMeta table.
    // The CSV is human-readable, loadable by pandas/DuckDB/Athena with no
    // extra dependencies, and serves as the join key for Parquet tick analysis.
    //
    // Schema: asset_id, condition_id, outcome, question, end_date, active,
    //         taker_fee_bps
    //
    // The question field is always double-quoted and internal double-quotes are
    // escaped by doubling ("CSV standard").
    //
    // Does NOT use Apache Arrow or any analytics library — pure std::fprintf.
    // ---------------------------------------------------------------------------
    void write_metadata_csv(
        const std::vector<TokenMeta> &metadata,
        const char *output_path);

    // ---------------------------------------------------------------------------
    // read_csv_tokens
    //
    // BOOT-SEQUENCE / REDISCOVERY ONLY.
    //
    // Reads an existing metadata CSV written by write_metadata_csv() and returns
    // all rows, including delisted markets (active=0).
    // Returns an empty vector if the file does not exist (first run is fine).
    // ---------------------------------------------------------------------------
    [[nodiscard]] std::vector<TokenMeta> read_csv_tokens(const char *path);

    // ---------------------------------------------------------------------------
    // merge_and_write_metadata_csv
    //
    // BOOT-SEQUENCE / REDISCOVERY ONLY.
    //
    // Merges fresh_tokens with the existing CSV on disk, then atomically rewrites:
    //   - token_id in both:         upsert (fresh fields win)
    //   - token_id only in fresh:   insert (new market, active=1)
    //   - token_id only in CSV:     preserve with active=0 (delisted — keeps
    //                               historical tick JOINs intact)
    //
    // Atomic write: writes to <path>.tmp then rename() — DuckDB never sees a
    // partial file.
    // ---------------------------------------------------------------------------
    void merge_and_write_metadata_csv(
        const std::vector<TokenMeta> &fresh_tokens,
        const char *path);

    // ---------------------------------------------------------------------------
    // append_new_metadata_csv
    //
    // REDISCOVERY HOT-PATH alternative to merge_and_write_metadata_csv.
    //
    // Appends ONLY tokens whose token_id is not already in `known` (seeded once at
    // boot, then kept current by this function). Per-cycle cost is O(new tokens),
    // not O(all history) — no full rewrite. Creates the file with a header if it
    // does not exist.
    //
    // Trade-off vs. merge: the `active` column reflects active-at-first-discovery
    // and is not flipped to 0 on later delisting (a full merge at the next boot
    // repairs it). Decoding only needs token_id → question/outcome, so this is
    // sufficient; "active now?" is derivable from end_date.
    // ---------------------------------------------------------------------------
    void append_new_metadata_csv(
        const std::vector<TokenMeta> &fresh_tokens,
        const char *path,
        std::unordered_set<std::string> &known);

    // ---------------------------------------------------------------------------
    // prune_metadata_csv
    //
    // RETENTION ONLY. Rewrites the CSV keeping rows whose market has not resolved
    // before cutoff_iso_date (kept if end_date empty or end_date >= cutoff, ISO
    // lexicographic compare). A market resolved before the cutoff has no retained
    // ticks referencing it, so dropping its metadata never breaks decoding.
    // Returns the number of rows dropped.
    // ---------------------------------------------------------------------------
    int prune_metadata_csv(const char *path, const std::string &cutoff_iso_date);

    // ---------------------------------------------------------------------------
    // filter_by_question
    //
    // Returns only tokens whose question contains at least one of the
    // comma-separated substrings in filter_csv (case-insensitive).
    // Returns all tokens unchanged when filter_csv is empty.
    //
    // Used to scope the harvester to a small set of markets (e.g. one game)
    // instead of the full ~10k-token firehose, eliminating OOM on small instances.
    // ---------------------------------------------------------------------------
    [[nodiscard]] std::vector<TokenMeta> filter_by_question(
        const std::vector<TokenMeta> &tokens,
        const std::string &filter_csv);

} // namespace polymarket::gateway
