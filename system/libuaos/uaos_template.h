/* uaos_template.h — AmigaDOS-style command template parser for userspace
 *
 * Header-only port of kernel/shell/cmd_template.c so that native x86-64
 * UAOS C: commands can parse AmigaDOS ReadArgs-style templates directly
 * from their argv-derived argument string.  No standard library required.
 *
 * Templates use the AmigaDOS qualifier syntax:
 *   /A  required argument
 *   /K  keyword argument (must be named: KEYWORD=value or KEYWORD value)
 *   /S  switch (boolean, presence means true)
 *   /N  numeric argument
 *   /M  multiple values
 *   /F  free-form argument (absorbs all remaining tokens)
 *
 * Example template:  "FROM/A,TO/A,ALL/S,CLONE/S,BUFFER/K/N"
 */

#ifndef UAOS_TEMPLATE_H
#define UAOS_TEMPLATE_H

#include <stdint.h>

#define UAOS_TMPL_MAX_ITEMS       16
#define UAOS_TMPL_MAX_NAME        32
#define UAOS_TMPL_MAX_VAL         128
#define UAOS_TMPL_MAX_MULT_ITEMS  2   /* /M items supported per template */
#define UAOS_TMPL_MAX_MULT_VALUES 32  /* values per /M item (== MAX_TOKENS) */
#define UAOS_TMPL_MAX_TOKENS      32

typedef struct {
    char name[UAOS_TMPL_MAX_NAME];
    int  required;      /* /A */
    int  keyword;       /* /K */
    int  sw;            /* /S */
    int  number;        /* /N */
    int  multiple;      /* /M */
    int  free_arg;      /* /F */
    int  present;
    char value[UAOS_TMPL_MAX_VAL];
    int  value_index;   /* /M value block in the result's multi_pool */
    int  value_count;
} UaosTmplItem;

typedef struct {
    UaosTmplItem items[UAOS_TMPL_MAX_ITEMS];
    /* /M items draw their value strings from this shared pool — each /M
     * item claims one block of UAOS_TMPL_MAX_MULT_VALUES slots at parse
     * time.  The old per-item values[4][128] silently dropped every
     * value past the fourth (echo printed only the first four words of
     * a line — UAOS-256); the pool covers the tokenizer's full token
     * count for one fixed cost instead of scaling per item. */
    char         multi_pool[UAOS_TMPL_MAX_MULT_ITEMS]
                           [UAOS_TMPL_MAX_MULT_VALUES]
                           [UAOS_TMPL_MAX_VAL];
    int          multi_used;   /* pool blocks claimed so far */
    int          count;
    char         error[UAOS_TMPL_MAX_VAL];
} UaosTmpl;

/* -------------------------------------------------------------------------
 * Tiny string helpers (no libc)
 * ------------------------------------------------------------------------- */
static inline int uaos_tmpl_slen(const char *s)
{
    int n = 0; while (s[n]) n++; return n;
}

static inline int uaos_tmpl_tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static inline int uaos_tmpl_casecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = uaos_tmpl_tolower((unsigned char)*a);
        int cb = uaos_tmpl_tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return uaos_tmpl_tolower((unsigned char)*a) - uaos_tmpl_tolower((unsigned char)*b);
}

static inline int uaos_tmpl_casecmp_n(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n && a[i] && b[i]; i++) {
        int ca = uaos_tmpl_tolower((unsigned char)a[i]);
        int cb = uaos_tmpl_tolower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
    }
    if (i == n) return 0;
    return uaos_tmpl_tolower((unsigned char)a[i]) - uaos_tmpl_tolower((unsigned char)b[i]);
}

