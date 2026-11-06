/*
 * Sessions: a terminal's state survives a snapshot (macos/snapshot.c).
 *
 * Each test feeds a terminal (A) some output, and a new terminal (B)
 * the snapshot of A; B must then be in the same state as A: the cells
 * of the scrollback and the screen(s), the wrapped lines, the cursor,
 * the modes, the colors. Then both are resized, and must still be the
 * same (wrapped lines reflow alike). Last, random output, many times.
 */

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <tllist.h>

#define LOG_MODULE "test-sessions"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "char32.h"
#include "composed.h"
#include "config.h"
#include "fdm.h"
#include "grid.h"
#include "headless.h"
#include "reaper.h"
#include "snapshot.h"
#include "terminal.h"
#include "user-notification.h"
#include "util.h"
#include "xmalloc.h"

static struct config conf;
static int failures;
static int tests;

/* The terminals' replies (that nobody wants) */
static struct {
    const struct terminal *term;
    int fd;
} replies[8];

static void
drain_replies(const struct terminal *term)
{
    for (size_t i = 0; i < ALEN(replies); i++) {
        if (replies[i].term != term)
            continue;

        char buf[4096];
        while (read(replies[i].fd, buf, sizeof(buf)) > 0)
            ;
    }
}

static struct terminal *
new_term(int cols, int rows)
{
    int pty, reply_fd;
    struct terminal *term = headless_term_new(
        &conf, "/tmp", 0, NULL, NULL, false, &pty, &reply_fd, NULL, NULL);
    if (term == NULL) {
        fprintf(stderr, "failed to create a terminal\n");
        exit(EXIT_FAILURE);
    }

    for (size_t i = 0; i < ALEN(replies); i++) {
        if (replies[i].term == NULL) {
            replies[i].term = term;
            replies[i].fd = reply_fd;
            break;
        }
    }

    headless_term_resize(term, cols, rows, 8, 16);
    return term;
}

static void
free_term(struct terminal *term)
{
    for (size_t i = 0; i < ALEN(replies); i++) {
        if (replies[i].term == term) {
            close(replies[i].fd);
            replies[i].term = NULL;
        }
    }
    term_destroy(term);
}

static void
feed(struct terminal *term, const char *data, size_t len)
{
    /* In pieces, as they'd come from a PTY (also splitting sequences) */
    for (size_t done = 0; done < len;) {
        const size_t n = min(len - done, (size_t)4096);
        headless_term_feed(term, data + done, n);
        drain_replies(term);
        done += n;
    }
}

/*
 * Comparing
 */

struct diff {
    char msg[512];
    bool found;
};

static void __attribute__((format(printf, 2, 3)))
differs(struct diff *d, const char *fmt, ...)
{
    if (d->found)
        return;
    d->found = true;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->msg, sizeof(d->msg), fmt, ap);
    va_end(ap);
}

static size_t
cell_chars(const struct terminal *term, char32_t wc, char32_t out[static 256])
{
    if (wc >= CELL_COMB_CHARS_LO && wc <= CELL_COMB_CHARS_HI) {
        const struct composed *c = composed_lookup(term->composed, wc - CELL_COMB_CHARS_LO);
        if (c == NULL)
            return 0;
        memcpy(out, c->chars, c->count * sizeof(out[0]));
        return c->count;
    }
    out[0] = wc == CELL_SPACER ? 0 : wc;
    return 1;
}

static bool
attrs_equal(struct attributes a, struct attributes b)
{
    return a.bold == b.bold && a.dim == b.dim && a.italic == b.italic &&
        a.underline == b.underline && a.strikethrough == b.strikethrough &&
        a.blink == b.blink && a.conceal == b.conceal && a.reverse == b.reverse &&
        a.fg_src == b.fg_src && (a.fg_src == COLOR_DEFAULT || a.fg == b.fg) &&
        a.bg_src == b.bg_src && (a.bg_src == COLOR_DEFAULT || a.bg == b.bg);
}

static const struct row_range *
range_at(const struct row *row, bool uri, int col)
{
    if (row->extra == NULL)
        return NULL;
    const struct row_ranges *ranges =
        uri ? &row->extra->uri_ranges : &row->extra->underline_ranges;
    for (int i = 0; i < ranges->count; i++) {
        if (ranges->v[i].start <= col && col <= ranges->v[i].end)
            return &ranges->v[i];
    }
    return NULL;
}

/* Link IDs differ (the receiver hashes them), but must group alike */
struct id_map {
    uint64_t a[64];
    uint64_t b[64];
    size_t count;
};

static bool
ids_match(struct id_map *map, uint64_t a, uint64_t b)
{
    for (size_t i = 0; i < map->count; i++) {
        if (map->a[i] == a)
            return map->b[i] == b;
        if (map->b[i] == b)
            return false;
    }
    if (map->count < ALEN(map->a)) {
        map->a[map->count] = a;
        map->b[map->count] = b;
        map->count++;
    }
    return true;
}

