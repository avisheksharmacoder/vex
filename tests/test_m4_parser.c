#include "vx_arena.h"
#include "vx_lexer.h"
#include "vx_parser.h"
#include "vx_test.h"

#include <string.h>

static bool parse_cstr(const char *q, vx_arena_t *a, vx_parse_result_t *out) {
    vx_arena_reset(a);
    return vx_parse(q, strlen(q), a, out);
}

/* ==================== 1. LEXER TESTS ==================== */

static void test_lexer_tokens_comments_and_backticks(void) {
    const char *src =
        "# header comment\n"
        "orders + users ++ logs & meta `sort` $min_age\n"
        "42 3.14 \"hello\" 'world' += |= -= -> == != <= >=\n"
        "by sort del table index unique and or not in true false null\n";

    vx_lexer_t lx;
    vx_lexer_init(&lx, src, strlen(src));

    const vx_tok_kind_t expected[] = {
        VX_TOK_NEWLINE,
        VX_TOK_IDENT, VX_TOK_PLUS, VX_TOK_IDENT, VX_TOK_PLUS_PLUS, VX_TOK_IDENT,
        VX_TOK_AMP, VX_TOK_IDENT, VX_TOK_IDENT, VX_TOK_PARAM, VX_TOK_NEWLINE,
        VX_TOK_INT, VX_TOK_FLOAT, VX_TOK_STR, VX_TOK_STR,
        VX_TOK_PLUS_EQ, VX_TOK_PIPE_EQ, VX_TOK_MINUS_EQ, VX_TOK_ARROW,
        VX_TOK_EQ_EQ, VX_TOK_BANG_EQ, VX_TOK_LE, VX_TOK_GE, VX_TOK_NEWLINE,
        VX_TOK_KW_BY, VX_TOK_KW_SORT, VX_TOK_KW_DEL, VX_TOK_KW_TABLE,
        VX_TOK_KW_INDEX, VX_TOK_KW_UNIQUE, VX_TOK_KW_AND, VX_TOK_KW_OR,
        VX_TOK_KW_NOT, VX_TOK_KW_IN, VX_TOK_KW_TRUE, VX_TOK_KW_FALSE,
        VX_TOK_KW_NULL, VX_TOK_NEWLINE, VX_TOK_EOF
    };

    size_t n = sizeof(expected) / sizeof(expected[0]);
    for (size_t i = 0; i < n; i++) {
        vx_tok_t pk = vx_lexer_peek(&lx);
        vx_tok_t tk = vx_lexer_next(&lx);
        VX_ASSERT(pk.kind == tk.kind);
        VX_ASSERT(tk.kind == expected[i]);

        if (i == 8) { /* `sort` backtick-escaped identifier */
            VX_ASSERT(tk.len == 4 && memcmp(tk.ptr, "sort", 4) == 0);
        } else if (i == 9) { /* $min_age */
            VX_ASSERT(tk.len == 7 && memcmp(tk.ptr, "min_age", 7) == 0);
        } else if (i == 13) { /* "hello" */
            VX_ASSERT(tk.len == 5 && memcmp(tk.ptr, "hello", 5) == 0);
        } else if (i == 14) { /* 'world' */
            VX_ASSERT(tk.len == 5 && memcmp(tk.ptr, "world", 5) == 0);
        }
    }
}

/* ==================== 2. SCAN, PROJECT & PK ==================== */

