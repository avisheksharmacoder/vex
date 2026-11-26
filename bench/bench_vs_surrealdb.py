#!/usr/bin/env python3
"""Reproducible small-scale head-to-head benchmark: Vex vs SurrealDB (Embedded).

Follows docs/testing_policy.md small-budget guardrails (<= 50 MB, default 10k rows).

Covers:
  1. Bulk row ingestion (Vex SPSC put+flush vs SurrealDB batched INSERT RETURN NONE)
  2. Primary-key point lookups (Vex Robin Hood PK index vs SurrealDB RecordID lookup)
  3. Numeric filter scan + projection (amount > 900.0)
  4. Compound range filter (bucket == 3 and 200.0 <= amount <= 600.0)
  5. Full-table multi-aggregation (count, sum, min, max)
  6. Filtered group-by aggregation (8 buckets; Vex morsel SIMD fast path)
  7. High-cardinality group-by aggregation (by user_id)
  8. Top-K filter + sort with limit ([:10])
  9. Relational join / record-link traversal + group aggregation
 10. End-to-end query -> NumPy float64 array sum (zero-copy vs materialization)
 11. Primary-key point updates (run last to preserve read invariants)
"""

from __future__ import annotations

import argparse
import gc
import json
import math
import os
import platform
import random
import statistics
import sys
import tempfile
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Callable

# Allow running directly from repo root without installing Vex wheel
ROOT = Path(__file__).resolve().parent.parent
PY_DIR = ROOT / "python"
if str(PY_DIR) not in sys.path:
    sys.path.insert(0, str(PY_DIR))

import vex
from surrealdb import RecordID, Surreal

try:
    import numpy as np
except ImportError:
    np = None  # type: ignore[assignment]

COUNTRIES = ("IN", "US", "JP", "DE", "UK", "SG", "BR", "CA")


@dataclass
class BenchRow:
    benchmark: str
    vex_ms: float
    surreal_ms: float
    vex_metric: str
    surreal_metric: str
    speedup: float


def time_fn(fn: Callable[[], Any], *, iters: int, warmup: int = 1) -> tuple[float, Any]:
    """Run warmup + timed iterations; return (median_ms, last_result)."""
    last = None
    for _ in range(warmup):
        last = fn()

    samples_ms: list[float] = []
    for _ in range(iters):
        t0 = time.perf_counter()
        last = fn()
        samples_ms.append((time.perf_counter() - t0) * 1000.0)
    return statistics.median(samples_ms), last


