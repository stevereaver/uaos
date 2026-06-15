/* cmd_template.c — AmigaDOS-style command template parser implementation */

#include "cmd_template.h"
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Tiny string helpers (no libc)
 * ------------------------------------------------------------------------- */
static int ct_slen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int ct_tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int ct_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = ct_tolower((unsigned char)*a);
        int cb = ct_tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return ct_tolower((unsigned char)*a) - ct_tolower((unsigned char)*b);
}

static int ct_strcasecmp_n(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n && a[i] && b[i]; i++) {
        int ca = ct_tolower((unsigned char)a[i]);
        int cb = ct_tolower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
    }
    if (i == n) return 0;
    return ct_tolower((unsigned char)a[i]) - ct_tolower((unsigned char)b[i]);
}

static void ct_scopy(char *d, const char *s, int max)
{
    int i = 0;
    while (i < max - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static void ct_scopy_n(char *d, const char *s, int n, int max)
{
    int i = 0;
    while (i < n && i < max - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static void ct_scat(char *d, const char *s, int max)
{
    int dl = ct_slen(d);
    ct_scopy(d + dl, s, max - dl);
}

/* -------------------------------------------------------------------------
 * Parse template string
 * ------------------------------------------------------------------------- */
void CmdTemplate_Parse(const char *template_str, CmdTemplateResult *out)
{
    const char *p = template_str;
    out->count = 0;
    out->error[0] = '\0';

    while (*p) {
        /* Skip leading whitespace / commas */
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;

        if (out->count >= CMD_MAX_TEMPLATE_ITEMS) {
            ct_scopy(out->error, "too many template items", CMD_MAX_TEMPLATE_VAL);
            return;
        }

        CmdTemplateItem *item = &out->items[out->count];
        /* Zero the item */
        {
            char *base = (char *)item;
            for (int i = 0; i < (int)sizeof(CmdTemplateItem); i++) base[i] = 0;
        }

        /* Read item name */
        const char *name_start = p;
        while (*p && *p != '/' && *p != ',' && *p != ' ' && *p != '\t') p++;
        int name_len = (int)(p - name_start);
        if (name_len == 0) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == ',') p++;
            continue;
        }
        if (name_len >= CMD_MAX_TEMPLATE_NAME) name_len = CMD_MAX_TEMPLATE_NAME - 1;
        ct_scopy_n(item->name, name_start, name_len, CMD_MAX_TEMPLATE_NAME);

        /* Parse qualifiers */
        while (*p == '/') {
            p++;
            char q = ct_tolower((unsigned char)*p);
            if (q) p++;
            switch (q) {
                case 'a': item->required = 1; break;
                case 'k': item->keyword = 1; break;
                case 's': item->sw = 1; break;
                case 'n': item->number = 1; break;
                case 'm': item->multiple = 1; break;
                case 'f': item->free_arg = 1; break;
                default:  break;
            }
        }

        /* Skip trailing spaces before comma */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ',') p++;

        out->count++;
    }
}

/* -------------------------------------------------------------------------
 * Tokenise argument string
 * ------------------------------------------------------------------------- */
typedef struct {
    char  tok[CMD_MAX_TOKENS][CMD_MAX_TEMPLATE_VAL];
    int   used[CMD_MAX_TOKENS];
    int   n;
} TokArray;

static void tokenise(const char *args, TokArray *ta)
{
    ta->n = 0;
    const char *p = args;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (ta->n >= CMD_MAX_TOKENS) break;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        int len = (int)(p - start);
        if (len >= CMD_MAX_TEMPLATE_VAL) len = CMD_MAX_TEMPLATE_VAL - 1;
        ct_scopy_n(ta->tok[ta->n], start, len, CMD_MAX_TEMPLATE_VAL);
        ta->used[ta->n] = 0;
        ta->n++;
    }
}

/* -------------------------------------------------------------------------
 * Match arguments against a parsed template
 * ------------------------------------------------------------------------- */