static void
compare_rows(struct diff *d, const char *where,
             const struct terminal *ta, const struct row *a,
             const struct terminal *tb, const struct row *b,
             int cols, bool linebreaks, struct id_map *ids)
{
    if (a == NULL || b == NULL) {
        if ((a == NULL) != (b == NULL))
            differs(d, "%s: one row is missing", where);
        return;
    }

    for (int col = 0; col < cols && !d->found; col++) {
        const struct cell *ca = &a->cells[col];
        const struct cell *cb = &b->cells[col];

        char32_t chars_a[256], chars_b[256];
        size_t count_a = cell_chars(ta, ca->wc, chars_a);
        size_t count_b = cell_chars(tb, cb->wc, chars_b);

        /*
         * foot may merge a regional indicator into a pair before it
         * (its grapheme state isn't reset by erasing): printed again,
         * three of them are a pair, and another one.
         */
        size_t ri = 0;
        for (size_t i = 0; i < count_a; i++)
            ri += chars_a[i] >= 0x1f1e6 && chars_a[i] <= 0x1f1ff;
        if (ri > 2)
            return;

        if (count_a != count_b || memcmp(chars_a, chars_b, count_a * sizeof(chars_a[0])) != 0) {
            differs(d, "%s, column %d: U+%04X (%zu code points) vs U+%04X (%zu)",
                    where, col, chars_a[0], count_a, chars_b[0], count_b);
            return;
        }

        if (!attrs_equal(ca->attrs, cb->attrs)) {
            const struct attributes x = ca->attrs, y = cb->attrs;
            differs(d, "%s, column %d: attributes differ: "
                    "fg %d:%06x/%d:%06x bg %d:%06x/%d:%06x bold %d/%d dim %d/%d italic %d/%d "
                    "underline %d/%d strike %d/%d blink %d/%d conceal %d/%d reverse %d/%d",
                    where, col, x.fg_src, x.fg, y.fg_src, y.fg, x.bg_src, x.bg, y.bg_src, y.bg,
                    x.bold, y.bold, x.dim, y.dim, x.italic, y.italic, x.underline, y.underline,
                    x.strikethrough, y.strikethrough, x.blink, y.blink, x.conceal, y.conceal,
                    x.reverse, y.reverse);
            return;
        }

        /*
         * Inserting and deleting characters shifts cells, but not their
         * links (rows' URI ranges): links on erased cells are never seen
         */
        const bool empty = ca->wc == 0 || ca->wc >= CELL_SPACER;
        const struct row_range *ua = !empty ? range_at(a, true, col) : NULL;
        const struct row_range *ub = !empty ? range_at(b, true, col) : NULL;
        uint64_t key_a = 0;
        if (ua != NULL) {
            /* The snapshot's IDs are per link: the URI is part of them */
            key_a = ua->uri.id;
            for (const char *p = ua->uri.uri; *p != '\0'; p++)
                key_a = key_a * 31 + (unsigned char)*p;
        }

        if ((ua == NULL) != (ub == NULL) ||
            (ua != NULL && (!streq(ua->uri.uri, ub->uri.uri) ||
                            !ids_match(ids, key_a, ub->uri.id))))
        {
            differs(d, "%s, column %d: links differ (%s vs %s)", where, col,
                    ua != NULL ? ua->uri.uri : "none", ub != NULL ? ub->uri.uri : "none");
            return;
        }

        /* Without the attribute, an underline's style and color are never seen */
        const struct row_range *la = ca->attrs.underline ? range_at(a, false, col) : NULL;
        const struct row_range *lb = cb->attrs.underline ? range_at(b, false, col) : NULL;
        const struct underline_range_data none = {0};
        struct underline_range_data da = la != NULL ? la->underline : none;
        struct underline_range_data db = lb != NULL ? lb->underline : none;

        /*
         * With the attribute, no style is a single underline (DECRC can
         * leave them like that; foot's renderer can't draw it, BUG()).
         */
        if (da.style == UNDERLINE_NONE)
            da.style = UNDERLINE_SINGLE;
        if (db.style == UNDERLINE_NONE)
            db.style = UNDERLINE_SINGLE;

        if (da.style != db.style || da.color_src != db.color_src ||
            (da.color_src != COLOR_DEFAULT && da.color != db.color))
        {
            differs(d, "%s, column %d: underlines differ (style %d vs %d)",
                    where, col, da.style, db.style);
            return;
        }
    }

    /*
     * A cut off wide character (without auto-wrap) in the last column
     * can only wrap after DECRC (that restores the last column flag)
     */
    bool cut_off = false;
    const struct cell *last = &a->cells[cols - 1];
    if (last->wc != 0 && last->wc < CELL_SPACER) {
        int width = 1;
        if (last->wc >= CELL_COMB_CHARS_LO && last->wc <= CELL_COMB_CHARS_HI) {
            const struct composed *c = composed_lookup(ta->composed, last->wc - CELL_COMB_CHARS_LO);
            if (c != NULL)
                width = c->forced_width > 0 ? c->forced_width : c->width;
        } else
            width = c32width(last->wc);
        cut_off = width > 1;
    }

    /*
     * Nor can the half of a wide character that's left in the last
     * column of a wrapped row be made again, without printing on it
     * (that unwraps it)
     */
    const bool orphan = last->wc == CELL_SPACER + 1 &&
        (a->cells[cols - 2].wc == 0 || a->cells[cols - 2].wc >= CELL_SPACER ||
         c32width(a->cells[cols - 2].wc) < 2);

    /* Padding (spacers) of more than a cell is left over from deleting characters */
    const bool wide_padding = a->cells[cols - 1].wc == CELL_SPACER &&
        cols >= 2 && a->cells[cols - 2].wc == CELL_SPACER;

    if (linebreaks && !cut_off && !orphan && !wide_padding && a->linebreak != b->linebreak)
        differs(d, "%s: linebreak %d vs %d", where, a->linebreak, b->linebreak);

    if (a->shell_integration.prompt_marker != b->shell_integration.prompt_marker ||
        a->shell_integration.cmd_start != b->shell_integration.cmd_start ||
        a->shell_integration.cmd_end != b->shell_integration.cmd_end)
    {
        differs(d, "%s: shell integration marks differ (%d/%d/%d vs %d/%d/%d)", where,
                a->shell_integration.prompt_marker, a->shell_integration.cmd_start,
                a->shell_integration.cmd_end, b->shell_integration.prompt_marker,
                b->shell_integration.cmd_start, b->shell_integration.cmd_end);
    }
}