def build_datasets(
    n_orders: int,
    n_users: int,
    n_lookups: int,
    n_updates: int,
    seed: int,
) -> dict[str, Any]:
    """Pre-generate deterministic dicts & RecordIDs so object creation is excluded from timing."""
    rng = random.Random(seed)

    vex_users: list[dict[str, Any]] = []
    surreal_users: list[dict[str, Any]] = []
    user_country: list[str] = [""] * (n_users + 1)

    for uid in range(1, n_users + 1):
        c = COUNTRIES[uid % len(COUNTRIES)]
        tier = uid % 5
        user_country[uid] = c
        vex_users.append({"id": uid, "country": c, "tier": tier})
        surreal_users.append({"id": RecordID("users", uid), "country": c, "tier": tier})

    vex_orders: list[dict[str, Any]] = []
    surreal_orders: list[dict[str, Any]] = []

    expect_agg = {b: [0, 0.0] for b in range(8)}
    expect_join = {c: [0, 0.0] for c in COUNTRIES}
    expect_total_amt = 0.0
    expect_min_amt = float("inf")
    expect_max_amt = float("-inf")
    expect_gt900_cnt = 0
    expect_range_cnt = 0
    paid_amounts: list[float] = []

    for oid in range(1, n_orders + 1):
        uid = ((oid * 2654435761) % n_users) + 1
        bucket = oid & 7
        amount = float((oid * 37) % 1000) + 0.5
        paid = (oid % 5) != 0  # 80% paid

        vex_orders.append(
            {
                "id": oid,
                "user_id": uid,
                "bucket": bucket,
                "amount": amount,
                "paid": paid,
            }
        )
        surreal_orders.append(
            {
                "id": RecordID("orders", oid),
                "user_id": RecordID("users", uid),
                "bucket": bucket,
                "amount": amount,
                "paid": paid,
            }
        )

        expect_total_amt += amount
        if amount < expect_min_amt:
            expect_min_amt = amount
        if amount > expect_max_amt:
            expect_max_amt = amount
        if amount > 900.0:
            expect_gt900_cnt += 1
        if bucket == 3 and 200.0 <= amount <= 600.0:
            expect_range_cnt += 1
        if amount > 500.0:
            expect_agg[bucket][0] += 1
            expect_agg[bucket][1] += amount
        if paid:
            paid_amounts.append(amount)
            c = user_country[uid]
            expect_join[c][0] += 1
            expect_join[c][1] += amount

    paid_amounts.sort(reverse=True)
    expect_topk = paid_amounts[:10]

    lookup_keys = [rng.randint(1, n_orders) for _ in range(n_lookups)]
    surreal_lookup_rids = [RecordID("orders", k) for k in lookup_keys]
    expect_pk_sum = sum(float((k * 37) % 1000) + 0.5 for k in lookup_keys)

    update_keys = [rng.randint(1, n_orders) for _ in range(n_updates)]
    surreal_update_rids = [RecordID("orders", k) for k in update_keys]
    update_vals = [float((k * 13) % 500) + 7000.5 for k in update_keys]

    return {
        "vex_users": vex_users,
        "surreal_users": surreal_users,
        "vex_orders": vex_orders,
        "surreal_orders": surreal_orders,
        "lookup_keys": lookup_keys,
        "surreal_lookup_rids": surreal_lookup_rids,
        "update_keys": update_keys,
        "surreal_update_rids": surreal_update_rids,
        "update_vals": update_vals,
        "expect_agg": expect_agg,
        "expect_join": expect_join,
        "expect_total_amt": expect_total_amt,
        "expect_min_amt": expect_min_amt,
        "expect_max_amt": expect_max_amt,
        "expect_gt900_cnt": expect_gt900_cnt,
        "expect_range_cnt": expect_range_cnt,
        "expect_topk": expect_topk,
        "expect_pk_sum": expect_pk_sum,
    }


def open_vex(path: str | None, workers: int, chunk_rows: int) -> vex.Database:
    db = vex.open(path, workers=workers, chunk_rows=chunk_rows, fsync=(path is not None))
    db.q("table users { id: u64 key, country: str, tier: i64 }", sync=True)
    db.q(
        "table orders { id: u64 key, user_id: u64 -> users.id, bucket: i64, amount: f64, paid: bool }",
        sync=True,
    )
    return db


def open_surreal(url: str) -> Surreal:
    db = Surreal(url)
    db.connect()
    if url.startswith(("ws://", "wss://", "http://", "https://")):
        db.signin({"username": os.environ.get("SURREAL_USER", "root"), "password": os.environ.get("SURREAL_PASS", "root")})
    db.use("bench", "bench")
    db.query(
        """
        REMOVE TABLE IF EXISTS orders;
        REMOVE TABLE IF EXISTS users;
        DEFINE TABLE users SCHEMAFULL;
        DEFINE FIELD country ON users TYPE string;
        DEFINE FIELD tier ON users TYPE int;
        DEFINE TABLE orders SCHEMAFULL;
        DEFINE FIELD user_id ON orders TYPE record<users>;
        DEFINE FIELD bucket ON orders TYPE int;
        DEFINE FIELD amount ON orders TYPE float;
        DEFINE FIELD paid ON orders TYPE bool;
        """
    ).execute()
    return db


def verify_agg(rows: list[tuple[Any, ...]], expect: dict[int, list[Any]], label: str) -> None:
    assert len(rows) == 8, f"{label}: expected 8 buckets, got {len(rows)}"
    for b in range(8):
        rb, cnt, total = rows[b]
        assert int(rb) == b, f"{label}: bucket mismatch {rb} != {b}"
        assert int(cnt) == expect[b][0], f"{label}: count mismatch on bucket {b}"
        assert math.isclose(float(total), expect[b][1], rel_tol=1e-9), (
            f"{label}: sum mismatch on bucket {b}: {total} vs {expect[b][1]}"
        )


def verify_join(rows: list[tuple[Any, ...]], expect: dict[str, list[Any]], label: str) -> None:
    assert len(rows) == len(expect), f"{label}: expected {len(expect)} countries, got {len(rows)}"
    for country, cnt, rev in rows:
        exp_cnt, exp_rev = expect[country]
        assert int(cnt) == exp_cnt, f"{label}: count mismatch for {country}"
        assert math.isclose(float(rev), exp_rev, rel_tol=1e-9), (
            f"{label}: rev mismatch for {country}: {rev} vs {exp_rev}"
        )


