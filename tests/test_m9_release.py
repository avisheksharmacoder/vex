import ast
import subprocess
import sys
from pathlib import Path

import pytest

import vex


def test_m9_py_checkpoint_compaction_and_reopen(tmp_path):
    wal = tmp_path / "ckpt.wal"
    snap = tmp_path / "ckpt.wal.snap"

    # In-memory DB rejects checkpoint cleanly
    with vex.open() as mem:
        with pytest.raises(vex.VexError):
            mem.checkpoint()

    with vex.open(str(wal), chunk_rows=32) as db:
        db.q("table items { id: u64 key, name: str, price: f64 }", sync=True)
        for i in range(1, 201):
            db.put("items", {"id": i, "name": f"item_label_{i:04d}", "price": float(i)}, sync=False)
        db.flush()

        for i in range(101, 201):
            db.q("del items[$k]", sync=True, k=i)

        wal_before = wal.stat().st_size
        assert wal_before > 2048

        db.checkpoint()

        assert snap.exists() and snap.stat().st_size > 0
        assert wal.stat().st_size == 16  # compacted back to header
        assert db.stats()["checkpoints"] == 1

        # Post-checkpoint write
        db.put("items", {"id": 1, "name": "updated_post_ckpt", "price": 999.5}, upsert=True, sync=True)

    # Reopen from .snap + post-checkpoint WAL tail
    with vex.open(str(wal)) as db2:
        assert db2.q("items { n: count() }")["n"][0] == 100
        assert db2.q("items[1] name, price").rows() == [("updated_post_ckpt", pytest.approx(999.5))]
        assert db2.q("items[150] name").rows() == []


def test_m9_py_exclusive_flock_rejection(tmp_path):
    wal = tmp_path / "locked.wal"
    py_dir = str(Path(__file__).resolve().parent.parent / "python")

    with vex.open(str(wal)) as db:
        db.q("table t { id: u64 key }", sync=True)

        # Intra-process second open must fail
        with pytest.raises(vex.VexError):
            vex.open(str(wal))

        # Cross-process open must also fail via OS flock
        code = (
            "import sys, vex\n"
            "try:\n"
            "    vex.open(sys.argv[1])\n"
            "    sys.exit(2)\n"
            "except vex.VexError:\n"
            "    sys.exit(0)\n"
        )
        proc = subprocess.run(
            [sys.executable, "-c", code, str(wal)],
            env={"PYTHONPATH": py_dir},
            capture_output=True,
            text=True,
        )
        assert proc.returncode == 0, proc.stderr

    # Succeeds once lock is released
    with vex.open(str(wal)) as db2:
        assert db2.q("t { n: count() }")["n"][0] == 0


def test_m9_py_simd_backend_introspection():
    backend = vex.simd_backend()
    assert backend in ("scalar", "avx2", "neon")


def test_m9_py_type_stubs_valid_and_complete():
    stub_path = Path(__file__).resolve().parent.parent / "python" / "vex.pyi"
    marker_path = Path(__file__).resolve().parent.parent / "python" / "py.typed"
    assert stub_path.exists()
    assert marker_path.exists()

    tree = ast.parse(stub_path.read_text(encoding="utf-8"))
    declared = {
        node.name
        for node in tree.body
        if isinstance(node, (ast.ClassDef, ast.FunctionDef))
    }
    for sym in ("VexError", "ColumnView", "Result", "Database", "open", "simd_backend"):
        assert sym in declared
        assert hasattr(vex, sym)