static inline void uaos_tmpl_scopy(char *d, const char *s, int max)
{
    int i = 0;
    while (i < max - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static inline void uaos_tmpl_scopy_n(char *d, const char *s, int n, int max)
{
    int i = 0;
    while (i < n && i < max - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static inline void uaos_tmpl_scat(char *d, const char *s, int max)
{
    int dl = uaos_tmpl_slen(d);
    uaos_tmpl_scopy(d + dl, s, max - dl);
}

/* -------------------------------------------------------------------------
 * Parse a template string into a UaosTmpl descriptor.
 * ------------------------------------------------------------------------- */
static inline void uaos_tmpl_parse(const char *template_str, UaosTmpl *out)
{
    const char *p = template_str;
    out->count = 0;
    out->multi_used = 0;
    out->error[0] = '\0';

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;

        if (out->count >= UAOS_TMPL_MAX_ITEMS) {
            uaos_tmpl_scopy(out->error, "too many template items", UAOS_TMPL_MAX_VAL);
            return;
        }

        UaosTmplItem *item = &out->items[out->count];
        char *base = (char *)item;
        for (int i = 0; i < (int)sizeof(UaosTmplItem); i++) base[i] = 0;
        item->value_index = -1;

        const char *name_start = p;
        while (*p && *p != '/' && *p != ',' && *p != ' ' && *p != '\t') p++;
        int name_len = (int)(p - name_start);
        if (name_len == 0) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == ',') p++;
            continue;
        }
        if (name_len >= UAOS_TMPL_MAX_NAME) name_len = UAOS_TMPL_MAX_NAME - 1;
        uaos_tmpl_scopy_n(item->name, name_start, name_len, UAOS_TMPL_MAX_NAME);

        while (*p == '/') {
            p++;
            char q = uaos_tmpl_tolower((unsigned char)*p);
            if (q) p++;
            switch (q) {
                case 'a': item->required = 1; break;
                case 'k': item->keyword = 1; break;
                case 's': item->sw = 1; break;
                case 'n': item->number = 1; break;
                case 'm':
                    item->multiple = 1;
                    /* Claim a contiguous value block from the shared pool. */
                    if (item->value_index < 0) {
                        if (out->multi_used >= UAOS_TMPL_MAX_MULT_ITEMS) {
                            uaos_tmpl_scopy(out->error, "too many /M template items",
                                            UAOS_TMPL_MAX_VAL);
                            return;
                        }
                        item->value_index = out->multi_used++;
                    }
                    break;
                case 'f': item->free_arg = 1; break;
                default:  break;
            }
        }

        while (*p == ' ' || *p == '\t') p++;
        if (*p == ',') p++;

        out->count++;
    }
}

/* -------------------------------------------------------------------------
 * Tokenise argument string
 * ------------------------------------------------------------------------- */
typedef struct {
    char tok[UAOS_TMPL_MAX_TOKENS][UAOS_TMPL_MAX_VAL];
    int  used[UAOS_TMPL_MAX_TOKENS];
    int  quoted[UAOS_TMPL_MAX_TOKENS];
    int  n;
} UaosTmplTokArray;

static inline void uaos_tmpl_tokenise(const char *args, UaosTmplTokArray *ta)
{
    ta->n = 0;
    const char *p = args;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (ta->n >= UAOS_TMPL_MAX_TOKENS) break;
        ta->quoted[ta->n] = 0;
        if (*p == '"') {
            /* Double-quoted token: consume through the closing quote so a
             * literal string may contain spaces and words that collide
             * with template keywords (echo "a TO b").  The quotes are
             * stripped and the token is exempt from keyword matching. */
            p++;
            const char *start = p;
            while (*p && *p != '"') p++;
            int len = (int)(p - start);
            if (*p == '"') p++;
            if (len >= UAOS_TMPL_MAX_VAL) len = UAOS_TMPL_MAX_VAL - 1;
            uaos_tmpl_scopy_n(ta->tok[ta->n], start, len, UAOS_TMPL_MAX_VAL);
            ta->quoted[ta->n] = 1;
        } else {
            const char *start = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            int len = (int)(p - start);
            if (len >= UAOS_TMPL_MAX_VAL) len = UAOS_TMPL_MAX_VAL - 1;
            uaos_tmpl_scopy_n(ta->tok[ta->n], start, len, UAOS_TMPL_MAX_VAL);
            /* Strip one layer of surrounding double-quotes (a quote pair
             * that happened to fit inside a single whitespace-delimited
             * word, e.g. cmd foo"bar"). */
            int tlen = uaos_tmpl_slen(ta->tok[ta->n]);
            if (tlen >= 2 && ta->tok[ta->n][0] == '"' &&
                ta->tok[ta->n][tlen - 1] == '"') {
                ta->tok[ta->n][tlen - 1] = '\0';
                uaos_tmpl_scopy(ta->tok[ta->n], ta->tok[ta->n] + 1,
                                UAOS_TMPL_MAX_VAL);
                ta->quoted[ta->n] = 1;
            }
        }
        ta->used[ta->n] = 0;
        ta->n++;
    }
}

