"""
Polymarket Arbitrage Bot — Market Fetcher
Fetches active BTC/ETH price-threshold markets and scans for
arbitrage opportunities across strike-price implications.
"""

import json
import re
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from itertools import combinations

import networkx as nx
import pandas as pd
import requests
from py_clob_client.client import ClobClient

CLOB_HOST = "https://clob.polymarket.com"
GAMMA_HOST = "https://gamma-api.polymarket.com"
CHAIN_ID = 137
MAX_ORDER_BOOK_WORKERS = 10
FEE_BUFFER = 0.02


# ---------------------------------------------------------------------------
# Progress helpers
# ---------------------------------------------------------------------------

def _status(msg: str) -> None:
    sys.stdout.write(f"\r\033[K{msg}")
    sys.stdout.flush()


def _elapsed(t0: float) -> str:
    s = int(time.time() - t0)
    return f"{s // 60}m {s % 60}s" if s >= 60 else f"{s}s"


# ---------------------------------------------------------------------------
# Market fetching via Gamma API (server-side filtering, ~2s vs ~3min)
# ---------------------------------------------------------------------------

ASSET_MAP = {"bitcoin": "BTC", "ethereum": "ETH"}

# Matches the real Polymarket question formats:
#   "Will the price of Bitcoin be above $66,000 on February 13?"
#   "Will Bitcoin reach $150,000 by December 31, 2026?"
#   "Will Bitcoin hit $150k by March 31, 2026?"
#   "Will Bitcoin dip to $60,000 in February?"
#   "Will the price of Ethereum be above $2,000 on February 14?"
_PRICE_RE = re.compile(
    r"(?:price of\s+)?"
    r"(?P<asset>Bitcoin|Ethereum)\s+"
    r"(?:be\s+)?(?:above|reach|hit|dip to|be greater than|be less than|be between)\s+"
    r"\$(?P<price>[\d,]+)(?P<suffix>[kKmM])?"
    r".*?(?:on|by|in|before)\s+(?P<date>.+?)\??$",
    re.IGNORECASE,
)

_SUFFIX_MULT = {"k": 1_000, "m": 1_000_000}

# Only keep "above / reach / hit / greater than" for upward thresholds
# (these are the ones where A > B implies A ⊂ B)
_UPWARD_RE = re.compile(r"above|reach|hit|greater than", re.IGNORECASE)


def parse_market_title(title: str) -> dict | None:
    """
    Parse a Polymarket crypto price question into structured data.
    Returns None if it doesn't match or isn't an upward threshold.
    """
    m = _PRICE_RE.search(title)
    if not m:
        return None

    # Only keep upward-threshold markets for implication logic
    if not _UPWARD_RE.search(title):
        return None

    asset = ASSET_MAP[m.group("asset").lower()]
    raw = m.group("price").replace(",", "")
    price = float(raw)
    suffix = m.group("suffix")
    if suffix:
        price *= _SUFFIX_MULT[suffix.lower()]

    date_str = m.group("date").strip().rstrip("?")
    return {"asset": asset, "strike_price": price, "date": date_str}


def fetch_crypto_markets() -> list[dict]:
    """
    Fetch all open BTC/ETH price-threshold markets from the Gamma API.
    Server-side filtering means we only download what we need.
    """
    all_markets: list[dict] = []
    offset = 0
    page_size = 200
    t0 = time.time()

    while True:
        _status(f"[{_elapsed(t0)}] Gamma API offset={offset} ({len(all_markets)} matches)")
        resp = requests.get(f"{GAMMA_HOST}/markets", params={
            "closed": "false",
            "active": "true",
            "limit": page_size,
            "offset": offset,
        }, timeout=10)
        resp.raise_for_status()
        data = resp.json()
        if not isinstance(data, list) or not data:
            break

        for m in data:
            q = m.get("question", "")
            ql = q.lower()
            has_asset = "bitcoin" in ql or "ethereum" in ql
            has_price = bool(re.search(r"\$[\d,]+", q))
            if has_asset and has_price:
                all_markets.append(m)

        offset += page_size
        if len(data) < page_size:
            break

    _status("")
    print(f"Fetched {len(all_markets)} BTC/ETH price markets from Gamma API ({_elapsed(t0)})")
    return all_markets


