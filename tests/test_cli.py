import io
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))

import vex
import vex_cli


def test_multiline_delimiter_balance():
    assert vex_cli._is_input_complete("users") is True
    assert vex_cli._is_input_complete("table u { id: u64 key") is False
    assert vex_cli._is_input_complete("table u { id: u64 key\n}") is True
    # Unbalanced delimiters inside strings or comments must not hang multiline mode
    assert vex_cli._is_input_complete('users += { id: 1, s: "([{unclosed" }') is True
    assert vex_cli._is_input_complete("users # comment with { [ (") is True
    assert vex_cli._is_input_complete('users += { id: 1, s: "unterminated') is False


def test_format_table_alignment_nulls_escapes_and_truncation():
    rendered = vex_cli.format_table(
        ("id", "name", "ok"),
        [
            (1, "Aarav\nLine2", True),
            (42, None, False),
            (100, "Zara", True),
        ],
        ("u64", "str", "bool"),
        row_limit=2,
    )
    lines = rendered.splitlines()
    assert lines[0].startswith("+")
    assert "| id | name" in lines[1]
    assert "|  1 | Aarav\\nLine2 | true  |" in lines[3]
    assert "| 42 | null         | false |" in lines[4]
    assert "showing 2 of 3 rows" in lines[-1]


def test_repl_dot_commands_and_wal_checkpoint_inspection(tmp_path):
    wal = tmp_path / "inspect.wal"
    out = io.StringIO()
    err = io.StringIO()

    with vex.open(str(wal), workers=2) as db:
        repl = vex_cli.VexREPL(db, str(wal), out=out, err=err)

        assert repl.execute("table items { id: u64 key, name: str, price: f64 }") is True
        assert "OK (rows_affected=0" in out.getvalue()

        out.seek(0)
        out.truncate(0)
        repl.execute(
            'items += [{ id: 1, name: "cpu", price: 250.0 }, { id: 2, name: "gpu", price: 600.0 }]'
        )
        assert "rows_affected=2" in out.getvalue()

        # .plan
        out.seek(0)
        out.truncate(0)
        repl.execute(".plan items[price > 200.0] name")
        assert "PLAN:" in out.getvalue()

        # .stats
        out.seek(0)
        out.truncate(0)
        repl.execute(".stats")
        stats_out = out.getvalue()
        assert "simd_backend" in stats_out
        assert "wal_file_size" in stats_out
        assert "dead_rows" in stats_out

        # .checkpoint
        out.seek(0)
        out.truncate(0)
        repl.execute(".checkpoint")
        ckpt_out = out.getvalue()
        assert "Checkpoint complete" in ckpt_out
        assert "wal=16 B" in ckpt_out
        assert Path(f"{wal}.snap").exists()

        # .limit & .sync toggles
        out.seek(0)
        out.truncate(0)
        repl.execute(".limit 1")
        repl.execute("items id, name sort id")
        assert "showing 1 of 2 rows" in out.getvalue()

        # Error handling does not crash REPL
        repl.execute("del items")
        assert "VexError:" in err.getvalue()

        # .quit returns False
        assert repl.execute(".quit") is False


def test_cli_main_batch_and_stream_inspection(tmp_path):
    wal = tmp_path / "cli_main.wal"

    # Populate + checkpoint via -c flags
    out1, err1 = io.StringIO(), io.StringIO()
    rc = vex_cli.main(
        [
            str(wal),
            "-c",
            "table u { id: u64 key, name: str }",
            "-c",
            'u += [{ id: 1, name: "Alice" }, { id: 2, name: "Bob" }]',
            "-c",
            ".checkpoint",
        ],
        out=out1,
        err=err1,
    )
    assert rc == 0
    assert err1.getvalue() == ""
    assert "Checkpoint complete" in out1.getvalue()

    # Reopen and inspect via piped stream (tests multiline + .stats + query)
    script = io.StringIO(
        "u[\n"
        "  id >= 1\n"
        "] id, name sort -id\n"
        ".stats\n"
        ".quit\n"
    )
    out2, err2 = io.StringIO(), io.StringIO()
    rc2 = vex_cli.main([str(wal)], out=out2, err=err2, stdin=script)
    assert rc2 == 0
    assert err2.getvalue() == ""
    text = out2.getvalue()
    assert "Bob" in text and "Alice" in text
    assert "snap_file_size" in text

