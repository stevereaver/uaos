/* ui_layout_test.c — host-side unit tests for kernel/display/uitree.c
 *
 * Builds UINode trees on a heap-backed arena, runs ui_measure() /
 * ui_arrange() at several window sizes, and asserts the resulting rects.
 * uitree.c is dependency-free, so this links against the real kernel
 * source — a failure here means the kernel layout engine is wrong.
 *
 * Build:  gcc -O2 -I kernel/display -o build/ui_layout_test \
 *             tools/ui_layout_test.c kernel/display/uitree.c
 * Run:    ./build/ui_layout_test
 */

#include <stdio.h>
#include <stdlib.h>
#include "uitree.h"

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond, msg) do {                                        \
    g_checks++;                                                      \
    if (!(cond)) {                                                   \
        g_fails++;                                                   \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);         \
    }                                                              \
} while (0)

#define CHECK_EQ(a, b, msg) do {                                     \
    g_checks++;                                                      \
    long _va = (long)(a), _vb = (long)(b);                           \
    if (_va != _vb) {                                                \
        g_fails++;                                                   \
        printf("FAIL %s:%d: %s (got %ld, want %ld)\n",               \
               __FILE__, __LINE__, msg, _va, _vb);                   \
    }                                                              \
} while (0)

#define ARENA_BYTES (64 * 1024)

static UIArena g_arena;

static void arena_reset(void)
{
    ui_arena_reset(&g_arena);
}

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */
static void dump_node(const UINode *n, int depth)
{
    for (int i = 0; i < depth; i++) printf("  ");
    printf("type=%d id=%d box=(%d,%d %dx%d) nat=(%d,%d) w=%d\n",
           n->type, n->id, n->x, n->y, n->w, n->h,
           n->nat_w, n->nat_h, n->weight);
    for (const UINode *c = n->first_child; c; c = c->next_sibling)
        dump_node(c, depth + 1);
}

/* Assert children tile the parent's main axis flush (no overlap, no gap). */
static void check_tiles(UINode *group, int horiz)
{
    int pos = horiz ? group->x : group->y;
    int end = pos + (horiz ? group->w : group->h);
    int before = g_fails;
    for (UINode *c = group->first_child; c; c = c->next_sibling) {
        int cs = horiz ? c->x : c->y;
        int ce = cs + (horiz ? c->w : c->h);
        CHECK_EQ(cs, pos, "child starts flush after previous");
        pos = ce;
        if (c->next_sibling)
            pos += group->spacing;
    }
    /* last child ends at group edge, or earlier if the group had no
     * weighted children and free space stays at the tail */
    CHECK(pos <= end, "children do not overflow group");
    if (g_fails != before)
        dump_node(group, 0);
}

/* -------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

/* A single leaf fills the whole window. */
static void test_single_fill(void)
{
    arena_reset();
    UINode *root = ui_window(&g_arena, "T",
                             ui_button(&g_arena, "OK", 1));
    ui_layout(root, 10, 20, 300, 200);

    UINode *btn = ui_find(root, 1);
    CHECK(btn != NULL, "button found");
    CHECK_EQ(btn->x, 10, "btn x");
    CHECK_EQ(btn->y, 20, "btn y");
    CHECK_EQ(btn->w, 300, "btn w fills");
    CHECK_EQ(btn->h, 200, "btn h fills");
}

/* VGroup{ label(w0), custom(w1), hgroup{3 buttons(w1 each)} } — the
 * button row sits at the bottom at natural height, custom takes slack. */