static void test_parse_scan_project_pk_and_explain(void) {
    vx_arena_t arena;
    VX_ASSERT(vx_arena_init(&arena, 64 * 1024));
    vx_parse_result_t r;

    /* Bare table scan */
    VX_ASSERT(parse_cstr("users", &arena, &r));
    VX_ASSERT(r.ok && r.program.nstmts == 1);
    vx_stmt_t *s = r.program.stmts[0];
    VX_ASSERT(s->kind == VX_STMT_QUERY);
    VX_ASSERT(!s->u.query.explain);
    VX_ASSERT(s->u.query.bind_name.len == 0);
    VX_ASSERT(vx_span_eq_cstr(s->u.query.source_table, "users"));
    VX_ASSERT(s->u.query.nstages == 0);

    /* Column projection with alias, arithmetic, and dotted path */
    VX_ASSERT(parse_cstr("orders id, total: price * qty, meta.os", &arena, &r));
    s = r.program.stmts[0];
    VX_ASSERT(s->u.query.nstages == 1);
    vx_stage_t *st = &s->u.query.stages[0];
    VX_ASSERT(st->kind == VX_STAGE_PROJECT && st->u.project.nitems == 3);
    VX_ASSERT(st->u.project.items[0].alias.len == 0);
    VX_ASSERT(st->u.project.items[0].expr->kind == VX_EXPR_IDENT);
    VX_ASSERT(vx_span_eq_cstr(st->u.project.items[1].alias, "total"));
    VX_ASSERT(st->u.project.items[1].expr->kind == VX_EXPR_BINARY);
    VX_ASSERT(st->u.project.items[1].expr->u.binary.op == VX_BINOP_MUL);
    VX_ASSERT(st->u.project.items[2].expr->kind == VX_EXPR_IDENT);
    VX_ASSERT(st->u.project.items[2].expr->u.ident.nparts == 2);
    VX_ASSERT(vx_span_eq_cstr(st->u.project.items[2].expr->u.ident.parts[0], "meta"));
    VX_ASSERT(vx_span_eq_cstr(st->u.project.items[2].expr->u.ident.parts[1], "os"));

    /* PK lookup + column projection */
    VX_ASSERT(parse_cstr("users[42] name, email", &arena, &r));
    s = r.program.stmts[0];
    VX_ASSERT(s->u.query.nstages == 2);
    VX_ASSERT(s->u.query.stages[0].kind == VX_STAGE_PK);
    VX_ASSERT(s->u.query.stages[0].u.pk.key->kind == VX_EXPR_INT);
    VX_ASSERT(s->u.query.stages[0].u.pk.key->u.i64 == 42);
    VX_ASSERT(s->u.query.stages[1].kind == VX_STAGE_PROJECT);
    VX_ASSERT(s->u.query.stages[1].u.project.nitems == 2);

    /* Parameterized PK + explain prefix */
    VX_ASSERT(parse_cstr("? users[$uid] name", &arena, &r));
    s = r.program.stmts[0];
    VX_ASSERT(s->u.query.explain);
    VX_ASSERT(s->u.query.stages[0].kind == VX_STAGE_PK);
    VX_ASSERT(s->u.query.stages[0].u.pk.key->kind == VX_EXPR_PARAM);
    VX_ASSERT(vx_span_eq_cstr(s->u.query.stages[0].u.pk.key->u.param, "uid"));

    vx_arena_destroy(&arena);
}

/* ==================== 3. FILTERS, PRECEDENCE & SLICES ==================== */

