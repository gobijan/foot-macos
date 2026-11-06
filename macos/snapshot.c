/*
 * A terminal's state, as control sequences; see snapshot.h.
 *
 * The receiving terminal is new: in its initial state, and of the same
 * size. The rows are printed in order, the oldest scrollback line
 * first, so that the ones above the screen scroll into the scrollback.
 * Lines that were wrapped are printed so that they wrap again (and
 * reflow like the original on a resize). Then come the alternate
 * screen, the saved cursors, and the cursor, modes and colors.
 *
 * The receiver's cursor (and pen) is tracked while printing, so that
 * no more is sent than needed.
 */

#include "snapshot.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(FOOT_GRAPHEME_CLUSTERING)
 #include <utf8proc.h>
#endif

#include <tllist.h>

#define LOG_MODULE "snapshot"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "char32.h"
#include "composed.h"
#include "config.h"
#include "grid.h"
#include "terminal.h"
#include "util.h"
#include "xmalloc.h"

struct buf {
    char *data;
    size_t len;
    size_t size;
};

static void
put(struct buf *b, const void *data, size_t len)
{
    if (b->len + len > b->size) {
        size_t size = b->size > 0 ? b->size : 64 * 1024;
        while (size < b->len + len)
            size *= 2;
        b->data = xrealloc(b->data, size);
        b->size = size;
    }
    memcpy(&b->data[b->len], data, len);
    b->len += len;
}

static void
put_str(struct buf *b, const char *s)
{
    put(b, s, strlen(s));
}

static void __attribute__((format(printf, 2, 3)))
put_fmt(struct buf *b, const char *fmt, ...)
{
    char small[128];

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);

    if (n < 0)
        return;

    if ((size_t)n < sizeof(small)) {
        put(b, small, n);
        return;
    }

    char *large = xmalloc(n + 1);
    va_start(ap, fmt);
    vsnprintf(large, n + 1, fmt, ap);
    va_end(ap);
    put(b, large, n);
    free(large);
}

static void
put_utf8(struct buf *b, char32_t c)
{
    char s[4];
    size_t n;

    if (c < 0x80) {
        s[0] = c;
        n = 1;
    } else if (c < 0x800) {
        s[0] = 0xc0 | (c >> 6);
        s[1] = 0x80 | (c & 0x3f);
        n = 2;
    } else if (c < 0x10000) {
        s[0] = 0xe0 | (c >> 12);
        s[1] = 0x80 | ((c >> 6) & 0x3f);
        s[2] = 0x80 | (c & 0x3f);
        n = 3;
    } else {
        s[0] = 0xf0 | (c >> 18);
        s[1] = 0x80 | ((c >> 12) & 0x3f);
        s[2] = 0x80 | ((c >> 6) & 0x3f);
        s[3] = 0x80 | (c & 0x3f);
        n = 4;
    }
    put(b, s, n);
}

static void
decset(struct buf *b, unsigned mode, bool enable)
{
    put_fmt(b, "\033[?%u%c", mode, enable ? 'h' : 'l');
}

/*
 * Attributes
 */

/* Only what SGR sets: the rest is the renderer's (and selection's) */
static struct attributes
sgr_attrs(struct attributes a)
{
    return (struct attributes){
        .bold = a.bold,
        .dim = a.dim,
        .italic = a.italic,
        .underline = a.underline,
        .strikethrough = a.strikethrough,
        .blink = a.blink,
        .conceal = a.conceal,
        .reverse = a.reverse,
        .fg_src = a.fg_src,
        .fg = a.fg_src != COLOR_DEFAULT ? a.fg : 0,
        .bg_src = a.bg_src,
        .bg = a.bg_src != COLOR_DEFAULT ? a.bg : 0,
    };
}

static bool
attrs_equal(struct attributes a, struct attributes b)
{
    a = sgr_attrs(a);
    b = sgr_attrs(b);
    return a.bold == b.bold && a.dim == b.dim && a.italic == b.italic &&
        a.underline == b.underline && a.strikethrough == b.strikethrough &&
        a.blink == b.blink && a.conceal == b.conceal && a.reverse == b.reverse &&
        a.fg_src == b.fg_src && a.fg == b.fg &&
        a.bg_src == b.bg_src && a.bg == b.bg;
}

static bool
underline_equal(const struct underline_range_data *a,
                const struct underline_range_data *b)
{
    return a->style == b->style && a->color_src == b->color_src &&
        (a->color_src == COLOR_DEFAULT || a->color == b->color);
}