# ---------------------------------------------------------------------------
# DataFrame builder
# ---------------------------------------------------------------------------

def build_dataframe(markets: list[dict]) -> pd.DataFrame:
    rows = []
    for m in markets:
        clob_ids = m.get("clobTokenIds")
        if isinstance(clob_ids, str):
            clob_ids = json.loads(clob_ids)

        outcomes = m.get("outcomes")
        if isinstance(outcomes, str):
            outcomes = json.loads(outcomes)

        prices = m.get("outcomePrices")
        if isinstance(prices, str):
            prices = json.loads(prices)

        tokens = []
        if clob_ids and outcomes:
            for i, tid in enumerate(clob_ids):
                tokens.append({
                    "token_id": tid,
                    "outcome": outcomes[i] if i < len(outcomes) else "?",
                    "price": float(prices[i]) if prices and i < len(prices) else None,
                })

        rows.append({
            "Market ID": m.get("conditionId", ""),
            "Title": m.get("question", ""),
            "Tokens": tokens,
            "End Date": m.get("endDateIso", "")[:10],
        })
    return pd.DataFrame(rows)


# ---------------------------------------------------------------------------
# Implication graph
# ---------------------------------------------------------------------------

def build_implication_graph(df: pd.DataFrame) -> nx.DiGraph:
    """
    Edge A → B  means  "A being true guarantees B is true"
    (A has a higher strike than B, same asset & date).
    """
    G = nx.DiGraph()
    G.add_nodes_from(df["Market ID"])

    for (_asset, _date), group in df.groupby(["Asset", "Date"]):
        if len(group) < 2:
            continue
        for (_, a), (_, b) in combinations(group.iterrows(), 2):
            if a["Strike"] > b["Strike"]:
                G.add_edge(a["Market ID"], b["Market ID"])
            elif b["Strike"] > a["Strike"]:
                G.add_edge(b["Market ID"], a["Market ID"])

    return G


# ---------------------------------------------------------------------------
# Arbitrage scanner
# ---------------------------------------------------------------------------

def _yes_token_id(tokens: list[dict]) -> str | None:
    for t in tokens:
        if t.get("outcome", "").lower() == "yes":
            return t["token_id"]
    return None


def _best_price(book, side: str) -> float | None:
    levels = getattr(book, side, None) or book.get(side, []) if isinstance(book, dict) else getattr(book, side, [])
    if not levels:
        return None
    entry = levels[0]
    price = entry.get("price") if isinstance(entry, dict) else getattr(entry, "price", None)
    return float(price) if price is not None else None


def _fetch_book(client: ClobClient, token_id: str) -> dict | None:
    try:
        return client.get_order_book(token_id)
    except Exception:
        return None


