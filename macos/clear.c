/*
 * Edit > Clear to Start and Clear Scrollback, like kitty's
 * clear_terminal actions to_cursor and scrollback. The terminal does
 * this on its own, without the shell, so it also works while a command
 * is running, and keeps what's been typed at the prompt.
 */

#include "clear.h"

#include <tllist.h>

#include "debug.h"
#include "fdm.h"
#include "grid.h"
#include "macros.h"
#include "render.h"
#include "selection.h"
#include "terminal.h"
#include "url-mode.h"
#include "xmalloc.h"

/* Like key bindings: not while search or URL mode own the keyboard */
static bool
keyboard_is_modal(const struct terminal *term)
{
    return term->is_searching || urls_mode_is_active(term);
}

/*
 * The start of the prompt the cursor is in, which can have several
 * rows. This takes shell integration (OSC 133). Otherwise, or in the
 * output of a command, the line with the cursor, and the rows it
 * wraps from.
 */
static int
first_row_to_keep(struct terminal *term)
{
    struct grid *grid = term->grid;
    const int cursor_row = grid->cursor.point.row;

    for (int r = cursor_row; r >= 0; r--) {
        const struct row *row = grid_row(grid, r);

        if (row->shell_integration.prompt_marker)
            return r;
        if (row->shell_integration.cmd_start >= 0 ||
            row->shell_integration.cmd_end >= 0)
        {
            break;
        }
    }

    int row = cursor_row;
    while (row > 0 && !grid_row(grid, row - 1)->linebreak)
        row--;
    return row;
}

void
clear_to_start(struct terminal *term)
{
    /* The alternate screen is the application's, which draws it */
    if (keyboard_is_modal(term) || term->grid != &term->normal)
        return;

    struct grid *grid = term->grid;
    const int rows = first_row_to_keep(term);

    /* Rows below the cursor stay: the shell's own, e.g. completions */
    if (rows > 0) {
        term_scroll_partial(
            term, (struct scroll_region){.start = 0, .end = term->rows}, rows);
        grid->cursor.point.row -= rows;
        grid->cur_row = grid_row(grid, grid->cursor.point.row);
    }

    term_erase_scrollback(term);
    render_refresh(term);
}

void
clear_scrollback(struct terminal *term)
{
    if (keyboard_is_modal(term))
        return;

    /*
     * It's the normal screen's, also while the alternate screen is
     * active. A selection is then on the other grid, and can't be
     * compared to this one's rows.
     */
    struct grid *active = term->grid;
    if (active != &term->normal)
        selection_cancel(term);

    term->grid = &term->normal;
    term_erase_scrollback(term);
    term->grid = active;

    render_refresh(term);
}

/* A screen of 5 rows, with the cursor on row 3, and a full scrollback */
static void UNUSED
test_reset(struct terminal *term)
{
    struct grid *grid = &term->normal;

    for (int i = 0; i < grid->num_rows; i++) {
        grid_row_free(grid->rows[i]);
        grid->rows[i] = grid_row_alloc(grid->num_cols, true);
    }

    /* The screen wraps around the end of the ring buffer */
    grid->offset = grid->view = 14;
    grid->cursor.point = (struct coord){.col = 2, .row = 3};
    grid->cur_row = grid_row(grid, 3);
}

static bool UNUSED
test_scrollback_is_empty(const struct terminal *term)
{
    const struct grid *grid = &term->normal;
    const int mask = grid->num_rows - 1;

    for (int i = term->rows; i < grid->num_rows; i++) {
        if (grid->rows[(grid->offset + i) & mask] != NULL)
            return false;
    }
    return true;
}