static void test_parse_filters_precedence_and_slices(void) {
    vx_arena_t arena;
    VX_ASSERT(vx_arena_init(&arena, 64 * 1024));
    vx_parse_result_t r;

    /* Comma-separated implicit AND + complex precedence + IN list */
    const char *q =
        "users[age >= $min, a + b * c == 10 or not banned and role in [\"admin\", \"ops\"]][20:30]";
    VX_ASSERT(parse_cstr(q, &arena, &r));
    vx_stmt_t *s = r.program.stmts[0];
    VX_ASSERT(s->u.query.nstages == 2);

    /* Stage 0: FILTER -> top-level implicit AND */
    vx_stage_t *f = &s->u.query.stages[0];
    VX_ASSERT(f->kind == VX_STAGE_FILTER);
    vx_expr_t *root = f->u.filter.pred;
    VX_ASSERT(root->kind == VX_EXPR_BINARY && root->u.binary.op == VX_BINOP_AND);
    VX_ASSERT(root->u.binary.lhs->u.binary.op == VX_BINOP_GE);
    VX_ASSERT(root->u.binary.lhs->u.binary.rhs->kind == VX_EXPR_PARAM);

    /* Right side of comma: (a + (b * c) == 10) OR ((not banned) AND (role in [...])) */
    vx_expr_t *or_e = root->u.binary.rhs;
    VX_ASSERT(or_e->kind == VX_EXPR_BINARY && or_e->u.binary.op == VX_BINOP_OR);
    vx_expr_t *eq_e = or_e->u.binary.lhs;
    VX_ASSERT(eq_e->u.binary.op == VX_BINOP_EQ);
    VX_ASSERT(eq_e->u.binary.lhs->u.binary.op == VX_BINOP_ADD);
    VX_ASSERT(eq_e->u.binary.lhs->u.binary.rhs->u.binary.op == VX_BINOP_MUL);

    vx_expr_t *and_e = or_e->u.binary.rhs;
    VX_ASSERT(and_e->u.binary.op == VX_BINOP_AND);
    VX_ASSERT(and_e->u.binary.lhs->kind == VX_EXPR_UNARY);
    VX_ASSERT(and_e->u.binary.lhs->u.unary.op == VX_UNOP_NOT);
    VX_ASSERT(and_e->u.binary.rhs->u.binary.op == VX_BINOP_IN);
    VX_ASSERT(and_e->u.binary.rhs->u.binary.rhs->kind == VX_EXPR_LIST);
    VX_ASSERT(and_e->u.binary.rhs->u.binary.rhs->u.list.nitems == 2);

    /* Stage 1: SLICE [20:30] */
    vx_stage_t *sl = &s->u.query.stages[1];
    VX_ASSERT(sl->kind == VX_STAGE_SLICE);
    VX_ASSERT(sl->u.slice.has_start && sl->u.slice.start == 20);
    VX_ASSERT(sl->u.slice.has_stop && sl->u.slice.stop == 30);

    /* Open-ended slices [:10], [5:], [:] */
    VX_ASSERT(parse_cstr("users[:10]", &arena, &r));
    sl = &r.program.stmts[0]->u.query.stages[0];
    VX_ASSERT(sl->kind == VX_STAGE_SLICE && !sl->u.slice.has_start &&
              sl->u.slice.has_stop && sl->u.slice.stop == 10);

    VX_ASSERT(parse_cstr("users[5:]", &arena, &r));
    sl = &r.program.stmts[0]->u.query.stages[0];
    VX_ASSERT(sl->kind == VX_STAGE_SLICE && sl->u.slice.has_start &&
              sl->u.slice.start == 5 && !sl->u.slice.has_stop);

    VX_ASSERT(parse_cstr("users[:]", &arena, &r));
    sl = &r.program.stmts[0]->u.query.stages[0];
    VX_ASSERT(sl->kind == VX_STAGE_SLICE && !sl->u.slice.has_start && !sl->u.slice.has_stop);

    vx_arena_destroy(&arena);
}

/* ==================== 4. JOINS (+, ++, &), GROUP, SORT, BINDING ==================== */

