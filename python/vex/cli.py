#!/usr/bin/env python3
"""Lightweight, zero-dependency interactive REPL & inspection CLI for Vex."""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path
from typing import Any, Sequence, TextIO

try:
    import readline
except ImportError:  # pragma: no cover
    readline = None  # type: ignore[assignment]

import vex

NUMERIC_DTYPES = frozenset({"i64", "u64", "f64"})

HELP_TEXT = """\
Commands:
  .stats                Show engine metrics, SIMD backend, and .wal/.snap sizes
  .checkpoint           Write atomic .snap snapshot, compact RAM, and truncate WAL
  .plan <query>         Inspect query execution plan (shorthand for '? <query>')
  .flush                Flush staged async writes to MVCC snapshot and WAL
  .limit [N]            Show or set max displayed rows (0 = unlimited)
  .sync [on|off]        Toggle synchronous write commit barrier (default: on)
  .help                 Show this help
  .quit / .exit         Flush and exit

Query quick-ref (6 keywords: by, sort, del, table, index, unique):
  users                                         Full table scan
  users[age >= 25] name, age sort -age [:10]    Filter, project, sort, slice
  orders + users(user_id == id)                 Left outer join (++ full, & inner)
  orders by country { n: count(), s: sum(amt) } Group & aggregate
  users += { id: 1, name: "Aarav" }             Insert (|= upsert, del users[...])
"""


def _is_input_complete(buf: str) -> bool:
    """Return True when all (), [], {} delimiters outside strings/comments are closed."""
    depth = 0
    in_quote: str | None = None
    in_comment = False
    escaped = False

    for ch in buf:
        if in_comment:
            if ch == "\n":
                in_comment = False
            continue
        if in_quote is not None:
            if escaped:
                escaped = False
            elif ch == "\\" and in_quote != "`":
                escaped = True
            elif ch == in_quote:
                in_quote = None
            continue

        if ch == "#":
            in_comment = True
        elif ch in ('"', "'", "`"):
            in_quote = ch
        elif ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth = max(0, depth - 1)

    return depth == 0 and in_quote is None


def _fmt_cell(val: Any, max_width: int) -> str:
    if val is None:
        s = "null"
    elif isinstance(val, bool):
        s = "true" if val else "false"
    else:
        s = str(val)

    s = s.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")
    if max_width > 3 and len(s) > max_width:
        return s[: max_width - 3] + "..."
    return s


def _fmt_bytes(n: int) -> str:
    size = float(n)
    for unit in ("B", "KB", "MB", "GB"):
        if size < 1024.0 or unit == "GB":
            return f"{int(size)} B" if unit == "B" else f"{size:.2f} {unit}"
        size /= 1024.0
    return f"{n} B"


def format_table(
    headers: Sequence[str],
    rows: Sequence[Sequence[Any]],
    dtypes: Sequence[str] | None = None,
    *,
    row_limit: int = 50,
    max_col_width: int = 48,
) -> str:
    """Render tabular output with aligned columns and optional row truncation."""
    ncols = len(headers)
    if ncols == 0:
        return ""

    total_rows = len(rows)
    shown_rows = rows if (row_limit <= 0 or total_rows <= row_limit) else rows[:row_limit]
    formatted = [[_fmt_cell(cell, max_col_width) for cell in row] for row in shown_rows]

    widths = [len(h) for h in headers]
    for row in formatted:
        for i, cell in enumerate(row):
            if len(cell) > widths[i]:
                widths[i] = len(cell)

    right_align = [
        bool(dtypes and i < len(dtypes) and dtypes[i] in NUMERIC_DTYPES)
        for i in range(ncols)
    ]

    sep = "+" + "+".join("-" * (w + 2) for w in widths) + "+"
    hdr_line = "| " + " | ".join(headers[i].ljust(widths[i]) for i in range(ncols)) + " |"

    lines = [sep, hdr_line, sep]
    for row in formatted:
        cells = [
            row[i].rjust(widths[i]) if right_align[i] else row[i].ljust(widths[i])
            for i in range(ncols)
        ]
        lines.append("| " + " | ".join(cells) + " |")
    lines.append(sep)

    if total_rows > len(shown_rows):
        lines.append(
            f"... showing {len(shown_rows)} of {total_rows} rows (use '.limit N' or '.limit 0')"
        )
    return "\n".join(lines)