/* Clears, expecting @first to become the first row */
static void UNUSED
test_clear_to_start(struct terminal *term, int first)
{
    struct grid *grid = &term->normal;
    struct row *kept = grid_row(grid, first);
    const int cursor_row = grid->cursor.point.row;

    clear_to_start(term);

    xassert(grid_row(grid, 0) == kept);
    xassert(grid->cursor.point.row == cursor_row - first);
    xassert(grid->cursor.point.col == 2);
    xassert(grid->cur_row == grid_row(grid, grid->cursor.point.row));
    xassert(grid->view == grid->offset);
    xassert(test_scrollback_is_empty(term));
}

UNITTEST
{
    const int cols = 5;

    struct fdm *fdm = fdm_init();
    xassert(fdm != NULL);

    struct terminal term = {
        .fdm = fdm,
        .rows = 5,
        .cols = cols,
        .normal = {
            .rows = xcalloc(16, sizeof(term.normal.rows[0])),
            .num_rows = 16,
            .num_cols = cols,
        },
        .grid = &term.normal,
        .selection = {
            .coords = {
                .start = {-1, -1},
                .end = {-1, -1},
            },
            .kind = SELECTION_NONE,
            .auto_scroll = {
                .fd = -1,
            },
        },
    };

    struct grid *grid = &term.normal;

    /* No shell integration: just the line with the cursor */
    test_reset(&term);
    test_clear_to_start(&term, 3);

    /* Scrolled back, to what goes away */
    test_reset(&term);
    grid->view = (grid->offset + term.rows) & (grid->num_rows - 1);
    test_clear_to_start(&term, 3);

    /* A line wrapped over rows 2 and 3 */
    test_reset(&term);
    grid_row(grid, 2)->linebreak = false;
    test_clear_to_start(&term, 2);

    /* Nothing to scroll: the scrollback still goes */
    test_reset(&term);
    grid_row(grid, 0)->linebreak = false;
    grid_row(grid, 1)->linebreak = false;
    grid_row(grid, 2)->linebreak = false;
    test_clear_to_start(&term, 0);

    /* A prompt of several rows */
    test_reset(&term);
    grid_row(grid, 1)->shell_integration.prompt_marker = true;
    test_clear_to_start(&term, 1);

    /* The prompt follows the end of the previous command, on its row */
    test_reset(&term);
    grid_row(grid, 2)->shell_integration.prompt_marker = true;
    grid_row(grid, 2)->shell_integration.cmd_end = 0;
    test_clear_to_start(&term, 2);

    /* In the output of a command: not at a prompt */
    test_reset(&term);
    grid_row(grid, 0)->shell_integration.prompt_marker = true;
    grid_row(grid, 1)->shell_integration.cmd_start = 0;
    test_clear_to_start(&term, 3);

    /* The screen stays as it is */
    test_reset(&term);
    struct row *first = grid_row(grid, 0);
    clear_scrollback(&term);
    xassert(grid_row(grid, 0) == first);
    xassert(grid->cursor.point.row == 3);
    xassert(test_scrollback_is_empty(&term));

    /* While searching, nothing happens */
    test_reset(&term);
    first = grid_row(grid, 0);
    term.is_searching = true;
    clear_to_start(&term);
    clear_scrollback(&term);
    term.is_searching = false;
    xassert(grid_row(grid, 0) == first);
    xassert(grid->cursor.point.row == 3);
    xassert(!test_scrollback_is_empty(&term));

    /* The alternate screen: it isn't cleared, but the scrollback can be */
    test_reset(&term);
    first = grid_row(grid, 0);
    term.grid = &term.alt;
    clear_to_start(&term);
    xassert(grid_row(grid, 0) == first);
    xassert(grid->cursor.point.row == 3);
    xassert(!test_scrollback_is_empty(&term));
    clear_scrollback(&term);
    xassert(term.grid == &term.alt);
    xassert(grid_row(grid, 0) == first);
    xassert(test_scrollback_is_empty(&term));

    for (int i = 0; i < grid->num_rows; i++)
        grid_row_free(grid->rows[i]);
    free(grid->rows);
    tll_free(grid->scroll_damage);
    fdm_destroy(fdm);
}
