/* cmd_template.h — AmigaDOS-style command template parser
 *
 * Provides ReadArgs-compatible template parsing for native UAOS shell
 * commands.  Templates use the AmigaDOS qualifier syntax:
 *
 *   /A  required argument
 *   /K  keyword argument (must be named: KEYWORD=value)
 *   /S  switch (boolean, presence means true)
 *   /N  numeric argument (value must be a number)
 *   /M  multiple values (can appear more than once)
 *   /F  free-form argument (absorbs all remaining tokens)
 *
 * Example template:  "FROM/A,TO/A,ALL/S,CLONE/S,BUFFER/K/N"
 */

#ifndef UAOS_CMD_TEMPLATE_H
#define UAOS_CMD_TEMPLATE_H

#include <stdint.h>

#define CMD_MAX_TEMPLATE_ITEMS 16
#define CMD_MAX_TEMPLATE_NAME  32
#define CMD_MAX_TEMPLATE_VAL   128
#define CMD_MAX_MULT_VALUES    8
#define CMD_MAX_TOKENS         32

typedef struct {
    char  name[CMD_MAX_TEMPLATE_NAME];
    int   required;      /* /A */
    int   keyword;       /* /K */
    int   sw;            /* /S */
    int   number;        /* /N */
    int   multiple;      /* /M */
    int   free_arg;      /* /F */
    int   present;
    char  value[CMD_MAX_TEMPLATE_VAL];
    char  values[CMD_MAX_MULT_VALUES][CMD_MAX_TEMPLATE_VAL];
    int   value_count;
} CmdTemplateItem;

typedef struct {
    CmdTemplateItem items[CMD_MAX_TEMPLATE_ITEMS];
    int             count;
    char            error[CMD_MAX_TEMPLATE_VAL];
} CmdTemplateResult;

/* -------------------------------------------------------------------------
 * Parse a template string into a CmdTemplateResult.
 * Does not validate arguments — only builds the item descriptor table.
 * ------------------------------------------------------------------------- */
void CmdTemplate_Parse(const char *template_str, CmdTemplateResult *out);

/* -------------------------------------------------------------------------
 * Match an argument string against a parsed template.
 * Fills in .present, .value and .values for each item.
 * On error, sets out->error to a descriptive message.
 * ------------------------------------------------------------------------- */
void CmdTemplate_MatchArgs(CmdTemplateResult *out, const char *args);

/* -------------------------------------------------------------------------
 * Query helpers — return NULL / 0 if the named item does not exist.
 * ------------------------------------------------------------------------- */
const CmdTemplateItem *CmdTemplate_Find(const CmdTemplateResult *res,
                                        const char *name);
int   CmdTemplate_GetSwitch(const CmdTemplateResult *res, const char *name);
const char *CmdTemplate_GetString(const CmdTemplateResult *res,
                                  const char *name);
/* Returns 1 and fills *out if the named item exists and is numeric. */
int   CmdTemplate_GetInt(const CmdTemplateResult *res,
                         const char *name, int *out);

/* Return the number of values collected for a /M item. */
int   CmdTemplate_GetCount(const CmdTemplateResult *res, const char *name);

/* Return the i-th value of a /M item, or NULL. */
const char *CmdTemplate_GetMulti(const CmdTemplateResult *res,
                                  const char *name, int idx);

#endif /* UAOS_CMD_TEMPLATE_H */