def check_arb(
    graph: nx.DiGraph,
    client: ClobClient,
    df: pd.DataFrame,
) -> list[dict]:
    rows = df.set_index("Market ID")
    edges = list(graph.edges())

    if not edges:
        print("\nNo edges to scan.")
        return []

    # Collect unique Yes tokens
    token_for: dict[str, str] = {}
    for a, b in edges:
        for mid in (a, b):
            if mid not in token_for:
                tid = _yes_token_id(rows.loc[mid]["Tokens"])
                if tid:
                    token_for[mid] = tid

    unique = set(token_for.values())
    print(f"\nFetching {len(unique)} order books ({len(edges)} edges)…")

    # Parallel fetch
    books: dict[str, dict] = {}
    t0 = time.time()
    done = 0
    with ThreadPoolExecutor(max_workers=MAX_ORDER_BOOK_WORKERS) as pool:
        futs = {pool.submit(_fetch_book, client, tid): tid for tid in unique}
        for f in as_completed(futs):
            tid = futs[f]
            res = f.result()
            if res is not None:
                books[tid] = res
            done += 1
            _status(f"[{_elapsed(t0)}] Order books: {done}/{len(unique)} ({len(books)} OK)")

    _status("")
    print(f"Fetched {len(books)}/{len(unique)} order books ({_elapsed(t0)})")

    # Evaluate
    opportunities: list[dict] = []
    skipped = 0
    for hard_id, easy_id in edges:
        tid_h, tid_e = token_for.get(hard_id), token_for.get(easy_id)
        if not tid_h or not tid_e:
            skipped += 1
            continue
        bk_h, bk_e = books.get(tid_h), books.get(tid_e)
        if not bk_h or not bk_e:
            skipped += 1
            continue

        ask_a = _best_price(bk_h, "asks")
        bid_b = _best_price(bk_e, "bids")
        if ask_a is None or bid_b is None:
            skipped += 1
            continue

        if bid_b > ask_a * (1 + FEE_BUFFER):
            spread = ((bid_b - ask_a) / ask_a) * 100
            hard, easy = rows.loc[hard_id], rows.loc[easy_id]
            print(
                f"  FOUND ARB: Sell [{easy['Title']}] @ {bid_b:.4f}"
                f" | Buy [{hard['Title']}] @ {ask_a:.4f}"
                f" | Spread: {spread:.2f}%"
            )
            opportunities.append({
                "buy_market": hard_id,
                "buy_title": hard["Title"],
                "buy_price": ask_a,
                "sell_market": easy_id,
                "sell_title": easy["Title"],
                "sell_price": bid_b,
                "spread_pct": spread,
            })

    if skipped:
        print(f"  ({skipped} edges skipped — missing token or empty book)")

    return opportunities


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    t_start = time.time()

    # 1. Fetch from Gamma API
    raw_markets = fetch_crypto_markets()
    if not raw_markets:
        print("No BTC/ETH price markets found.")
        return

    # 2. Build DataFrame
    df = build_dataframe(raw_markets)

    # 3. Parse titles
    parsed = df["Title"].apply(parse_market_title)
    df["Asset"] = parsed.apply(lambda p: p["asset"] if p else None)
    df["Strike"] = parsed.apply(lambda p: p["strike_price"] if p else None)
    df["Date"] = parsed.apply(lambda p: p["date"] if p else None)
    df = df.dropna(subset=["Asset", "Strike"])

    print(f"Parsed {len(df)} upward-threshold markets (above/reach/hit)\n")

    if df.empty:
        print("No parseable markets found.")
        return

    # Show summary by asset + date
    summary = df.groupby(["Asset", "Date"]).agg(
        count=("Strike", "size"),
        min_strike=("Strike", "min"),
        max_strike=("Strike", "max"),
    ).reset_index()
    print(summary.to_string(index=False))

    # 4. Graph
    G = build_implication_graph(df)
    print(f"\nImplication graph: {G.number_of_nodes()} nodes, {G.number_of_edges()} edges")

    # 5. Arb scan
    client = ClobClient(host=CLOB_HOST, chain_id=CHAIN_ID)
    arbs = check_arb(G, client, df)

    if arbs:
        print(f"\nTotal opportunities: {len(arbs)}")
        arb_df = pd.DataFrame(arbs).sort_values("spread_pct", ascending=False)
        print(f"\n{'='*70}")
        print(arb_df[["buy_title", "buy_price", "sell_title", "sell_price", "spread_pct"]].to_string(index=False))
        print(f"{'='*70}")
    else:
        print("\nNo arbitrage opportunities at current prices.")

    print(f"\nDone in {_elapsed(t_start)}")


if __name__ == "__main__":
    main()
