import threading

import pytest

import vex


def test_m10_py_bounded_slice_update_and_delete():
    with vex.open() as db:
        db.q("table seq { id: u64 key, v: i64 }", sync=True)
        db.q(
            "seq += [{ id: 1, v: 10 }, { id: 2, v: 20 }, { id: 3, v: 30 }, { id: 4, v: 40 }]",
            sync=True,
        )

        res = db.q("seq[1:3] = { v: v + 100 }", sync=True)
        assert res.rows_affected == 2
        assert db.q("seq id, v").rows() == [(1, 10), (2, 120), (3, 130), (4, 40)]

        res = db.q("del seq[1:3]", sync=True)
        assert res.rows_affected == 2
        assert db.q("seq id").rows() == [(1,), (4,)]


def test_m10_py_null_filtering_and_sum_u64():
    with vex.open() as db:
        db.q("table t { id: u64 key, tag: str, bytes: u64 }", sync=True)
        big = (1 << 54) + 3
        db.put("t", {"id": 1, "tag": None, "bytes": big}, sync=True)
        db.put("t", {"id": 2, "tag": "ok", "bytes": 2}, sync=True)
        db.put("t", {"id": 3, "tag": None, "bytes": 10}, sync=True)

        assert db.q("t[tag == null] id sort id").rows() == [(1,), (3,)]
        assert db.q("t[tag != null] id").rows() == [(2,)]
        assert db.q("t[null == tag] id sort id").rows() == [(1,), (3,)]

        agg = db.q("t { total: sum(bytes) }")
        assert agg["total"].dtype == "u64"
        assert agg["total"][0] == big + 12


def test_m10_py_duplicate_pk_and_put_schema_validation():
    with vex.open() as db:
        db.q("table users { id: u64 key, age: i64, email: str }", sync=True)
        db.q("unique users(email)", sync=True)

        db.q('users += { id: 1, age: 30, email: "a@b.io" }', sync=True)

        with pytest.raises(vex.VexError):
            db.q('users += { id: 1, age: 99, email: "other@b.io" }', sync=True)

        with pytest.raises(vex.VexError):
            db.q('users += { id: 2, age: 22, email: "a@b.io" }', sync=True)

        # db.put eager schema validation (even with sync=False)
        with pytest.raises(vex.VexError):
            db.put("no_such_table", {"id": 1}, sync=False)

        with pytest.raises(vex.VexError):
            db.put("users", {"id": 2, "unknown_col": 123}, sync=False)

        with pytest.raises(vex.VexError):
            db.put("users", {"id": 3, "age": "not_an_int"}, sync=False)

        assert db.q("users { n: count() }")["n"][0] == 1


def test_m10_py_string_escapes():
    with vex.open() as db:
        db.q("table notes { id: u64 key, body: str }", sync=True)
        db.q(r'notes += { id: 1, body: "a\nb\tc\"d\\e" }', sync=True)
        assert db.q("notes[1] body").rows() == [('a\nb\tc"d\\e',)]
        assert db.q(r'notes[body == "a\nb\tc\"d\\e"] id').rows() == [(1,)]


def test_m10_py_live_ram_compaction_and_ephemeral_writers(tmp_path):
    wal = tmp_path / "m10.wal"
    with vex.open(str(wal), chunk_rows=32) as db:
        db.q("table kv { id: u64 key, v: i64 }", sync=True)

        # 80 ephemeral writer threads exceeding 64-slot wmesh pool
        errors = []

        def writer(wid: int):
            try:
                db.put("kv", {"id": wid, "v": wid}, sync=True)
            except Exception as exc:
                errors.append(exc)

        for wave in range(20):
            ts = [threading.Thread(target=writer, args=(wave * 4 + j + 1,)) for j in range(4)]
            for t in ts:
                t.start()
            for t in ts:
                t.join()
        assert not errors
        assert db.q("kv { n: count() }")["n"][0] == 80

        # Create 40 tombstones, then compact live RAM via checkpoint
        for k in range(1, 41):
            db.q("del kv[$k]", sync=True, k=k)
        assert db.stats()["dead_rows"] == 40

        db.checkpoint()
        assert db.stats()["dead_rows"] == 0
        assert db.q("kv { n: count() }")["n"][0] == 40

        # Large 600-row batch query (>64KB arena)
        batch = ",".join(f"{{ id: {i}, v: {i} }}" for i in range(1000, 1600))
        db.q(f"kv += [{batch}]", sync=True)
        assert db.q("kv { n: count() }")["n"][0] == 640
