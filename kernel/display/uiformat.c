/* uiformat.c — .gui text format parser (UAOS-127)
 *
 * Recursive-descent parser for the grammar documented in uiformat.h.
 * Dependency-free like uitree.c: no libc, no heap — nodes and item arrays
 * come from the caller's UIArena; strings are terminated in place inside
 * the input buffer (ui_parse() must get a mutable text).
 */

#include "uiformat.h"

/* -------------------------------------------------------------------------
 * Local helpers (no libc)
 * ------------------------------------------------------------------------- */
static int p_isalpha(int c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int p_isdigit(int c) { return c >= '0' && c <= '9'; }
static int p_isalnum(int c) { return p_isalpha(c) || p_isdigit(c); }

/* -------------------------------------------------------------------------
 * Lexer
 * ------------------------------------------------------------------------- */
enum {
    T_EOF = 0, T_LBRACE, T_RBRACE, T_SEMI, T_EQ, T_COMMA,
    T_NUM, T_RANGE, T_STR, T_IDENT
};

typedef struct {
    char      *p;
    int        line, col;
    UIArena   *a;
    UIParseErr *err;
    int        failed;
    int        tok;            /* current token               */
    int        iv, iv2;        /* T_NUM / T_RANGE values      */
    char      *sv;             /* T_STR / T_IDENT text        */
} P;

static void p_fail(P *p, const char *msg)
{
    if (p->failed) return;
    p->failed = 1;
    if (p->err) {
        p->err->line = p->line;
        p->err->col  = p->col;
        int i = 0;
        while (msg[i] && i < 63) { p->err->msg[i] = msg[i]; i++; }
        p->err->msg[i] = '\0';
    }
}

static void p_skipws(P *p)
{
    for (;;) {
        char c = *p->p;
        if (c == '\n') { p->line++; p->col = 0; p->p++; p->col = 1; }
        else if (c == ' ' || c == '\t' || c == '\r') { p->p++; p->col++; }
        else if (c == '/' && p->p[1] == '/') {
            while (*p->p && *p->p != '\n') { p->p++; p->col++; }
        } else return;
    }
}

static void p_next(P *p)
{
    p_skipws(p);
    p->sv = NULL;
    char c = *p->p;
    if (!c)            { p->tok = T_EOF; return; }
    if (c == '{')      { p->tok = T_LBRACE; p->p++; p->col++; return; }
    if (c == '}')      { p->tok = T_RBRACE; p->p++; p->col++; return; }
    if (c == ';')      { p->tok = T_SEMI;   p->p++; p->col++; return; }
    if (c == '=')      { p->tok = T_EQ;     p->p++; p->col++; return; }
    if (c == ',')      { p->tok = T_COMMA;  p->p++; p->col++; return; }

    if (c == '"' || c == '\'') {
        /* in-place string: compact escapes, terminate at the closing
         * quote (' or " both accepted — ' is friendlier inside shell
         * commands that strip double quotes) */
        char q = c;
        char *src = ++p->p, *dst = src;
        p->sv = dst;
        while (*src && *src != q) {
            if (*src == '\\' && src[1]) {
                src++;
                if      (*src == 'n') *dst++ = '\n';
                else if (*src == 't') *dst++ = '\t';
                else                  *dst++ = *src;
                src++;
            } else {
                *dst++ = *src++;
            }
        }
        if (*src != q) { p_fail(p, "unterminated string"); return; }
        *dst = '\0';
        p->col += (int)(src - p->p) + 2;
        p->p = src + 1;
        p->tok = T_STR;
        return;
    }

    if (p_isdigit(c) || (c == '-' && p_isdigit(p->p[1]))) {
        int neg = 0;
        if (c == '-') { neg = 1; p->p++; p->col++; }
        long v = 0;
        while (p_isdigit(*p->p)) { v = v * 10 + (*p->p - '0'); p->p++; p->col++; }
        p->iv = (int)(neg ? -v : v);
        if (p->p[0] == '.' && p->p[1] == '.') {   /* range: min..max */
            p->p += 2; p->col += 2;
            neg = 0;
            if (*p->p == '-') { neg = 1; p->p++; p->col++; }
            if (!p_isdigit(*p->p)) { p_fail(p, "expected number after .."); return; }
            v = 0;
            while (p_isdigit(*p->p)) { v = v * 10 + (*p->p - '0'); p->p++; p->col++; }
            p->iv2 = (int)(neg ? -v : v);
            p->tok = T_RANGE;
            return;
        }
        p->tok = T_NUM;
        return;
    }

    if (p_isalpha(c)) {
        char *s = p->p;
        while (p_isalnum(*p->p) || *p->p == '-') { p->p++; p->col++; }
        p->sv = s;
        /* ident text is NOT NUL-terminated in place — comparisons go
         * through p_kw() which matches the span in p->iv */
        p->tok = T_IDENT;
        p->iv = (int)(p->p - s); /* ident length */
        return;
    }

    p_fail(p, "unexpected character");
}

/* Compare the current (non-terminated) ident against a keyword. */
static int p_kw(P *p, const char *kw)
{
    if (p->tok != T_IDENT) return 0;
    int i = 0;
    while (kw[i] && i < p->iv && p->sv[i] == kw[i]) i++;
    return kw[i] == '\0' && i == p->iv;
}

static int p_expect(P *p, int tok, const char *msg)
{
    if (p->tok != tok) { p_fail(p, msg); return 0; }
    p_next(p);
    return 1;
}

static int p_num(P *p, const char *msg)
{
    if (p->tok != T_NUM) { p_fail(p, msg); return 0; }
    int v = p->iv;
    p_next(p);
    return v;
}

/* -------------------------------------------------------------------------
 * Node construction
 * ------------------------------------------------------------------------- */
static UINode *p_new(P *p, UIType t)
{
    UINode *n = (UINode *)ui_arena_alloc(p->a, sizeof(UINode));
    if (!n) { p_fail(p, "arena exhausted"); return NULL; }
    unsigned char *b = (unsigned char *)n;
    for (uint32_t i = 0; i < sizeof(UINode); i++) b[i] = 0;
    n->type = t;
    return n;
}

/* Symbolic id -> ui_sym() hash; numeric id used directly. */
static int p_id(P *p)
{
    if (p->tok == T_NUM) { int v = p->iv; p_next(p); return v; }
    if (p->tok == T_IDENT) {
        /* ui_sym on a non-terminated ident — hash the span */
        uint32_t h = 2166136261u;
        for (int i = 0; i < p->iv; i++) {
            h ^= (unsigned char)p->sv[i];
            h *= 16777619u;
        }
        p_next(p);
        return (int)(h & 0x7fffffffu) | 1;
    }
    p_fail(p, "expected id");
    return 0;
}

/* Attributes: consumes IDENTs until `;` or `{`. */
static void p_attrs(P *p, UINode *n)
{
    while (!p->failed && p->tok == T_IDENT) {
        /* flag words */
        if      (p_kw(p, "disabled")) { n->flags |= UI_F_DISABLED; p_next(p); continue; }
        else if (p_kw(p, "readonly")) { n->flags |= UI_F_READONLY; p_next(p); continue; }
        else if (p_kw(p, "toggle"))   { n->flags |= UI_F_TOGGLE;   p_next(p); continue; }
        else if (p_kw(p, "selected")) { n->flags |= UI_F_SELECTED; p_next(p); continue; }
        else if (p_kw(p, "center"))   { n->flags |= UI_F_CENTER;   p_next(p); continue; }
        else if (p_kw(p, "vertical")) { n->flags |= UI_F_VERTICAL; p_next(p); continue; }

        if      (p_kw(p, "id"))       { p_next(p); p_expect(p, T_EQ, "expected = after id");
                                        n->id = p_id(p); }
        else if (p_kw(p, "weight") || p_kw(p, "w")) {
                                        p_next(p); p_expect(p, T_EQ, "expected =");
                                        n->weight = p_num(p, "expected weight"); }
        else if (p_kw(p, "min"))      { p_next(p); p_expect(p, T_EQ, "expected =");
                                        n->min_w = p_num(p, "expected min width");
                                        p_expect(p, T_COMMA, "expected , in min");
                                        n->min_h = p_num(p, "expected min height"); }
        else if (p_kw(p, "max"))      { p_next(p); p_expect(p, T_EQ, "expected =");
                                        n->max_w = p_num(p, "expected max width");
                                        p_expect(p, T_COMMA, "expected , in max");
                                        n->max_h = p_num(p, "expected max height"); }
        else if (p_kw(p, "pad"))      { p_next(p); p_expect(p, T_EQ, "expected =");
                                        n->pad = p_num(p, "expected pad"); }
        else if (p_kw(p, "spacing"))  { p_next(p); p_expect(p, T_EQ, "expected =");
                                        n->spacing = p_num(p, "expected spacing"); }
        else if (p_kw(p, "group"))    { p_next(p); p_expect(p, T_EQ, "expected =");
                                        n->group = p_num(p, "expected group"); }
        else if (p_kw(p, "tab"))      { p_next(p); p_expect(p, T_EQ, "expected =");
                                        if (p->tok != T_STR) { p_fail(p, "expected string"); return; }
                                        n->tab = p->sv; p_next(p); }
        else if (p_kw(p, "text"))     { p_next(p); p_expect(p, T_EQ, "expected =");
                                        if (p->tok != T_STR) { p_fail(p, "expected string"); return; }
                                        n->u.text = p->sv; p_next(p); }
        else if (p_kw(p, "active"))   { p_next(p); p_expect(p, T_EQ, "expected =");
                                        int v = p_num(p, "expected active index");
                                        if (n->type == UI_PAGE) n->u.page.active = v;
                                        else n->u.items.active = v; }
        else if (p_kw(p, "sel"))      { p_next(p); p_expect(p, T_EQ, "expected =");
                                        n->u.items.selected = p_num(p, "expected sel"); }
        else if (p_kw(p, "value"))    { p_next(p); p_expect(p, T_EQ, "expected =");
                                        int v = p_num(p, "expected value");
                                        if (n->type == UI_INTEGER) n->u.input.value = v;
                                        else n->u.range.cur = v; }
        else if (p_kw(p, "maxchars")) { p_next(p); p_expect(p, T_EQ, "expected =");
                                        n->u.input.max_chars = p_num(p, "expected maxchars"); }
        else { p_fail(p, "unknown attribute"); return; }
    }
}

/* Collect a STRING list into an arena array.  Items may be separated by
 * commas or whitespace: `cycle "a" "b" "c"`. */
static const char * const *p_items(P *p, int *count)
{
    const char *tmp[32];
    int n = 0;
    while (p->tok == T_STR) {
        if (n >= 32) { p_fail(p, "too many items (max 32)"); return NULL; }
        tmp[n++] = p->sv;
        p_next(p);
        if (p->tok == T_COMMA) p_next(p);
    }
    if (n == 0) { p_fail(p, "expected item strings"); return NULL; }
    const char **arr = (const char **)ui_arena_alloc(p->a,
                                                     n * sizeof(char *));
    if (!arr) { p_fail(p, "arena exhausted"); return NULL; }
    for (int i = 0; i < n; i++) arr[i] = tmp[i];
    *count = n;
    return arr;
}

/* -------------------------------------------------------------------------
 * Recursive descent
 * ------------------------------------------------------------------------- */
static UINode *p_block_list(P *p);   /* fwd */

/* Children of `{ ... }`: NULL-terminated parse; when more than one node
 * is present the caller decides whether to wrap them (window/tab) or use
 * them directly (vgroup/hgroup).  Returns a linked list head via parent
 * insertion: we build into a fresh vgroup shell internally. */
static UINode *p_children(P *p, UINode *into)
{
    if (!p_expect(p, T_LBRACE, "expected {")) return NULL;
    while (!p->failed && p->tok != T_RBRACE && p->tok != T_EOF) {
        UINode *c = p_block_list(p);
        if (c) ui_append(into, c);
    }
    p_expect(p, T_RBRACE, "expected }");
    return into;
}

static UINode *p_node(P *p)
{
    if (p->tok != T_IDENT) { p_fail(p, "expected node name"); return NULL; }

    /* ---- containers ---- */
    if (p_kw(p, "vgroup") || p_kw(p, "hgroup")) {
        UIType t = p_kw(p, "vgroup") ? UI_VGROUP : UI_HGROUP;
        p_next(p);
        UINode *n = p_new(p, t);
        if (!n) return NULL;
        n->spacing = UI_DEF_SPACING;
        p_attrs(p, n);
        p_children(p, n);
        return p->failed ? NULL : n;
    }

    if (p_kw(p, "page")) {
        p_next(p);
        UINode *n = p_new(p, UI_PAGE);
        if (!n) return NULL;
        n->u.page.active = 0;
        p_attrs(p, n);
        if (!p_expect(p, T_LBRACE, "expected {")) return NULL;
        while (!p->failed && p->tok != T_RBRACE && p->tok != T_EOF) {
            if (!p_kw(p, "tab")) { p_fail(p, "expected tab in page"); return NULL; }
            p_next(p);
            if (p->tok != T_STR) { p_fail(p, "expected tab label"); return NULL; }
            const char *lbl = p->sv;
            p_next(p);
            UINode *tab = p_new(p, UI_VGROUP);   /* implicit vgroup per tab */
            if (!tab) return NULL;
            tab->spacing = UI_DEF_SPACING;
            tab->tab = lbl;
            p_attrs(p, tab);                     /* attrs may override tab= */
            if (!tab->tab) tab->tab = lbl;
            p_children(p, tab);
            ui_append(n, tab);
        }
        p_expect(p, T_RBRACE, "expected }");
        return p->failed ? NULL : n;
    }

    /* ---- leaves ---- */
    UINode *n = NULL;
    if      (p_kw(p, "spacer"))   { p_next(p); n = p_new(p, UI_SPACER); if (n) n->weight = 1; }
    else if (p_kw(p, "label"))    {
        p_next(p);
        n = p_new(p, UI_LABEL);
        if (n && p->tok == T_STR) { n->u.text = p->sv; p_next(p); }
    }
    else if (p_kw(p, "button") || p_kw(p, "checkbox") || p_kw(p, "radio")) {
        UIType t = p_kw(p, "button") ? UI_BUTTON
                 : p_kw(p, "checkbox") ? UI_CHECKBOX : UI_RADIO;
        p_next(p);
        n = p_new(p, t);
        if (!n) return NULL;
        /* text and attrs may come in any order */
        while (!p->failed && (p->tok == T_STR || p->tok == T_IDENT)) {
            if (p->tok == T_STR) {
                if (n->u.text) { p_fail(p, "duplicate label"); return NULL; }
                n->u.text = p->sv;
                p_next(p);
            } else {
                p_attrs(p, n);
            }
        }
        if (!n->u.text) { p_fail(p, "expected label string"); return NULL; }
        p_expect(p, T_SEMI, "expected ;");
        return p->failed ? NULL : n;
    }
    else if (p_kw(p, "cycle") || p_kw(p, "listview")) {
        UIType t = p_kw(p, "cycle") ? UI_CYCLE : UI_LISTVIEW;
        p_next(p);
        n = p_new(p, t);
        if (!n) return NULL;
        int cnt = 0;
        const char * const *items = NULL;
        /* items may come before or after attrs — accept both orders */
        while (!p->failed) {
            if (p->tok == T_STR) {
                items = p_items(p, &cnt);
                if (!items) return NULL;
                n->u.items.items = items;
                n->u.items.count = cnt;
            } else if (p->tok == T_IDENT) {
                p_attrs(p, n);
            } else break;
        }
        if (!n->u.items.items) { p_fail(p, "expected items"); return NULL; }
        p_expect(p, T_SEMI, "expected ;");
        return p->failed ? NULL : n;
    }
    else if (p_kw(p, "slider") || p_kw(p, "integer")) {
        UIType t = p_kw(p, "slider") ? UI_SLIDER : UI_INTEGER;
        p_next(p);
        n = p_new(p, t);
        if (!n) return NULL;
        /* range and attrs may come in any order: slider id=1 0..100 value=50 */
        while (!p->failed && (p->tok == T_RANGE || p->tok == T_IDENT)) {
            if (p->tok == T_RANGE) {
                if (t == UI_SLIDER) {
                    n->u.range.min = p->iv; n->u.range.max = p->iv2;
                } else {
                    n->u.input.min = p->iv; n->u.input.max = p->iv2;
                }
                p_next(p);
            } else {
                p_attrs(p, n);
            }
        }
        p_expect(p, T_SEMI, "expected ;");
        return p->failed ? NULL : n;
    }
    else if (p_kw(p, "string")) {
        p_next(p);
        n = p_new(p, UI_STRING);
        if (!n) return NULL;
        n->u.input.max_chars = 63;
        while (!p->failed && (p->tok == T_STR || p->tok == T_IDENT)) {
            if (p->tok == T_STR) { n->u.input.text = p->sv; p_next(p); }
            else p_attrs(p, n);
        }
        p_expect(p, T_SEMI, "expected ;");
        return p->failed ? NULL : n;
    }
    else if (p_kw(p, "custom")) {
        p_next(p);
        n = p_new(p, UI_CUSTOM);
        /* draw/hit callbacks are attached by the app after parsing */
    }
    else {
        p_fail(p, "unknown node type");
        return NULL;
    }

    if (!n) return NULL;
    p_attrs(p, n);
    p_expect(p, T_SEMI, "expected ;");
    return p->failed ? NULL : n;
}

static UINode *p_block_list(P *p)
{
    return p_node(p);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */
UINode *ui_parse(UIArena *a, char *text, UIParseErr *err)
{
    if (!a || !text) return NULL;
    if (err) { err->line = 0; err->col = 0; err->msg[0] = '\0'; }

    P p;
    p.p = text; p.line = 1; p.col = 1;
    p.a = a; p.err = err; p.failed = 0;
    p.tok = 0; p.iv = p.iv2 = 0; p.sv = NULL;
    p_next(&p);

    if (!p_kw(&p, "window")) { p_fail(&p, "expected 'window'"); return NULL; }
    p_next(&p);
    if (p.tok != T_STR) { p_fail(&p, "expected window title"); return NULL; }
    const char *title = p.sv;
    p_next(&p);

    UINode *root = p_new(&p, UI_WINDOW);
    if (!root) return NULL;
    root->u.text = title;
    p_attrs(&p, root);

    /* collect children; multiple top-level nodes get an implicit vgroup */
    UINode *holder = p_new(&p, UI_VGROUP);
    if (!holder) return NULL;
    holder->spacing = UI_DEF_SPACING;
    if (!p_expect(&p, T_LBRACE, "expected {")) return NULL;
    while (!p.failed && p.tok != T_RBRACE && p.tok != T_EOF) {
        UINode *c = p_node(&p);
        if (c) ui_append(holder, c);
    }
    p_expect(&p, T_RBRACE, "expected }");
    p_expect(&p, T_EOF, "trailing content after window");
    if (p.failed) return NULL;

    if (!holder->first_child) { p_fail(&p, "window has no content"); return NULL; }
    if (holder->first_child->next_sibling) {
        ui_append(root, holder);        /* multiple roots: wrap in vgroup */
    } else {
        ui_append(root, holder->first_child);
    }
    return root;
}