static void test_vgroup_weights(void)
{
    arena_reset();
    UINode *custom = ui_w(ui_custom(&g_arena, 10, 10, NULL, NULL, 7), 1);
    UINode *row = ui_hgroup(&g_arena,
                            ui_w(ui_button(&g_arena, "Save",   1), 1),
                            ui_w(ui_button(&g_arena, "Use",    2), 1),
                            ui_w(ui_button(&g_arena, "Cancel", 3), 1),
                            NULL);
    UINode *root = ui_window(&g_arena, "T",
        ui_vgroup(&g_arena,
                  ui_label(&g_arena, "Hello"),
                  custom,
                  row,
                  NULL));

    ui_layout(root, 0, 0, 400, 300);

    UINode *lbl = ui_find(root, -1); /* label has id 0 */
    (void)lbl;
    UINode *grp = row->parent;       /* the vgroup */
    check_tiles(grp, 0);
    check_tiles(row, 1);

    /* button row height = natural button height; ends flush at bottom */
    CHECK_EQ(row->h, UI_BUTTON_H, "button row keeps natural height");
    CHECK_EQ(row->y + row->h, 300, "button row at bottom");

    /* three weighted buttons split the row evenly (minus gaps) */
    UINode *b1 = ui_find(root, 1), *b2 = ui_find(root, 2), *b3 = ui_find(root, 3);
    int inner = 400 - 2 * row->spacing;
    CHECK_EQ(b1->w + b2->w + b3->w, inner, "buttons tile row width");
    CHECK(b1->w > 0 && b2->w > 0 && b3->w > 0, "all buttons visible");
    CHECK_EQ(b2->x, b1->x + b1->w + row->spacing, "b2 follows b1 + gap");

    /* custom absorbed the slack between label and button row */
    CHECK(custom->h > UI_BUTTON_H, "custom grew");
    CHECK_EQ(custom->y + custom->h, row->y - grp->spacing, "custom above row");

    /* resize: same proportions hold */
    ui_layout(root, 0, 0, 600, 400);
    CHECK_EQ(row->h, UI_BUTTON_H, "row height stable on resize");
    CHECK_EQ(row->y + row->h, 400, "row still at bottom");
    check_tiles(grp, 0);
    check_tiles(row, 1);
}

/* Window smaller than the natural tree: children shrink proportionally,
 * still tile flush, no negative sizes. */
static void test_shrink(void)
{
    arena_reset();
    UINode *root = ui_window(&g_arena, "T",
        ui_vgroup(&g_arena,
                  ui_w(ui_custom(&g_arena, 200, 100, NULL, NULL, 1), 1),
                  ui_w(ui_custom(&g_arena, 200, 100, NULL, NULL, 2), 1),
                  ui_button(&g_arena, "OK", 3),
                  NULL));
    ui_layout(root, 0, 0, 200, 60);

    UINode *grp = ui_find(root, 1)->parent;
    check_tiles(grp, 0);
    UINode *b = ui_find(root, 3);
    CHECK(b->h > 0 && b->h < UI_BUTTON_H, "button shrank below natural");
    for (UINode *c = grp->first_child; c; c = c->next_sibling)
        CHECK(c->h >= 0, "no negative child height");
}

/* With no weighted children, free space stays at the tail (start-packed). */
static void test_no_weight_packed(void)
{
    arena_reset();
    UINode *root = ui_window(&g_arena, "T",
        ui_hgroup(&g_arena,
                  ui_label(&g_arena, "Name:"),
                  ui_string(&g_arena, "", 10, 5),
                  NULL));
    ui_layout(root, 0, 0, 400, 30);

    UINode *grp  = ui_find(root, 5)->parent;
    UINode *str  = ui_find(root, 5);
    UINode *lbl  = grp->first_child;
    CHECK_EQ(lbl->x, 0, "label left-packed");
    CHECK_EQ(str->x, lbl->w + grp->spacing, "string follows label");
    CHECK(str->x + str->w < grp->x + grp->w,
          "trailing space left empty (not absorbed)");
}

