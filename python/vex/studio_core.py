"""Shared VQL Studio session, lexer, formatting, and temp-DB inspection core."""

from __future__ import annotations

import pprint
import re
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import vex
from . import cli as vex_cli

VQL_KEYWORDS = frozenset(
    {"table", "del", "sort", "by", "index", "unique", "and", "or", "not", "in", "key"}
)
VQL_TYPES = frozenset({"i64", "u64", "f64", "bool", "str"})
VQL_CONSTANTS = frozenset({"true", "false", "null"})
VQL_AGGREGATES = frozenset({"count", "sum", "avg", "min", "max"})

_TOKEN_RE = re.compile(
    r"(?P<comment>#[^\n]*)"
    r'|(?P<string>"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|`[^`\n]+`)'
    r"|(?P<param>\$[A-Za-z_][A-Za-z0-9_]*)"
    r"|(?P<number>\b\d+(?:\.\d+)?(?:[eE][+-]?\d+)?\b)"
    r"|(?P<op>\+\+|\+=|\|=|-=|->|==|!=|<=|>=|[&+\-*/%=<>?:])"
    r"|(?P<ident>\b[A-Za-z_][A-Za-z0-9_]*\b)"
)

_MULTILINE_CONT_RE = re.compile(r"^\s*(?:by\b|sort\b|\+\+|\+|&|\[|\{)")

DEFAULT_SCRIPT = """\
# Vex VQL Studio (Ctrl+Enter to run, or select lines to run selection)
table users {
    id: u64 key,
    name: str,
    country: str,
    age: i64
}

table orders {
    id: u64 key,
    user_id: u64 -> users.id,
    amount: f64,
    paid: bool
}

users += [
    { id: 1, name: "Aarav", country: "IN", age: 29 },
    { id: 2, name: "Meera", country: "IN", age: 31 },
    { id: 3, name: "Kenji", country: "JP", age: 27 }
]

orders += [
    { id: 10, user_id: 1, amount: 250.5, paid: true },
    { id: 11, user_id: 1, amount: 149.5, paid: true },
    { id: 12, user_id: 2, amount: 520.0, paid: true },
    { id: 13, user_id: 3, amount: 80.0, paid: false },
    { id: 14, user_id: 3, amount: 310.0, paid: true }
]

orders[paid == true] & users
    by users.country { n: count(), total_rev: sum(amount), avg_rev: avg(amount) }
    sort -total_rev
"""

SNIPPETS: dict[str, str] = {
    "Quickstart (Tables + Join + Agg)": DEFAULT_SCRIPT,
    "Scan, Filter & Slice": "users[age >= 28] id, name, country, age sort -age [:10]",
    "O(1) PK Lookup & Update": (
        "users[1] = { age: 30 }\n"
        "users[1] id, name, age"
    ),
    "Explain Plan (?)": (
        "? orders[paid == true, amount > 100.0] & users users.name, amount sort -amount [:5]"
    ),
}


@dataclass(frozen=True)
class HighlightSpan:
    """Single syntax-highlighted token span using 0-based character offsets."""

    tag: str
    start: int
    end: int


@dataclass
class QuerySnapshot:
    """Captured output from executing VQL statements."""

    columns: tuple[str, ...]
    dtypes: tuple[str, ...]
    rows: list[tuple[Any, ...]]
    dicts: list[dict[str, Any]]
    rows_affected: int
    explain: str | None
    elapsed_ms: float
    statements_run: int
    status_log: str

    def to_payload(self) -> dict[str, Any]:
        """Serialize snapshot to JSON-friendly dictionary."""
        return {
            "columns": list(self.columns),
            "dtypes": list(self.dtypes),
            "rows": [list(r) for r in self.rows],
            "dicts": self.dicts,
            "rows_affected": self.rows_affected,
            "explain": self.explain,
            "elapsed_ms": round(self.elapsed_ms, 3),
            "statements_run": self.statements_run,
            "status_log": self.status_log,
            "table_text": format_output_table(self),
            "dict_text": format_output_dicts(self),
            "list_text": format_output_list(self),
        }


def get_process_rss_bytes() -> int | None:
    """Return current process resident set size in bytes on Linux, or None."""
    status_file = Path("/proc/self/status")
    if not status_file.exists():
        return None
    try:
        for line in status_file.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("VmRSS:"):
                parts = line.split()
                if len(parts) >= 2:
                    return int(parts[1]) * 1024
    except OSError:
        return None
    return None