static int
sb_count(const struct grid *grid, int rows)
{
    int start = grid_sb_start_ignore_uninitialized(grid, rows);
    return (grid->offset - start + grid->num_rows) & (grid->num_rows - 1);
}

/*
 * After the last column (lcf), on a row that's wrapped: the snapshot
 * keeps the row wrapped instead (snapshot.c).
 */
static bool
lcf_unrepresentable(const struct grid *grid, struct cursor c)
{
    const struct row *row = grid->rows[(grid->offset + c.point.row) & (grid->num_rows - 1)];
    if (!c.lcf || row == NULL)
        return false;

    /* Or over what's left of a wide character: nothing to print again */
    const struct cell *cell = &row->cells[c.point.col];
    const bool orphan = cell->wc > CELL_SPACER && c.point.col > 0 &&
        (row->cells[c.point.col - 1].wc == 0 || row->cells[c.point.col - 1].wc >= CELL_SPACER ||
         c32width(row->cells[c.point.col - 1].wc) < 2);
    return !row->linebreak || orphan;
}

/*
 * The cursor (not a saved one) after the last column, over an erased
 * cell, or a wide character that's cut off (with auto-wrap): only DECRC
 * leaves it like that, and nothing but DECRC could restore it.
 */
static bool
final_lcf_unrepresentable(const struct terminal *term)
{
    const struct cursor c = term->grid->cursor;
    const struct grid *grid = term->grid;
    const struct row *row = grid->rows[(grid->offset + c.point.row) & (grid->num_rows - 1)];
    if (!c.lcf || row == NULL)
        return false;

    const struct cell *cell = &row->cells[c.point.col];
    if (cell->wc == 0 || cell->wc == CELL_SPACER)
        return true;

    int start = c.point.col;
    while (start > 0 && row->cells[start].wc > CELL_SPACER)
        start--;
    const struct cell *first = &row->cells[start];

    int width = 1;
    if (first->wc >= CELL_COMB_CHARS_LO && first->wc <= CELL_COMB_CHARS_HI) {
        const struct composed *comp = composed_lookup(term->composed, first->wc - CELL_COMB_CHARS_LO);
        if (comp != NULL)
            width = comp->forced_width > 0 ? comp->forced_width : comp->width;
    } else if (first->wc < CELL_SPACER)
        width = c32width(first->wc);

    return term->auto_margin && start + width > term->cols;
}

static void
compare_cursor(struct diff *d, const char *what, const struct grid *grid,
               struct cursor a, struct cursor b)
{
    if (lcf_unrepresentable(grid, a))
        a.lcf = b.lcf;

    if (a.point.row != b.point.row || a.point.col != b.point.col || a.lcf != b.lcf) {
        differs(d, "%s: %d,%d%s vs %d,%d%s", what,
                a.point.row, a.point.col, a.lcf ? " (lcf)" : "",
                b.point.row, b.point.col, b.lcf ? " (lcf)" : "");
    }
}

static void
compare_kitty(struct diff *d, const char *what, const struct grid *a, const struct grid *b)
{
    if (a->kitty_kbd.idx != b->kitty_kbd.idx ||
        memcmp(a->kitty_kbd.flags, b->kitty_kbd.flags, sizeof(a->kitty_kbd.flags)) != 0)
    {
        differs(d, "%s: kitty keyboard flags differ", what);
    }
}

#define SAME(field) \
    do { \
        if ((a->field) != (b->field)) \
            differs(&d, "%s differs: %lld vs %lld", #field, \
                    (long long)(a->field), (long long)(b->field)); \
    } while (0)

static bool
same_str(const char *a, const char *b)
{
    return (a == NULL && b == NULL) || (a != NULL && b != NULL && streq(a, b));
}