/* Spacer pushes a button to the right edge. */
static void test_spacer_right_align(void)
{
    arena_reset();
    UINode *btn = ui_button(&g_arena, "OK", 9);
    UINode *root = ui_window(&g_arena, "T",
        ui_hgroup(&g_arena, ui_spacer(&g_arena), btn, NULL));
    ui_layout(root, 0, 0, 300, 30);

    CHECK_EQ(btn->x + btn->w, 300, "button flush right");
    CHECK_EQ(btn->w, btn->nat_w, "button kept natural width");
    CHECK(ui_find(root, 9)->x > 0, "spacer pushed button right");
}

/* Explicit min wins over natural; explicit max caps the box. */
static void test_min_max(void)
{
    arena_reset();
    UINode *big = ui_min(ui_label(&g_arena, "x"), 150, 40);
    UINode *cap = ui_max(ui_label(&g_arena, "wide label text"), 50, 0);
    UINode *root = ui_window(&g_arena, "T",
        ui_vgroup(&g_arena, big, cap, NULL));
    ui_layout(root, 0, 0, 400, 200);

    CHECK(big->nat_w >= 150 && big->nat_h >= 40, "min respected in measure");
    CHECK_EQ(cap->w, 50, "max caps arranged width");
}

/* UI_F_CENTER: node keeps natural size and centres in its cell. */
static void test_center(void)
{
    arena_reset();
    UINode *btn = ui_flags(ui_button(&g_arena, "OK", 4), UI_F_CENTER);
    UINode *root = ui_window(&g_arena, "T", ui_vgroup(&g_arena, btn, NULL));
    ui_layout(root, 0, 0, 300, 100);

    int bw = btn->nat_w;  /* natural button width */
    CHECK_EQ(btn->w, bw, "centred button kept natural width");
    CHECK_EQ(btn->x, (300 - bw) / 2, "centred horizontally");
    CHECK(btn->h < 100, "centred button kept natural height");
}

/* PAGE container: children share one content rect below the tab strip;
 * measure accounts for tab strip height and total tab width. */
static void test_page(void)
{
    arena_reset();
    UINode *pg = ui_page(&g_arena,
        ui_tab(ui_label(&g_arena, "page one content"), "One"),
        ui_tab(ui_label(&g_arena, "page two content"), "Two"),
        NULL);
    UINode *root = ui_window(&g_arena, "T", pg);
    ui_layout(root, 0, 0, 400, 200);

    UINode *p1 = pg->first_child, *p2 = p1->next_sibling;
    CHECK_EQ(p1->x, p2->x, "pages share x");
    CHECK_EQ(p1->y, p2->y, "pages share y");
    CHECK_EQ(p1->w, p2->w, "pages share w");
    CHECK_EQ(p1->y, pg->y + UI_PAGE_TAB_H, "content below tab strip");

    int tx, ty, tw, th, tx2;
    CHECK(ui_page_tab_rect(pg, 0, &tx, &ty, &tw, &th), "tab 0 rect");
    CHECK_EQ(ty, pg->y, "tab strip at top");
    CHECK_EQ(th, UI_PAGE_TAB_H, "tab height");
    CHECK(ui_page_tab_rect(pg, 1, &tx2, &ty, &tw, &th), "tab 1 rect");
    CHECK_EQ(tx2, tx + tw, "tabs lay out sequentially");
    CHECK(!ui_page_tab_rect(pg, 2, &tx, &ty, &tw, &th), "no tab 2");

    /* hit-test: tab strip returns the page, body returns active child */
    CHECK_EQ(ui_node_at(pg, tx + 2, ty + 2), pg, "tab strip hit -> page");
    CHECK_EQ(ui_node_at(pg, 10, pg->y + UI_PAGE_TAB_H + 2), p1,
             "body hit -> active page child");
    pg->u.page.active = 1;
    CHECK_EQ(ui_node_at(pg, 10, pg->y + UI_PAGE_TAB_H + 2), p2,
             "body hit -> second page when active");
}