def tokenize_vql(source: str) -> list[HighlightSpan]:
    """Tokenize VQL source into syntax highlight spans matching vx_lexer.c."""
    spans: list[HighlightSpan] = []
    for match in _TOKEN_RE.finditer(source):
        kind = match.lastgroup
        if not kind:
            continue
        text = match.group(kind)
        if kind == "ident":
            if text in VQL_KEYWORDS:
                tag = "keyword"
            elif text in VQL_TYPES:
                tag = "type"
            elif text in VQL_CONSTANTS:
                tag = "constant"
            elif text in VQL_AGGREGATES:
                tag = "aggregate"
            else:
                continue
        else:
            tag = kind
        spans.append(HighlightSpan(tag=tag, start=match.start(), end=match.end()))
    return spans


def split_vql_statements(source: str) -> list[str]:
    """Split multi-statement VQL buffer into complete executable statements.

    Mirrors vx_parser.c's delimiter balance and multiline stage continuation rules
    (lines starting with +, ++, &, by, sort, [, { continue the active query)
    so users can mix DDL, DML, and multi-line queries in one editor buffer.
    """
    lines = source.splitlines()
    stmts: list[str] = []
    buf: list[str] = []

    for idx, raw_line in enumerate(lines):
        stripped = raw_line.strip()
        if not buf and (not stripped or stripped.startswith("#")):
            continue
        buf.append(raw_line)
        joined = "\n".join(buf)
        if not vex_cli._is_input_complete(joined):
            continue

        if not joined.rstrip().endswith(";"):
            continued = False
            for nxt in lines[idx + 1 :]:
                nxt_s = nxt.strip()
                if not nxt_s or nxt_s.startswith("#"):
                    continue
                if _MULTILINE_CONT_RE.match(nxt_s):
                    continued = True
                break
            if continued:
                continue

        stmts.extend(_split_top_level_semicolons(joined))
        buf.clear()

    if buf:
        leftover = "\n".join(buf).strip()
        if leftover:
            stmts.append(leftover)
    return [s for s in stmts if s.strip()]


def _split_top_level_semicolons(text: str) -> list[str]:
    """Split a balanced statement chunk on top-level ';' separators."""
    parts: list[str] = []
    cur: list[str] = []
    depth = 0
    in_quote: str | None = None
    in_comment = False
    escaped = False

    for ch in text:
        if in_comment:
            cur.append(ch)
            if ch == "\n":
                in_comment = False
            continue
        if in_quote is not None:
            cur.append(ch)
            if escaped:
                escaped = False
            elif ch == "\\" and in_quote != "`":
                escaped = True
            elif ch == in_quote:
                in_quote = None
            continue

        if ch == "#":
            in_comment = True
            cur.append(ch)
        elif ch in ('"', "'", "`"):
            in_quote = ch
            cur.append(ch)
        elif ch in "([{":
            depth += 1
            cur.append(ch)
        elif ch in ")]}":
            depth = max(0, depth - 1)
            cur.append(ch)
        elif ch == ";" and depth == 0:
            seg = "".join(cur).strip()
            if seg:
                parts.append(seg)
            cur.clear()
        else:
            cur.append(ch)

    tail = "".join(cur).strip()
    if tail:
        parts.append(tail)
    return parts


def inspect_db_file(path: Path, max_bytes: int = 512) -> str:
    """Return human-readable metadata and hex/ASCII preview for a temp DB file."""
    if not path.exists():
        return f"File not found: {path}"
    st = path.stat()
    data = path.read_bytes()
    total = len(data)
    header_magic = repr(data[:8]) if total else "b''"
    lines = [
        f"Name:     {path.name}",
        f"Path:     {path}",
        f"Size:     {total} B ({vex_cli._fmt_bytes(total)})",
        f"Modified: {time.strftime('%H:%M:%S', time.localtime(st.st_mtime))}",
        f"Magic:    {header_magic}",
        "",
        f"--- Hex Preview (first {min(total, max_bytes)} of {total} bytes) ---",
    ]
    preview = data[:max_bytes]
    for offset in range(0, len(preview), 16):
        chunk = preview[offset : offset + 16]
        hex_part = " ".join(f"{b:02x}" for b in chunk).ljust(47)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{offset:04x}  {hex_part}  |{ascii_part}|")
    if total > max_bytes:
        lines.append(f"... ({total - max_bytes} more bytes)")
    return "\n".join(lines)


def format_output_table(snapshot: QuerySnapshot) -> str:
    """Format query snapshot as an aligned ASCII table."""
    if snapshot.explain is not None and not snapshot.columns:
        return snapshot.explain
    if not snapshot.columns:
        return snapshot.status_log
    return vex_cli.format_table(
        snapshot.columns,
        snapshot.rows,
        snapshot.dtypes,
        row_limit=0,
    )


def format_output_dicts(snapshot: QuerySnapshot) -> str:
    """Format query snapshot as Python dictionary records."""
    if snapshot.explain is not None and not snapshot.columns:
        return snapshot.explain
    if not snapshot.columns:
        return pprint.pformat({"rows_affected": snapshot.rows_affected}, sort_dicts=False)
    return pprint.pformat(snapshot.dicts, width=96, sort_dicts=False)