static struct diff
compare(const struct terminal *a, const struct terminal *b)
{
    struct diff d = {0};
    struct id_map ids = {0};

    SAME(cols);
    SAME(rows);
    if (d.found)
        return d;

    if ((a->grid == &a->alt) != (b->grid == &b->alt))
        differs(&d, "one is on the alternate screen, the other isn't");

    /* The normal screen, and its scrollback, from the bottom up */
    const struct grid *na = &a->normal;
    const struct grid *nb = &b->normal;
    const int count_a = sb_count(na, a->rows) + a->rows;
    const int count_b = sb_count(nb, b->rows) + b->rows;
    if (count_a != count_b)
        differs(&d, "%d vs %d lines (scrollback and screen)", count_a, count_b);

    for (int i = 0; i < count_a && !d.found; i++) {
        const int row_a = a->rows - 1 - i;
        const struct row *ra = na->rows[(na->offset + row_a + na->num_rows) & (na->num_rows - 1)];
        const struct row *rb = nb->rows[(nb->offset + row_a + nb->num_rows) & (nb->num_rows - 1)];

        /*
         * The screen's last line can't be wrapped (that would scroll):
         * only if something was scrolled down on top of it
         * (snapshot.h).
         */
        char where[64];
        snprintf(where, sizeof(where), "normal screen, row %d", row_a);
        compare_rows(&d, where, a, ra, b, rb, a->cols, i > 0, &ids);
    }

    if (a->grid == &a->alt && !d.found) {
        for (int r = 0; r < a->rows && !d.found; r++) {
            char where[64];
            snprintf(where, sizeof(where), "alternate screen, row %d", r);
            compare_rows(&d, where,
                         a, a->alt.rows[(a->alt.offset + r) & (a->alt.num_rows - 1)],
                         b, b->alt.rows[(b->alt.offset + r) & (b->alt.num_rows - 1)],
                         a->cols, false, &ids);
        }
    }

    struct cursor cursor_a = a->grid->cursor;
    if (final_lcf_unrepresentable(a))
        cursor_a.lcf = b->grid->cursor.lcf;
    compare_cursor(&d, "cursor", a->grid, cursor_a, b->grid->cursor);
    compare_cursor(&d, "normal screen's saved cursor", na, na->saved_cursor, nb->saved_cursor);
    if (!attrs_equal(na->saved_attrs, nb->saved_attrs))
        differs(&d, "normal screen's saved attributes differ");
    compare_kitty(&d, "normal screen", na, nb);

    if (a->grid == &a->alt) {
        compare_cursor(&d, "alternate screen's saved cursor", &a->alt,
                       a->alt.saved_cursor, b->alt.saved_cursor);
        if (!attrs_equal(a->alt.saved_attrs, b->alt.saved_attrs))
            differs(&d, "alternate screen's saved attributes differ");
        compare_kitty(&d, "alternate screen", &a->alt, &b->alt);
    }

    SAME(scroll_region.start);
    SAME(scroll_region.end);
    SAME(origin);
    SAME(charsets.selected);
    for (int i = 0; i < 4; i++) {
        SAME(charsets.set[i]);
        SAME(saved_charsets.set[i]);
    }
    SAME(saved_charsets.selected);

    SAME(auto_margin);
    SAME(insert_mode);
    SAME(reverse);
    SAME(hide_cursor);
    SAME(reverse_wrap);
    SAME(bracketed_paste);
    SAME(focus_events);
    SAME(visibility_reports);
    SAME(alt_scrolling);
    SAME(modify_other_keys_2);
    SAME(cursor_keys_mode);
    SAME(keypad_keys_mode);
    SAME(mouse_tracking);
    SAME(mouse_reporting);
    SAME(meta.esc_prefix);
    SAME(meta.eight_bit);
    SAME(num_lock_modifier);
    SAME(bell_action_enabled);
    SAME(report_theme_changes);
    SAME(size_notifications);
    SAME(grapheme_shaping);
    SAME(sixel.scrolling);
    SAME(sixel.use_private_palette);
    SAME(sixel.cursor_right_of_graphics);
    SAME(cursor_style);
    SAME(cursor_blink.decset);
    SAME(cursor_blink.deccsusr);
#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    SAME(ime_enabled);
#endif

    SAME(colors.fg);
    SAME(colors.bg);
    SAME(colors.cursor_bg);
    SAME(colors.selection_fg);
    SAME(colors.selection_bg);
    for (int i = 0; i < 256; i++)
        SAME(colors.table[i]);

    if (!same_str(a->window_title, b->window_title))
        differs(&d, "titles differ: %s vs %s", a->window_title, b->window_title);
    if (!same_str(a->app_id, b->app_id))
        differs(&d, "app IDs differ");
    if (!same_str(a->cwd, b->cwd))
        differs(&d, "working directories differ: %s vs %s", a->cwd, b->cwd);
    if (!same_str(a->mouse_user_cursor, b->mouse_user_cursor))
        differs(&d, "mouse cursors differ");

    if (tll_length(a->tab_stops) != tll_length(b->tab_stops))
        differs(&d, "tab stops differ");
    else {
        int stops_a[1024], stops_b[1024];
        size_t n = 0;
        tll_foreach(a->tab_stops, it) {
            if (n < ALEN(stops_a))
                stops_a[n++] = it->item;
        }
        n = 0;
        tll_foreach(b->tab_stops, it) {
            if (n < ALEN(stops_b))
                stops_b[n++] = it->item;
        }
        if (memcmp(stops_a, stops_b, n * sizeof(stops_a[0])) != 0)
            differs(&d, "tab stops differ");
    }

    if (!attrs_equal(a->vt.attrs, b->vt.attrs))
        differs(&d, "the pens' attributes differ");
    /*
     * After DECRC, the style may remain, without the attribute (never
     * seen), and vice versa (a single underline)
     */
    const enum underline_style style_a = a->vt.underline.style == UNDERLINE_NONE
        ? UNDERLINE_SINGLE : a->vt.underline.style;
    const enum underline_style style_b = b->vt.underline.style == UNDERLINE_NONE
        ? UNDERLINE_SINGLE : b->vt.underline.style;
    if (a->vt.attrs.underline &&
        (style_a != style_b || a->vt.underline.color_src != b->vt.underline.color_src))
    {
        differs(&d, "the pens' underlines differ");
    }
    if (!same_str(a->vt.osc8.uri, b->vt.osc8.uri))
        differs(&d, "the links being printed differ");

    return d;
}

#undef SAME

/*
 * Running a test
 */

static void print_escaped(const char *prefix, const char *s, size_t len);

static void
dump_snapshot(const char *snapshot, size_t len)
{
    print_escaped("    snapshot: ", snapshot, len < 4096 ? len : 4096);
}

