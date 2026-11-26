#!/usr/bin/env python3
"""Zero-dependency 3-pane Tkinter VQL Studio GUI for Vex.

Pane 1 (Left):   Temporary DB directory/file inspector & engine stats.
Pane 2 (Center): VQL syntax-highlighted editor & snippet controls.
Pane 3 (Right):  Query output viewer in Table, Dictionary, or List format.
"""

from __future__ import annotations

import sys
import tkinter as tk
from pathlib import Path
from tkinter import font as tkfont, ttk
from typing import Any

import vex
from vex import cli as vex_cli
from vex.studio_core import (
    DEFAULT_SCRIPT,
    SNIPPETS,
    HighlightSpan,
    QuerySnapshot,
    TempVexSession,
    VQL_AGGREGATES,
    VQL_CONSTANTS,
    VQL_KEYWORDS,
    VQL_TYPES,
    format_output_dicts,
    format_output_list,
    format_output_table,
    inspect_db_file,
    split_vql_statements,
    tokenize_vql,
)

__all__ = [
    "DEFAULT_SCRIPT",
    "SNIPPETS",
    "HighlightSpan",
    "QuerySnapshot",
    "TempVexSession",
    "VQL_AGGREGATES",
    "VQL_CONSTANTS",
    "VQL_KEYWORDS",
    "VQL_TYPES",
    "VexStudioApp",
    "format_output_dicts",
    "format_output_list",
    "format_output_table",
    "inspect_db_file",
    "main",
    "split_vql_statements",
    "tokenize_vql",
]