static void test_parse_joins_group_sort_and_multiline_pipeline(void) {
    vx_arena_t arena;
    VX_ASSERT(vx_arena_init(&arena, 64 * 1024));
    vx_parse_result_t r;

    const char *q =
        "vips = orders[status == \"paid\", amount > 100]\n"
        "  + users(user_id == id)\n"
        "  ++ audit(order_id)\n"
        "  & regions\n"
        "  by users.country { n: count(), rev: sum(amount) }\n"
        "  [rev > 5000]\n"
        "  sort -rev, users.country [:10]\n"
        "\n"
        "vips users.country, rev\n";

    VX_ASSERT(parse_cstr(q, &arena, &r));
    VX_ASSERT(r.ok && r.program.nstmts == 2);

    vx_stmt_t *s0 = r.program.stmts[0];
    VX_ASSERT(s0->kind == VX_STMT_QUERY);
    VX_ASSERT(vx_span_eq_cstr(s0->u.query.bind_name, "vips"));
    VX_ASSERT(vx_span_eq_cstr(s0->u.query.source_table, "orders"));
    VX_ASSERT(s0->u.query.nstages == 8);

    /* Stage 1: + users(user_id == id) -> LEFT OUTER JOIN */
    vx_stage_t *j1 = &s0->u.query.stages[1];
    VX_ASSERT(j1->kind == VX_STAGE_JOIN && j1->u.join.kind == VX_JOIN_LEFT);
    VX_ASSERT(vx_span_eq_cstr(j1->u.join.right_table, "users"));
    VX_ASSERT(j1->u.join.has_on);
    VX_ASSERT(vx_span_eq_cstr(j1->u.join.left_col, "user_id"));
    VX_ASSERT(vx_span_eq_cstr(j1->u.join.right_col, "id"));

    /* Stage 2: ++ audit(order_id) -> FULL OUTER JOIN shorthand */
    vx_stage_t *j2 = &s0->u.query.stages[2];
    VX_ASSERT(j2->kind == VX_STAGE_JOIN && j2->u.join.kind == VX_JOIN_FULL);
    VX_ASSERT(vx_span_eq_cstr(j2->u.join.right_table, "audit"));
    VX_ASSERT(j2->u.join.has_on);
    VX_ASSERT(vx_span_eq_cstr(j2->u.join.left_col, "order_id"));
    VX_ASSERT(vx_span_eq_cstr(j2->u.join.right_col, "order_id"));

    /* Stage 3: & regions -> INNER AUTO-JOIN (FK-backed) */
    vx_stage_t *j3 = &s0->u.query.stages[3];
    VX_ASSERT(j3->kind == VX_STAGE_JOIN && j3->u.join.kind == VX_JOIN_INNER);
    VX_ASSERT(vx_span_eq_cstr(j3->u.join.right_table, "regions"));
    VX_ASSERT(!j3->u.join.has_on);

    /* Stage 4: GROUP + AGG */
    vx_stage_t *g = &s0->u.query.stages[4];
    VX_ASSERT(g->kind == VX_STAGE_GROUP && g->u.group.nkeys == 1 && g->u.group.naggs == 2);
    VX_ASSERT(vx_span_eq_cstr(g->u.group.aggs[0].alias, "n"));
    VX_ASSERT(g->u.group.aggs[0].expr->kind == VX_EXPR_CALL);
    VX_ASSERT(g->u.group.aggs[0].expr->u.call.nargs == 0);
    VX_ASSERT(vx_span_eq_cstr(g->u.group.aggs[1].alias, "rev"));
    VX_ASSERT(g->u.group.aggs[1].expr->u.call.nargs == 1);

    /* Stage 5: Post-group HAVING-less filter */
    VX_ASSERT(s0->u.query.stages[5].kind == VX_STAGE_FILTER);

    /* Stage 6: SORT -rev, users.country */
    vx_stage_t *so = &s0->u.query.stages[6];
    VX_ASSERT(so->kind == VX_STAGE_SORT && so->u.sort.nitems == 2);
    VX_ASSERT(so->u.sort.items[0].desc);
    VX_ASSERT(!so->u.sort.items[1].desc);

    /* Stage 7: SLICE [:10] */
    VX_ASSERT(s0->u.query.stages[7].kind == VX_STAGE_SLICE);

    /* Statement 2: references vips */
    vx_stmt_t *s1 = r.program.stmts[1];
    VX_ASSERT(s1->kind == VX_STMT_QUERY);
    VX_ASSERT(vx_span_eq_cstr(s1->u.query.source_table, "vips"));
    VX_ASSERT(s1->u.query.nstages == 1 && s1->u.query.stages[0].kind == VX_STAGE_PROJECT);

    /* Global aggregate (no `by`) and distinct (`by` with no `{}`) */
    VX_ASSERT(parse_cstr("orders { n: count() }", &arena, &r));
    g = &r.program.stmts[0]->u.query.stages[0];
    VX_ASSERT(g->kind == VX_STAGE_GROUP && g->u.group.nkeys == 0 && g->u.group.naggs == 1);

    VX_ASSERT(parse_cstr("users by country, age", &arena, &r));
    g = &r.program.stmts[0]->u.query.stages[0];
    VX_ASSERT(g->kind == VX_STAGE_GROUP && g->u.group.nkeys == 2 && g->u.group.naggs == 0);

    vx_arena_destroy(&arena);
}

