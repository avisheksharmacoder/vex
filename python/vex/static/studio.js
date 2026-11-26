"use strict";

(() => {
  const VQL_KEYWORDS = new Set([
    "table", "del", "sort", "by", "index", "unique", "and", "or", "not", "in", "key",
  ]);
  const VQL_TYPES = new Set(["i64", "u64", "f64", "bool", "str"]);
  const VQL_CONSTANTS = new Set(["true", "false", "null"]);
  const VQL_AGGS = new Set(["count", "sum", "avg", "min", "max"]);
  const NUMERIC_DTYPES = new Set(["i64", "u64", "f64"]);

  const TOKEN_RE = new RegExp(
    [
      "(?<comment>#[^\\n]*)",
      "(?<string>\"(?:\\\\.|[^\"\\\\\\n])*\"|'(?:\\\\.|[^'\\\\\\n])*'|`[^`\\n]+`)",
      "(?<param>\\$[A-Za-z_][A-Za-z0-9_]*)",
      "(?<number>\\b\\d+(?:\\.\\d+)?(?:[eE][+-]?\\d+)?\\b)",
      "(?<op>\\+\\+|\\+=|\\|=|-=|->|==|!=|<=|>=|[&+\\-*/%=<>?:])",
      "(?<ident>\\b[A-Za-z_][A-Za-z0-9_]*\\b)",
    ].join("|"),
    "g"
  );

  const el = {
    simdChip: document.getElementById("simd-chip"),
    btnRun: document.getElementById("btn-run"),
    runLabel: document.getElementById("run-label"),
    btnCheckpoint: document.getElementById("btn-checkpoint"),
    btnReset: document.getElementById("btn-reset"),
    btnRefresh: document.getElementById("btn-refresh"),
    btnClear: document.getElementById("btn-clear"),
    btnFontDec: document.getElementById("btn-font-dec"),
    btnFontInc: document.getElementById("btn-font-inc"),
    fontSizeLabel: document.getElementById("font-size-label"),
    btnTheme: document.getElementById("btn-theme"),
    themeIcon: document.getElementById("theme-icon"),
    btnCopy: document.getElementById("btn-copy"),
    snippetSelect: document.getElementById("snippet-select"),
    dbPath: document.getElementById("db-path"),
    diskTotal: document.getElementById("disk-total"),
    fileList: document.getElementById("file-list"),
    rssChip: document.getElementById("rss-chip"),
    metricsGrid: document.getElementById("metrics-grid"),
    hexFilename: document.getElementById("hex-filename"),
    hexPreview: document.getElementById("hex-preview"),
    editor: document.getElementById("vql-editor"),
    gutter: document.getElementById("editor-gutter"),
    highlightLayer: document.getElementById("highlight-layer"),
    highlightCode: document.getElementById("highlight-code"),
    cursorPos: document.getElementById("cursor-pos"),
    outputViewport: document.getElementById("output-viewport"),
    resultSummary: document.getElementById("result-summary"),
    execStatusPill: document.getElementById("exec-status-pill"),
    traceLog: document.getElementById("trace-log"),
    segBtns: Array.from(document.querySelectorAll(".seg-btn")),
    workspace: document.getElementById("workspace"),
    paneLeft: document.getElementById("pane-left"),
    paneRight: document.getElementById("pane-right"),
    splitterLeft: document.getElementById("splitter-left"),
    splitterRight: document.getElementById("splitter-right"),
    btnToggleLeft: document.getElementById("btn-toggle-left"),
    btnToggleRight: document.getElementById("btn-toggle-right"),
    btnDocsFab: document.getElementById("btn-docs-fab"),
    docsBackdrop: document.getElementById("docs-backdrop"),
    docsDialog: document.getElementById("docs-dialog"),
    btnDocsClose: document.getElementById("btn-docs-close"),
    btnDocsExpandAll: document.getElementById("btn-docs-expand-all"),
    docsAccordion: document.getElementById("docs-accordion"),
  };

  let snippets = {};
  let selectedFile = "session.wal";
  let outputMode = "table";
  let lastResponse = null;
  const FONT_MIN = 10;
  const FONT_MAX = 26;
  const FONT_STORAGE_KEY = "vex.studio.editorFontSize";
  let editorFontSize = 13;

  const VQL_KEYWORD_DOCS = [
    {
      keyword: "table",
      role: "DDL · Schema",
      summary: "Create typed columnar tables with optional primary and foreign keys.",
      description:
        "Declares a 64-byte aligned in-memory columnar table and logs the schema DDL in the WAL. Supported types are i64, u64, f64, bool, and str. Mark a column with key for an O(1) Robin Hood primary key hash index, and use -> parent.col to declare a foreign key for automatic joins.",
      syntax: "table <name> { <col>: <type> [key | -> <tbl>.<col>], ... }",
      examples: [
        {
          title: "Primary key table definition",
          code: [
            "table users {",
            "    id: u64 key,",
            "    name: str,",
            "    country: str,",
            "    age: i64",
            "}",
          ].join("\n"),
        },
        {
          title: "Table with foreign key relationship",
          code: [
            "table orders {",
            "    id: u64 key,",
            "    user_id: u64 -> users.id,",
            "    amount: f64,",
            "    paid: bool",
            "}",
          ].join("\n"),
        },
      ],
    },
    {
      keyword: "del",
      role: "DML / DDL · Delete",
      summary: "Delete matching rows safely or drop a table schema with del table.",
      description:
        "Removes rows matching a primary key, predicate expression, or explicit slice [:]. As a compile-time safety guard, bare del tbl without [...] is rejected to prevent accidental data loss. Use del table <name> to drop an entire table definition and its data.",
      syntax: "del <table>[<selector>]   |   del table <name>",
      examples: [
        {
          title: "Point primary-key delete and filtered delete",
          code: [
            "del users[3]",
            "del orders[paid == false, amount < 100.0]",
          ].join("\n"),
        },
        {
          title: "Explicit full-table truncate vs dropping schema",
          code: [
            "del orders[:]       # removes all rows safely",
            "del table orders    # drops the table schema",
          ].join("\n"),
        },
      ],
    },
    {
      keyword: "sort",
      role: "Pipeline · Order",
      summary: "Order query results ascending or descending (-col) with optional Top-K.",
      description:
        "Appends an ordering stage to any table scan, join, or aggregation pipeline. Columns sort ascending by default; prefix any column or alias with - for descending order. Chaining a slice like [:10] automatically turns sort into a bounded Top-K heap execution.",
      syntax: "... sort [-]expr1[, [-]expr2, ...] [[:limit]]",
      examples: [
        {
          title: "Multi-column sort combined with slice limit",
          code: [
            "users[age >= 20] id, name, country, age",
            "    sort country, -age [:10]",
          ].join("\n"),
        },
        {
          title: "Sort aggregated totals descending (Top-5)",
          code: [
            "orders[paid == true] by user_id { total: sum(amount) }",
            "    sort -total [:5]",
          ].join("\n"),
        },
      ],
    },
    {
      keyword: "by",
      role: "Pipeline · Aggregate",
      summary: "Group rows for vector aggregates or distinct column combinations.",
      description:
        "Groups pipeline rows on one or more key columns and computes count(), sum(col), avg(col), min(col), and max(col) inside {...}. Omitting {...} produces distinct key combinations. Appending [predicate] right after {...} filters aggregated groups (HAVING).",
      syntax: "... by col1[, col2] [{ alias: agg(col), ... }] [[having_pred]]",
      examples: [
        {
          title: "Auto-join, group aggregation, HAVING filter, and sort",
          code: [
            "orders[paid == true] & users",
            "    by users.country { n: count(), total_rev: sum(amount), avg_rev: avg(amount) }",
            "    [total_rev > 200.0]",
            "    sort -total_rev",
          ].join("\n"),
        },
        {
          title: "Distinct combinations without an aggregate block",
          code: "users by country, age sort country, age",
        },
      ],
    },
    {
      keyword: "index",
      role: "DDL · Secondary Index",
      summary: "Register single or composite column indexes on an existing table.",
      description:
        "Attaches secondary index metadata covering one or more columns on a table so query planning (?) and filtered scans can track indexed columns. Works cleanly for both single-column lookups and composite multi-column filters.",
      syntax: "index <table>(<col1>[, <col2>, ...])",
      examples: [
        {
          title: "Create single-column index and inspect plan",
          code: [
            "index users(country)",
            '? users[country == "IN"] id, name, age',
          ].join("\n"),
        },
        {
          title: "Create composite multi-column index",
          code: [
            "index orders(user_id, paid)",
            "orders[user_id == 1, paid == true] id, amount",
          ].join("\n"),
        },
      ],
    },
    {
      keyword: "unique",
      role: "Constraint · Unique",
      summary: "Enforce single or composite uniqueness across existing and future rows.",
      description:
        "Adds a unique constraint on one or more non-primary-key columns. Vex immediately back-verifies all existing live rows (failing if duplicates exist) and enforces uniqueness on every subsequent insert (+=), upsert (|=), and point update ([pk] =).",
      syntax: "unique <table>(<col1>[, <col2>, ...])",
      examples: [
        {
          title: "Enforce unique email addresses",
          code: [
            "table accounts { id: u64 key, email: str, plan: str }",
            "unique accounts(email)",
            'accounts += { id: 1, email: "ada@vex.dev", plan: "pro" }',
          ].join("\n"),
        },
        {
          title: "Composite unique key per organization",
          code: [
            "table memberships { id: u64 key, org_id: u64, handle: str }",
            "unique memberships(org_id, handle)",
            'memberships += { id: 1, org_id: 10, handle: "core" }',
          ].join("\n"),
        },
      ],
    },
  ];

  function setEditorFontSize(px) {
    const next = Math.max(FONT_MIN, Math.min(FONT_MAX, Number(px) || 13));
    editorFontSize = next;
    document.documentElement.style.setProperty("--editor-font-size", `${next}px`);
    if (el.fontSizeLabel) el.fontSizeLabel.textContent = `${next}px`;
    if (el.btnFontDec) el.btnFontDec.disabled = next <= FONT_MIN;
    if (el.btnFontInc) el.btnFontInc.disabled = next >= FONT_MAX;
    try {
      localStorage.setItem(FONT_STORAGE_KEY, String(next));
    } catch (_err) {
      // Ignore storage errors in sandboxed/private origins.
    }
    syncEditorScroll();
  }

  function escapeHtml(text) {
    return String(text)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  function highlightVql(src) {
    TOKEN_RE.lastIndex = 0;
    let out = "";
    let last = 0;
    let match;
    while ((match = TOKEN_RE.exec(src)) !== null) {
      if (match.index > last) {
        out += escapeHtml(src.slice(last, match.index));
      }
      const g = match.groups;
      const raw = match[0];
      let cls = "";
      if (g.comment) cls = "tok-comment";
      else if (g.string) cls = "tok-string";
      else if (g.param) cls = "tok-param";
      else if (g.number) cls = "tok-number";
      else if (g.op) cls = "tok-op";
      else if (g.ident) {
        if (VQL_KEYWORDS.has(raw)) cls = "tok-keyword";
        else if (VQL_TYPES.has(raw)) cls = "tok-type";
        else if (VQL_CONSTANTS.has(raw)) cls = "tok-constant";
        else if (VQL_AGGS.has(raw)) cls = "tok-aggregate";
      }
      out += cls ? `<span class="${cls}">${escapeHtml(raw)}</span>` : escapeHtml(raw);
      last = match.index + raw.length;
    }
    if (last < src.length) {
      out += escapeHtml(src.slice(last));
    }
    if (src.endsWith("\n") || src.length === 0) {
      out += " ";
    }
    return out;
  }

  function syncEditorHighlight() {
    const src = el.editor.value;
    el.highlightCode.innerHTML = highlightVql(src);
    const lineCount = src.split("\n").length;
    let nums = "";
    for (let i = 1; i <= lineCount; i++) {
      nums += (i > 1 ? "\n" : "") + i;
    }
    el.gutter.textContent = nums;
    syncEditorScroll();
  }

  function syncEditorScroll() {
    el.highlightLayer.scrollTop = el.editor.scrollTop;
    el.highlightLayer.scrollLeft = el.editor.scrollLeft;
    el.gutter.scrollTop = el.editor.scrollTop;
  }

  function updateCursorInfo() {
    const start = el.editor.selectionStart || 0;
    const end = el.editor.selectionEnd || 0;
    const prefix = el.editor.value.slice(0, start);
    const lines = prefix.split("\n");
    const ln = lines.length;
    const col = lines[lines.length - 1].length + 1;
    el.cursorPos.textContent = `Ln ${ln}, Col ${col}`;
    el.runLabel.textContent = end > start ? "Run Selection" : "Run";
  }

  function renderState(state) {
    if (!state) return;
    selectedFile = state.selected_file || selectedFile;
    el.simdChip.textContent = `SIMD: ${String(state.simd_backend || "").toUpperCase()}`;
    el.dbPath.textContent = state.wal_path || state.db_dir;
    el.diskTotal.textContent = `Total: ${state.total_disk_human}`;
    el.rssChip.textContent = `RSS: ${state.process_rss_human}`;
    el.hexFilename.textContent = selectedFile;
    el.hexPreview.textContent = state.file_preview || "";

    el.fileList.innerHTML = (state.files || [])
      .map((f) => {
        const active = f.name === selectedFile ? "active" : "";
        return `
          <div class="file-item ${active}" data-file="${escapeHtml(f.name)}">
            <span>${escapeHtml(f.name)}</span>
            <span class="file-item-meta">${escapeHtml(f.size_human)} • ${escapeHtml(f.modified)}</span>
          </div>
        `;
      })
      .join("");

    const st = state.stats || {};
    const metricEntries = [
      ["WAL LSN", st.wal_lsn ?? 0],
      ["Queries", st.queries_executed ?? 0],
      ["Mutations", st.mutations_committed ?? 0],
      ["Checkpoints", st.checkpoints ?? 0],
      ["Dead Rows", st.dead_rows ?? 0],
      ["Chunks Pruned", st.chunks_pruned ?? 0],
      ["Logs Emitted", st.logs_emitted ?? 0],
      ["Logs Dropped", st.logs_dropped ?? 0],
    ];
    el.metricsGrid.innerHTML = metricEntries
      .map(
        ([label, val]) => `
        <div class="metric-box">
          <span class="metric-name">${escapeHtml(label)}</span>
          <span class="metric-val">${escapeHtml(val)}</span>
        </div>
      `
      )
      .join("");
  }

  function formatCellValue(val) {
    if (val === null || val === undefined) {
      return '<span class="null-pill">null</span>';
    }
    if (typeof val === "boolean") {
      return `<span class="out-bool">${val ? "true" : "false"}</span>`;
    }
    if (typeof val === "number") {
      return `<span class="out-num">${escapeHtml(val)}</span>`;
    }
    return escapeHtml(val);
  }

  function highlightStructuredText(text) {
    return escapeHtml(text).replace(
      /('[^']*':)|('[^']*')|(\b\d+(?:\.\d+)?\b)|(\bTrue\b|\bFalse\b|\bNone\b)/g,
      (m, key, str, num, lit) => {
        if (key) return `<span class="out-key">${key}</span>`;
        if (str) return `<span class="out-str">${str}</span>`;
        if (num) return `<span class="out-num">${num}</span>`;
        if (lit) return `<span class="${lit === "None" ? "out-null" : "out-bool"}">${lit}</span>`;
        return m;
      }
    );
  }

  function renderOutput() {
    if (!lastResponse) {
      el.outputViewport.innerHTML =
        '<div class="empty-state">Press <strong>Run</strong> (<code>Ctrl+Enter</code>) to execute VQL.</div>';
      el.resultSummary.textContent = "Ready";
      el.execStatusPill.textContent = "Idle";
      el.execStatusPill.className = "status-pill";
      el.traceLog.textContent = "Ready.";
      return;
    }

    if (!lastResponse.ok) {
      const err = lastResponse.error || "Unknown error";
      el.outputViewport.innerHTML = `<div class="error-banner">VexError: ${escapeHtml(err)}</div>`;
      el.resultSummary.textContent = "Error";
      el.execStatusPill.textContent = "Error";
      el.execStatusPill.className = "status-pill err";
      el.traceLog.textContent = err;
      return;
    }

    const res = lastResponse.result;
    if (!res) {
      return;
    }

    const nrows = (res.rows || []).length;
    el.resultSummary.textContent =
      `${nrows} row(s) • ${res.statements_run} stmt(s) • ${Number(res.elapsed_ms).toFixed(2)} ms`;
    el.execStatusPill.textContent = "OK";
    el.execStatusPill.className = "status-pill ok";
    el.traceLog.textContent = (res.explain ? `${res.explain}\n` : "") + (res.status_log || "");

    if (res.explain && (!res.columns || res.columns.length === 0)) {
      el.outputViewport.innerHTML = `<pre class="code-output">${escapeHtml(res.explain)}</pre>`;
      return;
    }

    if (outputMode === "table") {
      if (!res.columns || res.columns.length === 0) {
        el.outputViewport.innerHTML = `<pre class="code-output">${escapeHtml(res.status_log)}</pre>`;
        return;
      }
      const ths = res.columns
        .map((c, idx) => {
          const dt = (res.dtypes && res.dtypes[idx]) || "";
          const cls = NUMERIC_DTYPES.has(dt) ? "num" : "";
          return `<th class="${cls}">${escapeHtml(c)}<span class="dtype-badge">${escapeHtml(dt)}</span></th>`;
        })
        .join("");

      const trs = (res.rows || [])
        .map((row, rIdx) => {
          const tds = row
            .map((cell, cIdx) => {
              const dt = (res.dtypes && res.dtypes[cIdx]) || "";
              const cls = NUMERIC_DTYPES.has(dt) ? "num" : "";
              return `<td class="${cls}">${formatCellValue(cell)}</td>`;
            })
            .join("");
          return `<tr><td class="row-idx">${rIdx + 1}</td>${tds}</tr>`;
        })
        .join("");

      el.outputViewport.innerHTML = `
        <table class="data-table">
          <thead><tr><th class="row-idx">#</th>${ths}</tr></thead>
          <tbody>${trs}</tbody>
        </table>
      `;
    } else if (outputMode === "dict") {
      el.outputViewport.innerHTML =
        `<pre class="code-output">${highlightStructuredText(res.dict_text || "[]")}</pre>`;
    } else {
      el.outputViewport.innerHTML =
        `<pre class="code-output">${highlightStructuredText(res.list_text || "[]")}</pre>`;
    }
  }

  async function apiGet(url) {
    const resp = await fetch(url);
    return resp.json();
  }

  async function apiPost(url, payload) {
    const resp = await fetch(url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload || {}),
    });
    return resp.json();
  }

  async function runEditor() {
    const start = el.editor.selectionStart || 0;
    const end = el.editor.selectionEnd || 0;
    const script = end > start ? el.editor.value.slice(start, end) : el.editor.value;
    const data = await apiPost("/api/run", { script, selected_file: selectedFile });
    renderState(data.state);
    lastResponse = data;
    renderOutput();
  }

  async function runCheckpoint() {
    const data = await apiPost("/api/checkpoint", { selected_file: selectedFile });
    renderState(data.state);
    if (data.ok) {
      el.execStatusPill.textContent = "Checkpointed";
      el.execStatusPill.className = "status-pill ok";
      el.traceLog.textContent = "Checkpoint complete (.wal compacted & .snap created).";
    } else {
      el.execStatusPill.textContent = "Error";
      el.execStatusPill.className = "status-pill err";
      el.traceLog.textContent = data.error || "Checkpoint failed";
    }
  }

  async function resetTempDb() {
    const data = await apiPost("/api/reset", {});
    selectedFile = "session.wal";
    renderState(data.state);
    lastResponse = null;
    renderOutput();
  }

  async function refreshState(fileOverride) {
    const target = fileOverride || selectedFile;
    const data = await apiGet(`/api/file?name=${encodeURIComponent(target)}`);
    renderState(data.state);
  }

  function initSplitters() {
    let active = null;
    document.querySelectorAll(".splitter").forEach((sp) => {
      sp.addEventListener("pointerdown", (ev) => {
        active = sp.dataset.splitter;
        sp.classList.add("dragging");
        sp.setPointerCapture(ev.pointerId);
      });
      sp.addEventListener("pointermove", (ev) => {
        if (!active) return;
        const rect = el.workspace.getBoundingClientRect();
        const relX = ev.clientX - rect.left;
        if (active === "left") {
          const pct = Math.max(18, Math.min(45, (relX / rect.width) * 100));
          document.documentElement.style.setProperty("--pane-left", `${pct.toFixed(1)}%`);
        } else if (active === "right") {
          const pct = Math.max(22, Math.min(52, ((rect.right - ev.clientX) / rect.width) * 100));
          document.documentElement.style.setProperty("--pane-right", `${pct.toFixed(1)}%`);
        }
      });
      sp.addEventListener("pointerup", () => {
        active = null;
        sp.classList.remove("dragging");
      });
    });
  }

  function setDocPanelOpen(panel, open) {
    if (!panel) return;
    panel.classList.toggle("open", open);
    const trigger = panel.querySelector(".doc-panel-trigger");
    const body = panel.querySelector(".doc-panel-body");
    if (trigger) trigger.setAttribute("aria-expanded", open ? "true" : "false");
    if (body) body.hidden = !open;
  }

  function updateExpandAllLabel() {
    if (!el.docsAccordion || !el.btnDocsExpandAll) return;
    const panels = Array.from(el.docsAccordion.querySelectorAll(".doc-panel"));
    const allOpen = panels.length > 0 && panels.every((p) => p.classList.contains("open"));
    el.btnDocsExpandAll.textContent = allOpen ? "Collapse all" : "Expand all";
  }

  function renderDocsAccordion() {
    if (!el.docsAccordion) return;
    el.docsAccordion.innerHTML = VQL_KEYWORD_DOCS.map((item, kIdx) => {
      const examplesHtml = item.examples
        .map(
          (ex, eIdx) => `
            <div class="doc-example-card">
              <div class="doc-example-head">
                <span class="doc-example-title">${escapeHtml(ex.title)}</span>
                <div class="doc-example-actions">
                  <button class="doc-action-btn" data-doc-action="copy" data-kw-idx="${kIdx}" data-ex-idx="${eIdx}" type="button">Copy</button>
                  <button class="doc-action-btn primary" data-doc-action="load" data-kw-idx="${kIdx}" data-ex-idx="${eIdx}" type="button">Load in Editor</button>
                </div>
              </div>
              <pre class="doc-code"><code>${highlightVql(ex.code)}</code></pre>
            </div>
          `
        )
        .join("");

      return `
        <article class="doc-panel" data-keyword="${escapeHtml(item.keyword)}">
          <button
            class="doc-panel-trigger"
            type="button"
            aria-expanded="false"
            aria-controls="doc-panel-${escapeHtml(item.keyword)}"
          >
            <div class="doc-trigger-left">
              <code class="doc-kw-pill">${escapeHtml(item.keyword)}</code>
              <span class="doc-role">${escapeHtml(item.role)}</span>
              <span class="doc-summary">${escapeHtml(item.summary)}</span>
            </div>
            <span class="doc-chevron" aria-hidden="true">▾</span>
          </button>
          <div class="doc-panel-body" id="doc-panel-${escapeHtml(item.keyword)}" hidden>
            <p class="doc-desc">${escapeHtml(item.description)}</p>
            <div class="doc-syntax">
              <span class="doc-syntax-label">Syntax</span>
              <code>${highlightVql(item.syntax)}</code>
            </div>
            <div class="doc-examples">${examplesHtml}</div>
          </div>
        </article>
      `;
    }).join("");
    updateExpandAllLabel();
  }

  function openDocsDialog() {
    if (!el.docsBackdrop) return;
    el.docsBackdrop.hidden = false;
    if (el.btnDocsFab) el.btnDocsFab.setAttribute("aria-expanded", "true");
  }

  function closeDocsDialog() {
    if (!el.docsBackdrop || el.docsBackdrop.hidden) return;
    el.docsBackdrop.hidden = true;
    if (el.btnDocsFab) el.btnDocsFab.setAttribute("aria-expanded", "false");
  }

  function initDocsDialog() {
    renderDocsAccordion();

    if (el.btnDocsFab) {
      el.btnDocsFab.addEventListener("click", () => {
        if (el.docsBackdrop && el.docsBackdrop.hidden) openDocsDialog();
        else closeDocsDialog();
      });
    }
    if (el.btnDocsClose) {
      el.btnDocsClose.addEventListener("click", closeDocsDialog);
    }
    if (el.docsBackdrop) {
      el.docsBackdrop.addEventListener("click", (ev) => {
        if (ev.target === el.docsBackdrop) closeDocsDialog();
      });
    }
    if (el.btnDocsExpandAll && el.docsAccordion) {
      el.btnDocsExpandAll.addEventListener("click", () => {
        const panels = Array.from(el.docsAccordion.querySelectorAll(".doc-panel"));
        const allOpen = panels.length > 0 && panels.every((p) => p.classList.contains("open"));
        panels.forEach((panel) => setDocPanelOpen(panel, !allOpen));
        updateExpandAllLabel();
      });
    }
    if (el.docsAccordion) {
      el.docsAccordion.addEventListener("click", async (ev) => {
        const actionBtn = ev.target.closest("[data-doc-action]");
        if (actionBtn) {
          const item = VQL_KEYWORD_DOCS[Number(actionBtn.dataset.kwIdx)];
          const ex = item && item.examples[Number(actionBtn.dataset.exIdx)];
          if (!ex) return;
          if (actionBtn.dataset.docAction === "copy") {
            if (navigator.clipboard) {
              await navigator.clipboard.writeText(ex.code);
              actionBtn.textContent = "Copied!";
              setTimeout(() => {
                actionBtn.textContent = "Copy";
              }, 1100);
            }
          } else if (actionBtn.dataset.docAction === "load") {
            el.editor.value = ex.code;
            syncEditorHighlight();
            updateCursorInfo();
            closeDocsDialog();
            el.editor.focus();
          }
          return;
        }

        const trigger = ev.target.closest(".doc-panel-trigger");
        if (!trigger) return;
        const panel = trigger.closest(".doc-panel");
        if (!panel) return;
        setDocPanelOpen(panel, !panel.classList.contains("open"));
        updateExpandAllLabel();
      });
    }

    document.addEventListener("keydown", (ev) => {
      if (ev.key === "Escape" && el.docsBackdrop && !el.docsBackdrop.hidden) {
        closeDocsDialog();
      }
    });
  }

  function toggleSidePane(side, forceCollapsed) {
    const isLeft = side === "left";
    const pane = isLeft ? el.paneLeft : el.paneRight;
    const splitter = isLeft ? el.splitterLeft : el.splitterRight;
    const btn = isLeft ? el.btnToggleLeft : el.btnToggleRight;
    const label = isLeft ? "Temporary DB & Metrics pane" : "Output pane";
    if (!pane) return;

    const collapsed =
      typeof forceCollapsed === "boolean" ? forceCollapsed : !pane.classList.contains("collapsed");
    pane.classList.toggle("collapsed", collapsed);
    if (splitter) splitter.classList.toggle("hidden", collapsed);
    if (btn) {
      btn.setAttribute("aria-expanded", collapsed ? "false" : "true");
      const actionLabel = `${collapsed ? "Restore" : "Minimize"} ${label}`;
      btn.setAttribute("aria-label", actionLabel);
      btn.setAttribute("title", actionLabel);
    }
    syncEditorScroll();
  }

  function initSidePaneToggles() {
    [
      ["left", el.paneLeft, el.btnToggleLeft],
      ["right", el.paneRight, el.btnToggleRight],
    ].forEach(([side, pane, btn]) => {
      if (btn) {
        btn.addEventListener("click", (ev) => {
          ev.stopPropagation();
          toggleSidePane(side);
        });
      }
      if (pane) {
        const hdr = pane.querySelector(".pane-header");
        if (hdr) {
          hdr.addEventListener("click", () => {
            if (pane.classList.contains("collapsed")) {
              toggleSidePane(side, false);
            }
          });
        }
      }
    });
  }

  async function init() {
    initSplitters();
    initSidePaneToggles();
    initDocsDialog();
    try {
      const saved = Number(localStorage.getItem(FONT_STORAGE_KEY));
      setEditorFontSize(saved || 13);
    } catch (_err) {
      setEditorFontSize(13);
    }

    el.editor.addEventListener("input", () => {
      syncEditorHighlight();
      updateCursorInfo();
    });
    el.editor.addEventListener("scroll", syncEditorScroll);
    el.editor.addEventListener("click", updateCursorInfo);
    el.editor.addEventListener("keyup", updateCursorInfo);
    el.editor.addEventListener("keydown", (ev) => {
      if ((ev.ctrlKey || ev.metaKey) && ev.key === "Enter") {
        ev.preventDefault();
        runEditor();
      } else if ((ev.ctrlKey || ev.metaKey) && (ev.key === "=" || ev.key === "+")) {
        ev.preventDefault();
        setEditorFontSize(editorFontSize + 1);
      } else if ((ev.ctrlKey || ev.metaKey) && ev.key === "-") {
        ev.preventDefault();
        setEditorFontSize(editorFontSize - 1);
      } else if (ev.key === "Tab") {
        ev.preventDefault();
        const start = el.editor.selectionStart;
        const end = el.editor.selectionEnd;
        const val = el.editor.value;
        el.editor.value = val.slice(0, start) + "    " + val.slice(end);
        el.editor.selectionStart = el.editor.selectionEnd = start + 4;
        syncEditorHighlight();
        updateCursorInfo();
      }
    });

    el.btnRun.addEventListener("click", runEditor);
    el.btnCheckpoint.addEventListener("click", runCheckpoint);
    el.btnReset.addEventListener("click", resetTempDb);
    el.btnRefresh.addEventListener("click", () => refreshState());
    el.btnClear.addEventListener("click", () => {
      el.editor.value = "";
      syncEditorHighlight();
      updateCursorInfo();
    });
    el.btnFontDec.addEventListener("click", () => setEditorFontSize(editorFontSize - 1));
    el.btnFontInc.addEventListener("click", () => setEditorFontSize(editorFontSize + 1));

    el.btnTheme.addEventListener("click", () => {
      const root = document.documentElement;
      const next = root.getAttribute("data-theme") === "dark" ? "light" : "dark";
      root.setAttribute("data-theme", next);
      el.themeIcon.textContent = next === "dark" ? "☀" : "☾";
    });

    el.btnCopy.addEventListener("click", async () => {
      if (!lastResponse || !lastResponse.result) return;
      const r = lastResponse.result;
      const text =
        outputMode === "dict" ? r.dict_text : outputMode === "list" ? r.list_text : r.table_text;
      if (navigator.clipboard && text) {
        await navigator.clipboard.writeText(text);
        el.btnCopy.textContent = "Copied!";
        setTimeout(() => {
          el.btnCopy.textContent = "Copy Output";
        }, 1200);
      }
    });

    el.fileList.addEventListener("click", (ev) => {
      const item = ev.target.closest(".file-item");
      if (!item) return;
      refreshState(item.dataset.file);
    });

    el.segBtns.forEach((btn) => {
      btn.addEventListener("click", () => {
        outputMode = btn.dataset.mode;
        el.segBtns.forEach((b) => {
          const on = b === btn;
          b.classList.toggle("active", on);
          b.setAttribute("aria-selected", on ? "true" : "false");
        });
        renderOutput();
      });
    });

    el.snippetSelect.addEventListener("change", () => {
      const key = el.snippetSelect.value;
      if (snippets[key] !== undefined) {
        el.editor.value = snippets[key];
        syncEditorHighlight();
        updateCursorInfo();
      }
    });

    const initial = await apiGet("/api/state");
    snippets = initial.snippets || {};
    el.snippetSelect.innerHTML = Object.keys(snippets)
      .map((name) => `<option value="${escapeHtml(name)}">${escapeHtml(name)}</option>`)
      .join("");
    el.editor.value = initial.default_script || "";
    syncEditorHighlight();
    updateCursorInfo();
    renderState(initial.state);
    await runEditor();
  }

  init();
})();

