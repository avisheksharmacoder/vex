import gc
import math
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

import pytest

import vex


# ==================== 1. LIFECYCLE, DDL, CRUD & PYTHONIC QUERIES ====================


def test_open_context_manager_crud_and_joins(tmp_path):
    wal = tmp_path / "app.wal"

    with vex.open(str(wal), workers=2, chunk_rows=64, fsync=True) as db:
        db.q("table users { id: u64 key, name: str, country: str, age: i64 }", sync=True)
        db.q("table orders { id: u64 key, user_id: u64 -> users.id, amount: f64, paid: bool }", sync=True)

        db.q(
            'users += ['
            '  { id: 1, name: "Aarav", country: "IN", age: 29 },'
            '  { id: 2, name: "Meera", country: "IN", age: 31 },'
            '  { id: 3, name: "Kenji", country: "JP", age: 27 }'
            ']',
            sync=True,
        )
        # Fast-path dict insert (bypasses query parser)
        db.put("orders", {"id": 10, "user_id": 1, "amount": 250.5, "paid": True}, sync=True)
        db.put("orders", {"id": 11, "user_id": 1, "amount": 149.5, "paid": True}, sync=True)
        db.put("orders", {"id": 12, "user_id": 99, "amount": 80.0, "paid": False}, sync=True)

        # Bare table scan
        all_users = db.q("users")
        assert len(all_users) == 3
        assert all_users.ncols == 4
        assert all_users.columns == ("id", "name", "country", "age")

        # Parameterized filter + projection + sort + slice
        r = db.q(
            "users[country == $c, age >= $min_age] name, age sort -age [:1]",
            c="IN",
            min_age=30,
        )
        assert len(r) == 1
        assert r.dicts() == [{"name": "Meera", "age": 31}]
        assert r.rows() == [("Meera", 31)]

        # Left outer join (+) with NULL right side
        j = db.q("orders + users(user_id == id) orders.id, users.name sort orders.id")
        assert j.rows() == [(10, "Aarav"), (11, "Aarav"), (12, None)]
        assert j["name"].has_nulls is True

        # Auto-FK inner join (&) + group aggregate
        agg = db.q("orders[paid] & users by users.name { n: count(), rev: sum(amount) }")
        assert agg.dicts() == [{"name": "Aarav", "n": 2, "rev": pytest.approx(400.0)}]

        # Explain plan (?)
        plan = db.q("? users[age > 25] name")
        assert isinstance(plan.explain, str) and "PLAN:" in plan.explain

        stats = db.stats()
        assert stats["queries_executed"] >= 4
        assert stats["mutations_committed"] >= 4

    # Persistence check across reopen (WAL-logged query statements)
    with vex.open(str(wal), workers=2) as db2:
        r2 = db2.q("users[2] name")
        assert r2.rows() == [("Meera",)]


def test_error_handling_and_closed_db():
    db = vex.open()
    db.q("table t { id: u64 key, v: i64 }", sync=True)

    with pytest.raises(vex.VexError):
        db.q("del t")  # safety guard: missing [:]

    with pytest.raises(vex.VexError):
        db.q("t[v == $missing]")  # unbound parameter

    with pytest.raises(vex.VexError):
        db.q("nonexistent_table")

    db.close()

    with pytest.raises(vex.VexError):
        db.q("t")


# ==================== 2. ZERO-COPY PEP 3118 BUFFER PROTOCOL & NUMPY ====================


def test_zero_copy_memoryview_and_lifetime_pinning():
    db = vex.open()
    db.q("table vec { id: u64 key, x: i64, y: f64, ok: bool, tag: str }", sync=True)
    db.q(
        'vec += ['
        '  { id: 1, x: -10, y: 1.5, ok: true,  tag: "a" },'
        '  { id: 2, x:  20, y: 2.5, ok: false, tag: "b" },'
        '  { id: 3, x:  30, y: 3.5, ok: true,  tag: "long_tag_over_12_chars" }'
        ']',
        sync=True,
    )

    res = db.q("vec sort id")

    # Numeric & bool columns expose read-only C-contiguous PEP 3118 buffers
    col_id = res["id"]
    col_x = res["x"]
    col_y = res["y"]
    col_ok = res["ok"]
    col_tag = res["tag"]

    assert col_id.dtype == "u64"
    assert col_x.dtype == "i64"
    assert col_y.dtype == "f64"
    assert col_ok.dtype == "bool"
    assert col_tag.dtype == "str"

    mv_id = memoryview(col_id)
    mv_x = memoryview(col_x)
    mv_y = memoryview(col_y)
    mv_ok = memoryview(col_ok)

    assert mv_id.readonly is True and mv_id.c_contiguous is True and mv_id.format == "Q"
    assert mv_x.readonly is True and mv_x.format == "q"
    assert mv_y.readonly is True and mv_y.format == "d"
    assert mv_ok.readonly is True and mv_ok.format == "?"

    assert list(mv_id) == [1, 2, 3]
    assert list(mv_x) == [-10, 20, 30]
    assert list(mv_y) == [1.5, 2.5, 3.5]
    assert list(mv_ok) == [True, False, True]
    assert col_tag.to_list() == ["a", "b", "long_tag_over_12_chars"]

    # Read-only protection
    with pytest.raises(TypeError):
        mv_x[0] = 999

    # String column is 16B German strings -> must refuse raw numeric buffer export cleanly
    with pytest.raises((TypeError, BufferError)):
        memoryview(col_tag)

    # Lifetime pinning: deleting Result & ColumnView while memoryview is alive MUST stay valid
    del res
    del col_id, col_x, col_y, col_ok, col_tag
    gc.collect()

    assert list(mv_x) == [-10, 20, 30]
    assert list(mv_y) == [1.5, 2.5, 3.5]

    mv_id.release()
    mv_x.release()
    mv_y.release()
    mv_ok.release()
    db.close()