/* Hit-testing returns the deepest leaf; misses return NULL. */
static void test_hit(void)
{
    arena_reset();
    UINode *root = ui_window(&g_arena, "T",
        ui_vgroup(&g_arena,
                  ui_w(ui_button(&g_arena, "A", 10), 1),
                  ui_w(ui_button(&g_arena, "B", 11), 1),
                  NULL));
    ui_layout(root, 0, 0, 200, 100);

    UINode *a = ui_find(root, 10), *b = ui_find(root, 11);
    CHECK_EQ(ui_node_at(root, a->x + 1, a->y + 1), a, "hit button A");
    CHECK_EQ(ui_node_at(root, b->x + 1, b->y + 1), b, "hit button B");
    CHECK(ui_node_at(root, 9999, 9999) == NULL, "outside -> NULL");
    CHECK(ui_node_at(root, -5, -5) == NULL, "negative -> NULL");
}

/* Every leaf kind reports a sane natural size. */
static void test_natural_sizes(void)
{
    arena_reset();
    static const char * const cyc[]  = { "Short", "Much longer item" };
    static const char * const list[] = { "a", "b", "c" };
    UINode *root = ui_vgroup(&g_arena,
        ui_label(&g_arena, "lbl"),
        ui_button(&g_arena, "btn", 1),
        ui_checkbox(&g_arena, "chk", 2),
        ui_radio(&g_arena, "r1", 1, 3),
        ui_cycle(&g_arena, cyc, 2, 4),
        ui_slider(&g_arena, 0, 100, 50, 5),
        ui_string(&g_arena, "", 20, 6),
        ui_integer(&g_arena, 0, -999, 9999, 7),
        ui_listview(&g_arena, list, 3, 8),
        ui_spacer(&g_arena),
        NULL);
    ui_measure(root);

    UINode *c = root->first_child;
    CHECK(c->nat_w > 0 && c->nat_h == UI_LINE_H, "label natural");
    c = c->next_sibling;
    CHECK(c->nat_h == UI_BUTTON_H, "button height");
    c = c->next_sibling;
    CHECK(c->nat_h == UI_CHECK_H, "checkbox height");
    c = c->next_sibling;
    CHECK(c->nat_h == UI_CHECK_H, "radio height");
    c = c->next_sibling;
    CHECK(c->nat_w >= 17 * UI_CHAR_W, "cycle fits longest item");
    c = c->next_sibling;
    CHECK(c->nat_h == UI_SLIDER_H, "slider height");
    c = c->next_sibling;
    CHECK(c->nat_w >= 20 * UI_CHAR_W, "string fits max_chars");
    c = c->next_sibling;
    CHECK(c->nat_w >= 5 * UI_CHAR_W, "integer fits range digits");
    c = c->next_sibling;
    CHECK(c->nat_w > 0 && c->nat_h > 0, "listview has a size");
    c = c->next_sibling;
    CHECK_EQ(c->nat_w, 0, "spacer is 0x0");
    CHECK_EQ(c->nat_h, 0, "spacer is 0x0");
    CHECK_EQ(c->weight, 1, "spacer defaults to weight 1");
}

/* ui_append builds the same tree as the varargs constructors. */
static void test_append(void)
{
    arena_reset();
    UINode *grp = ui_hgroup(&g_arena, NULL);
    ui_append(grp, ui_button(&g_arena, "A", 1));
    ui_append(grp, ui_button(&g_arena, "B", 2));
    CHECK_EQ(ui_child_count(grp), 2, "append added 2 children");
    CHECK_EQ(grp->first_child->id, 1, "order preserved");
    CHECK_EQ(grp->first_child->next_sibling->id, 2, "order preserved");
    CHECK_EQ(grp->first_child->parent, grp, "parent link set");
}

/* Arena exhaustion returns NULL instead of corrupting memory. */
static void test_arena_exhaust(void)
{
    char small[sizeof(UINode)];   /* exactly one node fits */
    UIArena a;
    ui_arena_init(&a, small, sizeof(small));
    UINode *n1 = ui_button(&a, "one", 1);
    UINode *n2 = ui_button(&a, "two", 2);
    CHECK(n1 != NULL, "first alloc fits");
    CHECK(n2 == NULL, "second alloc returns NULL when full");
}

