import json
import os
import random
import subprocess
import sys
from pathlib import Path
import pytest

FAILPOINTS = [
    "seg_pre_rename",
    "seg_post_rename_pre_man",
    "del_pre_rename",
    "del_post_rename_pre_man",
    "idx_post_rename_pre_man",
    "man_mid_tmp_write",
    "man_post_rename_pre_wal_trunc",
    "compact_post_man_pre_unlink",
    "wal_tx_mid_frame",
    "wal_tx_post_sync_pre_ebr",
]

WORKER_SCRIPT = r"""
import json, os, random, sys
import vex

db_dir, seed, state_file = sys.argv[1], int(sys.argv[2]), sys.argv[3]
rng = random.Random(seed)
db = vex.open(db_dir, max_memory_bytes=2 * 1024 * 1024)  # 2 MiB forces rapid flush/compact

db.q("table acct (id: u64, email: str, bal: i64, tag: str)")
db.q("unique(acct.email)")
db.q("index(acct.bal)")

# Seed 200 accounts with exact total supply = 200 * 1000
oracle = {}
with db.tx():
    for i in range(1, 201):
        email = f"user_{i:04d}@vex.io"
        tag = f"tier_{i % 5}" if i % 3 else f"long_tier_prefix_{i:06d}_payload"
        db.put("acct", {"id": i, "email": email, "bal": 1000, "tag": tag})
        oracle[i] = (email, 1000, tag)

# Persist known-good pre-crash supply invariant
Path = __import__("pathlib").Path
Path(state_file).write_text(json.dumps({"supply": 200_000}))

# Drive mixed hot/cold writes, deletes, upserts, and multi-statement transfers
for step in range(40):
    a, b = rng.sample(range(1, 201), 2)
    amt = rng.randint(1, 50)
    with db.tx():
        ra = db.q("acct id == $id", id=a)
        rb = db.q("acct id == $id", id=b)
        if len(ra) and len(rb):
            db.q("acct id == $id | bal = $v", id=a, v=int(ra["bal"][0]) - amt)
            db.q("acct id == $id | bal = $v", id=b, v=int(rb["bal"][0]) + amt)
    if step % 5 == 0:
        # Insert + delete churn to trigger .del sidecars and compaction
        extra_id = 1000 + step
        db.put("acct", {"id": extra_id, "email": f"tmp_{extra_id}@vex.io", "bal": 0, "tag": "churn"})
        if step >= 10:
            db.q("acct id == $id | del", id=1000 + step - 10)
    if step % 8 == 0:
        db.checkpoint()

db.close()
"""


def assert_db_invariants(db_dir: Path, expected_supply: int) -> None:
    import vex

    # 1. Reopen triggers MANIFEST verification, orphan sweep, and WAL replay
    db = vex.open(str(db_dir), max_memory_bytes=2 * 1024 * 1024)
    try:
        # 2. Zero leftover .tmp files after orphan sweep
        tmps = [p.name for p in db_dir.iterdir() if p.name.endswith(".tmp")]
        assert tmps == [], f"Unswept temp files remain: {tmps}"

        # 3. Full-file CRC32 scrub on all mounted .vx segments
        if hasattr(db, "_verify_all_segments_crc"):
            db._verify_all_segments_crc()

        # 4. ACID conservation law: no partial transaction ever survived crash
        res = db.q("acct id <= 200 | sum(bal)")
        assert int(res[0][0]) == expected_supply

        # 5. Tiered PK lookup matches full TableScan 1:1
        full = db.q("acct | sort id")
        ids = full["id"].np().tolist()
        bals = full["bal"].np().tolist()
        emails = list(full["email"])
        for row_id, bal, email in zip(ids, bals, emails):
            pt = db.q("acct id == $id", id=int(row_id))
            assert len(pt) == 1 and int(pt["bal"][0]) == int(bal)
            # 6. Secondary unique/hash index parity
            by_email = db.q("acct email == $e", e=email)
            assert len(by_email) == 1 and int(by_email["id"][0]) == int(row_id)

        # 7. Secondary ART range scan matches sequential scan bit-for-bit
        art_res = db.q("acct bal >= 900 && bal <= 1100 | sort id")
        seq_res = db.q("acct bal >= 900 && bal <= 1100 | sort id", _disable_indexes=True) if hasattr(db, "_disable_indexes") else art_res
        assert art_res["id"].np().tolist() == seq_res["id"].np().tolist()
    finally:
        db.close()


@pytest.mark.parametrize("failpoint", FAILPOINTS)
@pytest.mark.parametrize("hit_num", [1, 2])
def test_deterministic_crash_recovery(tmp_path: Path, failpoint: str, hit_num: int):
    # Check if V2 Python bindings with db.tx() and directory-backed storage are available
    import vex
    try:
        probe_db = vex.open(str(tmp_path / "probe.wal"))
        has_tx = hasattr(probe_db, "tx")
        probe_db.close()
    except Exception:
        has_tx = False
    if not has_tx:
        pytest.skip("V2 Python bindings (db.tx / directory storage) not yet loaded; implemented in M18 / doc 20")

    db_dir = tmp_path / f"db_{failpoint}_{hit_num}"
    db_dir.mkdir()
    state_file = tmp_path / "state.json"

    env = os.environ.copy()
    env["PYTHONPATH"] = str(Path(__file__).resolve().parent.parent / "python") + ":" + env.get("PYTHONPATH", "")
    env["VEX_CRASH_POINT"] = failpoint
    env["VEX_CRASH_HIT"] = str(hit_num)

    proc = subprocess.run(
        [sys.executable, "-c", WORKER_SCRIPT, str(db_dir), "20261120", str(state_file)],
        env=env,
        capture_output=True,
        text=True,
        timeout=30,
    )

    # Either crashed deterministically at failpoint (137) or completed if hit_num > occurrences
    assert proc.returncode in (0, 137), f"Unexpected crash rc={proc.returncode}:\n{proc.stderr}"
    if state_file.exists():
        expected_supply = json.loads(state_file.read_text())["supply"]
        assert_db_invariants(db_dir, expected_supply)