def test_zero_copy_numpy_interop():
    np = pytest.importorskip("numpy")

    with vex.open() as db:
        db.q("table nums { id: u64 key, v: f64 }", sync=True)
        for i in range(100):
            db.put("nums", {"id": i + 1, "v": float(i) * 0.5}, sync=False)
        db.flush()

        res = db.q("nums sort id")
        col = res["v"]
        a1 = col.np()
        a2 = np.asarray(col)

        assert a1.dtype == np.float64
        assert a1.shape == (100,)
        assert a1.flags.writeable is False
        # Zero-copy proof: both NumPy arrays point to the exact same C arena address
        assert a1.__array_interface__["data"][0] == a2.__array_interface__["data"][0]
        assert math.isclose(float(a1.sum()), sum(i * 0.5 for i in range(100)))

        # Deleting Result while NumPy view is live stays valid
        del res, col
        gc.collect()
        assert a1[99] == pytest.approx(49.5)


# ==================== 3. GIL-FREE MULTI-THREADED CONCURRENCY ====================


def test_gil_free_concurrent_readers_and_writer():
    with vex.open(workers=2, chunk_rows=64) as db:
        db.q("table stream { id: u64 key, val: i64 }", sync=True)

        stop = threading.Event()
        errors = []
        read_counts = [0, 0, 0, 0]

        def reader(idx: int):
            prev_n = 0
            try:
                while not stop.is_set():
                    r = db.q("stream[val >= 0] { n: count(), s: sum(val) }")
                    n = r["n"][0]
                    s = r["s"][0]
                    assert n >= prev_n
                    assert s == (n * (n + 1)) // 2
                    prev_n = n
                    read_counts[idx] += 1
            except Exception as exc:
                errors.append(exc)

        threads = [threading.Thread(target=reader, args=(i,)) for i in range(4)]
        for t in threads:
            t.start()

        for i in range(1, 501):
            db.put("stream", {"id": i, "val": i}, sync=(i % 50 == 0))
        db.flush()

        time.sleep(0.02)
        stop.set()
        for t in threads:
            t.join()

        assert not errors
        assert all(c > 0 for c in read_counts)

        # Verify parallel reader fan-out executes cleanly under ThreadPoolExecutor
        with ThreadPoolExecutor(max_workers=4) as ex:
            futs = [
                ex.submit(db.q, "stream[val > $cut] { n: count(), s: sum(val) }", cut=250)
                for _ in range(16)
            ]
            for f in futs:
                res = f.result()
                assert res["n"][0] == 250


# ==================== 4. REFCOUNT & MEMORY LEAK SAFETY ====================


def test_refcount_stability_and_no_leaks():
    with vex.open() as db:
        db.q("table kv { id: u64 key, label: str, score: f64 }", sync=True)

        # Verify db.put does not leak references on input dict or values
        row = {"id": 1, "label": "stable_key_label", "score": 42.5}
        label_obj = row["label"]
        gc.collect()
        rc_row_before = sys.getrefcount(row)
        rc_lbl_before = sys.getrefcount(label_obj)

        for i in range(1, 201):
            row["id"] = i
            db.put("kv", row, sync=False)
        db.flush()

        gc.collect()
        assert sys.getrefcount(row) == rc_row_before
        assert sys.getrefcount(label_obj) == rc_lbl_before

        # Verify db.q parameter kwargs do not leak references
        param_str = "stable_key_label"
        gc.collect()
        rc_param_before = sys.getrefcount(param_str)

        for _ in range(500):
            r = db.q("kv[label == $lbl] id, score", lbl=param_str)
            mv = memoryview(r["score"])
            assert len(mv) == 200
            mv.release()
            del r

        gc.collect()
        assert sys.getrefcount(param_str) == rc_param_before

