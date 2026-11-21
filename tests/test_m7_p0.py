import threading
import time
from concurrent.futures import ThreadPoolExecutor

import pytest

import vex


def test_p0_py_put_wal_persistence_and_upsert_dedup(tmp_path):
    wal = tmp_path / "put_persist.wal"

    with vex.open(str(wal), fsync=True) as db:
        db.q("table users { id: u64 key, name: str, score: f64, active: bool }", sync=True)
        db.put("users", {"id": 1, "name": "Aarav", "score": 10.5, "active": True}, sync=True)
        db.put("users", {"id": 2, "name": "Meera", "score": 20.0, "active": False}, sync=True)
        db.put("users", {"id": 3, "name": None, "score": 30.0, "active": True}, sync=True)

        # Upsert existing PK=2 (must replace, not duplicate) and new PK=4
        db.put(
            "users",
            {"id": 2, "name": "Meera_Updated_LongName", "score": 99.25, "active": True},
            upsert=True,
            sync=True,
        )
        db.put(
            "users",
            {"id": 4, "name": "Zara", "score": 40.0, "active": True},
            upsert=True,
            sync=True,
        )

        r = db.q("users id, name, score, active sort id")
        assert r.rows() == [
            (1, "Aarav", pytest.approx(10.5), True),
            (2, "Meera_Updated_LongName", pytest.approx(99.25), True),
            (3, None, pytest.approx(30.0), True),
            (4, "Zara", pytest.approx(40.0), True),
        ]

    # Reopen from WAL: all db.put() mutations + upserts must survive
    with vex.open(str(wal)) as db2:
        r2 = db2.q("users id, name, score, active sort id")
        assert r2.rows() == [
            (1, "Aarav", pytest.approx(10.5), True),
            (2, "Meera_Updated_LongName", pytest.approx(99.25), True),
            (3, None, pytest.approx(30.0), True),
            (4, "Zara", pytest.approx(40.0), True),
        ]


def test_p0_py_parameterized_writes_wal_replay(tmp_path):
    wal = tmp_path / "param_writes.wal"

    with vex.open(str(wal), fsync=True) as db:
        db.q("table orders { id: u64 key, note: str, amount: f64, paid: bool }", sync=True)
        db.q(
            "orders += { id: $id, note: $note, amount: $amt, paid: $paid }",
            sync=True,
            id=10,
            note="first_order_paid",
            amt=120.0,
            paid=True,
        )
        db.q(
            "orders |= { id: $id, note: $note, amount: $amt, paid: $paid }",
            sync=True,
            id=11,
            note="small",
            amt=15.0,
            paid=False,
        )
        db.q("orders[$id] = { amount: amount + $delta }", sync=True, id=10, delta=30.5)
        db.q("del orders[amount < $cutoff]", sync=True, cutoff=50.0)

    with vex.open(str(wal)) as db2:
        r = db2.q("orders id, note, amount, paid")
        assert r.rows() == [(10, "first_order_paid", pytest.approx(150.5), True)]


def test_p0_py_async_put_auto_flush_without_manual_flush():
    with vex.open(flush_interval_us=3000) as db:
        db.q("table ticks { id: u64 key, px: f64 }", sync=True)

        # Submit only 10 async puts and NEVER call db.flush()
        for i in range(1, 11):
            db.put("ticks", {"id": i, "px": float(i)}, sync=False)

        deadline = time.monotonic() + 0.3
        seen = 0
        while time.monotonic() < deadline:
            seen = db.q("ticks { n: count() }")["n"][0]
            if seen == 10:
                break
            time.sleep(0.005)

        assert seen == 10
        stats = db.stats()
        assert stats["logs_emitted"] > 0


def test_p0_py_ephemeral_thread_churn_past_max_readers():
    with vex.open(max_readers=6, workers=2) as db:
        db.q("table kv { id: u64 key, v: i64 }", sync=True)
        db.q("kv += [{ id: 1, v: 7 }, { id: 2, v: 8 }]", sync=True)

        errors = []

        def ephemeral_read():
            try:
                assert db.q("kv { n: count() }")["n"][0] == 2
            except Exception as exc:
                errors.append(exc)

        # 80 ephemeral threads > max_readers=6 (in waves of 4)
        for _ in range(20):
            threads = [threading.Thread(target=ephemeral_read) for _ in range(4)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()

        assert not errors


def test_p0_py_concurrent_morsel_readers_stress():
    with vex.open(workers=4, chunk_rows=128, max_readers=16) as db:
        db.q("table metrics { id: u64 key, bucket: i64, val: f64 }", sync=True)
        expect = {b: [0, 0.0] for b in range(8)}
        for i in range(4096):
            b = i & 7
            v = float((i * 37) % 1000)
            db.put("metrics", {"id": i + 1, "bucket": b, "val": v}, sync=False)
            if v > 500.0:
                expect[b][0] += 1
                expect[b][1] += v
        db.flush()

        query = "metrics[val > 500.0] by bucket { n: count(), s: sum(val) } sort bucket"

        def worker():
            for _ in range(20):
                r = db.q(query)
                rows = r.rows()
                assert len(rows) == 8
                for b in range(8):
                    assert rows[b][0] == b
                    assert rows[b][1] == expect[b][0]
                    assert rows[b][2] == pytest.approx(expect[b][1])

        with ThreadPoolExecutor(max_workers=8) as ex:
            futs = [ex.submit(worker) for _ in range(8)]
            for f in futs:
                f.result()