/* ==================== 5. MUTATIONS & SAFETY GUARDS ==================== */

static void test_parse_mutations_and_safety_guards(void) {
    vx_arena_t arena;
    VX_ASSERT(vx_arena_init(&arena, 64 * 1024));
    vx_parse_result_t r;

    /* Single insert (+=) */
    VX_ASSERT(parse_cstr("users += { id: 1, name: \"Aarav\", age: 29 }", &arena, &r));
    vx_stmt_t *s = r.program.stmts[0];
    VX_ASSERT(s->kind == VX_STMT_INSERT && !s->u.insert.is_upsert);
    VX_ASSERT(s->u.insert.nrecords == 1 && s->u.insert.records[0].nfields == 3);

    /* Batch insert (+= [...]) */
    VX_ASSERT(parse_cstr("users += [{ id: 1 }, { id: 2 }]", &arena, &r));
    s = r.program.stmts[0];
    VX_ASSERT(s->kind == VX_STMT_INSERT && s->u.insert.nrecords == 2);

    /* Upsert (|=) */
    VX_ASSERT(parse_cstr("users |= { id: 1, age: 30 }", &arena, &r));
    s = r.program.stmts[0];
    VX_ASSERT(s->kind == VX_STMT_INSERT && s->u.insert.is_upsert);

    /* Update with PK and full slice [:] */
    VX_ASSERT(parse_cstr("users[42] = { age: 30, spend: spend + 50 }", &arena, &r));
    s = r.program.stmts[0];
    VX_ASSERT(s->kind == VX_STMT_UPDATE && s->u.update.target.kind == VX_STAGE_PK);
    VX_ASSERT(s->u.update.patch.nfields == 2);

    VX_ASSERT(parse_cstr("users[:] = { active: false }", &arena, &r));
    s = r.program.stmts[0];
    VX_ASSERT(s->kind == VX_STMT_UPDATE && s->u.update.target.kind == VX_STAGE_SLICE);

    /* Delete: both `del users[...]` and `users[...] -=` forms */
    VX_ASSERT(parse_cstr("del users[age < 18]", &arena, &r));
    s = r.program.stmts[0];
    VX_ASSERT(s->kind == VX_STMT_DELETE && s->u.del.target.kind == VX_STAGE_FILTER);

    VX_ASSERT(parse_cstr("del users[:]", &arena, &r));
    s = r.program.stmts[0];
    VX_ASSERT(s->kind == VX_STMT_DELETE && s->u.del.target.kind == VX_STAGE_SLICE);

    VX_ASSERT(parse_cstr("users[42] -=", &arena, &r));
    s = r.program.stmts[0];
    VX_ASSERT(s->kind == VX_STMT_DELETE && s->u.del.target.kind == VX_STAGE_PK);

    /* SAFETY GUARDS: full-table wipe/update without explicit [:] MUST FAIL */
    VX_ASSERT(!parse_cstr("del users", &arena, &r) && !r.ok && r.err_msg[0] != '\0');
    VX_ASSERT(!parse_cstr("users -=", &arena, &r) && !r.ok);
    VX_ASSERT(!parse_cstr("users = { active: false }", &arena, &r) && !r.ok);

    /* Malformed syntax MUST FAIL cleanly with line/col */
    VX_ASSERT(!parse_cstr("users[age > ", &arena, &r) && !r.ok && r.err_line == 1);
    VX_ASSERT(!parse_cstr("users \"unterminated", &arena, &r) && !r.ok);

    vx_arena_destroy(&arena);
}

/* ==================== 6. DDL (TABLE, INDEX, UNIQUE, DROP) ==================== */