static void
put_sgr_color(struct buf *b, int base, enum color_source src, uint32_t color)
{
    switch (src) {
    case COLOR_DEFAULT:
        break;

    case COLOR_BASE16:
        put_fmt(b, ";%u", color < 8 ? base + color : base + 60 + (color - 8));
        break;

    case COLOR_BASE256:
        put_fmt(b, ";%d:5:%u", base + 8, color & 0xff);
        break;

    case COLOR_RGB:
        put_fmt(b, ";%d:2::%u:%u:%u", base + 8,
                (color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff);
        break;
    }
}

/* What the receiver prints with */
struct pen {
    struct attributes attrs;
    struct underline_range_data underline;

    /* OSC 8 */
    const char *uri;
    uint64_t uri_id;
};

static const struct underline_range_data no_underline = {0};

static void
set_pen(struct buf *b, struct pen *pen, struct attributes attrs,
        const struct underline_range_data *ul)
{
    attrs = sgr_attrs(attrs);

    /* What SGR leaves the terminal with (see csi_sgr()) */
    struct underline_range_data underline = {
        .style = attrs.underline
            ? (ul->style > UNDERLINE_SINGLE ? ul->style : UNDERLINE_SINGLE)
            : UNDERLINE_NONE,
        .color_src = ul->color_src,
        .color = ul->color_src != COLOR_DEFAULT ? ul->color : 0,
    };

    if (attrs_equal(pen->attrs, attrs) && underline_equal(&pen->underline, &underline))
        return;

    put_str(b, "\033[0");
    if (attrs.bold)
        put_str(b, ";1");
    if (attrs.dim)
        put_str(b, ";2");
    if (attrs.italic)
        put_str(b, ";3");

    if (attrs.underline) {
        switch (underline.style) {
        case UNDERLINE_NONE:
        case UNDERLINE_SINGLE: put_str(b, ";4"); break;
        case UNDERLINE_DOUBLE: put_str(b, ";4:2"); break;
        case UNDERLINE_CURLY:  put_str(b, ";4:3"); break;
        case UNDERLINE_DOTTED: put_str(b, ";4:4"); break;
        case UNDERLINE_DASHED: put_str(b, ";4:5"); break;
        }
    }

    if (attrs.blink)
        put_str(b, ";5");
    if (attrs.reverse)
        put_str(b, ";7");
    if (attrs.conceal)
        put_str(b, ";8");
    if (attrs.strikethrough)
        put_str(b, ";9");

    put_sgr_color(b, 30, attrs.fg_src, attrs.fg);
    put_sgr_color(b, 40, attrs.bg_src, attrs.bg);

    switch (underline.color_src) {
    case COLOR_DEFAULT:
    case COLOR_BASE16:  /* Not set by SGR 58 */
        break;
    case COLOR_BASE256:
        put_fmt(b, ";58:5:%u", underline.color & 0xff);
        break;
    case COLOR_RGB:
        put_fmt(b, ";58:2::%u:%u:%u", (underline.color >> 16) & 0xff,
                (underline.color >> 8) & 0xff, underline.color & 0xff);
        break;
    }

    put_str(b, "m");

    pen->attrs = attrs;
    pen->underline = underline;
}

static void
set_uri(struct buf *b, struct pen *pen, const char *uri, uint64_t id)
{
    if (uri == NULL && pen->uri == NULL)
        return;
    if (uri != NULL && pen->uri != NULL && id == pen->uri_id && streq(uri, pen->uri))
        return;

    if (uri == NULL)
        put_str(b, "\033]8;;\033\\");
    else {
        /*
         * The receiver hashes the ID: links stay grouped as they were.
         * Cells with the same ID belong to the same link; only, programs
         * may (wrongly) use it for others too: the URI is part of it.
         */
        uint64_t uri_hash = 5381;
        for (const char *p = uri; *p != '\0'; p++)
            uri_hash = uri_hash * 33 + (unsigned char)*p;

        put_fmt(b, "\033]8;id=%llx-%llx;",
                (unsigned long long)id, (unsigned long long)uri_hash);
        put_str(b, uri);
        put_str(b, "\033\\");
    }

    pen->uri = uri;
    pen->uri_id = id;
}

/*
 * Rows
 */

static const struct row_range *
range_at(const struct row_ranges *ranges, int col)
{
    for (int i = 0; i < ranges->count; i++) {
        const struct row_range *r = &ranges->v[i];
        if (r->start <= col && col <= r->end)
            return r;
    }
    return NULL;
}

static const struct row_range *
uri_at(const struct row *row, int col)
{
    return row->extra != NULL ? range_at(&row->extra->uri_ranges, col) : NULL;
}

static const struct row_range *
underline_at(const struct row *row, int col)
{
    return row->extra != NULL ? range_at(&row->extra->underline_ranges, col) : NULL;
}

static bool
is_empty(const struct cell *cell)
{
    return cell->wc == 0 || cell->wc == CELL_SPACER;
}

/* Nothing there at all: not printed, and not erased with a color */
static bool
is_blank(const struct row *row, int col)
{
    const struct cell *cell = &row->cells[col];
    return is_empty(cell) && cell->attrs.bg_src == COLOR_DEFAULT &&
        uri_at(row, col) == NULL;
}

/* The code points in a cell ('chars' may be used for the storage) */
static const char32_t *
cell_chars(const struct terminal *term, const struct cell *cell,
           char32_t *chars, size_t *count)
{
    if (cell->wc >= CELL_COMB_CHARS_LO && cell->wc <= CELL_COMB_CHARS_HI) {
        const struct composed *composed =
            composed_lookup(term->composed, cell->wc - CELL_COMB_CHARS_LO);
        if (composed != NULL) {
            *count = composed->count;
            return composed->chars;
        }
        *count = 0;
        return chars;
    }

    chars[0] = cell->wc;
    *count = 1;
    return chars;
}

/* The columns the receiver gives the character in 'cell' when printing it */
static int
char_width(const struct terminal *term, const struct cell *cell)
{
    int width;

    if (cell->wc >= CELL_COMB_CHARS_LO && cell->wc <= CELL_COMB_CHARS_HI) {
        const struct composed *composed =
            composed_lookup(term->composed, cell->wc - CELL_COMB_CHARS_LO);
        width = composed == NULL ? 1
            : composed->forced_width > 0 ? composed->forced_width : composed->width;
    } else
        width = c32width(cell->wc);

    return max(width, 1);
}

/*
 * Columns taken by the character at 'col': it, and the spacers after
 * it (any further ones are left over from a wide character that was
 * partly overwritten)
 */
static int
cell_width(const struct terminal *term, const struct row *row, int col, int cols)
{
    const int max_width = char_width(term, &row->cells[col]);
    int width = 1;
    while (width < max_width && col + width < cols &&
           row->cells[col + width].wc > CELL_SPACER)
    {
        width++;
    }
    return width;
}

static bool
is_regional_indicator(char32_t c)
{
    return c >= 0x1f1e6 && c <= 0x1f1ff;
}

/*
 * Whether printing 'next' right after 'last' would merge them into a
 * single cell (a combining character, or a grapheme cluster)
 */
static bool
would_merge(const struct terminal *term, char32_t last, char32_t next)
{
    if (c32width(next) <= 0)
        return true;

#if defined(FOOT_GRAPHEME_CLUSTERING)
    if (term->grapheme_shaping) {
        /* They pair up, depending on how many there are before */
        if (is_regional_indicator(next))
            return is_regional_indicator(last);
        return !utf8proc_grapheme_break(last, next);
    }
#endif

    return false;
}

struct emitter {
    const struct terminal *term;
    struct buf *b;
    struct pen pen;
    int cols;

    /* The receiver's cursor, and auto-wrap (DECAWM) */
    int row;
    int col;
    bool lcf;
    bool no_autowrap;

    /* The last thing printed may merge with the next */
    bool merge;
    char32_t last;

    /* The receiver's tab stops were changed (for printing tabs) */
    bool tabs_changed;
};

static void
move_to_col(struct emitter *e, int col)
{
    put_fmt(e->b, "\033[%dG", col + 1);
    e->col = col;
    e->lcf = false;
    e->merge = false;
}

static void
emit_marks(struct emitter *e, const struct row *row, int col)
{
    if (row->shell_integration.cmd_start == col)
        put_str(e->b, "\033]133;C\033\\");
    if (row->shell_integration.cmd_end == col)
        put_str(e->b, "\033]133;D\033\\");
}

static bool
has_mark(const struct row *row, int col)
{
    return row->shell_integration.cmd_start == col ||
        row->shell_integration.cmd_end == col;
}

/*
 * Where the spacers a wrapped row ends with start (where a wide
 * character didn't fit), or 'cols'
 */
static int
wrap_padding(const struct row *row, int cols)
{
    if (row->linebreak)
        return cols;

    int start = cols;
    while (start > 0 && row->cells[start - 1].wc == CELL_SPACER)
        start--;
    return start;
}

/*
 * How many of a cell's code points the receiver merges into one cell,
 * printed one after the other. All of them, unless foot made the
 * cluster some other way (e.g. a regional indicator merged into a pair
 * before it): the rest would end up in cells of their own.
 */
static size_t
cluster_length(const struct terminal *term, const char32_t *chars, size_t count)
{
#if defined(FOOT_GRAPHEME_CLUSTERING)
    if (term->grapheme_shaping) {
        /* As term_process_and_print_non_ascii() does */
        utf8proc_int32_t state = 0;
        for (size_t i = 1; i < count; i++) {
            if (utf8proc_grapheme_break_stateful(chars[i - 1], chars[i], &state))
                return i;
        }
        return count;
    }
#endif

    /* Without grapheme clustering, only combining characters merge */
    for (size_t i = 1; i < count; i++) {
        if (c32width(chars[i]) != 0 || chars[i] < 0x300)
            return i;
    }
    return count;
}

/* Prints the character in 'cell' (at 'col'), with its attributes */
static void
print_cell(struct emitter *e, const struct row *row, int col,
           const char32_t *chars, size_t count)
{
    count = cluster_length(e->term, chars, count);

    const struct cell *cell = &row->cells[col];
    const struct row_range *ul = underline_at(row, col);
    const struct row_range *uri = uri_at(row, col);

    set_pen(e->b, &e->pen, cell->attrs, ul != NULL ? &ul->underline : &no_underline);
    set_uri(e->b, &e->pen, uri != NULL ? uri->uri.uri : NULL,
            uri != NULL ? uri->uri.id : 0);

    for (size_t i = 0; i < count; i++)
        put_utf8(e->b, chars[i]);

    e->merge = true;
    e->last = chars[count - 1];
}

/*
 * Whether the cell at 'col' is the second half of a wide character
 * that isn't there any more (it was partly overwritten, or erased)
 */
static bool
is_orphan_spacer(const struct terminal *term, const struct row *row, int col)
{
    if (col < 1 || row->cells[col].wc != CELL_SPACER + 1)
        return false;

    const struct cell *before = &row->cells[col - 1];
    return is_empty(before) || before->wc > CELL_SPACER ||
        char_width(term, before) != 2;
}

/*
 * What partly overwritten wide characters leave behind (from 'col' on)
 * is made the same way, before what's in front of it is printed: wide
 * characters are printed there first, from the right. Then the
 * receiver's cursor goes to 'resume'. Returns whether there were any.
 */
static bool
prepare_orphan_spacers(struct emitter *e, const struct row *row, int col,
                       int resume)
{
    int end = col;
    while (end < e->cols && is_orphan_spacer(e->term, row, end))
        end++;

    if (end == col)
        return false;

    set_pen(e->b, &e->pen, (struct attributes){0}, &no_underline);
    set_uri(e->b, &e->pen, NULL, 0);

    for (int x = end - 1; x >= col; x--) {
        move_to_col(e, x - 1);
        put_utf8(e->b, U'　');
    }

    move_to_col(e, resume);
    return true;
}

/*
 * A tab that was printed (HT, see vt.c) left a tab in its cell, and
 * spaces in the ones up to where it went. It's printed again, with a
 * tab stop where its spaces end: the cells are printed (as spaces)
 * first, for their attributes. Returns the column after it.
 */
static int
emit_tab(struct emitter *e, const struct row *row, int col, bool deferred)
{
    const int cols = e->cols;
    static const char32_t space = U' ';

    int end = col + 1;
    while (end < cols && row->cells[end].wc == U' ')
        end++;

    if (deferred) {
        /* On a continued row: the first space wraps (on to this row) */
        print_cell(e, row, col, &space, 1);
        move_to_col(e, col);
    }

    prepare_orphan_spacers(e, row, end, col);

    for (int c = col; c < end; c++)
        print_cell(e, row, c, &space, 1);

    if (col == cols - 1) {
        /* Past the last column: a tab stays (and keeps that, see vt.c) */
        put_str(e->b, "\t");
        e->col = cols - 1;
        e->lcf = true;
        e->merge = false;
        return end;
    }

    /* Without a tab stop after it, a tab goes to the last column */
    put_str(e->b, "\033[3g");
    e->tabs_changed = true;
    if (end < cols) {
        move_to_col(e, end);
        put_str(e->b, "\033H");
    }

    move_to_col(e, col);
    put_str(e->b, "\t");

    e->col = end < cols ? end : cols - 1;
    e->lcf = false;
    e->merge = false;

    if (end == cols) {
        /* The last cell (a space), printed again: past the last column */
        print_cell(e, row, cols - 1, &space, 1);
        e->lcf = true;
        e->merge = false;
    }
    return end;
}

/*
 * Moves the receiver's cursor to 'col' of the row it's printing; to
 * after the last column (with the last column flag) for 'cols', by
 * printing what's in the last column again.
 */
static void
restore_position(struct emitter *e, const struct row *row, int col)
{
    const int cols = e->cols;

    if (col < cols) {
        move_to_col(e, col);
        return;
    }

    int start = cols - 1;
    while (start > 0 && row->cells[start].wc > CELL_SPACER)
        start--;

    const struct cell *cell = &row->cells[start];
    char32_t storage[1];
    size_t count = 0;
    const char32_t *chars = !is_empty(cell) && cell->wc < CELL_SPACER
        ? cell_chars(e->term, cell, storage, &count) : storage;

    if (count == 0 || start + cell_width(e->term, row, start, cols) != cols) {
        move_to_col(e, cols - 1);
        return;
    }

    static const char32_t space = U' ';
    move_to_col(e, start);

    /* Wide, in the last column: printed without auto-wrap (see autowrap()) */
    if (start + char_width(e->term, cell) > cols && !e->no_autowrap) {
        decset(e->b, 7, false);
        e->no_autowrap = true;
    }

    if (count == 1 && chars[0] == U'\t') {
        /* A tab in the last column (see emit_tab()) */
        print_cell(e, row, start, &space, 1);
        put_str(e->b, "\t");
    } else
        print_cell(e, row, start, chars, count);

    e->col = cols - 1;
    e->lcf = true;
    e->merge = false;
}

/* Auto-wrap back on (it clears the last column flag) */
static void
autowrap(struct emitter *e)
{
    if (!e->no_autowrap)
        return;
    decset(e->b, 7, true);
    e->no_autowrap = false;
    e->lcf = false;
}

/*
 * Shell integration marks in the columns from 'from' up to 'to' (that
 * the receiver's cursor went past at once, e.g. with a wide character):
 * the cursor goes back to each, and then to 'to' again.
 */
static void
emit_marks_within(struct emitter *e, const struct row *row, int from, int to)
{
    bool any = false;
    for (int col = from; col < to && col < e->cols; col++) {
        if (!has_mark(row, col))
            continue;
        move_to_col(e, col);
        emit_marks(e, row, col);
        any = true;
    }

    if (any) {
        restore_position(e, row, to);
        autowrap(e);
    }
}

/*
 * Prints a row's cells. The receiver's cursor is at the row's first
 * column, or, if 'continued', it's at the end of the previous one,
 * which wraps on to this one with the first character printed.
 */
static void
emit_cells(struct emitter *e, const struct row *row, bool continued)
{
    const struct terminal *term = e->term;
    const int cols = e->cols;

    e->col = 0;
    e->lcf = false;
    e->merge = false;

    /* Until the first cell is printed, the cursor isn't on this row */
    bool deferred = continued;
    if (!deferred && row->shell_integration.prompt_marker)
        put_str(e->b, "\033]133;A\033\\");

    const int padding = wrap_padding(row, cols);
    int last = padding - 1;
    while (last >= 0 && is_blank(row, last) && !has_mark(row, last))
        last--;

    for (int col = 0; col <= last;) {
        const int start = col;

        if (!deferred)
            emit_marks(e, row, col);

        const struct cell *cell = &row->cells[col];
        char32_t storage[1];
        size_t count = 0;
        const char32_t *chars = !is_empty(cell) && cell->wc < CELL_SPACER
            ? cell_chars(term, cell, storage, &count) : storage;

        if (is_empty(cell)) {
            /* Erased cells, of the same color */
            int run = 1;
            while (col + run <= last &&
                   is_empty(&row->cells[col + run]) &&
                   row->cells[col + run].attrs.bg_src == cell->attrs.bg_src &&
                   row->cells[col + run].attrs.bg == cell->attrs.bg &&
                   !has_mark(row, col + run))
            {
                run++;
            }

            const bool orphans = prepare_orphan_spacers(e, row, col + run, col);

            if (cell->attrs.bg_src != COLOR_DEFAULT || orphans) {
                set_pen(e->b, &e->pen,
                        (struct attributes){.bg_src = cell->attrs.bg_src,
                                            .bg = cell->attrs.bg},
                        &no_underline);
                put_fmt(e->b, "\033[%dX", run);
            }

            col += run;

            /* Up to the last column: the cursor stops at it */
            move_to_col(e, min(col, cols - 1));
        }

        else if (count == 0) {
            /*
             * A spacer without its character: an erased cell. In the
             * first column, a wide character's spacer is shifted there.
             */
            if (col == 0 && cell->wc == CELL_SPACER + 1) {
                /*
                 * As many as there are in a row, after a wide character
                 * (see prepare_orphan_spacers()); then it's deleted
                 */
                int count = 1;
                while (count < cols - 1 && is_orphan_spacer(term, row, count))
                    count++;

                set_pen(e->b, &e->pen, (struct attributes){0}, &no_underline);
                set_uri(e->b, &e->pen, NULL, 0);
                for (int x = count; x >= 1; x--) {
                    move_to_col(e, x - 1);
                    put_utf8(e->b, U'\u3000');
                }
                move_to_col(e, 0);
                put_str(e->b, "\033[P");
                col = count;
                emit_marks_within(e, row, 1, col);
            } else
                col++;
            move_to_col(e, min(col, cols - 1));
        }

        else if (count == 1 && chars[0] == U'\t') {
            col = emit_tab(e, row, col, deferred);
            emit_marks_within(e, row, start + 1, col);
        }

        else {
            if (e->merge && would_merge(term, e->last, chars[0]))
                move_to_col(e, col);

            const int width = cell_width(term, row, col, cols);
            const int printed_width = char_width(term, cell);

            /*
             * On a continued row, the character must be printed first
             * (to wrap): then again, once they're there.
             */
            const bool orphans =
                col + width < cols && is_orphan_spacer(term, row, col + width);
            if (orphans && !deferred)
                prepare_orphan_spacers(e, row, col + width, col);

            /* Wide, in the last column: printed without auto-wrap */
            const bool truncated = printed_width > cols - col;
            if (truncated) {
                decset(e->b, 7, false);
                e->no_autowrap = true;
            }

            print_cell(e, row, col, chars, count);

            if (orphans && deferred) {
                prepare_orphan_spacers(e, row, col + width, col);
                print_cell(e, row, col, chars, count);
            }

            col += width;
            if (col >= cols) {
                e->col = cols - 1;
                e->lcf = true;
            } else
                e->col = col;

            autowrap(e);

            if (!truncated && printed_width > width && col < cols) {
                /* Its spacer was overwritten, or erased: the receiver's too */
                const struct cell *next = &row->cells[col];
                move_to_col(e, col);
                set_pen(e->b, &e->pen,
                        (struct attributes){.bg_src = next->attrs.bg_src,
                                            .bg = next->attrs.bg},
                        &no_underline);
                put_fmt(e->b, "\033[%dX", min(printed_width - width, cols - col));
            }

            emit_marks_within(e, row, start + 1, col);
        }

        if (deferred) {
            /* On this row now: what was deferred, at the first column */
            deferred = false;
            if (row->shell_integration.prompt_marker)
                put_str(e->b, "\033]133;A\033\\");
            emit_marks_within(e, row, 0, col);
        }
    }

    /* Shell integration marks after the last character */
    for (int col = last + 1; col < cols; col++) {
        if (!has_mark(row, col))
            continue;
        if (col != e->col || e->lcf)
            move_to_col(e, col);
        emit_marks(e, row, col);
    }

    /* Where the wide character that's on the next row didn't fit */
    if (padding < cols && (e->col != padding || e->lcf))
        move_to_col(e, padding);
}

enum transition {
    LINEBREAK,          /* A new line */
    WRAP,               /* Printing the next row wraps on to it */
    WRAP_PLACEHOLDER,   /* Printing anything wraps on to it */
    WRAP_ERASED,        /* As above, once the last column is printed (and erased) */
    WRAP_WIDE,          /* Printing a wide character wraps on to it */
};

/* How the receiver gets from the end of 'row' to the start of 'next' */
static enum transition
transition(const struct emitter *e, const struct row *row, const struct row *next)
{
    if (row->linebreak || next == NULL)
        return LINEBREAK;

    const struct cell *first = &next->cells[0];

    char32_t storage[1];
    size_t count = 0;
    const char32_t *chars = !is_empty(first) && first->wc < CELL_SPACER
        ? cell_chars(e->term, first, storage, &count) : storage;

    /* A tab is printed as a space first (see emit_tab()) */
    const char32_t next_char = count == 0 ? 0 : chars[0] == U'\t' ? U' ' : chars[0];
    const bool printable =
        count > 0 && !(e->merge && would_merge(e->term, e->last, next_char));

    const int padding = wrap_padding(row, e->cols);
    if (padding < e->cols) {
        /* Wrapped because a wide character didn't fit */
        if (e->col != padding || e->lcf)
            return LINEBREAK;
        /*
         * Printing it wraps: unless its first code point alone fits
         * (e.g. a heart, wide with an emoji variation selector), and the
         * receiver puts it there first (and leaves its attributes).
         */
        if (printable && next_char != U' ' &&
            char_width(e->term, first) > e->cols - padding &&
            c32width(chars[0]) > e->cols - padding)
        {
            return WRAP;
        }
        return padding > e->cols - 2 ? WRAP_WIDE : LINEBREAK;
    }

    if (!e->lcf) {
        /*
         * Its last cell is erased: it was, after it wrapped (what's
         * left of a wide character there can't be made again: printing
         * on the row unwraps it)
         */
        return is_empty(&row->cells[e->cols - 1]) ? WRAP_ERASED : LINEBREAK;
    }

    return printable ? WRAP : WRAP_PLACEHOLDER;
}

/*
 * Prints 'count' rows of 'grid', from 'first_abs' on, starting at the
 * receiver's home position. Beyond the screen's last row, the screen
 * scrolls.
 */
static void
emit_rows(struct emitter *e, struct grid *grid, int first_abs, int count)
{
    const int screen_rows = e->term->rows;
    bool continued = false;

    e->row = 0;

    for (int i = 0; i < count; i++) {
        const int abs = (first_abs + i) & (grid->num_rows - 1);
        const struct row *row = grid->rows[abs];

        if (row != NULL)
            emit_cells(e, row, continued);
        continued = false;

        if (i == count - 1)
            break;

        const struct row *next =
            grid->rows[(first_abs + i + 1) & (grid->num_rows - 1)];

        enum transition t = row != NULL && next != NULL
            ? transition(e, row, next) : LINEBREAK;

        /*
         * Scrolling erases the new line with the pen's background: it
         * must be the default one. Printing the first character of
         * the next row would wrap with its own.
         */
        const bool scrolls = e->row == screen_rows - 1;
        if (scrolls && t == WRAP && next->cells[0].attrs.bg_src != COLOR_DEFAULT) {
            if (e->lcf)
                t = WRAP_PLACEHOLDER;
            else if (wrap_padding(row, e->cols) == e->cols - 1)
                t = WRAP_WIDE;
        }
        if (scrolls && t != WRAP)
            set_pen(e->b, &e->pen, (struct attributes){0}, &no_underline);

        switch (t) {
        case WRAP:
            continued = true;
            break;

        case WRAP_ERASED: {
            /* Printed, wrapped, and then erased again, on both rows */
            const struct attributes last = row->cells[e->cols - 1].attrs;
            set_pen(e->b, &e->pen, (struct attributes){0}, &no_underline);
            set_uri(e->b, &e->pen, NULL, 0);
            move_to_col(e, e->cols - 1);
            put_str(e->b, "  \033[1G\033[X\033[A");
            put_fmt(e->b, "\033[%dG", e->cols);
            set_pen(e->b, &e->pen,
                    (struct attributes){.bg_src = last.bg_src, .bg = last.bg},
                    &no_underline);
            put_str(e->b, "\033[X\033[B\033[1G");
            e->col = 0;
            e->lcf = false;
            e->merge = false;
            break;
        }

        case WRAP_WIDE:
            /* A wide space doesn't fit, and wraps; then it's erased again */
            set_pen(e->b, &e->pen, (struct attributes){0}, &no_underline);
            set_uri(e->b, &e->pen, NULL, 0);
            put_utf8(e->b, U'\u3000');
            put_str(e->b, "\033[1G\033[2X");
            e->col = 0;
            e->lcf = false;
            e->merge = false;
            break;

        case WRAP_PLACEHOLDER:
            /* A space wraps; then it's erased again */
            set_pen(e->b, &e->pen, (struct attributes){0}, &no_underline);
            put_str(e->b, " \033[1G\033[X");
            e->col = 0;
            e->lcf = false;
            e->merge = false;
            break;

        case LINEBREAK:
            put_str(e->b, "\r\n");
            e->col = 0;
            e->lcf = false;
            e->merge = false;
            break;
        }

        e->row = min(e->row + 1, screen_rows - 1);
    }
}

/*
 * The cursor at (row, col) of the screen. With 'lcf', after the last
 * column, wrapping with the next character: the character there is
 * printed again. If there is none, with 'placeholder', a space is
 * printed instead (returns true then: it must be erased again).
 */
static bool
emit_cursor(struct emitter *e, const struct grid *grid, struct cursor cursor,
            int row_offset, bool placeholder)
{
    const int row = cursor.point.row;
    const int col = cursor.point.col;

    /* (With origin mode, foot's DECRC may leave it outside the region) */
    put_fmt(e->b, "\033[%d;%dH", max(row - row_offset, 0) + 1, col + 1);
    e->col = col;
    e->lcf = false;
    e->merge = false;

    if (!cursor.lcf || col != e->cols - 1)
        return false;

    /*
     * Printing on a wrapped row unwraps it (term_print()): the
     * row is more important.
     */
    const struct row *r = grid->rows[(grid->offset + row) & (grid->num_rows - 1)];
    if (r == NULL || !r->linebreak)
        return false;

    restore_position(e, r, e->cols);
    if (e->lcf || !placeholder || !is_empty(&r->cells[col]))
        return false;

    set_pen(e->b, &e->pen, (struct attributes){0}, &no_underline);
    set_uri(e->b, &e->pen, NULL, 0);
    put_str(e->b, " ");
    e->lcf = true;
    return true;
}

/*
 * Character sets
 */

static void
emit_charsets(struct buf *b, const struct charsets *charsets)
{
    static const char designators[] = {'(', ')', '*', '+'};

    for (size_t i = 0; i < ALEN(designators); i++) {
        if (charsets->set[i] == CHARSET_GRAPHIC)
            put_fmt(b, "\033%c0", designators[i]);
    }

    switch (charsets->selected) {
    case G0: break;
    case G1: put_str(b, "\x0e"); break;
    case G2: put_str(b, "\033n"); break;
    case G3: put_str(b, "\033o"); break;
    }
}

static bool
charsets_are_default(const struct charsets *charsets)
{
    for (size_t i = 0; i < ALEN(charsets->set); i++) {
        if (charsets->set[i] != CHARSET_ASCII)
            return false;
    }
    return charsets->selected == G0;
}

/* Back to the initial ones (all ASCII, G0) */
static void
reset_charsets(struct buf *b, const struct charsets *charsets)
{
    static const char designators[] = {'(', ')', '*', '+'};

    for (size_t i = 0; i < ALEN(designators); i++) {
        if (charsets->set[i] != CHARSET_ASCII)
            put_fmt(b, "\033%cB", designators[i]);
    }
    if (charsets->selected != G0)
        put_str(b, "\x0f");
}

/* A saved cursor (DECSC): position, attributes and character sets */
static void
emit_saved_cursor(struct emitter *e, const struct grid *grid,
                  const struct charsets *charsets)
{
    const bool placeholder = emit_cursor(e, grid, grid->saved_cursor, 0, true);

    set_pen(e->b, &e->pen, grid->saved_attrs, &grid->saved_underline);
    emit_charsets(e->b, charsets);
    put_str(e->b, "\0337");
    reset_charsets(e->b, charsets);
    autowrap(e);

    if (placeholder) {
        /* Saved after the last column; the cell there is erased again */
        const struct row *r = grid->rows[
            (grid->offset + grid->saved_cursor.point.row) & (grid->num_rows - 1)];
        const struct attributes attrs = r->cells[e->cols - 1].attrs;

        move_to_col(e, e->cols - 1);
        set_pen(e->b, &e->pen,
                (struct attributes){.bg_src = attrs.bg_src, .bg = attrs.bg},
                &no_underline);
        put_str(e->b, "\033[X");
    }
}

/*
 * The kitty keyboard protocol's flags (a stack of eight, in a ring).
 * Pushing 8 + idx times leaves every entry with its value, and the
 * index where it was.
 */
static void
emit_kitty_kbd(struct buf *b, const struct grid *grid)
{
    bool all_zero = grid->kitty_kbd.idx == 0;
    for (size_t i = 0; i < ALEN(grid->kitty_kbd.flags); i++) {
        if (grid->kitty_kbd.flags[i] != 0)
            all_zero = false;
    }
    if (all_zero)
        return;

    const size_t count = ALEN(grid->kitty_kbd.flags);
    for (size_t i = 1; i <= count + grid->kitty_kbd.idx; i++)
        put_fmt(b, "\033[>%uu", grid->kitty_kbd.flags[i % count]);
}

/*
 * Colors, title, working directory
 */

static void
put_color(struct buf *b, uint32_t color)
{
    put_fmt(b, "rgb:%02x/%02x/%02x",
            (color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff);
}

static void
emit_colors(struct buf *b, const struct terminal *term)
{
    const struct config *conf = term->conf;
    const struct color_theme *theme =
        term->colors.active_theme == COLOR_THEME_LIGHT
            ? &conf->colors_light : &conf->colors_dark;

    for (size_t i = 0; i < ALEN(term->colors.table); i++) {
        if (term->colors.table[i] == theme->table[i])
            continue;
        put_fmt(b, "\033]4;%zu;", i);
        put_color(b, term->colors.table[i]);
        put_str(b, "\033\\");
    }

    if (term->colors.fg != theme->fg) {
        put_str(b, "\033]10;");
        put_color(b, term->colors.fg);
        put_str(b, "\033\\");
    }

    if (term->colors.bg != theme->bg) {
        put_str(b, "\033]11;");
        put_color(b, term->colors.bg);
        put_str(b, "\033\\");
    }

    const uint32_t cursor_bg =
        (theme->use_custom.cursor ? 1u << 31 : 0) | theme->cursor.cursor;
    if (term->colors.cursor_bg != cursor_bg && (term->colors.cursor_bg >> 31)) {
        put_str(b, "\033]12;");
        put_color(b, term->colors.cursor_bg);
        put_str(b, "\033\\");
    }

    if (term->colors.selection_bg != theme->selection_bg) {
        put_str(b, "\033]17;");
        put_color(b, term->colors.selection_bg);
        put_str(b, "\033\\");
    }

    if (term->colors.selection_fg != theme->selection_fg) {
        put_str(b, "\033]19;");
        put_color(b, term->colors.selection_fg);
        put_str(b, "\033\\");
    }
}

/* Control characters would end the OSC (they never get there) */
static void
put_osc_text(struct buf *b, const char *s)
{
    for (const char *p = s; *p != '\0'; p++) {
        if ((unsigned char)*p >= 0x20 && *p != 0x7f)
            put(b, p, 1);
    }
}

static void
emit_title(struct buf *b, const struct terminal *term)
{
    const struct config *conf = term->conf;

    if (term->window_title != NULL &&
        (conf->title == NULL || !streq(term->window_title, conf->title)))
    {
        put_str(b, "\033]2;");
        put_osc_text(b, term->window_title);
        put_str(b, "\033\\");
    }

    if (term->app_id != NULL) {
        put_str(b, "\033]176;");
        put_osc_text(b, term->app_id);
        put_str(b, "\033\\");
    }

    if (term->mouse_user_cursor != NULL) {
        put_str(b, "\033]22;");
        put_osc_text(b, term->mouse_user_cursor);
        put_str(b, "\033\\");
    }
}

static void
emit_cwd(struct buf *b, const struct terminal *term)
{
    if (term->cwd == NULL || term->cwd[0] != '/')
        return;

    put_str(b, "\033]7;file://localhost");
    for (const unsigned char *p = (const unsigned char *)term->cwd; *p != '\0'; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || strchr("/-._~!$&'()*+,;=:@", *p) != NULL)
        {
            put(b, p, 1);
        } else
            put_fmt(b, "%%%02X", *p);
    }
    put_str(b, "\033\\");
}

/*
 * Modes
 */

/* The modes that aren't as in a new terminal */
static void
emit_modes(struct buf *b, const struct terminal *term)
{
    const struct config *conf = term->conf;

    if (term->cursor_keys_mode == CURSOR_KEYS_APPLICATION)
        decset(b, 1, true);
    if (term->reverse)
        decset(b, 5, true);
    if (term->cursor_blink.decset)
        decset(b, 12, true);
    if (term->hide_cursor)
        decset(b, 25, false);
    if (!term->reverse_wrap)
        decset(b, 45, false);
    if (term->keypad_keys_mode == KEYPAD_APPLICATION)
        decset(b, 66, true);
    if (!term->sixel.scrolling)
        decset(b, 80, true);

    switch (term->mouse_tracking) {
    case MOUSE_NONE:
    case MOUSE_X10:
        break;
    case MOUSE_CLICK:  decset(b, 1000, true); break;
    case MOUSE_DRAG:   decset(b, 1002, true); break;
    case MOUSE_MOTION: decset(b, 1003, true); break;
    }

    switch (term->mouse_reporting) {
    case MOUSE_NORMAL:
    case MOUSE_UTF8:
        break;
    case MOUSE_SGR:        decset(b, 1006, true); break;
    case MOUSE_URXVT:      decset(b, 1015, true); break;
    case MOUSE_SGR_PIXELS: decset(b, 1016, true); break;
    }

    /* Reports the focus, which is what the program wants after this */
    if (term->focus_events)
        decset(b, 1004, true);

    if (term->alt_scrolling != conf->mouse.alternate_scroll_mode)
        decset(b, 1007, term->alt_scrolling);
    if (!term->meta.eight_bit)
        decset(b, 1034, false);
    if (!term->num_lock_modifier)
        decset(b, 1035, false);
    if (!term->meta.esc_prefix)
        decset(b, 1036, false);
    if (!term->bell_action_enabled)
        decset(b, 1042, false);
    if (!term->sixel.use_private_palette)
        decset(b, 1070, false);
    if (term->bracketed_paste)
        decset(b, 2004, true);
    if (term->report_theme_changes)
        decset(b, 2031, true);

    /* These two report right away; the program learns what's current */
    if (term->visibility_reports)
        decset(b, 2033, true);
    if (term->size_notifications)
        decset(b, 2048, true);

    if (term->sixel.cursor_right_of_graphics)
        decset(b, 8452, true);

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    if (!term->ime_enabled)
        decset(b, 737769, false);
#endif

    if (term->insert_mode)
        put_str(b, "\033[4h");

    if (term->modify_other_keys_2)
        put_str(b, "\033[>4;2m");

    /* DECSCUSR */
    const enum cursor_style conf_style = conf->cursor.style;
    if (term->cursor_style != conf_style ||
        term->cursor_blink.deccsusr != conf->cursor.blink.enabled)
    {
        int base = -1;
        switch (term->cursor_style) {
        case CURSOR_BLOCK:
            base = conf_style != CURSOR_HOLLOW ? 1 : -1;
            break;
        case CURSOR_HOLLOW:
            base = conf_style == CURSOR_HOLLOW ? 1 : -1;
            break;
        case CURSOR_UNDERLINE: base = 3; break;
        case CURSOR_BEAM:      base = 5; break;
        }

        if (base > 0)
            put_fmt(b, "\033[%d q", base + (term->cursor_blink.deccsusr ? 0 : 1));
    }
}

static void
emit_tab_stops(struct emitter *e, const struct terminal *term)
{
    bool is_default = true;
    int expected = 0;
    tll_foreach(term->tab_stops, it) {
        if (it->item != expected) {
            is_default = false;
            break;
        }
        expected += 8;
    }
    if (expected < term->cols)
        is_default = false;

    if (is_default && !e->tabs_changed)
        return;

    put_str(e->b, "\033[3g");
    tll_foreach(term->tab_stops, it) {
        if (it->item >= term->cols)
            continue;
        move_to_col(e, it->item);
        put_str(e->b, "\033H");
    }
}

/*
 * The state
 */

char *
snapshot_create(const struct terminal *term, size_t *len)
{
    struct buf b = {0};

    struct emitter e = {
        .term = term,
        .b = &b,
        .cols = term->cols,
    };

    /* Everything is printed with the current grapheme clustering */
#if defined(FOOT_GRAPHEME_CLUSTERING)
    if (term->grapheme_shaping != term->conf->tweak.grapheme_shaping)
        decset(&b, 2027, term->grapheme_shaping);
#endif

    emit_colors(&b, term);
    emit_title(&b, term);
    emit_cwd(&b, term);

    struct grid *normal = (struct grid *)&term->normal;
    struct grid *alt = (struct grid *)&term->alt;
    const bool on_alt = term->grid == &term->alt;

    /*
     * The normal screen, and its scrollback above it: from the oldest
     * line on, in order. The ones beyond the screen's top scroll into
     * the receiver's scrollback.
     */
    if (normal->rows != NULL && normal->num_rows > 0) {
        const int screen_top = normal->offset;
        const int sb_start = grid_sb_start_ignore_uninitialized(normal, term->rows);
        const int sb_count =
            (screen_top - sb_start + normal->num_rows) & (normal->num_rows - 1);

        emit_rows(&e, normal, sb_start, sb_count + term->rows);

        /* The cursor saved on it (e.g. by the alternate screen's 1049) */
        emit_saved_cursor(&e, normal, &term->saved_charsets);
        emit_kitty_kbd(&b, normal);
    }

    if (on_alt && alt->rows != NULL) {
        /* Entering it (1047) erases it with the pen's background */
        set_pen(&b, &e.pen, (struct attributes){0}, &no_underline);
        set_uri(&b, &e.pen, NULL, 0);
        put_str(&b, "\033[?1047h\033[H");

        emit_rows(&e, alt, alt->offset, term->rows);
        emit_saved_cursor(&e, alt, &term->saved_charsets);
        emit_kitty_kbd(&b, alt);
    }

    emit_tab_stops(&e, term);

    /* Clears the last column flag: before the cursor */
    if (!term->auto_margin) {
        decset(&b, 7, false);
        e.no_autowrap = true;
    }

    /* Moves the cursor home */
    const struct scroll_region *region = &term->scroll_region;
    if (region->start != 0 || region->end != term->rows)
        put_fmt(&b, "\033[%d;%dr", region->start + 1, region->end);

    int row_offset = 0;
    if (term->origin == ORIGIN_RELATIVE) {
        decset(&b, 6, true);
        row_offset = region->start;
    }

    emit_cursor(&e, term->grid, term->grid->cursor, row_offset, false);
    if (term->auto_margin)
        autowrap(&e);

    emit_modes(&b, term);

    if (!charsets_are_default(&term->charsets))
        emit_charsets(&b, &term->charsets);

    /* The pen: attributes, and the link being printed */
    set_pen(&b, &e.pen, term->vt.attrs, &term->vt.underline);
    set_uri(&b, &e.pen, term->vt.osc8.uri, term->vt.osc8.id);

    *len = b.len;
    return b.data != NULL ? b.data : xstrdup("");
}