def format_markdown_table(rows: list[BenchRow]) -> str:
    headers = ["Benchmark", "Vex (ms)", "SurrealDB (ms)", "Vex Metric", "SurrealDB Metric", "Vex Speedup"]
    body: list[list[str]] = []
    for r in rows:
        body.append(
            [
                r.benchmark,
                f"{r.vex_ms:.2f}",
                f"{r.surreal_ms:.2f}",
                r.vex_metric,
                r.surreal_metric,
                f"{r.speedup:.2f}x",
            ]
        )

    widths = [max(len(h), *(len(row[i]) for row in body)) for i, h in enumerate(headers)]
    out = [
        "| " + " | ".join(headers[i].ljust(widths[i]) for i in range(len(headers))) + " |",
        "|-" + "-|-".join("-" * widths[i] for i in range(len(headers))) + "-|",
    ]
    for row in body:
        cells = [
            row[i].ljust(widths[i]) if i == 0 else row[i].rjust(widths[i])
            for i in range(len(headers))
        ]
        out.append("| " + " | ".join(cells) + " |")
    return "\n".join(out)


def fmt_rate_rows(n_rows: int, ms: float) -> str:
    rate = n_rows / (ms / 1000.0)
    if rate >= 1e6:
        return f"{rate / 1e6:.2f} M rows/s"
    if rate >= 1e3:
        return f"{rate / 1e3:,.1f}k rows/s"
    return f"{rate:,.0f} rows/s"


def fmt_rate_ops(n_ops: int, ms: float) -> str:
    rate = n_ops / (ms / 1000.0)
    if rate >= 1e3:
        return f"{rate / 1e3:,.1f}k ops/s"
    return f"{rate:,.0f} ops/s"


