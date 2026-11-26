#!/usr/bin/env python3
"""Compatibility entrypoint re-exporting vex.cli."""

from __future__ import annotations

from vex.cli import (
    HELP_TEXT,
    NUMERIC_DTYPES,
    VexREPL,
    _fmt_bytes,
    _fmt_cell,
    _is_input_complete,
    build_arg_parser,
    format_table,
    main,
)

__all__ = [
    "HELP_TEXT",
    "NUMERIC_DTYPES",
    "VexREPL",
    "_fmt_bytes",
    "_fmt_cell",
    "_is_input_complete",
    "build_arg_parser",
    "format_table",
    "main",
]

if __name__ == "__main__":
    raise SystemExit(main())