/*
 * Whether the normal screen has what isn't in a snapshot, but changes
 * how it reflows (and so, a comparison after a resize means nothing):
 *
 *  - underline styles and links on cells that don't show them
 *    (snapshot.h)
 *  - a wide character in the last column, without auto-wrap: foot's
 *    reflow reads past the end of the row then (grid.c)
 */
static bool
reflow_unreliable(const struct terminal *term)
{
    const struct grid *g = &term->normal;
    const int sb = sb_count(g, term->rows);

    for (int r = -sb; r < term->rows; r++) {
        const struct row *row = g->rows[(g->offset + r + g->num_rows) & (g->num_rows - 1)];
        if (row == NULL)
            continue;

        /* A wrapped row's end that the snapshot can't wrap (see compare_rows()) */
        const int cols = term->cols;
        if (!row->linebreak && cols >= 2) {
            const struct cell *last = &row->cells[cols - 1];
            const struct cell *before = &row->cells[cols - 2];
            if (last->wc == CELL_SPACER + 1 &&
                (before->wc == 0 || before->wc >= CELL_SPACER || c32width(before->wc) < 2))
            {
                return true;
            }
            if (last->wc == CELL_SPACER && before->wc == CELL_SPACER)
                return true;
        }

        for (int col = 0; col < term->cols; col++) {
            const struct cell *cell = &row->cells[col];
            const bool empty = cell->wc == 0 || cell->wc >= CELL_SPACER;

            /*
             * Padding that isn't at the end of a wrapped row (deleting
             * characters moved it): the snapshot erases it, the reflow
             * doesn't
             */
            if (cell->wc == CELL_SPACER &&
                (row->linebreak || (col + 1 < cols && row->cells[col + 1].wc != CELL_SPACER)))
            {
                return true;
            }

            if (!cell->attrs.underline && range_at(row, false, col) != NULL)
                return true;
            if (empty && range_at(row, true, col) != NULL)
                return true;
            if (empty)
                continue;

            int width = 1;
            if (cell->wc >= CELL_COMB_CHARS_LO && cell->wc <= CELL_COMB_CHARS_HI) {
                const struct composed *c = composed_lookup(term->composed, cell->wc - CELL_COMB_CHARS_LO);
                if (c != NULL)
                    width = c->forced_width > 0 ? c->forced_width : c->width;
            } else
                width = c32width(cell->wc);

            if (width > 1 && col + width > term->cols)
                return true;
        }
    }
    return false;
}

static bool
check(const char *name, const char *input, size_t input_len, int cols, int rows,
      bool verbose)
{
    tests++;

    struct terminal *a = new_term(cols, rows);
    feed(a, input, input_len);

    size_t len;
    char *snapshot = snapshot_create(a, &len);

    struct terminal *b = new_term(cols, rows);
    feed(b, snapshot, len);

    struct diff d = compare(a, b);
    const char *stage = "after the snapshot";

    /* Wrapped lines reflow alike (with the cursors, and their flags) */
    const struct row *last_row =
        a->normal.rows[(a->normal.offset + rows - 1) & (a->normal.num_rows - 1)];
    if (!d.found && a->grid == &a->normal && !reflow_unreliable(a) &&
        last_row->linebreak &&
        !final_lcf_unrepresentable(a) &&
        !lcf_unrepresentable(&a->normal, a->normal.cursor) &&
        !lcf_unrepresentable(&a->normal, a->normal.saved_cursor))
    {
        headless_term_resize(a, cols * 2 / 3 + 1, rows, 8, 16);
        headless_term_resize(b, cols * 2 / 3 + 1, rows, 8, 16);
        d = compare(a, b);
        stage = "after a resize";
    }

    bool ok = !d.found;
    if (!ok) {
        failures++;
        fprintf(stderr, "FAIL: %s (%dx%d), %s: %s\n", name, cols, rows, stage, d.msg);
        if (verbose)
            dump_snapshot(snapshot, len);
    }

    free(snapshot);
    free_term(a);
    free_term(b);
    return ok;
}

static void
test(const char *name, const char *input)
{
    check(name, input, strlen(input), 20, 6, true);
    check(name, input, strlen(input), 80, 24, true);
}

/*
 * Random output: printable text (also wide, and combining), and the
 * control sequences programs use, at random.
 */

static uint64_t rng_state;

static uint32_t
rnd(uint32_t n)
{
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(rng_state >> 33) % n;
}

/* Output, as a list of writes (to find the ones that matter) */
struct writes {
    char **v;
    size_t count;
};

static void __attribute__((format(printf, 2, 3)))
add(struct writes *w, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    w->v = xrealloc(w->v, (w->count + 1) * sizeof(w->v[0]));
    w->v[w->count++] = xstrdup(tmp);
}

static void
writes_free(struct writes *w)
{
    for (size_t i = 0; i < w->count; i++)
        free(w->v[i]);
    free(w->v);
}

static char *
writes_join(const struct writes *w, size_t skip, size_t *len)
{
    size_t total = 0;
    for (size_t i = 0; i < w->count; i++)
        total += i != skip ? strlen(w->v[i]) : 0;

    char *s = xmalloc(total + 1);
    *len = 0;
    for (size_t i = 0; i < w->count; i++) {
        if (i == skip)
            continue;
        size_t n = strlen(w->v[i]);
        memcpy(s + *len, w->v[i], n);
        *len += n;
    }
    s[*len] = '\0';
    return s;
}