void CmdTemplate_MatchArgs(CmdTemplateResult *out, const char *args)
{
    TokArray ta;
    int i, j;

    out->error[0] = '\0';
    if (!args) args = "";

    /* Reset presence / values from any prior match */
    for (i = 0; i < out->count; i++) {
        out->items[i].present = 0;
        out->items[i].value[0] = '\0';
        out->items[i].value_count = 0;
    }

    tokenise(args, &ta);

    /* --- Pass 1: switches (/S) and keyword args (/K) --- */
    for (i = 0; i < ta.n; i++) {
        if (ta.used[i]) continue;

        const char *t = ta.tok[i];
        int tlen = ct_slen(t);

        /* Look for KEYWORD=VALUE form */
        const char *eq = t;
        while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            int kwlen = (int)(eq - t);
            const char *val = eq + 1;
            int vallen = ct_slen(val);

            for (j = 0; j < out->count; j++) {
                CmdTemplateItem *it = &out->items[j];
                if (ct_strcasecmp_n(it->name, t, kwlen) == 0 &&
                    (int)ct_slen(it->name) == kwlen) {
                    if (it->sw) {
                        it->present = 1;
                    } else {
                        if (it->multiple && it->value_count < CMD_MAX_MULT_VALUES) {
                            ct_scopy_n(it->values[it->value_count], val,
                                       vallen, CMD_MAX_TEMPLATE_VAL);
                            it->value_count++;
                        } else {
                            ct_scopy(it->value, val, CMD_MAX_TEMPLATE_VAL);
                        }
                        it->present = 1;
                    }
                    ta.used[i] = 1;
                    break;
                }
            }
            continue;
        }

        /* Check for plain switch keyword (also covers /K args where the
         * keyword appears without '=' — in that case the NEXT token is the
         * value).  */
        for (j = 0; j < out->count; j++) {
            CmdTemplateItem *it = &out->items[j];
            if (ct_strcasecmp(it->name, t) != 0) continue;

            if (it->sw) {
                it->present = 1;
                ta.used[i] = 1;
                break;
            }
            if (it->keyword) {
                /* Next token is the value */
                if (i + 1 < ta.n && !ta.used[i + 1]) {
                    const char *val = ta.tok[i + 1];
                    int vallen = ct_slen(val);
                    if (it->multiple && it->value_count < CMD_MAX_MULT_VALUES) {
                        ct_scopy_n(it->values[it->value_count], val,
                                   vallen, CMD_MAX_TEMPLATE_VAL);
                        it->value_count++;
                    } else {
                        ct_scopy(it->value, val, CMD_MAX_TEMPLATE_VAL);
                    }
                    it->present = 1;
                    ta.used[i] = 1;
                    ta.used[i + 1] = 1;
                } else {
                    /* Keyword with no following value — still mark present,
                     * but leave value empty.  This happens for trailing
                     * keywords in some edge cases. */
                    it->present = 1;
                    ta.used[i] = 1;
                }
                break;
            }
        }
    }

    /* --- Pass 2: assign remaining tokens to positional args --- */
    for (i = 0; i < ta.n; i++) {
        if (ta.used[i]) continue;

        /* Find next positional (non-keyword, non-switch, non-present) item */
        for (j = 0; j < out->count; j++) {
            CmdTemplateItem *it = &out->items[j];
            if (it->sw || it->keyword) continue;
            if (it->present && !it->multiple && !it->free_arg) continue;

            if (it->multiple && it->value_count < CMD_MAX_MULT_VALUES) {
                ct_scopy(it->values[it->value_count], ta.tok[i],
                         CMD_MAX_TEMPLATE_VAL);
                it->value_count++;
                it->present = 1;
                ta.used[i] = 1;
                break;
            } else if (it->free_arg) {
                /* Absorb remaining tokens into free argument */
                if (it->value[0]) ct_scat(it->value, " ", CMD_MAX_TEMPLATE_VAL);
                ct_scat(it->value, ta.tok[i], CMD_MAX_TEMPLATE_VAL);
                it->present = 1;
                ta.used[i] = 1;
                break;
            } else {
                ct_scopy(it->value, ta.tok[i], CMD_MAX_TEMPLATE_VAL);
                it->present = 1;
                ta.used[i] = 1;
                break;
            }
        }
    }

    /* --- Pass 3: validate required args --- */
    for (j = 0; j < out->count; j++) {
        CmdTemplateItem *it = &out->items[j];
        if (it->required && !it->present) {
            char msg[CMD_MAX_TEMPLATE_VAL];
            ct_scopy(msg, "missing required argument: ", CMD_MAX_TEMPLATE_VAL);
            ct_scat(msg, it->name, CMD_MAX_TEMPLATE_VAL);
            ct_scopy(out->error, msg, CMD_MAX_TEMPLATE_VAL);
            return;
        }
    }

    /* Any leftover tokens after positional assignment are an error,
     * unless there's a free-arg that already absorbed them. */
    for (i = 0; i < ta.n; i++) {
        if (!ta.used[i]) {
            ct_scopy(out->error, "unexpected argument: ", CMD_MAX_TEMPLATE_VAL);
            ct_scat(out->error, ta.tok[i], CMD_MAX_TEMPLATE_VAL);
            return;
        }
    }
}

/* -------------------------------------------------------------------------
 * Query helpers
 * ------------------------------------------------------------------------- */
static CmdTemplateItem *find_item(CmdTemplateResult *res, const char *name)
{
    int i;
    for (i = 0; i < res->count; i++) {
        if (ct_strcasecmp(res->items[i].name, name) == 0)
            return &res->items[i];
    }
    return NULL;
}

const CmdTemplateItem *CmdTemplate_Find(const CmdTemplateResult *res,
                                        const char *name)
{
    return find_item((CmdTemplateResult *)res, name);
}

int CmdTemplate_GetSwitch(const CmdTemplateResult *res, const char *name)
{
    CmdTemplateItem *it = find_item((CmdTemplateResult *)res, name);
    return (it && it->sw && it->present) ? 1 : 0;
}

const char *CmdTemplate_GetString(const CmdTemplateResult *res,
                                  const char *name)
{
    CmdTemplateItem *it = find_item((CmdTemplateResult *)res, name);
    if (!it || !it->present) return NULL;
    return it->value;
}

int CmdTemplate_GetInt(const CmdTemplateResult *res, const char *name,
                       int *out)
{
    CmdTemplateItem *it = find_item((CmdTemplateResult *)res, name);
    if (!it || !it->present) return 0;
    const char *v = it->value;
    int neg = 0;
    int val = 0;
    const char *p = v;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') { p++; }
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    if (*p && *p != ' ') return 0; /* trailing junk */
    *out = neg ? -val : val;
    return 1;
}

int CmdTemplate_GetCount(const CmdTemplateResult *res, const char *name)
{
    CmdTemplateItem *it = find_item((CmdTemplateResult *)res, name);
    if (!it) return 0;
    return it->value_count;
}

const char *CmdTemplate_GetMulti(const CmdTemplateResult *res,
                                  const char *name, int idx)
{
    CmdTemplateItem *it = find_item((CmdTemplateResult *)res, name);
    if (!it || idx < 0 || idx >= it->value_count) return NULL;
    return it->values[idx];
}
