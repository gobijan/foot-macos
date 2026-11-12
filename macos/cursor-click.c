#include "cursor-click.h"

#include <string.h>

#include "debug.h"
#include "grid.h"
#include "macros.h"
#include "terminal.h"
#include "url-mode.h"
#include "wayland.h"
#include "xmalloc.h"

/* The row the prompt the cursor is in begins on; -1 if it's in none */
static int
prompt_row(struct terminal *term)
{
    struct grid *grid = term->grid;
    for (int r = grid->cursor.point.row; r >= 0; r--) {
        if (grid_row_in_view(grid, r)->shell_integration.prompt_marker)
            return r;
    }
    return -1;
}

static bool
has_text(const struct row *row, int cols)
{
    for (int c = 0; c < cols; c++) {
        if (row->cells[c].wc != 0)
            return true;
    }
    return false;
}

int
cursor_click_moves(struct terminal *term, int row, int col)
{
    struct grid *grid = term->grid;
    const struct coord cursor = grid->cursor.point;

    const int first = prompt_row(term);
    if (first < 0 || row < first || row >= term->rows || col < 0 || col >= term->cols)
        return 0;

    const bool left = row < cursor.row || (row == cursor.row && col < cursor.col);
    const struct coord from = left ? (struct coord){.row = row, .col = col} : cursor;
    const struct coord to = left ? cursor : (struct coord){.row = row, .col = col};

    /*
     * A key for each character in between: not for the cells nothing
     * was written to (after the text's end), nor for the second half
     * of a wide character. And one for each line's end, where the
     * shell edits several lines (there's text on the next row).
     */
    int moves = 0;
    for (int r = from.row; r <= to.row; r++) {
        const struct row *cells = grid_row_in_view(grid, r);
        const int start = r == from.row ? from.col : 0;
        const int end = r == to.row ? to.col : term->cols;

        for (int c = start; c < end; c++) {
            const char32_t wc = cells->cells[c].wc;
            moves += wc != 0 && wc < CELL_SPACER;
        }

        /* (Rows end with one, unless they wrap; also the text's last) */
        if (r < to.row && cells->linebreak && has_text(grid_row_in_view(grid, r + 1), term->cols))
            moves++;
    }

    return left ? -moves : moves;
}

bool
cursor_click(struct terminal *term, int row, int col)
{
    const struct wl_window *win = term->window;

    if (term->grid != &term->normal || term->grid->view != term->grid->offset ||
        term->mouse_tracking != MOUSE_NONE || term->is_searching ||
        urls_mode_is_active(term) || (win != NULL && win->command_running) ||
        prompt_row(term) < 0)
    {
        return false;
    }

    const int moves = cursor_click_moves(term, row, col);
    if (moves == 0)
        return row >= prompt_row(term);     /* The prompt's, still */

    const bool application = term->cursor_keys_mode == CURSOR_KEYS_APPLICATION;
    const char *key = moves < 0
        ? (application ? "\033OD" : "\033[D")
        : (application ? "\033OC" : "\033[C");

    const size_t count = moves < 0 ? -moves : moves;
    char *keys = xmalloc(count * 3);
    for (size_t i = 0; i < count; i++)
        memcpy(keys + i * 3, key, 3);

    term_to_slave(term, keys, count * 3);
    free(keys);
    return true;
}

static void UNUSED
test_type(struct grid *grid, int row, int col, const char32_t *text)
{
    for (; *text != U'\0'; text++, col++)
        grid_row(grid, row)->cells[col].wc = *text;
}

UNITTEST
{
    const int cols = 10;

    struct terminal term = {
        .rows = 4,
        .cols = cols,
        .normal = {
            .rows = xcalloc(4, sizeof(term.normal.rows[0])),
            .num_rows = 4,
            .num_cols = cols,
        },
        .grid = &term.normal,
    };

    struct grid *grid = &term.normal;
    for (int i = 0; i < grid->num_rows; i++)
        grid->rows[i] = grid_row_alloc(cols, true);

    /* "$ echo abc", with the cursor at its end; a line of output above */
    test_type(grid, 0, 0, U"output");
    grid_row(grid, 0)->linebreak = true;
    test_type(grid, 1, 0, U"$ echo abc");
    grid_row(grid, 1)->linebreak = false;
    test_type(grid, 2, 0, U"d ef");
    grid->cursor.point = (struct coord){.row = 2, .col = 4};

    /* Without the prompt's mark: no shell integration */
    xassert(cursor_click_moves(&term, 2, 1) == 0);

    /* The line wraps: to the "e" of "ef", of "echo", and to the cursor itself */
    grid_row(grid, 1)->shell_integration.prompt_marker = true;
    xassert(cursor_click_moves(&term, 2, 2) == -2);
    xassert(cursor_click_moves(&term, 1, 2) == -12);
    xassert(cursor_click_moves(&term, 2, 4) == 0);

    /* After the text's end, and below: nowhere to go. Above the prompt: not its */
    xassert(cursor_click_moves(&term, 2, 9) == 0);
    xassert(cursor_click_moves(&term, 3, 3) == 0);
    xassert(cursor_click_moves(&term, 0, 3) == 0);

    /* From within the text, to the right; a wide character is one key */
    grid_row(grid, 2)->cells[1].wc = CELL_SPACER;
    grid->cursor.point = (struct coord){.row = 1, .col = 7};
    xassert(cursor_click_moves(&term, 2, 3) == 5);

    /* Two lines the shell edits: the first one's end is a key too */
    grid_row(grid, 1)->linebreak = true;
    xassert(cursor_click_moves(&term, 2, 0) == 4);

    for (int i = 0; i < grid->num_rows; i++)
        grid_row_free(grid->rows[i]);
    free(grid->rows);
}