static const char *const texts[] = {
    "a", "hello", " ", "world ", "0123456789", "\t", "ü", "é", "e\xcc\x81",
    "日本", "語", "😀", "👍🏽", "🇩🇪", "👨‍👩‍👧", "❤️", "─│┼", "x\xcc\x88\xcc\x81",
};

static void
random_output(struct writes *w, int cols, int rows, int ops)
{
    /*
     * foot's own: with origin mode, DECRC may restore the cursor outside
     * the scroll region, and a line feed then fails an assertion
     * (term_cursor_down()). Not what's tested here.
     */
    bool origin = false;

    for (int i = 0; i < ops; i++) {
        switch (rnd(30)) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
            add(w, "%s", texts[rnd(ALEN(texts))]);
            break;
        case 8: add(w, "\r\n"); break;
        case 9: add(w, "\n"); break;
        case 10: {
            static const char *const sgr[] = {
                "0", "1", "2", "3", "4", "4:3", "5", "7", "8", "9", "22", "24",
                "31", "42", "93", "104", "38;5;200", "48;5;17", "38;2;1;2;3",
                "48:2::200:100:50", "58:2::255:0:0", "58;5;3", "39", "49", "59",
            };
            add(w, "\033[%sm", sgr[rnd(ALEN(sgr))]);
            break;
        }
        case 11: add(w, "\033[%u;%uH", rnd(rows + 2) + 1, rnd(cols + 2) + 1); break;
        case 12: {
            static const char *const erase[] = {"K", "1K", "2K", "J", "1J", "2J"};
            add(w, "\033[%s", erase[rnd(ALEN(erase))]);
            break;
        }
        case 13: add(w, "\033[%uX", rnd(10) + 1); break;
        case 14: add(w, "\033[%u%c", rnd(5) + 1, "@PLMST"[rnd(6)]); break;
        case 15: add(w, "\033[%u%c", rnd(10) + 1, "ABCDG"[rnd(5)]); break;
        case 16:
            if (rnd(4) == 0)
                add(w, "\033]8;;\033\\");
            else
                add(w, "\033]8;%s;https://example.com/%u\033\\", rnd(2) ? "id=x" : "", rnd(3));
            break;
        case 17: {
            unsigned top = rnd(rows) + 1;
            unsigned bottom = top + rnd(rows - top + 1);
            add(w, "\033[%u;%ur", top, bottom);
            break;
        }
        case 18: add(w, rnd(2) || origin ? "\0337" : "\0338"); break;
        case 19: {
            static const char *const modes[] = {
                "?1h", "?1l", "?5h", "?5l", "?6h", "?6l", "?7h", "?7l", "?12h",
                "?25l", "?25h", "?45l", "?66h", "?1000h", "?1002h", "?1003l",
                "?1004h", "?1006h", "?1007l", "?2004h", "?2004l", "4h", "4l",
                ">4;2m", ">4;0m",
            };
            const char *mode = modes[rnd(ALEN(modes))];
            if (streq(mode, "?6h"))
                origin = true;
            else if (streq(mode, "?6l"))
                origin = false;
            add(w, "\033[%s", mode);
            break;
        }
        case 20: add(w, rnd(2) ? "\033(0" : "\033(B"); break;
        case 21: add(w, rnd(2) ? "\016" : "\017"); break;
        case 22: add(w, "\033]2;title %u\033\\", rnd(100)); break;
        case 23: add(w, "\033]133;%c\033\\", "ACD"[rnd(3)]); break;
        case 24: add(w, "\033[%u q", rnd(7)); break;
        case 25: add(w, "\033[%c%uu", "><"[rnd(2)], rnd(32)); break;
        /* (Leaving it restores the cursor: see 'origin') */
        case 26: add(w, rnd(2) || origin ? "\033[?1049h" : "\033[?1049l"); break;
        case 27:
            if (rnd(2))
                add(w, "\033H");
            else
                add(w, "\033[%ug", rnd(2) * 3);
            break;
        case 28: add(w, "\033]4;%u;rgb:12/34/%02x\033\\", rnd(256), rnd(256)); break;
        case 29: add(w, "%*s", (int)rnd(cols * 2), ""); break;
        }
    }
}

static void
print_escaped(const char *prefix, const char *s, size_t len)
{
    fputs(prefix, stderr);
    for (size_t j = 0; j < len; j++) {
        unsigned char c = s[j];
        if (c == '\033')
            fputs("\\e", stderr);
        else if (c == '\r')
            fputs("\\r", stderr);
        else if (c == '\n')
            fputs("\\n", stderr);
        else if (c == '\\')
            fputs("\\\\", stderr);
        else if (c < 0x20 || c == 0x7f)
            fprintf(stderr, "\\x%02x", c);
        else
            fputc(c, stderr);
    }
    fputc('\n', stderr);
}

/* Whether the output fails (without reporting it) */
static bool
fails(const char *input, size_t len, int cols, int rows)
{
    const int saved_failures = failures, saved_tests = tests;
    FILE *saved = stderr;
    stderr = fopen("/dev/null", "w");
    bool ok = check("", input, len, cols, rows, false);
    fclose(stderr);
    stderr = saved;
    failures = saved_failures;
    tests = saved_tests;
    return !ok;
}