class VexREPL:
    def __init__(
        self,
        db: vex.Database,
        wal_path: str | None = None,
        *,
        out: TextIO | None = None,
        err: TextIO | None = None,
        row_limit: int = 50,
        sync: bool = True,
    ) -> None:
        self.db = db
        self.wal_path = wal_path
        self.out = out if out is not None else sys.stdout
        self.err = err if err is not None else sys.stderr
        self.row_limit = max(0, row_limit)
        self.sync = sync

    def execute(self, raw: str) -> bool:
        """Execute a single dot-command or Vex statement. Returns False on exit."""
        text = raw.strip()
        if not text:
            return True
        if text.startswith("."):
            return self._handle_dot(text)
        self._exec_query(text)
        return True

    def _handle_dot(self, text: str) -> bool:
        parts = text.split(None, 1)
        cmd = parts[0].lower()
        arg = parts[1].strip() if len(parts) > 1 else ""

        if cmd in (".quit", ".exit", ".q"):
            return False

        if cmd in (".help", ".h"):
            self.out.write(HELP_TEXT)
            return True

        if cmd == ".stats":
            self._cmd_stats()
            return True

        if cmd == ".checkpoint":
            self._cmd_checkpoint()
            return True

        if cmd == ".flush":
            t0 = time.perf_counter()
            self.db.flush()
            dt_ms = (time.perf_counter() - t0) * 1000.0
            self.out.write(f"Flushed in {dt_ms:.2f} ms\n")
            return True

        if cmd == ".plan":
            if not arg:
                self.err.write("Usage: .plan <query>\n")
                return True
            q = arg if arg.startswith("?") else f"? {arg}"
            self._exec_query(q, plan_only=True)
            return True

        if cmd == ".limit":
            if not arg:
                label = "unlimited" if self.row_limit == 0 else str(self.row_limit)
                self.out.write(f"limit = {label}\n")
                return True
            try:
                val = int(arg)
                if val < 0:
                    raise ValueError
                self.row_limit = val
                label = "unlimited" if val == 0 else str(val)
                self.out.write(f"limit set to {label}\n")
            except ValueError:
                self.err.write("Usage: .limit <non-negative integer>\n")
            return True

        if cmd == ".sync":
            if not arg:
                self.out.write(f"sync = {'on' if self.sync else 'off'}\n")
                return True
            low = arg.lower()
            if low in ("on", "true", "1"):
                self.sync = True
                self.out.write("sync set to on\n")
            elif low in ("off", "false", "0"):
                self.sync = False
                self.out.write("sync set to off\n")
            else:
                self.err.write("Usage: .sync [on|off]\n")
            return True

        self.err.write(f"Unknown command: {cmd} (try .help)\n")
        return True

    def _cmd_stats(self) -> None:
        st = self.db.stats()
        rows: list[tuple[str, Any]] = [
            ("mode", self.wal_path if self.wal_path else ":memory:"),
            ("simd_backend", vex.simd_backend()),
            ("queries_executed", st["queries_executed"]),
            ("mutations_committed", st["mutations_committed"]),
            ("wal_lsn", st["wal_lsn"]),
            ("chunks_pruned", st["chunks_pruned"]),
            ("dead_rows", st["dead_rows"]),
            ("checkpoints", st["checkpoints"]),
            ("logs_emitted", st["logs_emitted"]),
            ("logs_dropped", st["logs_dropped"]),
        ]
        if self.wal_path:
            wal_p = Path(self.wal_path)
            snap_p = Path(f"{self.wal_path}.snap")
            rows.append(("wal_file_size", _fmt_bytes(wal_p.stat().st_size) if wal_p.exists() else "missing"))
            rows.append(("snap_file_size", _fmt_bytes(snap_p.stat().st_size) if snap_p.exists() else "none"))

        self.out.write(format_table(("metric", "value"), rows, ("str", "str"), row_limit=0) + "\n")

    def _cmd_checkpoint(self) -> None:
        t0 = time.perf_counter()
        try:
            self.db.checkpoint()
        except vex.VexError as exc:
            self.err.write(f"VexError: {exc}\n")
            return
        dt_ms = (time.perf_counter() - t0) * 1000.0
        extra = ""
        if self.wal_path:
            wal_p = Path(self.wal_path)
            snap_p = Path(f"{self.wal_path}.snap")
            wsz = _fmt_bytes(wal_p.stat().st_size) if wal_p.exists() else "?"
            ssz = _fmt_bytes(snap_p.stat().st_size) if snap_p.exists() else "?"
            extra = f" | wal={wsz}, snap={ssz}"
        self.out.write(f"Checkpoint complete in {dt_ms:.2f} ms{extra}\n")

    def _exec_query(self, query: str, *, plan_only: bool = False) -> None:
        t0 = time.perf_counter()
        try:
            res = self.db.q(query, sync=self.sync)
        except vex.VexError as exc:
            self.err.write(f"VexError: {exc}\n")
            return
        dt_ms = (time.perf_counter() - t0) * 1000.0

        if res.explain is not None:
            self.out.write(f"{res.explain}\n")
            if plan_only:
                self.out.write(f"({dt_ms:.2f} ms)\n")
                return

        if res.ncols == 0:
            self.out.write(f"OK (rows_affected={res.rows_affected}, {dt_ms:.2f} ms)\n")
            return

        dtypes = [res[i].dtype for i in range(res.ncols)]
        table_str = format_table(
            res.columns,
            res.rows(),
            dtypes,
            row_limit=self.row_limit,
        )
        self.out.write(f"{table_str}\n({res.nrows} rows, {dt_ms:.2f} ms)\n")

    def run_interactive(self, history_path: Path | None = None) -> None:
        hist_file = history_path or Path(
            os.environ.get("VEX_HISTORY", str(Path.home() / ".vex_history"))
        )
        if readline is not None:
            readline.set_history_length(1000)
            if hist_file.exists():
                try:
                    readline.read_history_file(str(hist_file))
                except OSError:
                    pass

        target = self.wal_path if self.wal_path else ":memory:"
        self.out.write(
            f"Vex CLI [{target}] | SIMD: {vex.simd_backend()} | Type .help or .quit\n"
        )

        buf: list[str] = []
        try:
            while True:
                prompt = "vex> " if not buf else "...> "
                try:
                    line = input(prompt)
                except EOFError:
                    self.out.write("\n")
                    break
                except KeyboardInterrupt:
                    self.out.write("\n")
                    buf.clear()
                    continue

                if not buf and not line.strip():
                    continue
                if not buf and line.lstrip().startswith("."):
                    if not self.execute(line):
                        break
                    continue

                buf.append(line)
                joined = "\n".join(buf)
                if _is_input_complete(joined):
                    buf.clear()
                    if not self.execute(joined):
                        break
        finally:
            if readline is not None:
                try:
                    hist_file.parent.mkdir(parents=True, exist_ok=True)
                    readline.write_history_file(str(hist_file))
                except OSError:
                    pass

    def run_stream(self, stream: TextIO) -> None:
        buf: list[str] = []
        for raw_line in stream:
            line = raw_line.rstrip("\r\n")
            if not buf and not line.strip():
                continue
            if not buf and line.lstrip().startswith("."):
                if not self.execute(line):
                    return
                continue
            buf.append(line)
            joined = "\n".join(buf)
            if _is_input_complete(joined):
                buf.clear()
                if not self.execute(joined):
                    return
        if buf:
            self.execute("\n".join(buf))


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="vex",
        description="Interactive CLI & inspector for Vex .wal/.snap databases.",
    )
    p.add_argument("path", nargs="?", default=None, help="Path to .wal file (omit for :memory:)")
    p.add_argument("-c", "--command", action="append", default=[], help="Execute command/query and exit")
    p.add_argument("--workers", type=int, default=4, help="Morsel query worker threads (default: 4)")
    p.add_argument("--chunk-rows", type=int, default=4096, help="Rows per chunk (default: 4096)")
    p.add_argument("--fsync", action="store_true", help="Call fdatasync on every WAL commit")
    p.add_argument("--limit", type=int, default=50, help="Max displayed rows per query (0 = all)")
    return p


def main(
    argv: Sequence[str] | None = None,
    *,
    out: TextIO | None = None,
    err: TextIO | None = None,
    stdin: TextIO | None = None,
) -> int:
    args = build_arg_parser().parse_args(list(argv) if argv is not None else None)
    out_s = out if out is not None else sys.stdout
    err_s = err if err is not None else sys.stderr
    in_s = stdin if stdin is not None else sys.stdin

    try:
        db = vex.open(
            args.path,
            workers=args.workers,
            chunk_rows=args.chunk_rows,
            fsync=args.fsync,
        )
    except vex.VexError as exc:
        err_s.write(f"VexError: {exc}\n")
        return 1

    repl = VexREPL(db, args.path, out=out_s, err=err_s, row_limit=args.limit)
    try:
        if args.command:
            for cmd in args.command:
                if not repl.execute(cmd):
                    break
        elif not in_s.isatty():
            repl.run_stream(in_s)
        else:
            repl.run_interactive()
    finally:
        db.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