/* NULL / empty-tree safety. */
static void test_null_safety(void)
{
    ui_measure(NULL);
    ui_arrange(NULL, 0, 0, 100, 100);
    CHECK(ui_find(NULL, 1) == NULL, "find NULL");
    CHECK(ui_node_at(NULL, 5, 5) == NULL, "at NULL");
    CHECK_EQ(ui_child_count(NULL), 0, "count NULL");

    arena_reset();
    UINode *empty = ui_vgroup(&g_arena, NULL);
    ui_layout(empty, 0, 0, 100, 100);
    CHECK_EQ(ui_child_count(empty), 0, "empty group");
    CHECK_EQ(empty->nat_w, 0, "empty group natural 0");
}

/* Pad insets the node's box inside its cell. */
static void test_pad(void)
{
    arena_reset();
    UINode *lbl = ui_pad(ui_label(&g_arena, "x"), 8);
    UINode *root = ui_window(&g_arena, "T", lbl);
    ui_layout(root, 0, 0, 200, 100);

    CHECK_EQ(lbl->x, 8, "pad insets x");
    CHECK_EQ(lbl->y, 8, "pad insets y");
    CHECK_EQ(lbl->w, 200 - 16, "pad shrinks w");
    CHECK_EQ(lbl->h, 100 - 16, "pad shrinks h");
}

/* A realistic prefs-style window: header label, two rows, spacer, button
 * row — verifies the canonical tool shape tiles correctly. */
static void test_prefs_shape(void)
{
    arena_reset();
    UINode *root = ui_window(&g_arena, "Pointer",
        ui_pad(ui_vgroup(&g_arena,
            ui_label(&g_arena, "Pointer size:"),
            ui_hgroup(&g_arena,
                      ui_radio(&g_arena, "16x16", 1, 20),
                      ui_radio(&g_arena, "32x32", 1, 21),
                      ui_radio(&g_arena, "48x48", 1, 22),
                      NULL),
            ui_checkbox(&g_arena, "Shadow", 23),
            ui_spacer(&g_arena),
            ui_hgroup(&g_arena,
                      ui_spacer(&g_arena),
                      ui_button(&g_arena, "Save",   24),
                      ui_button(&g_arena, "Use",    25),
                      ui_button(&g_arena, "Cancel", 26),
                      NULL),
            NULL), 8));
    ui_layout(root, 0, 0, 320, 240);

    UINode *save   = ui_find(root, 24);
    UINode *cancel = ui_find(root, 26);
    UINode *outer  = save->parent->parent;      /* padded vgroup */
    check_tiles(outer, 0);

    /* outer group inset by pad */
    CHECK_EQ(outer->x, 8, "outer pad x");
    /* button row at bottom of the padded area */
    CHECK_EQ(save->parent->y + save->parent->h, 240 - 8, "buttons at bottom");
    /* right-aligned via leading spacer: last button flush right */
    CHECK_EQ(cancel->x + cancel->w, 320 - 8, "cancel flush right");
    /* radios share their row */
    UINode *r16 = ui_find(root, 20), *r32 = ui_find(root, 21);
    CHECK_EQ(r32->x, r16->x + r16->w + r16->parent->spacing,
             "radios in sequence");
}

int main(void)
{
    static char arena_buf[ARENA_BYTES];
    ui_arena_init(&g_arena, arena_buf, sizeof(arena_buf));

    test_single_fill();
    test_vgroup_weights();
    test_shrink();
    test_no_weight_packed();
    test_spacer_right_align();
    test_min_max();
    test_center();
    test_page();
    test_hit();
    test_natural_sizes();
    test_append();
    test_arena_exhaust();
    test_null_safety();
    test_pad();
    test_prefs_shape();

    printf("%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