static void test_parse_ddl(void) {
    vx_arena_t arena;
    VX_ASSERT(vx_arena_init(&arena, 64 * 1024));
    vx_parse_result_t r;

    const char *ddl =
        "table orders {\n"
        "  id: u64 key,\n"
        "  user_id: u64 -> users.id,\n"
        "  amount: f64,\n"
        "  status: str\n"
        "}\n"
        "index orders(user_id, status)\n"
        "unique users(email)\n"
        "del table orders\n";

    VX_ASSERT(parse_cstr(ddl, &arena, &r));
    VX_ASSERT(r.ok && r.program.nstmts == 4);

    vx_stmt_t *t = r.program.stmts[0];
    VX_ASSERT(t->kind == VX_STMT_CREATE_TABLE);
    VX_ASSERT(vx_span_eq_cstr(t->u.create_table.table, "orders"));
    VX_ASSERT(t->u.create_table.ncols == 4);
    VX_ASSERT(t->u.create_table.cols[0].is_key);
    VX_ASSERT(vx_span_eq_cstr(t->u.create_table.cols[0].type_name, "u64"));
    VX_ASSERT(t->u.create_table.cols[1].has_fk);
    VX_ASSERT(vx_span_eq_cstr(t->u.create_table.cols[1].fk_table, "users"));
    VX_ASSERT(vx_span_eq_cstr(t->u.create_table.cols[1].fk_col, "id"));

    vx_stmt_t *ix = r.program.stmts[1];
    VX_ASSERT(ix->kind == VX_STMT_CREATE_INDEX && !ix->u.create_index.is_unique);
    VX_ASSERT(ix->u.create_index.ncols == 2);

    vx_stmt_t *ux = r.program.stmts[2];
    VX_ASSERT(ux->kind == VX_STMT_CREATE_INDEX && ux->u.create_index.is_unique);
    VX_ASSERT(ux->u.create_index.ncols == 1);

    vx_stmt_t *dr = r.program.stmts[3];
    VX_ASSERT(dr->kind == VX_STMT_DROP_TABLE);
    VX_ASSERT(vx_span_eq_cstr(dr->u.drop_table.table, "orders"));

    vx_arena_destroy(&arena);
}

/* ==================== 7. PARSER THROUGHPUT SLA ==================== */

static void test_parser_perf_sla(void) {
    vx_arena_t arena;
    VX_ASSERT(vx_arena_init(&arena, 64 * 1024));
    vx_parse_result_t r;

    const char *q =
        "orders[status == \"paid\", amount > 100] + users(user_id == id) "
        "by users.country { n: count(), rev: sum(amount) } [rev > 5000] sort -rev [:10]";
    size_t qlen = strlen(q);

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    const int iters = 10000;
#else
    const int iters = 200000;
#endif

    double t0 = vx_now_sec();
    for (int i = 0; i < iters; i++) {
        vx_arena_reset(&arena);
        VX_ASSERT(vx_parse(q, qlen, &arena, &r));
    }
    double dt = vx_now_sec() - t0;

    double kqps = ((double)iters / dt) / 1e3;
    printf("    [METRIC] Zero-alloc parser throughput: %.1f k queries/sec (%.4fs)\n", kqps, dt);

#if !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
    /* Release-build SLA tripwire: >= 200k full pipeline parses/sec single-core */
    VX_ASSERT(kqps >= 200.0);
#endif

    vx_arena_destroy(&arena);
}

int main(void) {
    VX_RUN(test_lexer_tokens_comments_and_backticks);
    VX_RUN(test_parse_scan_project_pk_and_explain);
    VX_RUN(test_parse_filters_precedence_and_slices);
    VX_RUN(test_parse_joins_group_sort_and_multiline_pipeline);
    VX_RUN(test_parse_mutations_and_safety_guards);
    VX_RUN(test_parse_ddl);
    VX_RUN(test_parser_perf_sla);
    return vx_test_summary("M4 Language: Lexer + LL(1) Parser");
}