/* -------------------------------------------------------------------------
 * Match arguments against a parsed template.
 * ------------------------------------------------------------------------- */
static inline void uaos_tmpl_match(UaosTmpl *out, const char *args)
{
    UaosTmplTokArray ta;
    int i, j;

    out->error[0] = '\0';
    if (!args) args = "";

    for (i = 0; i < out->count; i++) {
        out->items[i].present = 0;
        out->items[i].value[0] = '\0';
        out->items[i].value_count = 0;
    }

    uaos_tmpl_tokenise(args, &ta);

    /* Pass 1: switches (/S), keyword args (/K), and KEYWORD=VALUE forms.
     * AmigaDOS ReadArgs treats every template item name as a keyword —
     * /K merely makes the keyword *required* — so "FROM dir" binds a
     * positional/M item by name too.  Quoted tokens are literal strings
     * and never match a keyword. */
    for (i = 0; i < ta.n; i++) {
        if (ta.used[i] || ta.quoted[i]) continue;
        const char *t = ta.tok[i];
        int tlen = uaos_tmpl_slen(t);

        const char *eq = t;
        while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            int kwlen = (int)(eq - t);
            const char *val = eq + 1;
            int vallen = uaos_tmpl_slen(val);
            for (j = 0; j < out->count; j++) {
                UaosTmplItem *it = &out->items[j];
                if (uaos_tmpl_casecmp_n(it->name, t, kwlen) == 0 &&
                    (int)uaos_tmpl_slen(it->name) == kwlen) {
                    if (it->sw) {
                        it->present = 1;
                    } else {
                        if (it->multiple && it->value_count < UAOS_TMPL_MAX_MULT_VALUES) {
                            uaos_tmpl_scopy_n(out->multi_pool[it->value_index]
                                                         [it->value_count],
                                              val, vallen, UAOS_TMPL_MAX_VAL);
                            it->value_count++;
                        } else {
                            uaos_tmpl_scopy(it->value, val, UAOS_TMPL_MAX_VAL);
                        }
                        it->present = 1;
                    }
                    ta.used[i] = 1;
                    break;
                }
            }
            continue;
        }

        for (j = 0; j < out->count; j++) {
            UaosTmplItem *it = &out->items[j];
            if (uaos_tmpl_casecmp(it->name, t) != 0) continue;
            (void)tlen;

            if (it->sw) {
                it->present = 1;
                ta.used[i] = 1;
                break;
            }
            /* /K keyword or bare item name used in keyword form: the
             * NEXT token is the value. */
            if (i + 1 < ta.n && !ta.used[i + 1]) {
                const char *val = ta.tok[i + 1];
                int vallen = uaos_tmpl_slen(val);
                if (it->multiple && it->value_count < UAOS_TMPL_MAX_MULT_VALUES) {
                    uaos_tmpl_scopy_n(out->multi_pool[it->value_index]
                                                 [it->value_count],
                                      val, vallen, UAOS_TMPL_MAX_VAL);
                    it->value_count++;
                } else {
                    uaos_tmpl_scopy(it->value, val, UAOS_TMPL_MAX_VAL);
                }
                it->present = 1;
                ta.used[i] = 1;
                ta.used[i + 1] = 1;
            } else {
                it->present = 1;
                ta.used[i] = 1;
            }
            break;
        }
    }

    /* Pass 2: assign remaining tokens to positional args */
    for (i = 0; i < ta.n; i++) {
        if (ta.used[i]) continue;
        for (j = 0; j < out->count; j++) {
            UaosTmplItem *it = &out->items[j];
            if (it->sw || it->keyword) continue;
            if (it->present && !it->multiple && !it->free_arg) continue;

            /* A /M (multiple) item must not starve positional args that
             * follow it: AmigaDOS only lets it absorb tokens while enough
             * remain to fill the later positional items.  e.g. with
             * "FROM/M,TO/A", `copy a b` must leave 'b' for TO. */
            if (it->multiple) {
                int needed = 0;
                for (int k = j + 1; k < out->count; k++) {
                    UaosTmplItem *later = &out->items[k];
                    if (later->sw || later->keyword) continue;
                    if (later->present) continue;
                    needed++;
                }
                int free_toks = 0;
                for (int k = i; k < ta.n; k++) {
                    if (!ta.used[k]) free_toks++;
                }
                if (free_toks <= needed) continue;
            }

            if (it->multiple && it->value_count < UAOS_TMPL_MAX_MULT_VALUES) {
                uaos_tmpl_scopy(out->multi_pool[it->value_index][it->value_count],
                                ta.tok[i], UAOS_TMPL_MAX_VAL);
                it->value_count++;
                it->present = 1;
                ta.used[i] = 1;
                break;
            } else if (it->free_arg) {
                if (it->value[0]) uaos_tmpl_scat(it->value, " ", UAOS_TMPL_MAX_VAL);
                uaos_tmpl_scat(it->value, ta.tok[i], UAOS_TMPL_MAX_VAL);
                it->present = 1;
                ta.used[i] = 1;
                break;
            } else {
                uaos_tmpl_scopy(it->value, ta.tok[i], UAOS_TMPL_MAX_VAL);
                it->present = 1;
                ta.used[i] = 1;
                break;
            }
        }
    }

    /* Pass 3: validate required args */
    for (j = 0; j < out->count; j++) {
        UaosTmplItem *it = &out->items[j];
        if (it->required && !it->present) {
            uaos_tmpl_scopy(out->error, "missing required argument: ", UAOS_TMPL_MAX_VAL);
            uaos_tmpl_scat(out->error, it->name, UAOS_TMPL_MAX_VAL);
            return;
        }
    }

    for (i = 0; i < ta.n; i++) {
        if (!ta.used[i]) {
            uaos_tmpl_scopy(out->error, "unexpected argument: ", UAOS_TMPL_MAX_VAL);
            uaos_tmpl_scat(out->error, ta.tok[i], UAOS_TMPL_MAX_VAL);
            return;
        }
    }
}

