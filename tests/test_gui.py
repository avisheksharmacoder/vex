import json
import os
import sys
import threading
import tkinter as tk
import urllib.request
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))

import vex_gui
from vex import web_studio


def test_tokenize_vql_covers_keywords_types_params_strings_and_comments():
    src = (
        "# comment\n"
        'table users { id: u64 key, name: str }\n'
        'users[age >= $min, active == true] name sort -age [:5]\n'
        'users by name { n: count(), total: sum(age) }'
    )
    spans = vex_gui.tokenize_vql(src)
    by_text = {(span.tag, src[span.start : span.end]) for span in spans}

    assert ("comment", "# comment") in by_text
    assert ("keyword", "table") in by_text
    assert ("keyword", "key") in by_text
    assert ("keyword", "sort") in by_text
    assert ("keyword", "by") in by_text
    assert ("type", "u64") in by_text
    assert ("type", "str") in by_text
    assert ("param", "$min") in by_text
    assert ("constant", "true") in by_text
    assert ("aggregate", "count") in by_text
    assert ("aggregate", "sum") in by_text
    assert ("number", "5") in by_text
    assert ("op", ">=") in by_text


def test_split_vql_statements_handles_multiline_semicolons_and_strings():
    script = (
        "# setup\n"
        "table t {\n"
        "  id: u64 key,\n"
        "  note: str\n"
        "}\n"
        't += { id: 1, note: "a;b" }; t[1] id, note\n'
        "t[id >= 1]\n"
        "  by note { n: count() }\n"
        "  sort -n\n"
    )
    stmts = vex_gui.split_vql_statements(script)
    assert len(stmts) == 4
    assert stmts[0].startswith("table t {")
    assert stmts[1] == 't += { id: 1, note: "a;b" }'
    assert stmts[2] == "t[1] id, note"
    assert stmts[3].startswith("t[id >= 1]") and "sort -n" in stmts[3]


def test_temp_session_execution_formats_and_checkpoint():
    session = vex_gui.TempVexSession()
    db_dir = session.db_dir
    try:
        assert session.wal_path.exists()
        assert any(name == "session.wal" for name, _sz, _p in session.list_files())

        snap = session.run_script(vex_gui.DEFAULT_SCRIPT)
        assert snap.columns == ("country", "n", "total_rev", "avg_rev")
        assert len(snap.rows) == 2

        tbl = vex_gui.format_output_table(snap)
        dicts = vex_gui.format_output_dicts(snap)
        lst = vex_gui.format_output_list(snap)
        assert "IN" in tbl and "JP" in tbl
        assert "'country': 'IN'" in dicts
        assert "('IN', 3," in lst

        # All other dropdown snippets execute cleanly against the seeded tables
        for name, snippet in vex_gui.SNIPPETS.items():
            if name == "Quickstart (Tables + Join + Agg)":
                continue
            s_snap = session.run_script(snippet)
            assert s_snap.statements_run >= 1

        session.checkpoint()
        names = {name for name, _sz, _p in session.list_files()}
        assert "session.wal" in names
        assert "session.wal.snap" in names

        preview = vex_gui.inspect_db_file(session.wal_path)
        assert "session.wal" in preview
        assert "Magic:" in preview
    finally:
        session.close()

    assert not db_dir.exists()


