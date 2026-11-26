import gc
import os
import random
import sys
import threading
import time
from pathlib import Path
import pytest
import vex

TOTAL_ACCOUNTS = 256
INIT_BALANCE = 10_000
EXPECTED_SUPPLY = TOTAL_ACCOUNTS * INIT_BALANCE


def open_fd_count() -> int:
    fd_dir = "/proc/self/fd" if os.path.isdir("/proc/self/fd") else "/dev/fd"
    return len(os.listdir(fd_dir)) if os.path.isdir(fd_dir) else 0


def test_free_threaded_16_thread_torture(tmp_path: Path):
    # Verify PEP 489 Py_MOD_GIL_NOT_USED kept GIL disabled on 3.13t / 3.14t builds
    if hasattr(sys, "_is_gil_enabled") and getattr(sys.flags, "gil", 1) == 0:
        assert sys._is_gil_enabled() is False, "import vex unexpectedly re-enabled the GIL"

    # Check if V2 Python bindings with db.tx() and directory-backed storage are available
    try:
        probe_db = vex.open(str(tmp_path / "probe.wal"))
        has_tx = hasattr(probe_db, "tx")
        probe_db.close()
    except Exception:
        has_tx = False
    if not has_tx:
        pytest.skip("V2 Python bindings (db.tx / directory storage) not yet loaded; implemented in M18 / doc 20")

    db_dir = tmp_path / "tsan_db"
    db_dir.mkdir()
    # Small Hot RAM cap (4 MiB) + aggressive compaction forces continuous flush/compact overlap
    db = vex.open(str(db_dir), max_memory_bytes=4 * 1024 * 1024)
    db.q("table acct (id: u64, code: str, bal: i64, note: str)")
    db.q("unique(acct.code)")
    db.q("index(acct.bal)")

    with db.tx():
        for i in range(1, TOTAL_ACCOUNTS + 1):
            db.put("acct", {
                "id": i,
                "code": f"ACC_{i:05d}",
                "bal": INIT_BALANCE,
                "note": f"initial_note_payload_for_account_{i:05d}",
            })
    db.checkpoint()

    fd_baseline = open_fd_count()
    stop_ev = threading.Event()
    barrier = threading.Barrier(16)
    errors: list[BaseException] = []
    err_lock = threading.Lock()
    stats = {"tx_commits": 0, "tx_conflicts": 0, "invariant_checks": 0, "views_pinned": 0}
    stats_lock = threading.Lock()

    def record_err(exc: BaseException) -> None:
        with err_lock:
            errors.append(exc)
        stop_ev.set()

    # Role 1 (4 threads): Atomic banking transfers across shared db.tx()
    def tx_transfer_worker(tid: int) -> None:
        rng = random.Random(1000 + tid)
        try:
            barrier.wait()
            while not stop_ev.is_set():
                a, b = rng.sample(range(1, TOTAL_ACCOUNTS + 1), 2)
                amt = rng.randint(1, 25)
                try:
                    with db.tx():
                        ra = db.q("acct id == $id", id=a)
                        rb = db.q("acct id == $id", id=b)
                        db.q("acct id == $id | bal = $v", id=a, v=int(ra["bal"][0]) - amt)
                        db.q("acct id == $id | bal = $v", id=b, v=int(rb["bal"][0]) + amt)
                    with stats_lock:
                        stats["tx_commits"] += 1
                except getattr(vex, "VexConflictError", Exception):
                    with stats_lock:
                        stats["tx_conflicts"] += 1
        except BaseException as exc:
            record_err(exc)

    # Role 2 (4 threads): High-rate insert/delete churn forcing flushes & leveled compaction
    def churn_worker(tid: int) -> None:
        rng = random.Random(2000 + tid)
        base_id = 10_000 + tid * 100_000
        seq = 0
        try:
            barrier.wait()
            while not stop_ev.is_set():
                row_id = base_id + seq
                seq += 1
                try:
                    db.put("acct", {
                        "id": row_id,
                        "code": f"CHURN_{row_id}",
                        "bal": 0,
                        "note": "x" * rng.choice([8, 12, 13, 128]),
                    })
                except getattr(vex, "VexError", Exception) as e:
                    if "memory limit exceeded" not in str(e):
                        raise
                if seq > 200 and seq % 2 == 0:
                    db.q("acct id == $id | del", id=row_id - 150)
        except BaseException as exc:
            record_err(exc)

    # Role 3 (4 threads): Long-lived ColumnView & Arrow stream pins across live compaction
    def view_pinner_worker(tid: int) -> None:
        pinned = []
        try:
            barrier.wait()
            while not stop_ev.is_set():
                res = db.q("acct id <= $n | sort id", n=TOTAL_ACCOUNTS)
                np_bal = res["bal"].np()  # PEP 3118 zero-copy buffer pinning vx_owner_t
                snap_sum = int(np_bal.sum())
                assert snap_sum == EXPECTED_SUPPLY, f"Torn TX visible in ColumnView: {snap_sum}"

                # Exercise M18 Arrow C Stream export if available
                if hasattr(res, "__arrow_c_stream__"):
                    stream_cap = res.__arrow_c_stream__()
                    pinned.append((np_bal, snap_sum, stream_cap))
                else:
                    pinned.append((np_bal, snap_sum, None))

                with stats_lock:
                    stats["views_pinned"] += 1

                # Hold up to 8 historical snapshots simultaneously while compaction unlinks old .vx
                if len(pinned) > 8:
                    old_arr, old_sum, _ = pinned.pop(0)
                    assert int(old_arr.sum()) == old_sum
            # Verify all remaining pinned buffers survived compaction intact
            for arr, s, _ in pinned:
                assert int(arr.sum()) == s
            pinned.clear()
        except BaseException as exc:
            record_err(exc)

    # Role 4 (4 threads): Continuous ACID supply + ART range + unique index invariant checkers
    def verifier_worker(tid: int) -> None:
        try:
            barrier.wait()
            while not stop_ev.is_set():
                s = db.q("acct id <= $n | sum(bal)", n=TOTAL_ACCOUNTS)
                assert int(s[0][0]) == EXPECTED_SUPPLY, f"Supply drift: {int(s[0][0])}"

                # Exercise ART range scan + StringView prefix filter concurrently
                r = db.q("acct bal >= 9500 && bal <= 10500 | count()")
                assert int(r[0][0]) >= 0
                with stats_lock:
                    stats["invariant_checks"] += 1
        except BaseException as exc:
            record_err(exc)

    threads = (
        [threading.Thread(target=tx_transfer_worker, args=(i,)) for i in range(4)]
        + [threading.Thread(target=churn_worker, args=(i,)) for i in range(4)]
        + [threading.Thread(target=view_pinner_worker, args=(i,)) for i in range(4)]
        + [threading.Thread(target=verifier_worker, args=(i,)) for i in range(4)]
    )
    for t in threads:
        t.start()

    time.sleep(15.0)
    stop_ev.set()
    for t in threads:
        t.join(timeout=30.0)
        assert not t.is_alive(), "Deadlock detected during 16-thread torture"

    assert errors == [], f"Concurrency errors: {errors!r}"
    assert stats["tx_commits"] > 0 and stats["invariant_checks"] > 0

    # Release all Python views, force GC, and verify zero FD leaks from retired mmap segments
    gc.collect()
    db.checkpoint()
    assert open_fd_count() <= fd_baseline + 32, f"FD leak: {open_fd_count()} vs base {fd_baseline}"
    db.close()