/* -------------------------------------------------------------------------
 * Convenience: parse + match in one call.
 * ------------------------------------------------------------------------- */
static inline void uaos_tmpl_run(const char *template_str, UaosTmpl *out,
                                 const char *args)
{
    uaos_tmpl_parse(template_str, out);
    if (out->error[0]) return;
    uaos_tmpl_match(out, args);
}

/* -------------------------------------------------------------------------
 * Query helpers
 * ------------------------------------------------------------------------- */
static inline UaosTmplItem *uaos_tmpl_find(UaosTmpl *res, const char *name)
{
    for (int i = 0; i < res->count; i++)
        if (uaos_tmpl_casecmp(res->items[i].name, name) == 0)
            return &res->items[i];
    return NULL;
}

static inline int uaos_tmpl_switch(const UaosTmpl *res, const char *name)
{
    UaosTmplItem *it = uaos_tmpl_find((UaosTmpl *)res, name);
    return (it && it->sw && it->present) ? 1 : 0;
}

static inline const char *uaos_tmpl_string(const UaosTmpl *res, const char *name)
{
    UaosTmplItem *it = uaos_tmpl_find((UaosTmpl *)res, name);
    if (!it || !it->present) return NULL;
    return it->value;
}

static inline int uaos_tmpl_int(const UaosTmpl *res, const char *name, int *out)
{
    UaosTmplItem *it = uaos_tmpl_find((UaosTmpl *)res, name);
    if (!it || !it->present) return 0;
    const char *v = it->value;
    int neg = 0, val = 0;
    const char *p = v;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') { p++; }
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
    if (*p && *p != ' ') return 0;
    *out = neg ? -val : val;
    return 1;
}

static inline int uaos_tmpl_count(const UaosTmpl *res, const char *name)
{
    UaosTmplItem *it = uaos_tmpl_find((UaosTmpl *)res, name);
    return it ? it->value_count : 0;
}

static inline const char *uaos_tmpl_multi(const UaosTmpl *res, const char *name, int idx)
{
    UaosTmplItem *it = uaos_tmpl_find((UaosTmpl *)res, name);
    if (!it || idx < 0 || idx >= it->value_count) return NULL;
    return res->multi_pool[it->value_index][idx];
}

#endif /* UAOS_TEMPLATE_H */
