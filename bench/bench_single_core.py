#!/usr/bin/env python3
"""Comprehensive single-core benchmark suite: Vex v1.0 vs SQLite3 vs DuckDB.

Evaluates single-threaded performance across:
  1. Bulk row ingestion (Vex SPSC put+flush vs SQLite executemany vs DuckDB Appender)
  2. O(1) primary key point lookups (Robin Hood hash index vs B-Tree vs DuckDB)
  3. O(1) primary key point updates (CoW tombstoning vs B-Tree update)
  4. High-selectivity branchless filter scan (Raw columnar scan + AVX2 predicate)
  5. Compound range filter (Multi-condition predicate evaluation)
  6. Full-table multi-aggregation (SIMD vector reduction: count, sum, min, max)
  7. Filtered group-by aggregation (Morsel SIMD fast-path on low cardinality)
  8. High-cardinality group-by aggregation (Robin Hood hash grouping across 40k groups)
  9. Top-K sort with limit (Quicksort + slice vs ORDER BY ... LIMIT)
 10. Inner hash join + group aggregation (Morsel hash join on single thread)
 11. End-to-end query -> NumPy array sum (PEP 3118 zero-copy export)
 12. Zero-copy NumPy view handoff (Sub-microsecond direct buffer mapping)
"""

from __future__ import annotations

import argparse
import gc
import json
import math
import os
import platform
import random
import sqlite3
import statistics
import sys
import tempfile
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Callable

ROOT = Path(__file__).resolve().parent.parent
PY_DIR = ROOT / "python"
if str(PY_DIR) not in sys.path:
    sys.path.insert(0, str(PY_DIR))

import vex

try:
    import numpy as np
except ImportError:
    np = None  # type: ignore[assignment]

try:
    import duckdb
except ImportError:
    duckdb = None  # type: ignore[assignment]

COUNTRIES = ("IN", "US", "JP", "DE", "UK", "SG", "BR", "CA")