def test_tk_three_pane_app_smoke():
    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        pytest.skip("No graphical display available")

    root = tk.Tk()
    root.withdraw()
    app = vex_gui.VexStudioApp(root)
    try:
        db_dir = app.session.db_dir
        assert len(app.paned.panes()) == 3
        assert app.last_snapshot is not None
        assert len(app.result_tree.get_children()) == 2

        from tkinter import font as tkfont

        installed = {name.lower() for name in tkfont.families(root)}
        if "jetbrains mono" in installed or "jetbrains mono nl" in installed:
            assert "jetbrains mono" in app._editor_font_family.lower()

        assert app._editor_font_size == 11
        app.font_inc_btn.invoke()
        assert app._editor_font_size == 12
        assert app._editor_font_size_var.get() == "12pt"
        assert app._editor_font.cget("size") == 12
        assert app._editor_font_bold.cget("size") == 12
        app.font_dec_btn.invoke()
        assert app._editor_font_size == 11
        assert app.set_editor_font_size(2) == 8
        assert app.set_editor_font_size(99) == 28
        assert app.set_editor_font_size(11) == 11

        # Switch through Table -> Dictionary -> List output formats
        app.output_mode.set("dict")
        app.render_output()
        assert "'country': 'IN'" in app.output_text.get("1.0", tk.END)

        app.output_mode.set("list")
        app.render_output()
        assert "('IN', 3," in app.output_text.get("1.0", tk.END)

        app.output_mode.set("table")
        app.render_output()
        assert len(app.result_tree.get_children()) == 2

        # Checkpoint creates session.wal.snap inside Pane 1's tree
        app.do_checkpoint()
        assert any(Path(iid).name == "session.wal.snap" for iid in app.file_tree.get_children())
    finally:
        app.close()

    assert not db_dir.exists()


def test_web_studio_http_endpoints_and_static_assets():
    srv = web_studio.create_server("127.0.0.1", 0)
    db_dir = srv.session.db_dir
    port = srv.server_address[1]
    thread = threading.Thread(target=srv.serve_forever, daemon=True)
    thread.start()

    base = f"http://127.0.0.1:{port}"
    try:
        html = urllib.request.urlopen(f"{base}/").read().decode("utf-8")
        css = urllib.request.urlopen(f"{base}/static/studio.css").read().decode("utf-8")
        js = urllib.request.urlopen(f"{base}/static/studio.js").read().decode("utf-8")
        assert "VQL Studio" in html
        assert "btn-font-dec" in html
        assert "btn-font-inc" in html
        assert "font-size-label" in html
        assert "btn-toggle-left" in html
        assert "btn-toggle-right" in html
        assert "btn-docs-fab" in html
        assert "docs-dialog" in html
        assert "docs-accordion" in html
        assert "--tok-kw" in css
        assert "JetBrains Mono NL" in css
        assert "--editor-font-size" in css
        assert ".pane.collapsed" in css
        assert ".pane-collapse-btn" in css
        assert ".docs-fab" in css
        assert ".docs-dialog" in css
        assert "highlightVql" in js
        assert "setEditorFontSize" in js
        assert "toggleSidePane" in js
        assert "VQL_KEYWORD_DOCS" in js
        for kw in ("table", "del", "sort", "by", "index", "unique"):
            assert f'keyword: "{kw}"' in js

        state_resp = json.loads(urllib.request.urlopen(f"{base}/api/state").read().decode("utf-8"))
        assert state_resp["ok"] is True
        assert state_resp["state"]["files"][0]["name"] == "session.wal"

        req = urllib.request.Request(
            f"{base}/api/run",
            data=json.dumps({"script": web_studio.DEFAULT_SCRIPT}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        run_resp = json.loads(urllib.request.urlopen(req).read().decode("utf-8"))
        assert run_resp["ok"] is True
        assert run_resp["result"]["columns"] == ["country", "n", "total_rev", "avg_rev"]
        assert len(run_resp["result"]["rows"]) == 2
        assert run_resp["state"]["stats"]["queries_executed"] >= 1
        assert "logs_emitted" in run_resp["state"]["stats"]

        ckpt_req = urllib.request.Request(
            f"{base}/api/checkpoint",
            data=b"{}",
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        ckpt_resp = json.loads(urllib.request.urlopen(ckpt_req).read().decode("utf-8"))
        assert ckpt_resp["ok"] is True
        names = {f["name"] for f in ckpt_resp["state"]["files"]}
        assert "session.wal.snap" in names
    finally:
        srv.shutdown()
        srv.server_close()
        thread.join(timeout=5.0)

    assert not db_dir.exists()