def format_output_list(snapshot: QuerySnapshot) -> str:
    """Format query snapshot as a Python list of row tuples."""
    if snapshot.explain is not None and not snapshot.columns:
        return snapshot.explain
    if not snapshot.columns:
        return pprint.pformat([])
    return pprint.pformat(snapshot.rows, width=96)


class TempVexSession:
    """Manages an isolated temporary database directory and VQL execution."""

    def __init__(self) -> None:
        self._tmpdir: tempfile.TemporaryDirectory[str] | None = None
        self.db_dir: Path
        self.wal_path: Path
        self.db: vex.Database
        self.open_new()

    def open_new(self) -> None:
        """Close any active DB and spin up a fresh temporary database file."""
        self.close()
        self._tmpdir = tempfile.TemporaryDirectory(prefix="vex_gui_")
        self.db_dir = Path(self._tmpdir.name)
        self.wal_path = self.db_dir / "session.wal"
        self.db = vex.open(str(self.wal_path), workers=2, fsync=False)

    def list_files(self) -> list[tuple[str, int, Path]]:
        """List files currently present in the temporary database directory."""
        if not self.db_dir.exists():
            return []
        entries: list[tuple[str, int, Path]] = []
        for p in sorted(self.db_dir.iterdir()):
            if p.is_file():
                entries.append((p.name, p.stat().st_size, p))
        return entries

    def stats_payload(self, selected_file: str | None = None) -> dict[str, Any]:
        """Collect temporary DB files, hex preview, and runtime engine metrics."""
        files = self.list_files()
        total_disk_bytes = sum(sz for _name, sz, _p in files)
        file_items = [
            {
                "name": name,
                "size": sz,
                "size_human": vex_cli._fmt_bytes(sz),
                "modified": time.strftime("%H:%M:%S", time.localtime(path.stat().st_mtime)),
            }
            for name, sz, path in files
        ]
        target_name = selected_file or "session.wal"
        if files and all(f["name"] != target_name for f in file_items):
            target_name = file_items[0]["name"]
        target_path = (self.db_dir / target_name).resolve()
        if target_path.parent != self.db_dir.resolve():
            target_path = self.wal_path
            target_name = self.wal_path.name

        st = self.db.stats()
        rss = get_process_rss_bytes()
        return {
            "db_dir": str(self.db_dir),
            "wal_path": str(self.wal_path),
            "selected_file": target_name,
            "file_preview": inspect_db_file(target_path),
            "files": file_items,
            "total_disk_bytes": total_disk_bytes,
            "total_disk_human": vex_cli._fmt_bytes(total_disk_bytes),
            "process_rss_bytes": rss,
            "process_rss_human": vex_cli._fmt_bytes(rss) if rss is not None else "n/a",
            "simd_backend": vex.simd_backend(),
            "stats": st,
        }

    def run_script(self, script: str) -> QuerySnapshot:
        """Execute one or more VQL statements and return the latest result snapshot."""
        stmts = split_vql_statements(script)
        if not stmts:
            raise vex.VexError("editor has no executable VQL statements")

        t0 = time.perf_counter()
        last_res: vex.Result | None = None
        total_affected = 0
        logs: list[str] = []

        for idx, stmt in enumerate(stmts, start=1):
            res = self.db.q(stmt, sync=True)
            last_res = res
            total_affected += int(res.rows_affected)
            if res.explain is not None:
                logs.append(f"[{idx}] EXPLAIN: {res.explain}")
            elif res.ncols == 0:
                logs.append(f"[{idx}] OK (rows_affected={res.rows_affected})")
            else:
                logs.append(f"[{idx}] Query returned {res.nrows} row(s), {res.ncols} col(s)")

        elapsed_ms = (time.perf_counter() - t0) * 1000.0
        assert last_res is not None
        cols = tuple(last_res.columns)
        dtypes = tuple(last_res[i].dtype for i in range(last_res.ncols))
        return QuerySnapshot(
            columns=cols,
            dtypes=dtypes,
            rows=last_res.rows() if last_res.ncols else [],
            dicts=last_res.dicts() if last_res.ncols else [],
            rows_affected=total_affected,
            explain=last_res.explain,
            elapsed_ms=elapsed_ms,
            statements_run=len(stmts),
            status_log="\n".join(logs),
        )

    def checkpoint(self) -> None:
        """Write an atomic .snap file and compact the temporary WAL."""
        self.db.checkpoint()

    def close(self) -> None:
        """Close the active database and remove the temporary directory."""
        db = getattr(self, "db", None)
        if db is not None:
            try:
                db.close()
            except Exception:
                pass
            self.db = None  # type: ignore[assignment]
        tmpdir = getattr(self, "_tmpdir", None)
        if tmpdir is not None:
            tmpdir.cleanup()
            self._tmpdir = None