@dataclass
class BenchRow:
    benchmark: str
    vex_ms: float
    sqlite_ms: float
    duckdb_ms: float | None
    vex_metric: str
    vs_sqlite: float
    vs_duckdb: float | None


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
    """Pre-generate deterministic dicts & tuples so RNG/object creation is never timed."""
    rng = random.Random(seed)

    user_tuples: list[tuple[int, str, int]] = []
    user_dicts: list[dict[str, Any]] = []
    user_country: list[str] = [""] * (n_users + 1)

    for uid in range(1, n_users + 1):
        c = COUNTRIES[uid % len(COUNTRIES)]
        tier = uid % 5
        user_country[uid] = c
        user_tuples.append((uid, c, tier))
        user_dicts.append({"id": uid, "country": c, "tier": tier})

    order_tuples_sqlite: list[tuple[int, int, int, float, int]] = []
    order_tuples_duck: list[tuple[int, int, int, float, bool]] = []
    order_dicts: list[dict[str, Any]] = []

    expect_agg = {b: [0, 0.0] for b in range(8)}
    expect_join = {c: [0, 0.0] for c in COUNTRIES}
    expect_total_amt = 0.0
    expect_min_amt = float("inf")
    expect_max_amt = float("-inf")
    amount_by_id: dict[int, float] = {}

    for oid in range(1, n_orders + 1):
        uid = ((oid * 2654435761) % n_users) + 1
        bucket = oid & 7
        amount = float((oid * 37) % 1000) + 0.5
        paid = (oid % 5) != 0  # 80% paid

        order_tuples_sqlite.append((oid, uid, bucket, amount, 1 if paid else 0))
        order_tuples_duck.append((oid, uid, bucket, amount, paid))
        order_dicts.append(
            {
                "id": oid,
                "user_id": uid,
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

        if amount > 500.0:
            expect_agg[bucket][0] += 1
            expect_agg[bucket][1] += amount
        if paid:
            c = user_country[uid]
            expect_join[c][0] += 1
            expect_join[c][1] += amount

    lookup_keys = [rng.randint(1, n_orders) for _ in range(n_lookups)]
    for k in lookup_keys:
        amount_by_id[k] = float((k * 37) % 1000) + 0.5
    expect_pk_sum = sum(amount_by_id[k] for k in lookup_keys)

    update_keys = [rng.randint(1, n_orders) for _ in range(n_updates)]
    update_vals = [float((k * 13) % 500) + 7000.5 for k in update_keys]

    return {
        "user_tuples": user_tuples,
        "user_dicts": user_dicts,
        "order_tuples_sqlite": order_tuples_sqlite,
        "order_tuples_duck": order_tuples_duck,
        "order_dicts": order_dicts,
        "lookup_keys": lookup_keys,
        "update_keys": update_keys,
        "update_vals": update_vals,
        "expect_agg": expect_agg,
        "expect_join": expect_join,
        "expect_total_amt": expect_total_amt,
        "expect_min_amt": expect_min_amt,
        "expect_max_amt": expect_max_amt,
        "expect_pk_sum": expect_pk_sum,
    }


def open_sqlite(path: str | None) -> sqlite3.Connection:
    conn = sqlite3.connect(":memory:" if path is None else path)
    conn.execute("PRAGMA temp_store = MEMORY")
    conn.execute("PRAGMA cache_size = -65536")
    if path is not None:
        conn.execute("PRAGMA journal_mode = WAL")
        conn.execute("PRAGMA synchronous = NORMAL")
    else:
        conn.execute("PRAGMA synchronous = OFF")
    conn.execute(
        "CREATE TABLE users (id INTEGER PRIMARY KEY, country TEXT NOT NULL, tier INTEGER NOT NULL)"
    )
    conn.execute(
        "CREATE TABLE orders ("
        "  id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL, bucket INTEGER NOT NULL,"
        "  amount REAL NOT NULL, paid INTEGER NOT NULL"
        ")"
    )
    return conn


def open_duckdb(path: str | None) -> Any:
    assert duckdb is not None
    con = duckdb.connect(
        database=":memory:" if path is None else path,
        config={"threads": 1},
    )
    con.execute(
        "CREATE TABLE users (id UBIGINT PRIMARY KEY, country VARCHAR NOT NULL, tier BIGINT NOT NULL)"
    )
    con.execute(
        "CREATE TABLE orders ("
        "  id UBIGINT PRIMARY KEY, user_id UBIGINT NOT NULL, bucket BIGINT NOT NULL,"
        "  amount DOUBLE NOT NULL, paid BOOLEAN NOT NULL"
        ")"
    )
    return con


def open_vex(path: str | None, chunk_rows: int) -> vex.Database:
    db = vex.open(path, workers=1, chunk_rows=chunk_rows, fsync=(path is not None))
    db.q("table users { id: u64 key, country: str, tier: i64 }", sync=True)
    db.q(
        "table orders { id: u64 key, user_id: u64 -> users.id, bucket: i64, amount: f64, paid: bool }",
        sync=True,
    )
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


def format_markdown_table(rows: list[BenchRow], has_duckdb: bool) -> str:
    headers = ["Single-Core Benchmark", "Vex (ms)", "SQLite3 (ms)"]
    if has_duckdb:
        headers.append("DuckDB (ms)")
    headers.extend(["Vex Metric", "vs SQLite3"])
    if has_duckdb:
        headers.append("vs DuckDB")

    body: list[list[str]] = []
    for r in rows:
        line = [r.benchmark, f"{r.vex_ms:.2f}", f"{r.sqlite_ms:.2f}"]
        if has_duckdb:
            line.append(f"{r.duckdb_ms:.2f}" if r.duckdb_ms is not None else "n/a")
        line.extend([r.vex_metric, f"{r.vs_sqlite:.2f}x"])
        if has_duckdb:
            line.append(f"{r.vs_duckdb:.2f}x" if r.vs_duckdb is not None else "n/a")
        body.append(line)

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


def run_suite(args: argparse.Namespace) -> list[BenchRow]:
    n_orders = args.rows
    n_users = max(100, args.users if args.users is not None else n_orders // 5)
    n_lookups = args.pk_lookups
    n_updates = args.pk_updates

    data = build_datasets(n_orders, n_users, n_lookups, n_updates, args.seed)
    total_inserted = n_orders + n_users
    results: list[BenchRow] = []

    with tempfile.TemporaryDirectory(prefix="vex_sc_bench_") as tmpdir:
        vex_path = None if args.mode == "memory" else str(Path(tmpdir) / "bench.wal")
        sql_path = None if args.mode == "memory" else str(Path(tmpdir) / "bench.sqlite")
        duck_path = None if args.mode == "memory" else str(Path(tmpdir) / "bench.duckdb")

        vdb = open_vex(vex_path, args.chunk_rows)
        sdb = open_sqlite(sql_path)
        ddb = open_duckdb(duck_path) if duckdb is not None else None

        try:
            # ------------------------------------------------------------------
            # 1. BULK INSERTS (users + orders)
            # ------------------------------------------------------------------
            gc.collect()
            t0 = time.perf_counter()
            for u in data["user_dicts"]:
                vdb.put("users", u, sync=False)
            for o in data["order_dicts"]:
                vdb.put("orders", o, sync=False)
            vdb.flush()
            vex_ins_ms = (time.perf_counter() - t0) * 1000.0

            gc.collect()
            t0 = time.perf_counter()
            with sdb:
                sdb.executemany("INSERT INTO users VALUES (?, ?, ?)", data["user_tuples"])
                sdb.executemany("INSERT INTO orders VALUES (?, ?, ?, ?, ?)", data["order_tuples_sqlite"])
            sql_ins_ms = (time.perf_counter() - t0) * 1000.0

            duck_ins_ms: float | None = None
            if ddb is not None:
                gc.collect()
                t0 = time.perf_counter()
                if hasattr(ddb, "appender"):
                    with ddb.appender("users") as app:
                        app.append_rows(data["user_tuples"])
                    with ddb.appender("orders") as app:
                        app.append_rows(data["order_tuples_duck"])
                else:
                    ddb.executemany("INSERT INTO users VALUES (?, ?, ?)", data["user_tuples"])
                    ddb.executemany("INSERT INTO orders VALUES (?, ?, ?, ?, ?)", data["order_tuples_duck"])
                duck_ins_ms = (time.perf_counter() - t0) * 1000.0

            assert vdb.q("orders { n: count() }")["n"][0] == n_orders
            assert sdb.execute("SELECT COUNT(*) FROM orders").fetchone()[0] == n_orders
            if ddb is not None:
                assert ddb.execute("SELECT COUNT(*) FROM orders").fetchone()[0] == n_orders

            ins_kops = (total_inserted / (vex_ins_ms / 1000.0)) / 1e3
            results.append(
                BenchRow(
                    benchmark=f"1. Bulk Insert ({total_inserted:,} rows)",
                    vex_ms=vex_ins_ms,
                    sqlite_ms=sql_ins_ms,
                    duckdb_ms=duck_ins_ms,
                    vex_metric=f"{ins_kops:,.0f}k rows/s",
                    vs_sqlite=sql_ins_ms / vex_ins_ms,
                    vs_duckdb=(duck_ins_ms / vex_ins_ms) if duck_ins_ms else None,
                )
            )

            # ------------------------------------------------------------------
            # 2. O(1) PK POINT LOOKUPS
            # ------------------------------------------------------------------
            lookup_keys = data["lookup_keys"]

            def vex_pk() -> float:
                acc = 0.0
                for k in lookup_keys:
                    acc += vdb.q("orders[$k] amount", k=k)["amount"][0]
                return acc

            def sql_pk() -> float:
                cur = sdb.cursor()
                acc = 0.0
                for k in lookup_keys:
                    acc += cur.execute("SELECT amount FROM orders WHERE id = ?", (k,)).fetchone()[0]
                return acc

            vex_pk_ms, v_sum = time_fn(vex_pk, iters=args.iters)
            sql_pk_ms, s_sum = time_fn(sql_pk, iters=args.iters)
            assert math.isclose(v_sum, data["expect_pk_sum"], rel_tol=1e-9)
            assert math.isclose(s_sum, data["expect_pk_sum"], rel_tol=1e-9)

            duck_pk_ms: float | None = None
            if ddb is not None:
                def duck_pk() -> float:
                    acc = 0.0
                    for k in lookup_keys:
                        acc += ddb.execute("SELECT amount FROM orders WHERE id = ?", [k]).fetchone()[0]
                    return acc

                duck_pk_ms, d_sum = time_fn(duck_pk, iters=max(1, min(3, args.iters)))
                assert math.isclose(d_sum, data["expect_pk_sum"], rel_tol=1e-9)

            pk_kops = (n_lookups / (vex_pk_ms / 1000.0)) / 1e3
            results.append(
                BenchRow(
                    benchmark=f"2. O(1) PK Lookups ({n_lookups:,} ops)",
                    vex_ms=vex_pk_ms,
                    sqlite_ms=sql_pk_ms,
                    duckdb_ms=duck_pk_ms,
                    vex_metric=f"{pk_kops:,.1f}k ops/s",
                    vs_sqlite=sql_pk_ms / vex_pk_ms,
                    vs_duckdb=(duck_pk_ms / vex_pk_ms) if duck_pk_ms else None,
                )
            )

            # ------------------------------------------------------------------
            # 3. HIGH-SELECTIVITY BRANCHLESS FILTER SCAN
            # ------------------------------------------------------------------
            vex_scan_q = "orders[amount > 900.0] amount"
            sql_scan_q = "SELECT amount FROM orders WHERE amount > 900.0"

            vex_scan_ms, v_cnt = time_fn(lambda: vdb.q(vex_scan_q).nrows, iters=args.iters)
            sql_scan_ms, s_cnt = time_fn(lambda: len(sdb.execute(sql_scan_q).fetchall()), iters=args.iters)
            assert v_cnt == s_cnt

            duck_scan_ms: float | None = None
            if ddb is not None:
                duck_scan_ms, d_cnt = time_fn(lambda: len(ddb.execute(sql_scan_q).fetchall()), iters=args.iters)
                assert v_cnt == d_cnt

            scan_mrows = (n_orders / (vex_scan_ms / 1000.0)) / 1e6
            results.append(
                BenchRow(
                    benchmark=f"3. Branchless Filter Scan (>900.0)",
                    vex_ms=vex_scan_ms,
                    sqlite_ms=sql_scan_ms,
                    duckdb_ms=duck_scan_ms,
                    vex_metric=f"{scan_mrows:.1f} M rows/s",
                    vs_sqlite=sql_scan_ms / vex_scan_ms,
                    vs_duckdb=(duck_scan_ms / vex_scan_ms) if duck_scan_ms else None,
                )
            )

            # ------------------------------------------------------------------
            # 4. COMPOUND RANGE FILTER (Conjunction && / and)
            # ------------------------------------------------------------------
            vex_range_q = "orders[bucket == 3 and amount >= 200.0 and amount <= 600.0] amount"
            sql_range_q = "SELECT amount FROM orders WHERE bucket = 3 AND amount >= 200.0 AND amount <= 600.0"

            vex_rng_ms, v_rcnt = time_fn(lambda: vdb.q(vex_range_q).nrows, iters=args.iters)
            sql_rng_ms, s_rcnt = time_fn(lambda: len(sdb.execute(sql_range_q).fetchall()), iters=args.iters)
            assert v_rcnt == s_rcnt

            duck_rng_ms: float | None = None
            if ddb is not None:
                duck_rng_ms, d_rcnt = time_fn(lambda: len(ddb.execute(sql_range_q).fetchall()), iters=args.iters)
                assert v_rcnt == d_rcnt

            rng_mrows = (n_orders / (vex_rng_ms / 1000.0)) / 1e6
            results.append(
                BenchRow(
                    benchmark=f"4. Compound Range Filter (3 preds)",
                    vex_ms=vex_rng_ms,
                    sqlite_ms=sql_rng_ms,
                    duckdb_ms=duck_rng_ms,
                    vex_metric=f"{rng_mrows:.1f} M rows/s",
                    vs_sqlite=sql_rng_ms / vex_rng_ms,
                    vs_duckdb=(duck_rng_ms / vex_rng_ms) if duck_rng_ms else None,
                )
            )

            # ------------------------------------------------------------------
            # 5. FULL-TABLE MULTI-AGGREGATION (SIMD Reduction)
            # ------------------------------------------------------------------
            vex_magg_q = "orders { n: count(), s: sum(amount), mi: min(amount), ma: max(amount) }"
            sql_magg_q = "SELECT COUNT(*), SUM(amount), MIN(amount), MAX(amount) FROM orders"

            vex_magg_ms, v_magg = time_fn(lambda: vdb.q(vex_magg_q).rows()[0], iters=args.iters)
            sql_magg_ms, s_magg = time_fn(lambda: sdb.execute(sql_magg_q).fetchone(), iters=args.iters)
            assert v_magg[0] == s_magg[0] == n_orders
            assert math.isclose(v_magg[1], s_magg[1], rel_tol=1e-5)
            assert math.isclose(v_magg[2], s_magg[2], rel_tol=1e-5)
            assert math.isclose(v_magg[3], s_magg[3], rel_tol=1e-5)

            duck_magg_ms: float | None = None
            if ddb is not None:
                duck_magg_ms, d_magg = time_fn(lambda: ddb.execute(sql_magg_q).fetchone(), iters=args.iters)
                assert v_magg[0] == d_magg[0]

            magg_mrows = (n_orders / (vex_magg_ms / 1000.0)) / 1e6
            results.append(
                BenchRow(
                    benchmark=f"5. Multi-Agg (count, sum, min, max)",
                    vex_ms=vex_magg_ms,
                    sqlite_ms=sql_magg_ms,
                    duckdb_ms=duck_magg_ms,
                    vex_metric=f"{magg_mrows:.1f} M rows/s",
                    vs_sqlite=sql_magg_ms / vex_magg_ms,
                    vs_duckdb=(duck_magg_ms / vex_magg_ms) if duck_magg_ms else None,
                )
            )

            # ------------------------------------------------------------------
            # 6. FILTERED GROUP-BY AGGREGATION (Low Cardinality: 8 Buckets)
            # ------------------------------------------------------------------
            vex_gagg_q = "orders[amount > 500.0] by bucket { n: count(), s: sum(amount) } sort bucket"
            sql_gagg_q = (
                "SELECT bucket, COUNT(*), SUM(amount) FROM orders "
                "WHERE amount > 500.0 GROUP BY bucket ORDER BY bucket"
            )

            vex_gagg_ms, v_gagg = time_fn(lambda: vdb.q(vex_gagg_q).rows(), iters=args.iters)
            sql_gagg_ms, s_gagg = time_fn(lambda: sdb.execute(sql_gagg_q).fetchall(), iters=args.iters)
            verify_agg(v_gagg, data["expect_agg"], "Vex low-card agg")
            verify_agg(s_gagg, data["expect_agg"], "SQLite low-card agg")

            duck_gagg_ms: float | None = None
            if ddb is not None:
                duck_gagg_ms, d_gagg = time_fn(lambda: ddb.execute(sql_gagg_q).fetchall(), iters=args.iters)
                verify_agg(d_gagg, data["expect_agg"], "DuckDB low-card agg")

            gagg_mrows = (n_orders / (vex_gagg_ms / 1000.0)) / 1e6
            results.append(
                BenchRow(
                    benchmark=f"6. Filtered Group-By (8 buckets)",
                    vex_ms=vex_gagg_ms,
                    sqlite_ms=sql_gagg_ms,
                    duckdb_ms=duck_gagg_ms,
                    vex_metric=f"{gagg_mrows:.1f} M rows/s",
                    vs_sqlite=sql_gagg_ms / vex_gagg_ms,
                    vs_duckdb=(duck_gagg_ms / vex_gagg_ms) if duck_gagg_ms else None,
                )
            )

            # ------------------------------------------------------------------
            # 7. HIGH-CARDINALITY GROUP-BY (40,000 User Groups)
            # ------------------------------------------------------------------
            vex_hcard_q = "orders by user_id { n: count(), s: sum(amount) }"
            sql_hcard_q = "SELECT user_id, COUNT(*), SUM(amount) FROM orders GROUP BY user_id"

            vex_hc_ms, v_hres = time_fn(lambda: vdb.q(vex_hcard_q).nrows, iters=args.iters)
            sql_hc_ms, s_hres = time_fn(lambda: len(sdb.execute(sql_hcard_q).fetchall()), iters=args.iters)
            assert v_hres == s_hres == n_users

            duck_hc_ms: float | None = None
            if ddb is not None:
                duck_hc_ms, d_hres = time_fn(lambda: len(ddb.execute(sql_hcard_q).fetchall()), iters=args.iters)
                assert v_hres == d_hres

            hc_mrows = (n_orders / (vex_hc_ms / 1000.0)) / 1e6
            results.append(
                BenchRow(
                    benchmark=f"7. High-Card Group-By ({n_users:,} groups)",
                    vex_ms=vex_hc_ms,
                    sqlite_ms=sql_hc_ms,
                    duckdb_ms=duck_hc_ms,
                    vex_metric=f"{hc_mrows:.1f} M rows/s",
                    vs_sqlite=sql_hc_ms / vex_hc_ms,
                    vs_duckdb=(duck_hc_ms / vex_hc_ms) if duck_hc_ms else None,
                )
            )

            # ------------------------------------------------------------------
            # 8. TOP-K SORT WITH LIMIT (Filtered Top-10 Slice)
            # ------------------------------------------------------------------
            vex_topk_q = "orders[paid] amount sort -amount [:10]"
            sql_topk_q = "SELECT amount FROM orders WHERE paid = 1 ORDER BY amount DESC LIMIT 10"
            duck_topk_q = "SELECT amount FROM orders WHERE paid ORDER BY amount DESC LIMIT 10"

            vex_topk_ms, v_topk = time_fn(lambda: [r[0] for r in vdb.q(vex_topk_q).rows()], iters=args.iters)
            sql_topk_ms, s_topk = time_fn(lambda: [r[0] for r in sdb.execute(sql_topk_q).fetchall()], iters=args.iters)
            assert len(v_topk) == len(s_topk) == 10
            for i in range(10):
                assert math.isclose(v_topk[i], s_topk[i], rel_tol=1e-9)

            duck_topk_ms: float | None = None
            if ddb is not None:
                duck_topk_ms, d_topk = time_fn(lambda: [r[0] for r in ddb.execute(duck_topk_q).fetchall()], iters=args.iters)
                assert len(d_topk) == 10
                for i in range(10):
                    assert math.isclose(v_topk[i], d_topk[i], rel_tol=1e-9)

            topk_mrows = (n_orders / (vex_topk_ms / 1000.0)) / 1e6
            results.append(
                BenchRow(
                    benchmark=f"8. Top-K Filter + Sort ([:10])",
                    vex_ms=vex_topk_ms,
                    sqlite_ms=sql_topk_ms,
                    duckdb_ms=duck_topk_ms,
                    vex_metric=f"{topk_mrows:.1f} M rows/s",
                    vs_sqlite=sql_topk_ms / vex_topk_ms,
                    vs_duckdb=(duck_topk_ms / vex_topk_ms) if duck_topk_ms else None,
                )
            )

            # ------------------------------------------------------------------
            # 9. INNER HASH JOIN + GROUP AGGREGATION
            # ------------------------------------------------------------------
            vex_join_q = (
                "orders[paid] & users by users.country { n: count(), rev: sum(amount) } sort -rev"
            )
            sql_join_q = (
                "SELECT u.country, COUNT(*), SUM(o.amount) "
                "FROM orders o INNER JOIN users u ON o.user_id = u.id "
                "WHERE o.paid = 1 GROUP BY u.country ORDER BY SUM(o.amount) DESC"
            )
            duck_join_q = (
                "SELECT u.country, COUNT(*), SUM(o.amount) "
                "FROM orders o INNER JOIN users u ON o.user_id = u.id "
                "WHERE o.paid GROUP BY u.country ORDER BY SUM(o.amount) DESC"
            )

            vex_join_ms, v_join = time_fn(lambda: vdb.q(vex_join_q).rows(), iters=args.iters)
            sql_join_ms, s_join = time_fn(lambda: sdb.execute(sql_join_q).fetchall(), iters=args.iters)
            verify_join(v_join, data["expect_join"], "Vex single-core join")
            verify_join(s_join, data["expect_join"], "SQLite single-core join")

            duck_join_ms: float | None = None
            if ddb is not None:
                duck_join_ms, d_join = time_fn(lambda: ddb.execute(duck_join_q).fetchall(), iters=args.iters)
                verify_join(d_join, data["expect_join"], "DuckDB single-core join")

            join_mrows = (n_orders / (vex_join_ms / 1000.0)) / 1e6
            results.append(
                BenchRow(
                    benchmark=f"9. Hash Join + Agg ({n_orders:,} x {n_users:,})",
                    vex_ms=vex_join_ms,
                    sqlite_ms=sql_join_ms,
                    duckdb_ms=duck_join_ms,
                    vex_metric=f"{join_mrows:.1f} M rows/s",
                    vs_sqlite=sql_join_ms / vex_join_ms,
                    vs_duckdb=(duck_join_ms / vex_join_ms) if duck_join_ms else None,
                )
            )

            # ------------------------------------------------------------------
            # 10 & 11. ZERO-COPY NUMPY EXPORT & VIEW HANDOFF
            # ------------------------------------------------------------------
            if np is not None:
                def vex_np_e2e() -> float:
                    res = vdb.q("orders amount")
                    arr = res["amount"].np()
                    return float(arr.sum())

                def sql_np_e2e() -> float:
                    arr = np.fromiter(
                        (r[0] for r in sdb.execute("SELECT amount FROM orders")),
                        dtype=np.float64,
                        count=n_orders,
                    )
                    return float(arr.sum())

                vex_np_ms, v_nsum = time_fn(vex_np_e2e, iters=args.iters)
                sql_np_ms, s_nsum = time_fn(sql_np_e2e, iters=args.iters)
                assert math.isclose(v_nsum, s_nsum, rel_tol=1e-5)

                duck_np_ms: float | None = None
                if ddb is not None:
                    def duck_np_e2e() -> float:
                        arr = ddb.execute("SELECT amount FROM orders").fetchnumpy()["amount"]
                        return float(arr.sum())

                    duck_np_ms, d_nsum = time_fn(duck_np_e2e, iters=args.iters)
                    assert math.isclose(v_nsum, d_nsum, rel_tol=1e-5)

                np_mrows = (n_orders / (vex_np_ms / 1000.0)) / 1e6
                results.append(
                    BenchRow(
                        benchmark=f"10. Query -> NumPy + sum ({n_orders:,} f64)",
                        vex_ms=vex_np_ms,
                        sqlite_ms=sql_np_ms,
                        duckdb_ms=duck_np_ms,
                        vex_metric=f"{np_mrows:.1f} M rows/s",
                        vs_sqlite=sql_np_ms / vex_np_ms,
                        vs_duckdb=(duck_np_ms / vex_np_ms) if duck_np_ms else None,
                    )
                )

                # Pure zero-copy view handoff
                pinned_res = vdb.q("orders amount")
                col_view = pinned_res["amount"]
                sql_rows = sdb.execute("SELECT amount FROM orders").fetchall()

                def vex_view_only() -> Any:
                    return col_view.np()

                def sql_copy_only() -> Any:
                    return np.fromiter((r[0] for r in sql_rows), dtype=np.float64, count=n_orders)

                vex_v_ms, a1 = time_fn(vex_view_only, iters=max(args.iters, 20))
                a2 = col_view.np()
                assert a1.__array_interface__["data"][0] == a2.__array_interface__["data"][0]
                sql_v_ms, _ = time_fn(sql_copy_only, iters=args.iters)

                results.append(
                    BenchRow(
                        benchmark=f"11. Result -> NumPy Handoff (0-copy)",
                        vex_ms=vex_v_ms,
                        sqlite_ms=sql_v_ms,
                        duckdb_ms=None,
                        vex_metric=f"{vex_v_ms * 1000.0:.1f} us (0 B copied)",
                        vs_sqlite=sql_v_ms / vex_v_ms,
                        vs_duckdb=None,
                    )
                )

            # ------------------------------------------------------------------
            # 12. O(1) PK POINT UPDATES (Run last to preserve read invariants)
            # ------------------------------------------------------------------
            up_keys = data["update_keys"]
            up_vals = data["update_vals"]

            def vex_pk_update() -> None:
                for k, v in zip(up_keys, up_vals):
                    vdb.q("orders[$k] = { amount: $v }", k=k, v=v)

            def sql_pk_update() -> None:
                with sdb:
                    for k, v in zip(up_keys, up_vals):
                        sdb.execute("UPDATE orders SET amount = ? WHERE id = ?", (v, k))

            vex_up_ms, _ = time_fn(vex_pk_update, iters=args.iters)
            sql_up_ms, _ = time_fn(sql_pk_update, iters=args.iters)

            duck_up_ms: float | None = None
            if ddb is not None:
                def duck_pk_update() -> None:
                    for k, v in zip(up_keys, up_vals):
                        ddb.execute("UPDATE orders SET amount = ? WHERE id = ?", [v, k])

                duck_up_ms, _ = time_fn(duck_pk_update, iters=max(1, min(3, args.iters)))

            k0, v0 = up_keys[0], up_vals[0]
            assert math.isclose(vdb.q("orders[$k] amount", k=k0)["amount"][0], v0, rel_tol=1e-9)

            up_kops = (n_updates / (vex_up_ms / 1000.0)) / 1e3
            results.append(
                BenchRow(
                    benchmark=f"12. O(1) PK Updates ({n_updates:,} ops)",
                    vex_ms=vex_up_ms,
                    sqlite_ms=sql_up_ms,
                    duckdb_ms=duck_up_ms,
                    vex_metric=f"{up_kops:,.1f}k ops/s",
                    vs_sqlite=sql_up_ms / vex_up_ms,
                    vs_duckdb=(duck_up_ms / vex_up_ms) if duck_up_ms else None,
                )
            )
        finally:
            vdb.close()
            sdb.close()
            if ddb is not None:
                ddb.close()

    return results


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Single-Core Benchmark: Vex v1.0 vs SQLite3 vs DuckDB.")
    p.add_argument("--rows", type=int, default=200_000, help="Number of order rows (default: 200000)")
    p.add_argument("--users", type=int, default=None, help="Number of user rows (default: rows // 5)")
    p.add_argument("--pk-lookups", type=int, default=5_000, help="Random PK lookups (default: 5000)")
    p.add_argument("--pk-updates", type=int, default=1_000, help="Random PK updates (default: 1000)")
    p.add_argument("--iters", type=int, default=7, help="Timed iterations per query (default: 7)")
    p.add_argument("--chunk-rows", type=int, default=4096, help="Vex rows per chunk (default: 4096)")
    p.add_argument("--mode", choices=("memory", "disk"), default="memory", help="Storage mode")
    p.add_argument("--seed", type=int, default=42, help="Deterministic RNG seed")
    p.add_argument("--json", type=Path, default=None, help="Optional JSON output file path")
    return p.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    has_duck = duckdb is not None
    duck_ver = getattr(duckdb, "__version__", "not installed") if has_duck else "not installed"

    print(
        f"Vex v1.0 Single-Core Benchmark | mode={args.mode} | rows={args.rows:,} | "
        f"workers=1 | chunk_rows={args.chunk_rows} | iters={args.iters}"
    )
    print(
        f"Env: Python {platform.python_version()} | {platform.machine()} ({os.cpu_count()} CPUs) | "
        f"Vex SIMD={vex.simd_backend()} | SQLite={sqlite3.sqlite_version} | DuckDB={duck_ver}\n"
    )

    if np is None:
        print("[note] NumPy not installed — skipping benchmarks #11 and #12.\n")

    rows = run_suite(args)
    print(format_markdown_table(rows, has_duckdb=has_duck))

    if args.json is not None:
        payload = {
            "meta": {
                "mode": args.mode,
                "rows": args.rows,
                "pk_lookups": args.pk_lookups,
                "pk_updates": args.pk_updates,
                "workers": 1,
                "simd_backend": vex.simd_backend(),
                "python": platform.python_version(),
                "sqlite": sqlite3.sqlite_version,
                "duckdb": duck_ver,
            },
            "results": [asdict(r) for r in rows],
        }
        args.json.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        print(f"\nWrote JSON results to {args.json}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