class VexStudioApp:
    """3-pane Tkinter VQL Studio with temp DB file browser, highlighter, and output views."""

    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("Vex VQL Studio")
        self.root.geometry("1280x760")
        self.root.minsize(920, 560)

        self.session = TempVexSession()
        self.last_snapshot: QuerySnapshot | None = None
        self.last_error: str | None = None
        self.output_mode = tk.StringVar(value="table")
        self.snippet_var = tk.StringVar(value="Quickstart (Tables + Join + Agg)")
        self._highlight_after_id: str | None = None
        self._selected_file: Path = self.session.wal_path
        self._editor_font_family = self._resolve_editor_font_family()
        self._editor_font_size = 11
        self._editor_font_size_var = tk.StringVar(value=f"{self._editor_font_size}pt")
        self._editor_font = tkfont.Font(root=self.root, family=self._editor_font_family, size=self._editor_font_size)
        self._editor_font_bold = tkfont.Font(
            root=self.root, family=self._editor_font_family, size=self._editor_font_size, weight="bold"
        )
        self._editor_font_italic = tkfont.Font(
            root=self.root, family=self._editor_font_family, size=self._editor_font_size, slant="italic"
        )

        self._configure_styles()
        self._build_toolbar()
        self._build_panes()
        self._bind_events()

        self.editor.insert("1.0", DEFAULT_SCRIPT)
        self.highlight_editor()
        self.refresh_db_pane()
        self.run_editor()

    def _configure_styles(self) -> None:
        style = ttk.Style(self.root)
        if "clam" in style.theme_names():
            style.theme_use("clam")
        self.root.configure(bg="#181b22")
        style.configure("TFrame", background="#181b22")
        style.configure("TLabel", background="#181b22", foreground="#e6edf3")
        style.configure("Header.TLabel", background="#20242e", foreground="#9ecbff", font=("TkDefaultFont", 10, "bold"))
        style.configure("Toolbar.TFrame", background="#20242e")
        style.configure("TButton", padding=(8, 4))
        style.configure("TRadiobutton", background="#20242e", foreground="#e6edf3")
        style.configure("Treeview", background="#0f1218", fieldbackground="#0f1218", foreground="#e6edf3", rowheight=24)
        style.configure("Treeview.Heading", background="#252a36", foreground="#9ecbff", font=("TkDefaultFont", 9, "bold"))

    def _build_toolbar(self) -> None:
        bar = ttk.Frame(self.root, style="Toolbar.TFrame", padding=(10, 6))
        bar.pack(fill=tk.X)

        ttk.Button(bar, text="Run (Ctrl+Enter)", command=self.run_editor).pack(side=tk.LEFT, padx=(0, 6))
        ttk.Button(bar, text="Checkpoint (.snap)", command=self.do_checkpoint).pack(side=tk.LEFT, padx=(0, 6))
        ttk.Button(bar, text="New Temp DB", command=self.reset_temp_db).pack(side=tk.LEFT, padx=(0, 12))

        ttk.Label(bar, text="Snippet:", style="Header.TLabel").pack(side=tk.LEFT, padx=(4, 4))
        combo = ttk.Combobox(
            bar,
            textvariable=self.snippet_var,
            values=list(SNIPPETS.keys()),
            state="readonly",
            width=30,
        )
        combo.pack(side=tk.LEFT, padx=(0, 10))
        combo.bind("<<ComboboxSelected>>", self._on_snippet_selected)

        self.status_label = ttk.Label(
            bar,
            text=f"SIMD: {vex.simd_backend()}",
            style="Header.TLabel",
        )
        self.status_label.pack(side=tk.RIGHT)

    def _build_panes(self) -> None:
        self.paned = ttk.PanedWindow(self.root, orient=tk.HORIZONTAL)
        self.paned.pack(fill=tk.BOTH, expand=True, padx=6, pady=6)

        # --- Pane 1 (Left): Temporary DB File Inspector ---
        left_frame = ttk.Frame(self.paned)
        self.paned.add(left_frame, weight=24)

        left_hdr = ttk.Frame(left_frame, style="Toolbar.TFrame", padding=(8, 5))
        left_hdr.pack(fill=tk.X)
        ttk.Label(left_hdr, text="1. Temporary DB Files", style="Header.TLabel").pack(side=tk.LEFT)
        ttk.Button(left_hdr, text="Refresh", command=self.refresh_db_pane).pack(side=tk.RIGHT)

        self.db_path_label = ttk.Label(
            left_frame,
            text="",
            padding=(8, 4),
            wraplength=260,
        )
        self.db_path_label.pack(fill=tk.X)

        self.file_tree = ttk.Treeview(left_frame, columns=("size",), show="tree headings", height=6)
        self.file_tree.heading("#0", text="File")
        self.file_tree.heading("size", text="Size")
        self.file_tree.column("#0", width=155)
        self.file_tree.column("size", width=85, anchor=tk.E)
        self.file_tree.pack(fill=tk.X, padx=4, pady=(0, 4))
        self.file_tree.bind("<<TreeviewSelect>>", self._on_file_selected)

        self.stats_label = ttk.Label(left_frame, text="", padding=(8, 4), justify=tk.LEFT)
        self.stats_label.pack(fill=tk.X)

        self.file_preview = tk.Text(
            left_frame,
            wrap=tk.NONE,
            bg="#0f1218",
            fg="#c9d1d9",
            insertbackground="#e6edf3",
            font=("Monospace", 9),
            relief=tk.FLAT,
            state=tk.DISABLED,
        )
        self.file_preview.pack(fill=tk.BOTH, expand=True, padx=4, pady=(0, 4))

        # --- Pane 2 (Center): VQL Syntax Highlighter & Editor ---
        center_frame = ttk.Frame(self.paned)
        self.paned.add(center_frame, weight=40)

        center_hdr = ttk.Frame(center_frame, style="Toolbar.TFrame", padding=(8, 5))
        center_hdr.pack(fill=tk.X)
        ttk.Label(center_hdr, text="2. VQL Editor (Syntax Highlighted)", style="Header.TLabel").pack(side=tk.LEFT)
        ttk.Button(center_hdr, text="Clear", command=lambda: self._set_editor_text("")).pack(side=tk.RIGHT)
        self.font_inc_btn = ttk.Button(
            center_hdr, text="A+", width=3, command=lambda: self.adjust_editor_font_size(1)
        )
        self.font_inc_btn.pack(side=tk.RIGHT, padx=(2, 6))
        self.font_size_lbl = ttk.Label(center_hdr, textvariable=self._editor_font_size_var, style="Header.TLabel")
        self.font_size_lbl.pack(side=tk.RIGHT, padx=4)
        self.font_dec_btn = ttk.Button(
            center_hdr, text="A-", width=3, command=lambda: self.adjust_editor_font_size(-1)
        )
        self.font_dec_btn.pack(side=tk.RIGHT, padx=(6, 2))

        editor_wrap = ttk.Frame(center_frame)
        editor_wrap.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        self.editor = tk.Text(
            editor_wrap,
            wrap=tk.NONE,
            undo=True,
            bg="#0f1218",
            fg="#e6edf3",
            insertbackground="#79c0ff",
            selectbackground="#264f78",
            font=self._editor_font,
            relief=tk.FLAT,
            padx=8,
            pady=8,
        )
        ed_y = ttk.Scrollbar(editor_wrap, orient=tk.VERTICAL, command=self.editor.yview)
        ed_x = ttk.Scrollbar(editor_wrap, orient=tk.HORIZONTAL, command=self.editor.xview)
        self.editor.configure(yscrollcommand=ed_y.set, xscrollcommand=ed_x.set)
        ed_y.pack(side=tk.RIGHT, fill=tk.Y)
        ed_x.pack(side=tk.BOTTOM, fill=tk.X)
        self.editor.pack(fill=tk.BOTH, expand=True)

        self.editor.tag_configure("keyword", foreground="#ff7b72", font=self._editor_font_bold)
        self.editor.tag_configure("type", foreground="#79c0ff")
        self.editor.tag_configure("constant", foreground="#ffa657")
        self.editor.tag_configure("aggregate", foreground="#d2a8ff", font=self._editor_font_bold)
        self.editor.tag_configure("param", foreground="#f2cc60")
        self.editor.tag_configure("string", foreground="#a5d6ff")
        self.editor.tag_configure("number", foreground="#7ee787")
        self.editor.tag_configure("op", foreground="#ffab70")
        self.editor.tag_configure("comment", foreground="#8b949e", font=self._editor_font_italic)

        # --- Pane 3 (Right): Output Viewer (Table / Dict / List) ---
        right_frame = ttk.Frame(self.paned)
        self.paned.add(right_frame, weight=36)

        right_hdr = ttk.Frame(right_frame, style="Toolbar.TFrame", padding=(8, 5))
        right_hdr.pack(fill=tk.X)
        ttk.Label(right_hdr, text="3. Output", style="Header.TLabel").pack(side=tk.LEFT, padx=(0, 8))

        for label, mode in (("Table", "table"), ("Dictionary", "dict"), ("List", "list")):
            ttk.Radiobutton(
                right_hdr,
                text=label,
                value=mode,
                variable=self.output_mode,
                command=self.render_output,
            ).pack(side=tk.LEFT, padx=4)

        self.output_container = ttk.Frame(right_frame)
        self.output_container.pack(fill=tk.BOTH, expand=True, padx=4, pady=(4, 2))

        self.table_frame = ttk.Frame(self.output_container)
        self.result_tree = ttk.Treeview(self.table_frame, show="headings")
        rt_y = ttk.Scrollbar(self.table_frame, orient=tk.VERTICAL, command=self.result_tree.yview)
        rt_x = ttk.Scrollbar(self.table_frame, orient=tk.HORIZONTAL, command=self.result_tree.xview)
        self.result_tree.configure(yscrollcommand=rt_y.set, xscrollcommand=rt_x.set)
        rt_y.pack(side=tk.RIGHT, fill=tk.Y)
        rt_x.pack(side=tk.BOTTOM, fill=tk.X)
        self.result_tree.pack(fill=tk.BOTH, expand=True)

        self.text_frame = ttk.Frame(self.output_container)
        self.output_text = tk.Text(
            self.text_frame,
            wrap=tk.NONE,
            bg="#0f1218",
            fg="#e6edf3",
            font=("Monospace", 10),
            relief=tk.FLAT,
            padx=8,
            pady=8,
            state=tk.DISABLED,
        )
        ot_y = ttk.Scrollbar(self.text_frame, orient=tk.VERTICAL, command=self.output_text.yview)
        ot_x = ttk.Scrollbar(self.text_frame, orient=tk.HORIZONTAL, command=self.output_text.xview)
        self.output_text.configure(yscrollcommand=ot_y.set, xscrollcommand=ot_x.set)
        ot_y.pack(side=tk.RIGHT, fill=tk.Y)
        ot_x.pack(side=tk.BOTTOM, fill=tk.X)
        self.output_text.pack(fill=tk.BOTH, expand=True)

        self.output_meta = tk.Text(
            right_frame,
            height=5,
            wrap=tk.WORD,
            bg="#141821",
            fg="#8b949e",
            font=("Monospace", 9),
            relief=tk.FLAT,
            padx=8,
            pady=6,
            state=tk.DISABLED,
        )
        self.output_meta.tag_configure("error", foreground="#ff7b72")
        self.output_meta.tag_configure("ok", foreground="#7ee787")
        self.output_meta.pack(fill=tk.X, padx=4, pady=(0, 4))

    def _bind_events(self) -> None:
        self.editor.bind("<KeyRelease>", self._schedule_highlight)
        self.editor.bind("<Control-Return>", lambda _e: (self.run_editor(), "break")[1])
        self.editor.bind("<F5>", lambda _e: (self.run_editor(), "break")[1])
        self.editor.bind("<Tab>", self._insert_spaces)
        self.editor.bind("<Control-plus>", lambda _e: (self.adjust_editor_font_size(1), "break")[1])
        self.editor.bind("<Control-equal>", lambda _e: (self.adjust_editor_font_size(1), "break")[1])
        self.editor.bind("<Control-minus>", lambda _e: (self.adjust_editor_font_size(-1), "break")[1])
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def _resolve_editor_font_family(self) -> str:
        """Pick JetBrains Mono if installed on the system, falling back to Monospace."""
        try:
            installed = {name.lower(): name for name in tkfont.families(self.root)}
        except tk.TclError:
            return "Monospace"
        for candidate in ("jetbrains mono", "jetbrains mono nl", "fira code", "cascadia code", "monospace"):
            if candidate in installed:
                return installed[candidate]
        return "Monospace"

    def set_editor_font_size(self, size: int) -> int:
        """Clamp and apply editor font size across editor widget, syntax tags, and header label."""
        clamped = max(8, min(28, int(size)))
        self._editor_font_size = clamped
        self._editor_font_size_var.set(f"{clamped}pt")
        for font_obj in (self._editor_font, self._editor_font_bold, self._editor_font_italic):
            font_obj.configure(size=clamped)
        self.font_dec_btn.state(["disabled"] if clamped <= 8 else ["!disabled"])
        self.font_inc_btn.state(["disabled"] if clamped >= 28 else ["!disabled"])
        return clamped

    def adjust_editor_font_size(self, delta: int) -> int:
        """Increment or decrement the editor font size by delta points."""
        return self.set_editor_font_size(self._editor_font_size + delta)

    def _insert_spaces(self, _event: tk.Event[Any]) -> str:
        self.editor.insert(tk.INSERT, "    ")
        return "break"

    def _schedule_highlight(self, _event: tk.Event[Any] | None = None) -> None:
        if self._highlight_after_id is not None:
            self.root.after_cancel(self._highlight_after_id)
        self._highlight_after_id = self.root.after(40, self.highlight_editor)

    def highlight_editor(self) -> None:
        """Apply VQL syntax highlight tags across the editor buffer."""
        self._highlight_after_id = None
        src = self.editor.get("1.0", "end-1c")
        for tag in ("keyword", "type", "constant", "aggregate", "param", "string", "number", "op", "comment"):
            self.editor.tag_remove(tag, "1.0", tk.END)
        for span in tokenize_vql(src):
            self.editor.tag_add(span.tag, f"1.0+{span.start}c", f"1.0+{span.end}c")

    def _set_editor_text(self, text: str) -> None:
        self.editor.delete("1.0", tk.END)
        self.editor.insert("1.0", text)
        self.highlight_editor()

    def _on_snippet_selected(self, _event: tk.Event[Any]) -> None:
        key = self.snippet_var.get()
        if key in SNIPPETS:
            self._set_editor_text(SNIPPETS[key])

    def _on_file_selected(self, _event: tk.Event[Any]) -> None:
        sel = self.file_tree.selection()
        if not sel:
            return
        self._selected_file = Path(sel[0])
        self._refresh_file_preview()

    def _refresh_file_preview(self) -> None:
        content = inspect_db_file(self._selected_file)
        self.file_preview.configure(state=tk.NORMAL)
        self.file_preview.delete("1.0", tk.END)
        self.file_preview.insert("1.0", content)
        self.file_preview.configure(state=tk.DISABLED)

    def refresh_db_pane(self) -> None:
        """Refresh the temporary directory file tree, DB stats, and file preview."""
        payload = self.session.stats_payload(selected_file=self._selected_file.name)
        self.db_path_label.configure(text=f"Temp DB: {payload['wal_path']}")
        for item in self.file_tree.get_children():
            self.file_tree.delete(item)

        files = self.session.list_files()
        for name, size, path in files:
            self.file_tree.insert("", tk.END, iid=str(path), text=name, values=(vex_cli._fmt_bytes(size),))

        if files and str(self._selected_file) not in {str(p) for _, _, p in files}:
            self._selected_file = files[0][2]
        if self._selected_file.exists():
            self.file_tree.selection_set(str(self._selected_file))

        st = payload["stats"]
        self.stats_label.configure(
            text=(
                f"LSN: {st['wal_lsn']} | Queries: {st['queries_executed']} | "
                f"Mutations: {st['mutations_committed']}\n"
                f"Checkpoints: {st['checkpoints']} | Dead: {st['dead_rows']} | "
                f"Pruned: {st['chunks_pruned']}\n"
                f"Logs: {st['logs_emitted']} (drop {st['logs_dropped']}) | "
                f"Disk: {payload['total_disk_human']} | RSS: {payload['process_rss_human']}"
            )
        )
        self._refresh_file_preview()

    def _get_target_script(self) -> str:
        try:
            return self.editor.get(tk.SEL_FIRST, tk.SEL_LAST)
        except tk.TclError:
            return self.editor.get("1.0", "end-1c")

    def run_editor(self) -> None:
        """Execute selected VQL text (or entire editor buffer) and update panes."""
        script = self._get_target_script()
        try:
            self.last_snapshot = self.session.run_script(script)
            self.last_error = None
        except vex.VexError as exc:
            self.last_error = str(exc)
        self.refresh_db_pane()
        self.render_output()

    def do_checkpoint(self) -> None:
        """Create an atomic .snap checkpoint and refresh the temp DB file pane."""
        try:
            self.session.checkpoint()
            self.last_error = None
        except vex.VexError as exc:
            self.last_error = f"Checkpoint failed: {exc}"
        self.refresh_db_pane()
        self._update_meta_box(
            self.last_error or "Checkpoint written (.wal compacted & .snap created).",
            is_error=self.last_error is not None,
        )

    def reset_temp_db(self) -> None:
        """Close the current temporary DB and open a fresh one."""
        self.session.open_new()
        self._selected_file = self.session.wal_path
        self.last_snapshot = None
        self.last_error = None
        self.refresh_db_pane()
        self.render_output()

    def _update_meta_box(self, text: str, *, is_error: bool) -> None:
        self.output_meta.configure(state=tk.NORMAL)
        self.output_meta.delete("1.0", tk.END)
        self.output_meta.insert("1.0", text, ("error" if is_error else "ok",))
        self.output_meta.configure(state=tk.DISABLED)

    def _set_output_text(self, text: str) -> None:
        self.output_text.configure(state=tk.NORMAL)
        self.output_text.delete("1.0", tk.END)
        self.output_text.insert("1.0", text)
        self.output_text.configure(state=tk.DISABLED)

    def render_output(self) -> None:
        """Render the current QuerySnapshot in Table, Dictionary, or List format."""
        mode = self.output_mode.get()
        self.table_frame.pack_forget()
        self.text_frame.pack_forget()

        if self.last_error is not None:
            self.text_frame.pack(fill=tk.BOTH, expand=True)
            self._set_output_text(f"VexError: {self.last_error}")
            self._update_meta_box(f"Error: {self.last_error}", is_error=True)
            self.status_label.configure(text=f"Error | SIMD: {vex.simd_backend()}")
            return

        snap = self.last_snapshot
        if snap is None:
            self.text_frame.pack(fill=tk.BOTH, expand=True)
            self._set_output_text("Ready. Press Run (Ctrl+Enter) to execute VQL.")
            self._update_meta_box("Ready.", is_error=False)
            self.status_label.configure(text=f"Ready | SIMD: {vex.simd_backend()}")
            return

        if mode == "table" and snap.columns:
            self.table_frame.pack(fill=tk.BOTH, expand=True)
            self._populate_tree(snap)
        else:
            self.text_frame.pack(fill=tk.BOTH, expand=True)
            if mode == "dict":
                self._set_output_text(format_output_dicts(snap))
            elif mode == "list":
                self._set_output_text(format_output_list(snap))
            else:
                self._set_output_text(format_output_table(snap))

        meta = f"{snap.status_log}\n({len(snap.rows)} rows, {snap.statements_run} stmt(s), {snap.elapsed_ms:.2f} ms)"
        if snap.explain:
            meta = f"{snap.explain}\n{meta}"
        self._update_meta_box(meta, is_error=False)
        self.status_label.configure(
            text=f"{len(snap.rows)} rows | {snap.elapsed_ms:.2f} ms | SIMD: {vex.simd_backend()}"
        )

    def _populate_tree(self, snap: QuerySnapshot) -> None:
        self.result_tree.delete(*self.result_tree.get_children())
        col_ids = [f"c{i}" for i in range(len(snap.columns))]
        self.result_tree["columns"] = col_ids
        for cid, name, dtype in zip(col_ids, snap.columns, snap.dtypes):
            anchor = tk.E if dtype in vex_cli.NUMERIC_DTYPES else tk.W
            self.result_tree.heading(cid, text=f"{name} ({dtype})", anchor=anchor)
            self.result_tree.column(cid, width=130, anchor=anchor, stretch=True)

        for row in snap.rows:
            vals = [vex_cli._fmt_cell(cell, 96) for cell in row]
            self.result_tree.insert("", tk.END, values=vals)

    def close(self) -> None:
        """Close the temporary database cleanly and destroy the window."""
        self.session.close()
        if self.root.winfo_exists():
            self.root.destroy()


def main() -> int:
    """Launch the Vex VQL Studio GUI."""
    try:
        root = tk.Tk()
    except tk.TclError as exc:
        print(f"Cannot initialize Tkinter GUI: {exc}", file=sys.stderr)
        return 1

    app = VexStudioApp(root)
    try:
        root.mainloop()
    finally:
        app.session.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