def run_suite(args: argparse.Namespace, mode: str) -> tuple[str, list[BenchRow]]:
    n_orders = args.rows
    n_users = max(100, args.users if args.users is not None else n_orders // 5)
    n_lookups = args.pk_lookups
    n_updates = args.pk_updates
    total_inserted = n_orders + n_users

    data = build_datasets(n_orders, n_users, n_lookups, n_updates, args.seed)
    results: list[BenchRow] = []

    with tempfile.TemporaryDirectory(prefix="vex_surreal_bench_") as tmpdir:
        vex_path = None if mode == "memory" else str(Path(tmpdir) / "bench.wal")
        if args.surreal_url:
            surreal_url = args.surreal_url
        elif mode == "memory":
            surreal_url = "mem://"
        else:
            surreal_url = f"surrealkv://{Path(tmpdir) / 'surreal.db'}"

        vdb = open_vex(vex_path, args.workers, args.chunk_rows)
        sdb = open_surreal(surreal_url)
        surreal_ver = str(sdb.version())

        try:
            # ------------------------------------------------------------------
            # 1. BULK INSERT (users + orders)
            # ------------------------------------------------------------------
            gc.collect()
            t0 = time.perf_counter()
            for u in data["vex_users"]:
                vdb.put("users", u, sync=False)
            for o in data["vex_orders"]:
                vdb.put("orders", o, sync=False)
            vdb.flush()
            vex_ins_ms = (time.perf_counter() - t0) * 1000.0

            gc.collect()
            batch_sz = args.surreal_batch_size
            t0 = time.perf_counter()
            for i in range(0, n_users, batch_sz):
                sdb.query(
                    "INSERT INTO users $rows RETURN NONE",
                    {"rows": data["surreal_users"][i : i + batch_sz]},
                ).execute()
            for i in range(0, n_orders, batch_sz):
                sdb.query(
                    "INSERT INTO orders $rows RETURN NONE",
                    {"rows": data["surreal_orders"][i : i + batch_sz]},
                ).execute()
            surreal_ins_ms = (time.perf_counter() - t0) * 1000.0

            assert vdb.q("orders { n: count() }")["n"][0] == n_orders
            assert sdb.query("SELECT count() AS n FROM orders GROUP ALL").execute()[0][0]["n"] == n_orders

            results.append(
                BenchRow(
                    benchmark=f"1. Bulk Insert ({total_inserted:,} rows)",
                    vex_ms=vex_ins_ms,
                    surreal_ms=surreal_ins_ms,
                    vex_metric=fmt_rate_rows(total_inserted, vex_ins_ms),
                    surreal_metric=fmt_rate_rows(total_inserted, surreal_ins_ms),
                    speedup=surreal_ins_ms / vex_ins_ms,
                )
            )

            # ------------------------------------------------------------------
            # 2. O(1) PRIMARY-KEY POINT LOOKUPS
            # ------------------------------------------------------------------
            v_keys = data["lookup_keys"]
            s_rids = data["surreal_lookup_rids"]

            def vex_pk() -> float:
                acc = 0.0
                for k in v_keys:
                    acc += vdb.q("orders[$k] amount", k=k)["amount"][0]
                return acc

            def surreal_pk() -> float:
                acc = 0.0
                for rid in s_rids:
                    acc += sdb.query("SELECT VALUE amount FROM ONLY $id", {"id": rid}).execute()[0]
                return acc

            vex_pk_ms, v_sum = time_fn(vex_pk, iters=args.iters)
            surreal_pk_ms, s_sum = time_fn(surreal_pk, iters=args.iters)
            assert math.isclose(v_sum, data["expect_pk_sum"], rel_tol=1e-9)
            assert math.isclose(s_sum, data["expect_pk_sum"], rel_tol=1e-9)

            results.append(
                BenchRow(
                    benchmark=f"2. PK Point Lookups ({n_lookups:,} ops)",
                    vex_ms=vex_pk_ms,
                    surreal_ms=surreal_pk_ms,
                    vex_metric=fmt_rate_ops(n_lookups, vex_pk_ms),
                    surreal_metric=fmt_rate_ops(n_lookups, surreal_pk_ms),
                    speedup=surreal_pk_ms / vex_pk_ms,
                )
            )

            # ------------------------------------------------------------------
            # 3. NUMERIC FILTER SCAN + PROJECTION
            # ------------------------------------------------------------------
            vex_scan_q = "orders[amount > 900.0] id, amount"
            surreal_scan_q = "SELECT id, amount FROM orders WHERE amount > 900.0"

            vex_scan_ms, v_scnt = time_fn(lambda: vdb.q(vex_scan_q).nrows, iters=args.iters)
            surreal_scan_ms, s_scnt = time_fn(
                lambda: len(sdb.query(surreal_scan_q).execute()[0]), iters=args.iters
            )
            assert v_scnt == s_scnt == data["expect_gt900_cnt"]

            results.append(
                BenchRow(
                    benchmark="3. Filter Scan (amount > 900.0)",
                    vex_ms=vex_scan_ms,
                    surreal_ms=surreal_scan_ms,
                    vex_metric=fmt_rate_rows(n_orders, vex_scan_ms),
                    surreal_metric=fmt_rate_rows(n_orders, surreal_scan_ms),
                    speedup=surreal_scan_ms / vex_scan_ms,
                )
            )

            # ------------------------------------------------------------------
            # 4. COMPOUND RANGE FILTER (3 predicates)
            # ------------------------------------------------------------------
            vex_rng_q = "orders[bucket == 3 and amount >= 200.0 and amount <= 600.0] amount"
            surreal_rng_q = (
                "SELECT VALUE amount FROM orders WHERE bucket = 3 AND amount >= 200.0 AND amount <= 600.0"
            )

            vex_rng_ms, v_rcnt = time_fn(lambda: vdb.q(vex_rng_q).nrows, iters=args.iters)
            surreal_rng_ms, s_rcnt = time_fn(
                lambda: len(sdb.query(surreal_rng_q).execute()[0]), iters=args.iters
            )
            assert v_rcnt == s_rcnt == data["expect_range_cnt"]

            results.append(
                BenchRow(
                    benchmark="4. Compound Range Filter (3 preds)",
                    vex_ms=vex_rng_ms,
                    surreal_ms=surreal_rng_ms,
                    vex_metric=fmt_rate_rows(n_orders, vex_rng_ms),
                    surreal_metric=fmt_rate_rows(n_orders, surreal_rng_ms),
                    speedup=surreal_rng_ms / vex_rng_ms,
                )
            )

            # ------------------------------------------------------------------
            # 5. FULL-TABLE MULTI-AGGREGATION (count, sum, min, max)
            # ------------------------------------------------------------------
            vex_magg_q = "orders { n: count(), s: sum(amount), mi: min(amount), ma: max(amount) }"
            surreal_magg_q = (
                "SELECT count() AS n, math::sum(amount) AS s, "
                "math::min(amount) AS mi, math::max(amount) AS ma FROM orders GROUP ALL"
            )

            vex_magg_ms, v_magg = time_fn(lambda: vdb.q(vex_magg_q).rows()[0], iters=args.iters)
            surreal_magg_ms, s_magg = time_fn(
                lambda: sdb.query(surreal_magg_q).execute()[0][0], iters=args.iters
            )
            assert v_magg[0] == s_magg["n"] == n_orders
            assert math.isclose(v_magg[1], s_magg["s"], rel_tol=1e-9)
            assert math.isclose(v_magg[1], data["expect_total_amt"], rel_tol=1e-9)
            assert math.isclose(v_magg[2], s_magg["mi"], rel_tol=1e-9)
            assert math.isclose(v_magg[3], s_magg["ma"], rel_tol=1e-9)

            results.append(
                BenchRow(
                    benchmark="5. Multi-Agg (count, sum, min, max)",
                    vex_ms=vex_magg_ms,
                    surreal_ms=surreal_magg_ms,
                    vex_metric=fmt_rate_rows(n_orders, vex_magg_ms),
                    surreal_metric=fmt_rate_rows(n_orders, surreal_magg_ms),
                    speedup=surreal_magg_ms / vex_magg_ms,
                )
            )

            # ------------------------------------------------------------------
            # 6. FILTERED GROUP-BY AGGREGATION (8 buckets, Vex Morsel SIMD fast path)
            # ------------------------------------------------------------------
            vex_gagg_q = "orders[amount > 500.0] by bucket { n: count(), s: sum(amount) } sort bucket"
            surreal_gagg_q = (
                "SELECT bucket, count() AS n, math::sum(amount) AS s "
                "FROM orders WHERE amount > 500.0 GROUP BY bucket ORDER BY bucket"
            )

            vex_gagg_ms, v_gagg = time_fn(lambda: vdb.q(vex_gagg_q).rows(), iters=args.iters)
            surreal_gagg_ms, s_gagg = time_fn(
                lambda: [(r["bucket"], r["n"], r["s"]) for r in sdb.query(surreal_gagg_q).execute()[0]],
                iters=args.iters,
            )
            verify_agg(v_gagg, data["expect_agg"], "Vex filtered group-by")
            verify_agg(s_gagg, data["expect_agg"], "SurrealDB filtered group-by")

            results.append(
                BenchRow(
                    benchmark="6. Filtered Group-By (8 buckets)",
                    vex_ms=vex_gagg_ms,
                    surreal_ms=surreal_gagg_ms,
                    vex_metric=fmt_rate_rows(n_orders, vex_gagg_ms),
                    surreal_metric=fmt_rate_rows(n_orders, surreal_gagg_ms),
                    speedup=surreal_gagg_ms / vex_gagg_ms,
                )
            )

            # ------------------------------------------------------------------
            # 7. HIGH-CARDINALITY GROUP-BY (by user_id)
            # ------------------------------------------------------------------
            vex_hc_q = "orders by user_id { n: count(), s: sum(amount) }"
            surreal_hc_q = "SELECT user_id, count() AS n, math::sum(amount) AS s FROM orders GROUP BY user_id"

            vex_hc_ms, v_hc = time_fn(lambda: vdb.q(vex_hc_q).nrows, iters=args.iters)
            surreal_hc_ms, s_hc = time_fn(
                lambda: len(sdb.query(surreal_hc_q).execute()[0]), iters=args.iters
            )
            assert v_hc == s_hc == n_users

            results.append(
                BenchRow(
                    benchmark=f"7. High-Card Group-By ({n_users:,} groups)",
                    vex_ms=vex_hc_ms,
                    surreal_ms=surreal_hc_ms,
                    vex_metric=fmt_rate_rows(n_orders, vex_hc_ms),
                    surreal_metric=fmt_rate_rows(n_orders, surreal_hc_ms),
                    speedup=surreal_hc_ms / vex_hc_ms,
                )
            )

            # ------------------------------------------------------------------
            # 8. TOP-K FILTER + SORT ([:10])
            # ------------------------------------------------------------------
            vex_topk_q = "orders[paid] amount sort -amount [:10]"
            surreal_topk_q = "SELECT VALUE amount FROM orders WHERE paid = true ORDER BY amount DESC LIMIT 10"

            vex_topk_ms, v_topk = time_fn(
                lambda: [r[0] for r in vdb.q(vex_topk_q).rows()], iters=args.iters
            )
            surreal_topk_ms, s_topk = time_fn(
                lambda: sdb.query(surreal_topk_q).execute()[0], iters=args.iters
            )
            assert len(v_topk) == len(s_topk) == len(data["expect_topk"]) == 10
            for i in range(10):
                assert math.isclose(v_topk[i], data["expect_topk"][i], rel_tol=1e-9)
                assert math.isclose(s_topk[i], data["expect_topk"][i], rel_tol=1e-9)

            results.append(
                BenchRow(
                    benchmark="8. Top-K Filter + Sort ([:10])",
                    vex_ms=vex_topk_ms,
                    surreal_ms=surreal_topk_ms,
                    vex_metric=fmt_rate_rows(n_orders, vex_topk_ms),
                    surreal_metric=fmt_rate_rows(n_orders, surreal_topk_ms),
                    speedup=surreal_topk_ms / vex_topk_ms,
                )
            )

            # ------------------------------------------------------------------
            # 9. JOIN / RECORD-LINK TRAVERSAL + GROUP AGGREGATION
            # ------------------------------------------------------------------
            vex_join_q = "orders[paid] & users by users.country { n: count(), rev: sum(amount) } sort -rev"
            surreal_join_q = (
                "SELECT user_id.country AS country, count() AS n, math::sum(amount) AS rev "
                "FROM orders WHERE paid = true GROUP BY country ORDER BY rev DESC"
            )

            vex_join_ms, v_join = time_fn(lambda: vdb.q(vex_join_q).rows(), iters=args.iters)
            surreal_join_ms, s_join = time_fn(
                lambda: [(r["country"], r["n"], r["rev"]) for r in sdb.query(surreal_join_q).execute()[0]],
                iters=args.iters,
            )
            verify_join(v_join, data["expect_join"], "Vex join")
            verify_join(s_join, data["expect_join"], "SurrealDB record-link join")

            results.append(
                BenchRow(
                    benchmark=f"9. Join / Link + Agg ({n_orders:,} x {n_users:,})",
                    vex_ms=vex_join_ms,
                    surreal_ms=surreal_join_ms,
                    vex_metric=fmt_rate_rows(n_orders, vex_join_ms),
                    surreal_metric=fmt_rate_rows(n_orders, surreal_join_ms),
                    speedup=surreal_join_ms / vex_join_ms,
                )
            )

            # ------------------------------------------------------------------
            # 10. QUERY -> NUMPY FLOAT64 VECTOR SUM (PEP 3118 zero-copy vs Python marshalling)
            # ------------------------------------------------------------------
            if np is not None:
                def vex_np_sum() -> float:
                    arr = vdb.q("orders amount")["amount"].np()
                    assert arr.flags.writeable is False
                    return float(arr.sum())

                def surreal_np_sum() -> float:
                    vals = sdb.query("SELECT VALUE amount FROM orders").execute()[0]
                    arr = np.asarray(vals, dtype=np.float64)
                    return float(arr.sum())

                vex_np_ms, v_nsum = time_fn(vex_np_sum, iters=args.iters)
                surreal_np_ms, s_nsum = time_fn(surreal_np_sum, iters=args.iters)
                assert math.isclose(v_nsum, data["expect_total_amt"], rel_tol=1e-9)
                assert math.isclose(s_nsum, data["expect_total_amt"], rel_tol=1e-9)

                results.append(
                    BenchRow(
                        benchmark=f"10. Query -> NumPy + sum ({n_orders:,} f64)",
                        vex_ms=vex_np_ms,
                        surreal_ms=surreal_np_ms,
                        vex_metric=fmt_rate_rows(n_orders, vex_np_ms),
                        surreal_metric=fmt_rate_rows(n_orders, surreal_np_ms),
                        speedup=surreal_np_ms / vex_np_ms,
                    )
                )

            # ------------------------------------------------------------------
            # 11. PRIMARY-KEY POINT UPDATES (Run last to preserve read invariants)
            # ------------------------------------------------------------------
            up_keys = data["update_keys"]
            up_rids = data["surreal_update_rids"]
            up_vals = data["update_vals"]

            def vex_pk_update() -> None:
                for k, v in zip(up_keys, up_vals):
                    vdb.q("orders[$k] = { amount: $v }", k=k, v=v)

            def surreal_pk_update() -> None:
                for rid, v in zip(up_rids, up_vals):
                    sdb.query("UPDATE $id SET amount = $v RETURN NONE", {"id": rid, "v": v}).execute()

            vex_up_ms, _ = time_fn(vex_pk_update, iters=args.iters)
            surreal_up_ms, _ = time_fn(surreal_pk_update, iters=args.iters)

            k0, rid0, v0 = up_keys[0], up_rids[0], up_vals[0]
            assert math.isclose(vdb.q("orders[$k] amount", k=k0)["amount"][0], v0, rel_tol=1e-9)
            assert math.isclose(
                sdb.query("SELECT VALUE amount FROM ONLY $id", {"id": rid0}).execute()[0],
                v0,
                rel_tol=1e-9,
            )

            results.append(
                BenchRow(
                    benchmark=f"11. PK Point Updates ({n_updates:,} ops)",
                    vex_ms=vex_up_ms,
                    surreal_ms=surreal_up_ms,
                    vex_metric=fmt_rate_ops(n_updates, vex_up_ms),
                    surreal_metric=fmt_rate_ops(n_updates, surreal_up_ms),
                    speedup=surreal_up_ms / vex_up_ms,
                )
            )
        finally:
            vdb.close()
            sdb.close()

    return surreal_ver, results


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Small-scale benchmark: Vex vs SurrealDB (Embedded).")
    p.add_argument("--rows", type=int, default=10_000, help="Order rows (default: 10000)")
    p.add_argument("--users", type=int, default=None, help="User rows (default: rows // 5)")
    p.add_argument("--pk-lookups", type=int, default=500, help="Random PK lookups (default: 500)")
    p.add_argument("--pk-updates", type=int, default=250, help="Random PK updates (default: 250)")
    p.add_argument("--iters", type=int, default=5, help="Timed iterations per query (default: 5)")
    p.add_argument("--workers", type=int, default=min(4, os.cpu_count() or 1), help="Vex worker threads")
    p.add_argument("--chunk-rows", type=int, default=4096, help="Vex rows per chunk (default: 4096)")
    p.add_argument("--surreal-batch-size", type=int, default=2500, help="Rows per SurrealDB INSERT batch")
    p.add_argument(
        "--mode",
        choices=("memory", "disk", "both"),
        default="memory",
        help="Storage mode: memory (RAM/mem://), disk (WAL+fsync/surrealkv://), or both",
    )
    p.add_argument("--surreal-url", type=str, default=None, help="Optional explicit SurrealDB URL (e.g. ws://localhost:8000/rpc)")
    p.add_argument("--seed", type=int, default=42, help="Deterministic RNG seed")
    p.add_argument("--json", type=Path, default=None, help="Optional JSON output file path")
    return p.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.rows > 100_000:
        raise SystemExit(
            f"Refusing --rows={args.rows:,}: docs/testing_policy.md caps small-scale benchmark datasets at <= 100,000 rows (<= 50 MB)."
        )

    modes = ("memory", "disk") if args.mode == "both" else (args.mode,)
    all_payloads: dict[str, Any] = {}
    surreal_ver = "unknown"

    for idx, mode in enumerate(modes):
        if idx > 0:
            print()
        surreal_ver, rows = run_suite(args, mode)
        engine_desc = "RAM vs mem://" if mode == "memory" else "WAL(fsync=True) vs surrealkv://"
        print(
            f"Vex vs SurrealDB ({surreal_ver}) | mode={mode} ({engine_desc}) | "
            f"orders={args.rows:,} | workers={args.workers} | iters={args.iters}"
        )
        print(
            f"Env: Python {platform.python_version()} | {platform.machine()} ({os.cpu_count()} CPUs) | "
            f"Vex SIMD={vex.simd_backend()}\n"
        )
        print(format_markdown_table(rows))
        all_payloads[mode] = [asdict(r) for r in rows]

    if args.json is not None:
        payload = {
            "meta": {
                "rows": args.rows,
                "users": max(100, args.users if args.users is not None else args.rows // 5),
                "pk_lookups": args.pk_lookups,
                "pk_updates": args.pk_updates,
                "workers": args.workers,
                "iters": args.iters,
                "simd_backend": vex.simd_backend(),
                "python": platform.python_version(),
                "surrealdb_embedded": surreal_ver,
            },
            "results": all_payloads,
        }
        args.json.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        print(f"\nWrote JSON results to {args.json}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