static void
fuzz(int first, int iterations)
{
    int failed = 0;

    for (int i = first; i < first + iterations && failed < 10; i++) {
        rng_state = 0x9e3779b97f4a7c15ULL * (i + 1);

        const int cols = 5 + rnd(30);
        const int rows = 2 + rnd(10);

        struct writes w = {0};
        random_output(&w, cols, rows, 20 + rnd(300));

        size_t len;
        char *input = writes_join(&w, SIZE_MAX, &len);

        if (iterations == 1)
            fprintf(stderr, "size: %dx%d\n", cols, rows), print_escaped("input: ", input, len);

        char name[64];
        snprintf(name, sizeof(name), "random output #%d", i);
        bool ok = check(name, input, len, cols, rows, false);
        free(input);

        if (!ok) {
            failed++;

            /* The writes it fails without, are left out */
            for (size_t j = w.count; j-- > 0;) {
                size_t l;
                char *without = writes_join(&w, j, &l);
                if (fails(without, l, cols, rows)) {
                    free(w.v[j]);
                    memmove(&w.v[j], &w.v[j + 1], (w.count - j - 1) * sizeof(w.v[0]));
                    w.count--;
                }
                free(without);
            }

            input = writes_join(&w, SIZE_MAX, &len);
            print_escaped("    minimal input: ", input, len);

            failures--;
            tests--;
            check("    minimal", input, len, cols, rows, true);
            free(input);
        }

        writes_free(&w);
    }
}

/* For debugging: test-sessions --dump COLS ROWS INPUT (\e for ESC) */
static void
dump_term(const char *title, const struct terminal *term)
{
    fprintf(stderr, "%s: %dx%d, on %s screen, cursor %d,%d%s\n", title, term->cols, term->rows,
            term->grid == &term->alt ? "the alternate" : "the normal",
            term->grid->cursor.point.row, term->grid->cursor.point.col,
            term->grid->cursor.lcf ? " (lcf)" : "");

    const struct grid *g = &term->normal;
    const int sb = sb_count(g, term->rows);
    for (int r = -sb; r < term->rows; r++) {
        const struct row *row = g->rows[(g->offset + r + g->num_rows) & (g->num_rows - 1)];
        if (row == NULL) {
            fprintf(stderr, "  %3d: (null)\n", r);
            continue;
        }
        fprintf(stderr, "  %3d%s%s: ", r, row->linebreak ? "" : " wrapped",
                row->shell_integration.prompt_marker ? " prompt" : "");
        for (int c = 0; c < term->cols; c++) {
            const struct cell *cell = &row->cells[c];
            const struct row_range *uri = range_at(row, true, c);
            if (cell->wc == 0)
                fputs(uri ? "_" : ".", stderr);
            else if (cell->wc >= CELL_SPACER)
                fprintf(stderr, "[s%u%s]", (unsigned)(cell->wc - CELL_SPACER), uri ? "+" : "");
            else if (cell->wc >= CELL_COMB_CHARS_LO)
                fprintf(stderr, "[c%s]", uri ? "+" : "");
            else if (cell->wc < 0x80 && cell->wc >= 0x20)
                fprintf(stderr, "%c%s", (char)cell->wc, uri ? "+" : "");
            else
                fprintf(stderr, "[%04X%s]", cell->wc, uri ? "+" : "");
        }
        if (row->shell_integration.cmd_start >= 0 || row->shell_integration.cmd_end >= 0)
            fprintf(stderr, "  (cmd %d-%d)", row->shell_integration.cmd_start, row->shell_integration.cmd_end);
        fputc('\n', stderr);
    }
}

static char *
unescape(const char *s, size_t *len)
{
    char *out = xmalloc(strlen(s) + 1);
    *len = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (p[0] == '\\' && p[1] == 'e') { out[(*len)++] = '\033'; p++; }
        else if (p[0] == '\\' && p[1] == 'r') { out[(*len)++] = '\r'; p++; }
        else if (p[0] == '\\' && p[1] == 'n') { out[(*len)++] = '\n'; p++; }
        else if (p[0] == '\\' && p[1] == '\\') { out[(*len)++] = '\\'; p++; }
        else if (p[0] == '\\' && p[1] == 'x' && p[2] && p[3]) {
            char hex[3] = {p[2], p[3], 0};
            out[(*len)++] = (char)strtol(hex, NULL, 16);
            p += 3;
        } else
            out[(*len)++] = *p;
    }
    return out;
}

static void
dump(int cols, int rows, const char *escaped)
{
    size_t len;
    char *input = unescape(escaped, &len);

    struct terminal *a = new_term(cols, rows);
    feed(a, input, len);
    dump_term("original", a);

    size_t snap_len;
    char *snapshot = snapshot_create(a, &snap_len);
    print_escaped("snapshot: ", snapshot, snap_len);

    struct terminal *b = new_term(cols, rows);
    feed(b, snapshot, snap_len);
    dump_term("restored", b);

    struct diff d = compare(a, b);
    fprintf(stderr, "%s\n", d.found ? d.msg : "same");

    if (!d.found && !reflow_unreliable(a)) {
        headless_term_resize(a, cols * 2 / 3 + 1, rows, 8, 16);
        headless_term_resize(b, cols * 2 / 3 + 1, rows, 8, 16);
        dump_term("original, resized", a);
        dump_term("restored, resized", b);
        d = compare(a, b);
        fprintf(stderr, "%s\n", d.found ? d.msg : "same");
    }

    free(snapshot);
    free(input);
    free_term(a);
    free_term(b);
}

