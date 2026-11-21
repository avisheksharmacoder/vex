#!/usr/bin/env python3
"""Reproducible head-to-head benchmark: Vex v1.0 vs SQLite3 (and DuckDB if installed).

Covers:
  1. Bulk row ingestion (Vex SPSC put+flush vs SQLite executemany vs DuckDB Appender/executemany)
  2. O(1) primary-key point lookups
  3. Filtered group aggregation (Vex morsel SIMD fast path)
  4. Inner hash join + group aggregation
  5. End-to-end query -> NumPy array export (PEP 3118 zero-copy vs materialization)
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

# Allow running directly from repo root without installing wheel
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

    return {
        "user_tuples": user_tuples,
        "user_dicts": user_dicts,
        "order_tuples_sqlite": order_tuples_sqlite,
        "order_tuples_duck": order_tuples_duck,
        "order_dicts": order_dicts,
        "lookup_keys": lookup_keys,
        "expect_agg": expect_agg,
        "expect_join": expect_join,
        "expect_total_amt": expect_total_amt,
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


def open_duckdb(path: str | None, workers: int) -> Any:
    assert duckdb is not None
    con = duckdb.connect(
        database=":memory:" if path is None else path,
        config={"threads": workers},
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


def open_vex(path: str | None, workers: int, chunk_rows: int) -> vex.Database:
    db = vex.open(path, workers=workers, chunk_rows=chunk_rows, fsync=(path is not None))
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
    headers = ["Benchmark", "Vex (ms)", "SQLite3 (ms)"]
    if has_duckdb:
        headers.append("DuckDB (ms)")
    headers.extend(["Vex Throughput", "vs SQLite3"])
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
    workers = args.workers

    data = build_datasets(n_orders, n_users, n_lookups, args.seed)
    total_inserted = n_orders + n_users
    results: list[BenchRow] = []

    with tempfile.TemporaryDirectory(prefix="vex_bench_") as tmpdir:
        vex_path = None if args.mode == "memory" else str(Path(tmpdir) / "bench.wal")
        sql_path = None if args.mode == "memory" else str(Path(tmpdir) / "bench.sqlite")
        duck_path = None if args.mode == "memory" else str(Path(tmpdir) / "bench.duckdb")

        vdb = open_vex(vex_path, workers, args.chunk_rows)
        sdb = open_sqlite(sql_path)
        ddb = open_duckdb(duck_path, workers) if duckdb is not None else None

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

            # Sanity-check row counts
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
            # 2. O(1) PK LOOKUPS
            # ------------------------------------------------------------------
            keys = data["lookup_keys"]

            def vex_pk() -> float:
                acc = 0.0
                for k in keys:
                    acc += vdb.q("orders[$k] amount", k=k)["amount"][0]
                return acc

            def sql_pk() -> float:
                cur = sdb.cursor()
                acc = 0.0
                for k in keys:
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
                    for k in keys:
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
            # 3. FILTERED GROUP AGGREGATION (Vex Morsel SIMD Fast Path)
            # ------------------------------------------------------------------
            vex_agg_q = "orders[amount > 500.0] by bucket { n: count(), s: sum(amount) } sort bucket"
            sql_agg_q = (
                "SELECT bucket, COUNT(*), SUM(amount) FROM orders "
                "WHERE amount > 500.0 GROUP BY bucket ORDER BY bucket"
            )

            vex_agg_ms, v_agg = time_fn(lambda: vdb.q(vex_agg_q).rows(), iters=args.iters)
            sql_agg_ms, s_agg = time_fn(lambda: sdb.execute(sql_agg_q).fetchall(), iters=args.iters)
            verify_agg(v_agg, data["expect_agg"], "Vex agg")
            verify_agg(s_agg, data["expect_agg"], "SQLite agg")

            duck_agg_ms: float | None = None
            if ddb is not None:
                duck_agg_ms, d_agg = time_fn(lambda: ddb.execute(sql_agg_q).fetchall(), iters=args.iters)
                verify_agg(d_agg, data["expect_agg"], "DuckDB agg")

            agg_mrows = (n_orders / (vex_agg_ms / 1000.0)) / 1e6
            results.append(
                BenchRow(
                    benchmark=f"3. Filtered Group Agg ({n_orders:,} rows)",
                    vex_ms=vex_agg_ms,
                    sqlite_ms=sql_agg_ms,
                    duckdb_ms=duck_agg_ms,
                    vex_metric=f"{agg_mrows:.1f} M rows/s",
                    vs_sqlite=sql_agg_ms / vex_agg_ms,
                    vs_duckdb=(duck_agg_ms / vex_agg_ms) if duck_agg_ms else None,
                )
            )

            # ------------------------------------------------------------------
            # 4. HASH JOIN + GROUP AGGREGATION (orders & users)
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
            verify_join(v_join, data["expect_join"], "Vex join")
            verify_join(s_join, data["expect_join"], "SQLite join")

            duck_join_ms: float | None = None
            if ddb is not None:
                duck_join_ms, d_join = time_fn(lambda: ddb.execute(duck_join_q).fetchall(), iters=args.iters)
                verify_join(d_join, data["expect_join"], "DuckDB join")

            join_mrows = (n_orders / (vex_join_ms / 1000.0)) / 1e6
            results.append(
                BenchRow(
                    benchmark=f"4. Hash Join + Agg ({n_orders:,} x {n_users:,})",
                    vex_ms=vex_join_ms,
                    sqlite_ms=sql_join_ms,
                    duckdb_ms=duck_join_ms,
                    vex_metric=f"{join_mrows:.1f} M rows/s",
                    vs_sqlite=sql_join_ms / vex_join_ms,
                    vs_duckdb=(duck_join_ms / vex_join_ms) if duck_join_ms else None,
                )
            )

            # ------------------------------------------------------------------
            # 5. ZERO-COPY NUMPY EXPORT (Query -> ndarray + pure view handoff)
            # ------------------------------------------------------------------
            if np is not None:
                def vex_np_e2e() -> float:
                    res = vdb.q("orders amount")
                    arr = res["amount"].np()
                    assert arr.flags.writeable is False
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
                assert math.isclose(v_nsum, data["expect_total_amt"], rel_tol=1e-9)
                assert math.isclose(s_nsum, data["expect_total_amt"], rel_tol=1e-9)

                duck_np_ms: float | None = None
                if ddb is not None:
                    def duck_np_e2e() -> float:
                        arr = ddb.execute("SELECT amount FROM orders").fetchnumpy()["amount"]
                        return float(arr.sum())

                    duck_np_ms, d_nsum = time_fn(duck_np_e2e, iters=args.iters)
                    assert math.isclose(d_nsum, data["expect_total_amt"], rel_tol=1e-9)

                np_mrows = (n_orders / (vex_np_ms / 1000.0)) / 1e6
                results.append(
                    BenchRow(
                        benchmark=f"5a. Query -> NumPy + sum ({n_orders:,} f64)",
                        vex_ms=vex_np_ms,
                        sqlite_ms=sql_np_ms,
                        duckdb_ms=duck_np_ms,
                        vex_metric=f"{np_mrows:.1f} M rows/s",
                        vs_sqlite=sql_np_ms / vex_np_ms,
                        vs_duckdb=(duck_np_ms / vex_np_ms) if duck_np_ms else None,
                    )
                )

                # Pure zero-copy view handoff (already-executed Result -> NumPy ndarray)
                pinned_res = vdb.q("orders amount")
                col_view = pinned_res["amount"]
                sql_rows = sdb.execute("SELECT amount FROM orders").fetchall()

                def vex_view_only() -> Any:
                    a = col_view.np()
                    return a

                def sql_copy_only() -> Any:
                    return np.fromiter((r[0] for r in sql_rows), dtype=np.float64, count=n_orders)

                vex_v_ms, a1 = time_fn(vex_view_only, iters=max(args.iters, 20))
                a2 = col_view.np()
                assert a1.__array_interface__["data"][0] == a2.__array_interface__["data"][0]
                sql_v_ms, _ = time_fn(sql_copy_only, iters=args.iters)

                results.append(
                    BenchRow(
                        benchmark=f"5b. Result -> NumPy Handoff (0-copy)",
                        vex_ms=vex_v_ms,
                        sqlite_ms=sql_v_ms,
                        duckdb_ms=None,
                        vex_metric=f"{vex_v_ms * 1000.0:.1f} us (0 B copied)",
                        vs_sqlite=sql_v_ms / vex_v_ms,
                        vs_duckdb=None,
                    )
                )
        finally:
            vdb.close()
            sdb.close()
            if ddb is not None:
                ddb.close()

    return results


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Benchmark Vex v1.0 vs SQLite3 and DuckDB.")
    p.add_argument("--rows", type=int, default=200_000, help="Number of order rows (default: 200000)")
    p.add_argument("--users", type=int, default=None, help="Number of user rows (default: rows // 5)")
    p.add_argument("--pk-lookups", type=int, default=5_000, help="Random PK lookups (default: 5000)")
    p.add_argument("--iters", type=int, default=7, help="Timed iterations per query (default: 7)")
    p.add_argument("--workers", type=int, default=min(4, os.cpu_count() or 1), help="Worker threads")
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
        f"Vex v1.0 Benchmark | mode={args.mode} | rows={args.rows:,} | "
        f"workers={args.workers} | chunk_rows={args.chunk_rows} | iters={args.iters}"
    )
    print(
        f"Env: Python {platform.python_version()} | {platform.machine()} ({os.cpu_count()} CPUs) | "
        f"Vex SIMD={vex.simd_backend()} | SQLite={sqlite3.sqlite_version} | DuckDB={duck_ver}\n"
    )

    if np is None:
        print("[note] NumPy not installed — skipping benchmark #5.\n")

    rows = run_suite(args)
    print(format_markdown_table(rows, has_duckdb=has_duck))

    if args.json is not None:
        payload = {
            "meta": {
                "mode": args.mode,
                "rows": args.rows,
                "pk_lookups": args.pk_lookups,
                "workers": args.workers,
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