int
main(int argc, char *argv[])
{
    log_init(LOG_COLORIZE_AUTO, false, LOG_FACILITY_USER, LOG_CLASS_WARNING);
    signal(SIGPIPE, SIG_IGN);

    struct fdm *fdm = fdm_init();
    struct reaper *reaper = reaper_init(fdm);
    headless_init(fdm, reaper);

    /* The defaults: an empty configuration */
    char conf_path[] = "/tmp/foot-test-sessions-XXXXXX";
    int conf_fd = mkstemp(conf_path);
    if (conf_fd < 0) {
        fprintf(stderr, "failed to create a configuration: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    close(conf_fd);

    user_notifications_t notifications = tll_init();
    config_override_t overrides = tll_init();
    bool loaded = config_load(&conf, conf_path, &notifications, &overrides, false, false);
    unlink(conf_path);
    user_notifications_free(&notifications);

    if (!loaded) {
        fprintf(stderr, "failed to load the configuration\n");
        return EXIT_FAILURE;
    }
    conf.render_worker_count = 0;
    conf.scrollback.lines = 100;

    if (argc == 3 && streq(argv[1], "--fuzz")) {
        /* One iteration, e.g. one that failed */
        fuzz(atoi(argv[2]), 1);
        return failures > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    if (argc == 5 && streq(argv[1], "--dump")) {
        dump(atoi(argv[2]), atoi(argv[3]), argv[4]);
        return EXIT_SUCCESS;
    }

    test("empty", "");
    test("text", "hello world");
    test("lines", "one\r\ntwo\r\nthree\r\n");
    test("scrollback",
         "1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8\r\n9\r\n10\r\n11\r\n12\r\n13\r\n"
         "14\r\n15\r\n16\r\n17\r\n18\r\n19\r\n20\r\n21\r\n22\r\n23\r\n24\r\n25\r\n"
         "26\r\n27\r\n28\r\n29\r\n30");
    test("wrapped",
         "a line that is quite a bit longer than the screen is wide, "
         "so that it wraps, several times over, at least on the small "
         "screen\r\n$ ");
    test("exactly full line", "01234567890123456789");
    test("exactly full lines", "0123456789012345678901234567890123456789x");
    test("colors",
         "\033[1;31mred\033[0m \033[38;5;200mpink\033[0m \033[38;2;1;2;3mrgb\033[m"
         " \033[44mblue bg\033[m \033[101mbright\033[m \033[3;4;9mstyles\033[m"
         " \033[4:3;58:2::255:0:0curly\033[m \033[7mrev\033[m \033[2mdim\033[m");
    test("erased with a color", "\033[44m\033[2Jabc\033[K\r\n\033[42m  \033[X\033[m");
    test("wide", "日本語のテキストは幅が広いので、折り返しがおかしくなる");
    test("wide at the end", "0123456789012345678日本");
    test("emoji", "😀 👍🏽 🇩🇪🇫🇷 👨‍👩‍👧 ❤️ ☺︎");
    test("combining", "e\xcc\x81 a\xcc\x88 x\xcc\x88\xcc\x81 ");
    test("links",
         "\033]8;;https://example.com\033\\a link\033]8;;\033\\ and "
         "\033]8;id=1;https://example.org/long\033\\a long one, which "
         "wraps around\033]8;;\033\\.");
    test("alternate screen",
         "on the normal screen\r\n$ vim\033[?1049h\033[H\033[2J\033[1;1H~\033[2;1H~"
         "\033[3;5H\033[1mtext\033[m\033]2;vim\033\\\033[?1h\033[?2004h");
    test("alternate screen, 1047", "abc\033[?1047hdef");
    test("scroll region",
         "\033[2;4r\033[?6h\033[2;3Hrelative\033[5;1H");
    test("tab stops", "\033[3g\033[5G\033H\033[15G\033H\r\ta\tb");
    test("charsets", "\033(0lqqk\033(B \033)0\016xx\017 \033(0");
    test("saved cursor", "\033[3;5H\033[1;32m\033(0\0337\033(B\033[m\033[H");
    test("modes",
         "\033[?1h\033[?5h\033[?12h\033[?25l\033[?45l\033[?66h\033[?1002h\033[?1006h"
         "\033[?1004h\033[?1007l\033[?2004h\033[4h\033[>4;2m\033[5 q\033[>1u\033[>5u");
    test("kitty keyboard stack", "\033[>1u\033[>3u\033[>31u\033[<u");
    test("shell integration",
         "\033]133;A\033\\$ ls\033]133;C\033\\\r\nfile\r\n\033]133;D\033\\"
         "\033]133;A\033\\$ ");
    test("osc colors",
         "\033]4;1;rgb:12/34/56\033\\\033]10;rgb:ff/ff/00\033\\\033]11;rgb:00/00/10\033\\"
         "\033]12;rgb:ff/00/00\033\\");
    test("title and cwd", "\033]2;my title\033\\\033]7;file://localhost/Users/me/a%20dir\033\\");
    test("last column", "\033[1;20HX");
    test("last column, wrapped next", "01234567890123456789\r");
    test("no auto wrap", "\033[?7l0123456789012345678901234");
    test("insert mode", "abc\033[4h\033[1GX");
    test("pen", "\033[1;33;44mpen\033]8;;https://example.com\033\\");
    test("deleted lines", "a\r\nb\r\nc\r\nd\033[2;1H\033[2M");

    fuzz(0, argc == 2 ? atoi(argv[1]) : 2000);

    config_free(&conf);
    headless_fini();
    reaper_destroy(reaper);
    fdm_destroy(fdm);
    log_deinit();

    if (failures > 0) {
        fprintf(stderr, "%d of %d tests failed\n", failures, tests);
        return EXIT_FAILURE;
    }

    printf("%d tests passed\n", tests);
    return EXIT_SUCCESS;
}
