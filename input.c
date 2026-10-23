#include "input.h"

#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <threads.h>
#include <locale.h>
#include <errno.h>
#include <wctype.h>
#include <math.h>
#include <sys/epoll.h>
#include <fcntl.h>

#include <linux/input-event-codes.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-compose.h>

#define LOG_MODULE "input"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "commands.h"
#include "config.h"
#include "grid.h"
#include "keymap.h"
#include "kitty-keymap.h"
#include "macros.h"
#include "render.h"
#include "search.h"
#include "selection.h"
#include "spawn.h"
#include "terminal.h"
#include "tokenize.h"
#include "unicode-mode.h"
#include "url-mode.h"
#include "util.h"
#include "vt.h"
#include "xkbcommon-vmod.h"
#include "xmalloc.h"
#include "xsnprintf.h"

struct pipe_context {
    char *text;
    size_t idx;
    size_t left;
};

static bool
fdm_write_pipe(struct fdm *fdm, int fd, int events, void *data)
{
    struct pipe_context *ctx = data;

    if (events & EPOLLHUP)
        goto pipe_closed;

    xassert(events & EPOLLOUT);
    ssize_t written = write(fd, &ctx->text[ctx->idx], ctx->left);

    if (written < 0) {
        LOG_WARN("failed to write to pipe: %s", strerror(errno));
        goto pipe_closed;
    }

    xassert(written <= ctx->left);
    ctx->idx += written;
    ctx->left -= written;

    if (ctx->left == 0)
        goto pipe_closed;

    return true;

pipe_closed:
    free(ctx->text);
    free(ctx);
    fdm_del(fdm, fd);
    return true;
}

static void alternate_scroll(struct seat *seat, int amount, int button);

static bool
execute_binding(struct seat *seat, struct terminal *term,
                const struct key_binding *binding, uint32_t serial, int amount)
{
    const enum bind_action_normal action = binding->action;

    switch (action) {
    case BIND_ACTION_NONE:
        return true;

    case BIND_ACTION_NOOP:
        return true;

    case BIND_ACTION_SCROLLBACK_UP_PAGE:
        if (term->grid == &term->normal) {
            cmd_scrollback_up(term, term->rows);
            return true;
        }
        break;

    case BIND_ACTION_SCROLLBACK_UP_HALF_PAGE:
        if (term->grid == &term->normal) {
            cmd_scrollback_up(term, max(term->rows / 2, 1));
            return true;
        }
        break;

    case BIND_ACTION_SCROLLBACK_UP_LINE:
        if (term->grid == &term->normal) {
            cmd_scrollback_up(term, 1);
            return true;
        }
        break;

    case BIND_ACTION_SCROLLBACK_UP_MOUSE:
        if (term->grid == &term->alt) {
            if (term->alt_scrolling) {
                alternate_scroll(seat, amount, BTN_BACK);
                return true;
            }
        } else {
            cmd_scrollback_up(term, amount);
            return true;
        }
        break;

    case BIND_ACTION_SCROLLBACK_DOWN_PAGE:
        if (term->grid == &term->normal) {
            cmd_scrollback_down(term, term->rows);
            return true;
        }
        break;

    case BIND_ACTION_SCROLLBACK_DOWN_HALF_PAGE:
        if (term->grid == &term->normal) {
            cmd_scrollback_down(term, max(term->rows / 2, 1));
            return true;
        }
        break;

    case BIND_ACTION_SCROLLBACK_DOWN_LINE:
        if (term->grid == &term->normal) {
            cmd_scrollback_down(term, 1);
            return true;
        }
        break;

    case BIND_ACTION_SCROLLBACK_DOWN_MOUSE:
        if (term->grid == &term->alt) {
            if (term->alt_scrolling) {
                alternate_scroll(seat, amount, BTN_FORWARD);
                return true;
            }
        } else {
            cmd_scrollback_down(term, amount);
            return true;
        }
        break;

    case BIND_ACTION_SCROLLBACK_HOME:
        if (term->grid == &term->normal) {
            cmd_scrollback_up(term, term->grid->num_rows);
            return true;
        }
        break;

    case BIND_ACTION_SCROLLBACK_END:
        if (term->grid == &term->normal) {
            cmd_scrollback_down(term, term->grid->num_rows);
            return true;
        }
        break;

    case BIND_ACTION_CLIPBOARD_COPY:
        selection_to_clipboard(seat, term, serial);
        return true;

    case BIND_ACTION_CLIPBOARD_PASTE:
        selection_from_clipboard(seat, term, serial);
        term_reset_view(term);
        return true;

    case BIND_ACTION_PRIMARY_PASTE:
        selection_from_primary(seat, term);
        term_reset_view(term);
        return true;

    case BIND_ACTION_SEARCH_START:
        search_begin(term);
        return true;

    case BIND_ACTION_FONT_SIZE_UP:
        term_font_size_increase(term);
        return true;

    case BIND_ACTION_FONT_SIZE_DOWN:
        term_font_size_decrease(term);
        return true;

    case BIND_ACTION_FONT_SIZE_RESET:
        term_font_size_reset(term);
        return true;

    case BIND_ACTION_SPAWN_TERMINAL:
        term_spawn_new(term);
        return true;

    case BIND_ACTION_MINIMIZE:
        wayl_win_minimize(term->window);
        return true;

    case BIND_ACTION_MAXIMIZE:
        if (term->window->is_fullscreen)
            wayl_win_set_fullscreen(term->window, false);
        wayl_win_set_maximized(term->window, !term->window->is_maximized);
        return true;

    case BIND_ACTION_FULLSCREEN:
        wayl_win_set_fullscreen(term->window, !term->window->is_fullscreen);
        return true;

    case BIND_ACTION_PIPE_SCROLLBACK:
        if (term->grid == &term->alt)
            break;
        /* FALLTHROUGH */
    case BIND_ACTION_PIPE_VIEW:
    case BIND_ACTION_PIPE_SELECTED:
    case BIND_ACTION_PIPE_COMMAND_OUTPUT: {
        if (binding->aux->type != BINDING_AUX_PIPE)
            return true;

        struct pipe_context *ctx = NULL;

        int pipe_fd[2] = {-1, -1};
        int stdout_fd = -1;
        int stderr_fd = -1;

        char *text = NULL;
        size_t len = 0;

        if (pipe(pipe_fd) < 0) {
            LOG_ERRNO("failed to create pipe");
            goto pipe_err;
        }

        stdout_fd = open("/dev/null", O_WRONLY);
        stderr_fd = open("/dev/null", O_WRONLY);

        if (stdout_fd < 0 || stderr_fd < 0) {
            LOG_ERRNO("failed to open /dev/null");
            goto pipe_err;
        }

        bool success;
        switch (action) {
        case BIND_ACTION_PIPE_SCROLLBACK:
            success = term_scrollback_to_text(term, &text, &len);
            break;

        case BIND_ACTION_PIPE_VIEW:
            success = term_view_to_text(term, &text, &len);
            break;

        case BIND_ACTION_PIPE_SELECTED:
            text = selection_to_text(term);
            success = text != NULL;
            len = text != NULL ? strlen(text) : 0;
            break;

        case BIND_ACTION_PIPE_COMMAND_OUTPUT:
            success = term_command_output_to_text(term, &text, &len);
            break;

        default:
            BUG("Unhandled action type");
            success = false;
            break;
        }

        if (!success)
            goto pipe_err;

        /* Make write-end non-blocking; required by the FDM */
        {
            int flags = fcntl(pipe_fd[1], F_GETFL);
            if (flags < 0 ||
                fcntl(pipe_fd[1], F_SETFL, flags | O_NONBLOCK) < 0)
            {
                LOG_ERRNO("failed to make write-end of pipe non-blocking");
                goto pipe_err;
            }
        }

        /* Make sure write-end is closed on exec() - or the spawned
         * program may not terminate*/
        {
            int flags = fcntl(pipe_fd[1], F_GETFD);
            if (flags < 0 ||
                fcntl(pipe_fd[1], F_SETFD, flags | FD_CLOEXEC) < 0)
            {
                LOG_ERRNO("failed to set FD_CLOEXEC on writeend of pipe");
                goto pipe_err;
            }
        }

        if (spawn(term->reaper, term->cwd, binding->aux->pipe.args,
                  pipe_fd[0], stdout_fd, stderr_fd, NULL, NULL, NULL) < 0)
            goto pipe_err;

        /* Close read end */
        close(pipe_fd[0]);

        ctx = xmalloc(sizeof(*ctx));
        *ctx = (struct pipe_context){
            .text = text,
            .left = len,
        };

        /* Asynchronously write the output to the pipe */
        if (!fdm_add(term->fdm, pipe_fd[1], EPOLLOUT, &fdm_write_pipe, ctx))
            goto pipe_err;

        return true;

        pipe_err:
        if (stdout_fd >= 0)
            close(stdout_fd);
        if (stderr_fd >= 0)
            close(stderr_fd);
        if (pipe_fd[0] >= 0)
            close(pipe_fd[0]);
        if (pipe_fd[1] >= 0)
            close(pipe_fd[1]);
        free(text);
        free(ctx);
        return true;
    }

    case BIND_ACTION_SHOW_URLS_COPY:
    case BIND_ACTION_SHOW_URLS_LAUNCH:
    case BIND_ACTION_SHOW_URLS_PERSISTENT: {
        xassert(!urls_mode_is_active(term));

        enum url_action url_action =
            action == BIND_ACTION_SHOW_URLS_COPY ? URL_ACTION_COPY :
            action == BIND_ACTION_SHOW_URLS_LAUNCH ? URL_ACTION_LAUNCH :
            URL_ACTION_PERSISTENT;

        urls_collect(term, url_action, &term->conf->url.preg, true, &term->url.list);
        urls_assign_key_combos(term->conf, &term->url.list);
        urls_render(term, &term->conf->url.launch);
        return true;
    }

    case BIND_ACTION_TEXT_BINDING:
        xassert(binding->aux->type == BINDING_AUX_TEXT);
        term_to_slave(term, binding->aux->text.data, binding->aux->text.len);
        return true;

    case BIND_ACTION_PROMPT_PREV: {
        if (term->grid != &term->normal)
            return false;

        struct grid *grid = term->grid;
        const int sb_start =
            grid_sb_start_ignore_uninitialized(grid, term->rows);

        /* Check each row from current view-1 (that is, the first
         * currently not visible row), up to, and including, the
         * scrollback start */
        for (int r_sb_rel =
                 grid_row_abs_to_sb_precalc_sb_start(
                     grid, sb_start, grid->view) - 1;
             r_sb_rel >= 0; r_sb_rel--)
        {
            const int r_abs =
                grid_row_sb_to_abs_precalc_sb_start(grid, sb_start, r_sb_rel);

            const struct row *row = grid->rows[r_abs];
            xassert(row != NULL);

            if (!row->shell_integration.prompt_marker)
                continue;

            grid->view = r_abs;
            term_damage_view(term);
            render_refresh(term);
            break;
        }

        return true;
    }

    case BIND_ACTION_PROMPT_NEXT: {
        if (term->grid != &term->normal)
            return false;

        struct grid *grid = term->grid;
        const int num_rows = grid->num_rows;

        if (grid->view == grid->offset) {
            /* Already at the bottom */
            return true;
        }

        /* Check each row from view+1, to the bottom of the scrollback */
        for (int r_abs = (grid->view + 1) & (num_rows - 1);
             ;
             r_abs = (r_abs + 1) & (num_rows - 1))
        {
            const struct row *row = grid->rows[r_abs];
            xassert(row != NULL);

            if (!row->shell_integration.prompt_marker) {
                if (r_abs == grid->offset + term->rows - 1) {
                    /* We've reached the bottom of the scrollback */
                    break;
                }
                continue;
            }

            int sb_start = grid_sb_start_ignore_uninitialized(grid, term->rows);
            int ofs_sb_rel =
                grid_row_abs_to_sb_precalc_sb_start(grid, sb_start, grid->offset);
            int new_view_sb_rel =
                grid_row_abs_to_sb_precalc_sb_start(grid, sb_start, r_abs);

            new_view_sb_rel = min(ofs_sb_rel, new_view_sb_rel);
            grid->view = grid_row_sb_to_abs_precalc_sb_start(
                grid, sb_start, new_view_sb_rel);

            term_damage_view(term);
            render_refresh(term);
            break;
        }

        return true;
    }

    case BIND_ACTION_UNICODE_INPUT:
        unicode_mode_activate(term);
        return true;

    case BIND_ACTION_QUIT:
        term_shutdown(term);
        return true;

    case BIND_ACTION_REGEX_LAUNCH:
    case BIND_ACTION_REGEX_COPY:
        if (binding->aux->type != BINDING_AUX_REGEX)
            return true;

        tll_foreach(term->conf->custom_regexes, it) {
            const struct custom_regex *regex = &it->item;

            if (streq(regex->name, binding->aux->regex_name)) {
                xassert(!urls_mode_is_active(term));

                enum url_action url_action = action == BIND_ACTION_REGEX_LAUNCH
                    ? URL_ACTION_LAUNCH : URL_ACTION_COPY;

                if (regex->regex == NULL) {
                    LOG_ERR("regex:%s has no regex defined", regex->name);
                    return true;
                }
                if (url_action == URL_ACTION_LAUNCH && regex->launch.argv.args == NULL) {
                    LOG_ERR("regex:%s has no launch command defined", regex->name);
                    return true;
                }

                urls_collect(term, url_action, &regex->preg, false, &term->url.list);
                urls_assign_key_combos(term->conf, &term->url.list);
                urls_render(term, &regex->launch);
                return true;
            }
        }

        LOG_ERR(
            "no regex section named '%s' defined in the configuration",
            binding->aux->regex_name);

        return true;

    case BIND_ACTION_THEME_SWITCH_DARK:
        term_theme_switch_to_dark(term);
        return true;

    case BIND_ACTION_THEME_SWITCH_LIGHT:
        term_theme_switch_to_light(term);
        return true;

    case BIND_ACTION_THEME_TOGGLE:
        term_theme_toggle(term);
        return true;

    case BIND_ACTION_SELECT_BEGIN:
        selection_start(
            term, seat->mouse.col, seat->mouse.row, SELECTION_CHAR_WISE, false);
        return true;

    case BIND_ACTION_SELECT_BEGIN_BLOCK:
        selection_start(
            term, seat->mouse.col, seat->mouse.row, SELECTION_BLOCK, false);
        return true;

    case BIND_ACTION_SELECT_EXTEND:
        selection_extend(
            seat, term, seat->mouse.col, seat->mouse.row, term->selection.kind);
        return true;

    case BIND_ACTION_SELECT_EXTEND_CHAR_WISE:
        if (term->selection.kind != SELECTION_BLOCK) {
            selection_extend(
                seat, term, seat->mouse.col, seat->mouse.row, SELECTION_CHAR_WISE);
            return true;
        }
        return false;

    case BIND_ACTION_SELECT_WORD:
        selection_start(
            term, seat->mouse.col, seat->mouse.row, SELECTION_WORD_WISE, false);
        return true;

    case BIND_ACTION_SELECT_WORD_WS:
        selection_start(
            term, seat->mouse.col, seat->mouse.row, SELECTION_WORD_WISE, true);
        return true;

    case BIND_ACTION_SELECT_QUOTE:
        selection_start(
            term, seat->mouse.col, seat->mouse.row, SELECTION_QUOTE_WISE, false);
        return true;

    case BIND_ACTION_SELECT_ROW:
        selection_start(
            term, seat->mouse.col, seat->mouse.row, SELECTION_LINE_WISE, false);
        return true;

    case BIND_ACTION_COUNT:
        BUG("Invalid action type");
        return false;
    }

    return false;
}

bool
input_execute_action(struct seat *seat, struct terminal *term,
                     enum bind_action_normal action)
{
    const struct key_binding binding = {.action = action};
    return execute_binding(
        seat, term, &binding, seat->kbd.serial != 0 ? seat->kbd.serial : 1, 1);
}

void
input_keymap(struct seat *seat, const char *map_str, size_t size)
{
    LOG_DBG("keymap: seat=%s (size=%zu)", seat->name, size);

    struct wayland *wayl = seat->wayl;

    /*
     * Free old keymap state
     */

    if (seat->kbd.xkb_keymap != NULL) {
        xkb_keymap_unref(seat->kbd.xkb_keymap);
        seat->kbd.xkb_keymap = NULL;
    }
    if (seat->kbd.xkb_state != NULL) {
        xkb_state_unref(seat->kbd.xkb_state);
        seat->kbd.xkb_state = NULL;
    }

    key_binding_unload_keymap(wayl->key_binding_manager, seat);

    if (seat->kbd.xkb != NULL) {
        seat->kbd.xkb_keymap = xkb_keymap_new_from_buffer(
            seat->kbd.xkb, map_str, size, XKB_KEYMAP_FORMAT_TEXT_V1,
            XKB_KEYMAP_COMPILE_NO_FLAGS);

        if (seat->kbd.xkb_keymap == NULL)
            LOG_ERR("%s: failed to compile keymap", seat->name);
    }

    if (seat->kbd.xkb_keymap != NULL) {
        seat->kbd.xkb_state = xkb_state_new(seat->kbd.xkb_keymap);

        seat->kbd.mod_shift = xkb_keymap_mod_get_index(seat->kbd.xkb_keymap, XKB_MOD_NAME_SHIFT);
        seat->kbd.mod_alt = xkb_keymap_mod_get_index(seat->kbd.xkb_keymap, XKB_MOD_NAME_ALT) ;
        seat->kbd.mod_ctrl = xkb_keymap_mod_get_index(seat->kbd.xkb_keymap, XKB_MOD_NAME_CTRL);
        seat->kbd.mod_super = xkb_keymap_mod_get_index(seat->kbd.xkb_keymap, XKB_MOD_NAME_LOGO);
        seat->kbd.mod_caps = xkb_keymap_mod_get_index(seat->kbd.xkb_keymap, XKB_MOD_NAME_CAPS);
        seat->kbd.mod_num = xkb_keymap_mod_get_index(seat->kbd.xkb_keymap, XKB_MOD_NAME_NUM);

        /* Significant modifiers in the legacy keyboard protocol */
        seat->kbd.legacy_significant = 0;
        if (seat->kbd.mod_shift != XKB_MOD_INVALID)
            seat->kbd.legacy_significant |= 1 << seat->kbd.mod_shift;
        if (seat->kbd.mod_alt != XKB_MOD_INVALID)
            seat->kbd.legacy_significant |= 1 << seat->kbd.mod_alt;
        if (seat->kbd.mod_ctrl != XKB_MOD_INVALID)
            seat->kbd.legacy_significant |= 1 << seat->kbd.mod_ctrl;
        if (seat->kbd.mod_super != XKB_MOD_INVALID)
            seat->kbd.legacy_significant |= 1 << seat->kbd.mod_super;

        /* Significant modifiers in the kitty keyboard protocol */
        seat->kbd.kitty_significant = seat->kbd.legacy_significant;
        if (seat->kbd.mod_caps != XKB_MOD_INVALID)
            seat->kbd.kitty_significant |= 1 << seat->kbd.mod_caps;
        if (seat->kbd.mod_num != XKB_MOD_INVALID)
            seat->kbd.kitty_significant |= 1 << seat->kbd.mod_num;

        /*
         * Mask of all "virtual" modifiers. Our keymaps map these
         * 1:1 to real modifiers, and we only ever set the real ones,
         * but keep the mask for consistency with upstream.
         */
        {
            xkb_mod_index_t alt = xkb_keymap_mod_get_index(seat->kbd.xkb_keymap, XKB_VMOD_NAME_ALT);
            xkb_mod_index_t meta = xkb_keymap_mod_get_index(seat->kbd.xkb_keymap, XKB_VMOD_NAME_META);
            xkb_mod_index_t super = xkb_keymap_mod_get_index(seat->kbd.xkb_keymap, XKB_VMOD_NAME_SUPER);
            xkb_mod_index_t hyper = xkb_keymap_mod_get_index(seat->kbd.xkb_keymap, XKB_VMOD_NAME_HYPER);
            xkb_mod_index_t num_lock = xkb_keymap_mod_get_index(seat->kbd.xkb_keymap, XKB_VMOD_NAME_NUM);
            xkb_mod_index_t scroll_lock = xkb_keymap_mod_get_index(seat->kbd.xkb_keymap, XKB_VMOD_NAME_SCROLL);
            xkb_mod_index_t level_three = xkb_keymap_mod_get_index(seat->kbd.xkb_keymap, XKB_VMOD_NAME_LEVEL3);
            xkb_mod_index_t level_five = xkb_keymap_mod_get_index(seat->kbd.xkb_keymap, XKB_VMOD_NAME_LEVEL5);

            xkb_mod_index_t ignore = 0;

            if (alt != XKB_MOD_INVALID) ignore |= 1 << alt;
            if (meta != XKB_MOD_INVALID) ignore |= 1 << meta;
            if (super != XKB_MOD_INVALID) ignore |= 1 << super;
            if (hyper != XKB_MOD_INVALID) ignore |= 1 << hyper;
            if (num_lock != XKB_MOD_INVALID) ignore |= 1 << num_lock;
            if (scroll_lock != XKB_MOD_INVALID) ignore |= 1 << scroll_lock;
            if (level_three != XKB_MOD_INVALID) ignore |= 1 << level_three;
            if (level_five != XKB_MOD_INVALID) ignore |= 1 << level_five;

            seat->kbd.virtual_modifiers = ignore;
        }

        seat->kbd.key_arrow_up = xkb_keymap_key_by_name(seat->kbd.xkb_keymap, "UP");
        seat->kbd.key_arrow_down = xkb_keymap_key_by_name(seat->kbd.xkb_keymap, "DOWN");
    }

    key_binding_load_keymap(wayl->key_binding_manager, seat);
}

void
input_kbd_enter(struct seat *seat, struct terminal *term, uint32_t serial)
{
    xassert(serial != 0);

    LOG_DBG("%s: keyboard_enter: term=%p, serial=%u",
            seat->name, (void *)term, serial);

    term_kbd_focus_in(term);
    seat->kbd_focus = term;
    seat->kbd.serial = serial;
}

/*
 * macOS generates key repeat events itself (honoring the user's
 * keyboard settings); see input_key().
 */
static bool
start_repeater(struct seat *seat, uint32_t key)
{
    return true;
}

static bool
stop_repeater(struct seat *seat, uint32_t key)
{
    return true;
}

void
input_kbd_leave(struct seat *seat)
{
    LOG_DBG("%s: keyboard_leave", seat->name);

    struct terminal *old_focused = seat->kbd_focus;
    seat->kbd_focus = NULL;

    stop_repeater(seat, -1);
    seat->kbd.shift = false;
    seat->kbd.alt = false;
    seat->kbd.ctrl = false;
    seat->kbd.super = false;

    if (seat->kbd.xkb_compose_state != NULL)
        xkb_compose_state_reset(seat->kbd.xkb_compose_state);

    if (seat->kbd.xkb_state != NULL && seat->kbd.xkb_keymap != NULL) {
        const xkb_layout_index_t layout_count = xkb_keymap_num_layouts(seat->kbd.xkb_keymap);

        for (xkb_layout_index_t i = 0; i < layout_count; i++)
            xkb_state_update_mask(seat->kbd.xkb_state, 0, 0, 0, i, i, i);
    }

    if (old_focused != NULL) {
        seat->pointer.hidden = false;
        term_xcursor_update_for_seat(old_focused, seat);
        term_kbd_focus_out(old_focused);
    }
}

static const struct key_data *
keymap_data_for_sym(xkb_keysym_t sym, size_t *count)
{
    switch (sym) {
    case XKB_KEY_Escape:       *count = ALEN(key_escape);       return key_escape;
    case XKB_KEY_Return:       *count = ALEN(key_return);       return key_return;
    case XKB_KEY_ISO_Left_Tab: *count = ALEN(key_iso_left_tab); return key_iso_left_tab;
    case XKB_KEY_Tab:          *count = ALEN(key_tab);          return key_tab;
    case XKB_KEY_BackSpace:    *count = ALEN(key_backspace);    return key_backspace;
    case XKB_KEY_Up:           *count = ALEN(key_up);           return key_up;
    case XKB_KEY_Down:         *count = ALEN(key_down);         return key_down;
    case XKB_KEY_Right:        *count = ALEN(key_right);        return key_right;
    case XKB_KEY_Left:         *count = ALEN(key_left);         return key_left;
    case XKB_KEY_Home:         *count = ALEN(key_home);         return key_home;
    case XKB_KEY_End:          *count = ALEN(key_end);          return key_end;
    case XKB_KEY_Insert:       *count = ALEN(key_insert);       return key_insert;
    case XKB_KEY_Delete:       *count = ALEN(key_delete);       return key_delete;
    case XKB_KEY_Page_Up:      *count = ALEN(key_pageup);       return key_pageup;
    case XKB_KEY_Page_Down:    *count = ALEN(key_pagedown);     return key_pagedown;
    case XKB_KEY_F1:           *count = ALEN(key_f1);           return key_f1;
    case XKB_KEY_F2:           *count = ALEN(key_f2);           return key_f2;
    case XKB_KEY_F3:           *count = ALEN(key_f3);           return key_f3;
    case XKB_KEY_F4:           *count = ALEN(key_f4);           return key_f4;
    case XKB_KEY_F5:           *count = ALEN(key_f5);           return key_f5;
    case XKB_KEY_F6:           *count = ALEN(key_f6);           return key_f6;
    case XKB_KEY_F7:           *count = ALEN(key_f7);           return key_f7;
    case XKB_KEY_F8:           *count = ALEN(key_f8);           return key_f8;
    case XKB_KEY_F9:           *count = ALEN(key_f9);           return key_f9;
    case XKB_KEY_F10:          *count = ALEN(key_f10);          return key_f10;
    case XKB_KEY_F11:          *count = ALEN(key_f11);          return key_f11;
    case XKB_KEY_F12:          *count = ALEN(key_f12);          return key_f12;
    case XKB_KEY_F13:          *count = ALEN(key_f13);          return key_f13;
    case XKB_KEY_F14:          *count = ALEN(key_f14);          return key_f14;
    case XKB_KEY_F15:          *count = ALEN(key_f15);          return key_f15;
    case XKB_KEY_F16:          *count = ALEN(key_f16);          return key_f16;
    case XKB_KEY_F17:          *count = ALEN(key_f17);          return key_f17;
    case XKB_KEY_F18:          *count = ALEN(key_f18);          return key_f18;
    case XKB_KEY_F19:          *count = ALEN(key_f19);          return key_f19;
    case XKB_KEY_F20:          *count = ALEN(key_f20);          return key_f20;
    case XKB_KEY_F21:          *count = ALEN(key_f21);          return key_f21;
    case XKB_KEY_F22:          *count = ALEN(key_f22);          return key_f22;
    case XKB_KEY_F23:          *count = ALEN(key_f23);          return key_f23;
    case XKB_KEY_F24:          *count = ALEN(key_f24);          return key_f24;
    case XKB_KEY_F25:          *count = ALEN(key_f25);          return key_f25;
    case XKB_KEY_F26:          *count = ALEN(key_f26);          return key_f26;
    case XKB_KEY_F27:          *count = ALEN(key_f27);          return key_f27;
    case XKB_KEY_F28:          *count = ALEN(key_f28);          return key_f28;
    case XKB_KEY_F29:          *count = ALEN(key_f29);          return key_f29;
    case XKB_KEY_F30:          *count = ALEN(key_f30);          return key_f30;
    case XKB_KEY_F31:          *count = ALEN(key_f31);          return key_f31;
    case XKB_KEY_F32:          *count = ALEN(key_f32);          return key_f32;
    case XKB_KEY_F33:          *count = ALEN(key_f33);          return key_f33;
    case XKB_KEY_F34:          *count = ALEN(key_f34);          return key_f34;
    case XKB_KEY_F35:          *count = ALEN(key_f35);          return key_f35;
    case XKB_KEY_KP_Up:        *count = ALEN(key_kp_up);        return key_kp_up;
    case XKB_KEY_KP_Down:      *count = ALEN(key_kp_down);      return key_kp_down;
    case XKB_KEY_KP_Right:     *count = ALEN(key_kp_right);     return key_kp_right;
    case XKB_KEY_KP_Left:      *count = ALEN(key_kp_left);      return key_kp_left;
    case XKB_KEY_KP_Begin:     *count = ALEN(key_kp_begin);     return key_kp_begin;
    case XKB_KEY_KP_Home:      *count = ALEN(key_kp_home);      return key_kp_home;
    case XKB_KEY_KP_End:       *count = ALEN(key_kp_end);       return key_kp_end;
    case XKB_KEY_KP_Insert:    *count = ALEN(key_kp_insert);    return key_kp_insert;
    case XKB_KEY_KP_Delete:    *count = ALEN(key_kp_delete);    return key_kp_delete;
    case XKB_KEY_KP_Page_Up:   *count = ALEN(key_kp_pageup);    return key_kp_pageup;
    case XKB_KEY_KP_Page_Down: *count = ALEN(key_kp_pagedown);  return key_kp_pagedown;
    case XKB_KEY_KP_Enter:     *count = ALEN(key_kp_enter);     return key_kp_enter;
    case XKB_KEY_KP_Divide:    *count = ALEN(key_kp_divide);    return key_kp_divide;
    case XKB_KEY_KP_Multiply:  *count = ALEN(key_kp_multiply);  return key_kp_multiply;
    case XKB_KEY_KP_Subtract:  *count = ALEN(key_kp_subtract);  return key_kp_subtract;
    case XKB_KEY_KP_Add:       *count = ALEN(key_kp_add);       return key_kp_add;
    case XKB_KEY_KP_Separator: *count = ALEN(key_kp_separator); return key_kp_separator;
    case XKB_KEY_KP_Decimal:   *count = ALEN(key_kp_decimal);   return key_kp_decimal;
    case XKB_KEY_KP_0:         *count = ALEN(key_kp_0);         return key_kp_0;
    case XKB_KEY_KP_1:         *count = ALEN(key_kp_1);         return key_kp_1;
    case XKB_KEY_KP_2:         *count = ALEN(key_kp_2);         return key_kp_2;
    case XKB_KEY_KP_3:         *count = ALEN(key_kp_3);         return key_kp_3;
    case XKB_KEY_KP_4:         *count = ALEN(key_kp_4);         return key_kp_4;
    case XKB_KEY_KP_5:         *count = ALEN(key_kp_5);         return key_kp_5;
    case XKB_KEY_KP_6:         *count = ALEN(key_kp_6);         return key_kp_6;
    case XKB_KEY_KP_7:         *count = ALEN(key_kp_7);         return key_kp_7;
    case XKB_KEY_KP_8:         *count = ALEN(key_kp_8);         return key_kp_8;
    case XKB_KEY_KP_9:         *count = ALEN(key_kp_9);         return key_kp_9;
    }

    return NULL;
}

static const struct key_data *
keymap_lookup(struct terminal *term, xkb_keysym_t sym, enum modifier mods)
{
    size_t count;
    const struct key_data *info = keymap_data_for_sym(sym, &count);

    if (info == NULL)
        return NULL;

    const enum cursor_keys cursor_keys_mode = term->cursor_keys_mode;
    const enum keypad_keys keypad_keys_mode
        = term->num_lock_modifier ? KEYPAD_NUMERICAL : term->keypad_keys_mode;

    LOG_DBG("keypad mode: %d", keypad_keys_mode);

    for (size_t j = 0; j < count; j++) {
        enum modifier modifiers = info[j].modifiers;

        if (modifiers & MOD_MODIFY_OTHER_KEYS_STATE1) {
            if (term->modify_other_keys_2)
                continue;
            modifiers &= ~MOD_MODIFY_OTHER_KEYS_STATE1;
        }
        if (modifiers & MOD_MODIFY_OTHER_KEYS_STATE2) {
            if (!term->modify_other_keys_2)
                continue;
            modifiers &= ~MOD_MODIFY_OTHER_KEYS_STATE2;
        }

        if (modifiers != MOD_ANY && modifiers != mods)
            continue;

        if (info[j].cursor_keys_mode != CURSOR_KEYS_DONTCARE &&
            info[j].cursor_keys_mode != cursor_keys_mode)
            continue;

        if (info[j].keypad_keys_mode != KEYPAD_DONTCARE &&
            info[j].keypad_keys_mode != keypad_keys_mode)
            continue;

        return &info[j];
    }

    return NULL;
}

UNITTEST
{
    struct terminal term = {
        .num_lock_modifier = false,
        .keypad_keys_mode = KEYPAD_NUMERICAL,
        .cursor_keys_mode = CURSOR_KEYS_NORMAL,
    };

    const struct key_data *info = keymap_lookup(&term, XKB_KEY_ISO_Left_Tab, MOD_SHIFT | MOD_CTRL);
    xassert(info != NULL);
    xassert(streq(info->seq, "\033[27;6;9~"));
}

UNITTEST
{
    struct terminal term = {
        .modify_other_keys_2 = false,
    };

    const struct key_data *info = keymap_lookup(&term, XKB_KEY_Return, MOD_ALT);
    xassert(info != NULL);
    xassert(streq(info->seq, "\033\r"));

    term.modify_other_keys_2 = true;
    info = keymap_lookup(&term, XKB_KEY_Return, MOD_ALT);
    xassert(info != NULL);
    xassert(streq(info->seq, "\033[27;3;13~"));
}

void
get_current_modifiers(const struct seat *seat,
                      xkb_mod_mask_t *effective,
                      xkb_mod_mask_t *consumed, uint32_t key,
                      bool filter_locked)
{
    if (unlikely(seat->kbd.xkb_state == NULL)) {
        if (effective != NULL)
            *effective = 0;
        if (consumed != NULL)
            *consumed = 0;
    }

    else {
        const xkb_mod_mask_t locked =
            xkb_state_serialize_mods(seat->kbd.xkb_state, XKB_STATE_MODS_LOCKED);

        if (effective != NULL) {
            *effective = xkb_state_serialize_mods(
                seat->kbd.xkb_state, XKB_STATE_MODS_EFFECTIVE);

            if (filter_locked)
                *effective &= ~locked;
        }

        if (consumed != NULL) {
            *consumed = xkb_state_key_get_consumed_mods2(
                seat->kbd.xkb_state, key, XKB_CONSUMED_MODE_XKB);

            if (filter_locked)
                *consumed &= ~locked;
        }
    }
}

struct kbd_ctx {
    xkb_layout_index_t layout;
    xkb_keycode_t key;
    xkb_keysym_t sym;

    struct {
        const xkb_keysym_t *syms;
        size_t count;
    } level0_syms;

    xkb_mod_mask_t mods;
    xkb_mod_mask_t consumed;

    struct {
        const uint8_t *buf;
        size_t count;
    } utf8;
    uint32_t *utf32;

    enum xkb_compose_status compose_status;
    enum key_state key_state;
};

static bool
legacy_kbd_protocol(struct seat *seat, struct terminal *term,
                    const struct kbd_ctx *ctx)
{
    if (ctx->key_state != KEY_STATE_PRESSED)
        return false;
    if (ctx->compose_status == XKB_COMPOSE_COMPOSING)
        return false;

    enum modifier keymap_mods = MOD_NONE;
    keymap_mods |= seat->kbd.shift ? MOD_SHIFT : MOD_NONE;
    keymap_mods |= seat->kbd.alt ? MOD_ALT : MOD_NONE;
    keymap_mods |= seat->kbd.ctrl ? MOD_CTRL : MOD_NONE;
    keymap_mods |= seat->kbd.super ? MOD_META : MOD_NONE;

    const xkb_keysym_t sym = ctx->sym;
    const size_t count = ctx->utf8.count;
    const uint8_t *const utf8 = ctx->utf8.buf;

    const struct key_data *keymap = keymap_lookup(term, sym, keymap_mods);
    if (keymap != NULL) {
        term_to_slave(term, keymap->seq, strlen(keymap->seq));
        return true;
    }

    if (count == 0)
        return false;

#define is_control_key(x) ((x) >= 0x40 && (x) <= 0x7f)
#define IS_CTRL(x) ((x) < 0x20 || ((x) >= 0x7f && (x) <= 0x9f))

    //LOG_DBG("term->modify_other_keys=%d, count=%zu, is_ctrl=%d (utf8=0x%02x), sym=%d",
    //term->modify_other_keys_2, count, IS_CTRL(utf8[0]), utf8[0], sym);

    bool ctrl_is_in_effect = (keymap_mods & MOD_CTRL) != 0;
    bool ctrl_seq = is_control_key(sym) || (count == 1 && IS_CTRL(utf8[0]));

    bool modify_other_keys2_in_effect = false;

    if (term->modify_other_keys_2) {
        /*
         * Try to mimic XTerm's behavior, when holding shift:
         *
         *   - if other modifiers are pressed (e.g. Alt), emit a CSI escape
         *   - upper-case symbols A-Z are encoded as an CSI escape
         *   - other upper-case symbols (e.g 'Ö') or emitted as is
         *   - non-upper cased symbols are _mostly_ emitted as is (foot
         *     always emits as is)
         *
         * Examples (assuming Swedish layout):
         *   - Shift-a ('A') emits a CSI
         *   - Shift-, (';') emits ';'
         *   - Shift-Alt-, (Alt-;) emits a CSI
         *   - Shift-ö ('Ö') emits 'Ö'
         */

        /* Any modifiers, besides shift active? */
        const xkb_mod_mask_t shift_mask = 1 << seat->kbd.mod_shift;
        if ((ctx->mods & ~shift_mask & seat->kbd.legacy_significant) != 0)
            modify_other_keys2_in_effect = true;

        else {
            const xkb_layout_index_t layout_idx = xkb_state_key_get_layout(
                seat->kbd.xkb_state, ctx->key);

            /*
             * Get pressed key's base symbol.
             *   - for 'A' (shift-a), that's 'a'
             *   - for ';' (shift-,), that's ','
             */
            const xkb_keysym_t *base_syms = NULL;
            size_t base_count = xkb_keymap_key_get_syms_by_level(
                seat->kbd.xkb_keymap, ctx->key, layout_idx, 0, &base_syms);

            /* Check if base symbol(s) is a-z. If so, emit CSI */
            const xkb_keysym_t lower_cased_sym = xkb_keysym_to_lower(ctx->sym);
            for (size_t i = 0; i < base_count; i++) {
                const xkb_keysym_t s = base_syms[i];
                if (lower_cased_sym == s && s >= XKB_KEY_a && s <= XKB_KEY_z) {
                    modify_other_keys2_in_effect = true;
                    break;
                }
            }
        }
    }

    if (keymap_mods != MOD_NONE && (modify_other_keys2_in_effect ||
                                    (ctrl_is_in_effect && !ctrl_seq)))
    {
        static const int mod_param_map[32] = {
            [MOD_SHIFT] = 2,
            [MOD_ALT] = 3,
            [MOD_SHIFT | MOD_ALT] = 4,
            [MOD_CTRL] = 5,
            [MOD_SHIFT | MOD_CTRL] = 6,
            [MOD_ALT | MOD_CTRL] = 7,
            [MOD_SHIFT | MOD_ALT | MOD_CTRL] = 8,
            [MOD_META] = 9,
            [MOD_META | MOD_SHIFT] = 10,
            [MOD_META | MOD_ALT] = 11,
            [MOD_META | MOD_SHIFT | MOD_ALT] = 12,
            [MOD_META | MOD_CTRL] = 13,
            [MOD_META | MOD_SHIFT | MOD_CTRL] = 14,
            [MOD_META | MOD_ALT | MOD_CTRL] = 15,
            [MOD_META | MOD_SHIFT | MOD_ALT | MOD_CTRL] = 16,
        };

        xassert(keymap_mods < ALEN(mod_param_map));
        int modify_param = mod_param_map[keymap_mods];
        xassert(modify_param != 0);

        char reply[32];
        size_t n = xsnprintf(reply, sizeof(reply), "\x1b[27;%d;%d~", modify_param, sym);
        term_to_slave(term, reply, n);
    }

    else if (keymap_mods & MOD_ALT) {
        /*
         * When the alt modifier is pressed, we do one out of three things:
         *
         *  1. we prefix the output bytes with ESC
         *  2. we set the 8:th bit in the output byte
         *  3. we ignore the alt modifier
         *
         * #1 is configured with \E[?1036, and is on by default
         *
         * If #1 has been disabled, we use #2, *if* it's a single byte
         * we're emitting. Since this is a UTF-8 terminal, we then
         * UTF8-encode the 8-bit character. #2 is configured with
         * \E[?1034, and is on by default.
         *
         * Lastly, if both #1 and #2 have been disabled, the alt
         * modifier is ignored.
         */
        if (term->meta.esc_prefix) {
            term_to_slave(term, "\x1b", 1);
            term_to_slave(term, utf8, count);
        }

        else if (term->meta.eight_bit && count == 1) {
            const char32_t wc = 0x80 | utf8[0];

            char utf8_meta[MB_CUR_MAX];
            size_t chars = c32rtomb(utf8_meta, wc, &(mbstate_t){0});

            if (chars != (size_t)-1)
                term_to_slave(term, utf8_meta, chars);
            else
                term_to_slave(term, utf8, count);
        }

        else {
            /* Alt ignored */
            term_to_slave(term, utf8, count);
        }
    } else
        term_to_slave(term, utf8, count);

    return true;
}

UNITTEST
{
    /* Verify the kitty keymap is sorted */
    xkb_keysym_t last = 0;
    for (size_t i = 0; i < ALEN(kitty_keymap); i++) {
        const struct kitty_key_data *e = &kitty_keymap[i];
        xassert(e->sym > last);
        last = e->sym;
    }
}

static int
kitty_search(const void *_key, const void *_e)
{
    const xkb_keysym_t *key = _key;
    const struct kitty_key_data *e = _e;
    return *key - e->sym;
}

static bool
kitty_kbd_protocol(struct seat *seat, struct terminal *term,
                   const struct kbd_ctx *ctx)
{
    const bool repeating = seat->kbd.repeat.dont_re_repeat;
    const bool pressed = ctx->key_state == KEY_STATE_PRESSED && !repeating;
    const bool released = ctx->key_state == KEY_STATE_RELEASED;
    const bool composing = ctx->compose_status == XKB_COMPOSE_COMPOSING;
    const bool composed = ctx->compose_status == XKB_COMPOSE_COMPOSED;

    const enum kitty_kbd_flags flags =
        term->grid->kitty_kbd.flags[term->grid->kitty_kbd.idx];

    const bool disambiguate = flags & KITTY_KBD_DISAMBIGUATE;
    const bool report_events = flags & KITTY_KBD_REPORT_EVENT;
    const bool report_alternate = flags & KITTY_KBD_REPORT_ALTERNATE;
    const bool report_all_as_escapes = flags & KITTY_KBD_REPORT_ALL;

    if (!report_events && released)
        return false;

    /* TODO: should we even bother with this, or just say it's not supported? */
    if (!disambiguate && !report_all_as_escapes && pressed)
        return legacy_kbd_protocol(seat, term, ctx);

    const xkb_keysym_t sym = ctx->sym;
    const uint32_t *utf32 = ctx->utf32;
    const uint8_t *const utf8 = ctx->utf8.buf;
    const size_t count = ctx->utf8.count;

    /* Lookup sym in the pre-defined keysym table */
    const struct kitty_key_data *info = bsearch(
        &sym, kitty_keymap, ALEN(kitty_keymap), sizeof(kitty_keymap[0]),
        &kitty_search);
    xassert(info == NULL || info->sym == sym);

    xkb_mod_mask_t mods = 0;
    xkb_mod_mask_t locked = 0;
    xkb_mod_mask_t consumed = ctx->consumed;

    if (info != NULL && info->is_modifier) {
        /*
         * Special-case modifier keys.
         *
         * Normally, the "current" XKB state reflects the state
         * *before* the current key event. In other words, the
         * modifiers for key events that affect the modifier state
         * (e.g. one of the control keys, or shift keys etc) does
         * *not* include the key itself.
         *
         * Put another way, if you press "control", the modifier set
         * is empty in the key press event, but contains "ctrl" in the
         * release event.
         *
         * The kitty protocol mandates the modifier list contain the
         * key itself, in *both* the press and release event.
         *
         * We handle this by updating the XKB state to *include* the
         * current key, retrieve the set of modifiers (including the
         * set of consumed modifiers), and then revert the XKB update.
         */
        xkb_state_update_key(
            seat->kbd.xkb_state, ctx->key, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);

        get_current_modifiers(seat, &mods, NULL, 0, false);

        locked = xkb_state_serialize_mods(
            seat->kbd.xkb_state, XKB_STATE_MODS_LOCKED);
        consumed = xkb_state_key_get_consumed_mods2(
            seat->kbd.xkb_state, ctx->key, XKB_CONSUMED_MODE_XKB);

#if 0
        /*
         * TODO: according to the XKB docs, state updates should
         * always be in pairs: each press should be followed by a
         * release. However, doing this just breaks the xkb state.
         *
         * *Not* pairing the above press/release with a corresponding
         * release/press appears to do exactly what we want.
         */
        xkb_state_update_key(
            seat->kbd.xkb_state, ctx->key, pressed ? XKB_KEY_UP : XKB_KEY_DOWN);
#endif
    } else {
        /* Same as ctx->mods, but *without* filtering locked modifiers */
        get_current_modifiers(seat, &mods, NULL, 0, false);
        locked = xkb_state_serialize_mods(
            seat->kbd.xkb_state, XKB_STATE_MODS_LOCKED);
    }

    mods &= seat->kbd.kitty_significant;
    consumed &= seat->kbd.kitty_significant;

    /*
     * A note on locked modifiers; they *are* a part of the protocol,
     * and *should* be included in the modifier set reported in the
     * key event.
     *
     * However, *only* if the key would result in a CSIu *without* the
     * locked modifier being enabled
     *
     * Translated: if *another* modifier is active, or if
     * report-all-keys-as-escapes is enabled, then we include the
     * locked modifier in the key event.
     *
     * But, if the key event would result in plain text output without
     * the locked modifier, then we "ignore" the locked modifier and
     * emit plain text anyway.
     */

    bool is_text = count > 0 && utf32 != NULL && (mods & ~locked & ~consumed) == 0;
    for (size_t i = 0; utf32[i] != U'\0'; i++) {
        if (!isc32print(utf32[i])) {
            is_text = false;
            break;
        }
    }

    const bool report_associated_text =
        (flags & KITTY_KBD_REPORT_ASSOCIATED) && is_text && !released;

    if (composing) {
        /* We never emit anything while composing, *except* modifiers
         * (and only in report-all-keys-as-escape-codes mode) */
        if (info != NULL && info->is_modifier)
            goto emit_escapes;

        return false;
    }

    if (report_all_as_escapes)
        goto emit_escapes;

    if ((mods & ~locked & ~consumed) == 0) {
        switch (sym) {
        case XKB_KEY_Return:
            if (!released)
                term_to_slave(term, "\r", 1);
            return  true;

        case XKB_KEY_BackSpace:
            if (!released)
                term_to_slave(term, "\x7f", 1);
            return true;

        case XKB_KEY_Tab:
            if (!released)
                term_to_slave(term, "\t", 1);
            return true;
        }
    }

    /* Plain-text without modifiers, or commposed text, is emitted as-is */
    if (is_text && !released) {
        term_to_slave(term, utf8, count);
        return true;
    }

emit_escapes:
    ;
    unsigned int encoded_mods = 0;
    if (seat->kbd.mod_shift != XKB_MOD_INVALID)
        encoded_mods |= mods & (1 << seat->kbd.mod_shift) ? (1 << 0) : 0;
    if (seat->kbd.mod_alt != XKB_MOD_INVALID)
        encoded_mods |= mods & (1 << seat->kbd.mod_alt)   ? (1 << 1) : 0;
    if (seat->kbd.mod_ctrl != XKB_MOD_INVALID)
        encoded_mods |= mods & (1 << seat->kbd.mod_ctrl)  ? (1 << 2) : 0;
    if (seat->kbd.mod_super != XKB_MOD_INVALID)
        encoded_mods |= mods & (1 << seat->kbd.mod_super)  ? (1 << 3) : 0;
    if (seat->kbd.mod_caps != XKB_MOD_INVALID)
        encoded_mods |= mods & (1 << seat->kbd.mod_caps)  ? (1 << 6) : 0;
    if (seat->kbd.mod_num != XKB_MOD_INVALID)
        encoded_mods |= mods & (1 << seat->kbd.mod_num)   ? (1 << 7) : 0;
    encoded_mods++;

    /*
     * Figure out the main, alternate and base key codes.
     *
     * The main key is the unshifted version of the generated symbol,
     * the alternate key is the shifted version, and base is the
     * (unshifted) key assuming the default layout.
     *
     * For example, the user presses shift+a, then:
     *   - unshifted = 'a'
     *   - shifted = 'A'
     *   - base = 'a'
     *
     * Base will in many cases be the same as the unshifted key, but
     * may differ if the active keyboard layout is non-ASCII (examples
     * would be russian, or alternative layouts like neo etc).
     *
     * The shifted key is what we get from XKB, i.e. the resulting key
     * from all active modifiers, plus the pressed key.
     */
    int unshifted = -1, shifted = -1, base = -1;
    char final;

    if (info != NULL) {
        /* Use code from lookup table (cursor keys, enter, tab etc)*/
        if (!info->is_modifier || report_all_as_escapes) {
            shifted = info->key;
            final = info->final;
        }
    } else {
        /* Use keysym (typically its Unicode codepoint value) */

        if (composed)
            shifted = utf32[0];  /* TODO: what if there are multiple codepoints? */
        else
            shifted = xkb_keysym_to_utf32(sym);

        final = 'u';
    }

    if (shifted <= 0)
        return false;

    /* Base layout key. I.e the symbol the pressed key produces in
     * the base/default layout (layout idx 0) */
    const xkb_keysym_t *base_syms;
    int base_sym_count = xkb_keymap_key_get_syms_by_level(
        seat->kbd.xkb_keymap, ctx->key, 0, 0, &base_syms);

    if (base_sym_count > 0)
        base = xkb_keysym_to_utf32(base_syms[0]);

    /*
     * If the keysym is shifted, use its unshifted codepoint
     * instead. In other words, ctrl+a and ctrl+shift+a should both
     * use the same value for 'key' (97 - i.a. 'a').
     *
     * However, don't do this if a non-significant modifier was used
     * to generate the symbol. This is needed since we cannot encode
     * non-significant modifiers, and thus the "extra" modifier(s)
     * would get lost.
     *
     * Example:
     *
     * the Swedish layout has '2', QUOTATION MARK ("double quote"),
     * '@', and '²' on the same key. '2' is the base symbol.
     *
     * Shift+2 results in QUOTATION MARK
     * AltGr+2 results in '@'
     * AltGr+Shift+2 results in '²'
     *
     * The kitty kbd protocol can't encode AltGr. So, if we always
     * used the base symbol ('2'), Alt+Shift+2 would result in the
     * same escape sequence as AltGr+Alt+Shift+2.
     *
     * (yes, this matches what kitty does, as of 0.23.1)
     */
    const bool use_level0_sym =
        (ctx->mods & ~seat->kbd.kitty_significant) == 0 && ctx->level0_syms.count > 0;

    unshifted = use_level0_sym ? xkb_keysym_to_utf32(ctx->level0_syms.syms[0]) : 0;

    xassert(encoded_mods >= 1);

    char event[4];
    if (report_events /*&& !pressed*/) {
        /* Note: this deviates slightly from Kitty, which omits the
         * ":1" subparameter for key press events */
        event[0] = ':';
        event[1] = '0' + (pressed ? 1 : repeating ? 2 : 3);
        event[2] = '\0';
    } else
        event[0] = '\0';

    char buf[128], *p = buf;
    size_t left = sizeof(buf);
    size_t bytes;

    const int key = unshifted > 0 && isc32print(unshifted) && !composed ? unshifted : shifted;
    const int alternate = shifted;

    if (final == 'u' || final == '~') {
        bytes = snprintf(p, left, "\x1b[%u", key);
        p += bytes; left -= bytes;

        if (report_alternate) {
            bool emit_alternate = alternate > 0 && alternate != key;
            bool emit_base = base > 0 && base != key && base != alternate && isc32print(base);

            if (emit_alternate) {
                bytes = snprintf(p, left, ":%u", alternate);
                p += bytes; left -= bytes;
            }

            if (emit_base) {
                bytes = snprintf(
                    p, left, "%s:%u", !emit_alternate ? ":" : "", base);
                p += bytes; left -= bytes;
            }
        }

        bool emit_mods = encoded_mods > 1 || event[0] != '\0';

        if (emit_mods) {
            bytes = snprintf(p, left, ";%u%s", encoded_mods, event);
            p += bytes; left -= bytes;
        }

        if (report_associated_text) {
            bytes = snprintf(p, left, "%s;%u", !emit_mods ? ";" : "", utf32[0]);
            p += bytes; left -= bytes;

            /* Additional text codepoints */
            if (utf32[0] != U'\0') {
                for (size_t i = 1; utf32[i] != U'\0'; i++) {
                    bytes = snprintf(p, left, ":%u", utf32[i]);
                    p += bytes; left -= bytes;
                }
            }
        }

        bytes = snprintf(p, left, "%c", final);
        p += bytes; left -= bytes;
    } else {
        if (encoded_mods > 1 || event[0] != '\0') {
            bytes = snprintf(p, left, "\x1b[1;%u%s%c", encoded_mods, event, final);
            p += bytes; left -= bytes;
        } else {
            bytes = snprintf(p, left, "\x1b[%c", final);
            p += bytes; left -= bytes;
        }
    }

    return term_to_slave(term, buf, sizeof(buf) - left);
}

/* Copied from libxkbcommon (internal function) */
static bool
keysym_is_modifier(xkb_keysym_t keysym)
{
    return
        (keysym >= XKB_KEY_Shift_L && keysym <= XKB_KEY_Hyper_R) ||
        /* libX11 only goes up to XKB_KEY_ISO_Level5_Lock. */
        (keysym >= XKB_KEY_ISO_Lock && keysym <= XKB_KEY_ISO_Last_Group_Lock) ||
        keysym == XKB_KEY_Mode_switch ||
        keysym == XKB_KEY_Num_Lock;
}

#if defined(_DEBUG)
static void
modifier_string(xkb_mod_mask_t mods, size_t sz, char mod_str[static sz], const struct seat *seat)
{
    if (sz == 0)
        return;

    mod_str[0] = '\0';

    for (size_t i = 0; i < sizeof(xkb_mod_mask_t) * 8; i++) {
        if (!(mods & (1u << i)))
            continue;

        strcat(mod_str, xkb_keymap_mod_get_name(seat->kbd.xkb_keymap, i));
        strcat(mod_str, "+");
    }

    if (mod_str[0] != '\0') {
        /* Strip the last '+' */
        mod_str[strlen(mod_str) - 1] = '\0';
    }

    if (mod_str[0] == '\0') {
        strcpy(mod_str, "<none>");
    }
}
#endif

static void
key_press_release(struct seat *seat, struct terminal *term, uint32_t serial,
                  uint32_t key, uint32_t state)
{
    xassert(serial != 0);

    seat->kbd.serial = serial;
    if (seat->kbd.xkb == NULL ||
        seat->kbd.xkb_keymap == NULL ||
        seat->kbd.xkb_state == NULL)
    {
        return;
    }

    const bool pressed = state == KEY_STATE_PRESSED;
    //const bool repeated = pressed && seat->kbd.repeat.dont_re_repeat;
    const bool released = state == KEY_STATE_RELEASED;

    if (released)
        stop_repeater(seat, key);

    if (pressed)
        seat->kbd.last_shortcut_sym = XKB_KEYSYM_MAX + 1;

    bool should_repeat =
        pressed && xkb_keymap_key_repeats(seat->kbd.xkb_keymap, key);

    xkb_keysym_t sym = xkb_state_key_get_one_sym(seat->kbd.xkb_state, key);

    if (pressed && term->conf->mouse.hide_when_typing && !keysym_is_modifier(sym)) {
        seat->pointer.hidden = true;
        term_xcursor_update_for_seat(term, seat);
    }

    enum xkb_compose_status compose_status = XKB_COMPOSE_NOTHING;

    if (seat->kbd.xkb_compose_state != NULL) {
        if (pressed)
            xkb_compose_state_feed(seat->kbd.xkb_compose_state, sym);
        compose_status = xkb_compose_state_get_status(
            seat->kbd.xkb_compose_state);
    }

    const bool composed = compose_status == XKB_COMPOSE_COMPOSED;

    xkb_mod_mask_t mods, consumed;
    get_current_modifiers(seat, &mods, &consumed, key, true);

    xkb_layout_index_t layout_idx =
        xkb_state_key_get_layout(seat->kbd.xkb_state, key);

    const xkb_keysym_t *raw_syms = NULL;
    size_t raw_count = xkb_keymap_key_get_syms_by_level(
        seat->kbd.xkb_keymap, key, layout_idx, 0, &raw_syms);

    const struct key_binding_set *bindings = key_binding_for(
        seat->wayl->key_binding_manager, term->conf, seat);
    xassert(bindings != NULL);

    if (term->unicode_mode.active) {
        if (pressed)
            unicode_mode_input(seat, term, sym);
        return;
    }

    else if (term->is_searching) {
        if (pressed) {
            if (should_repeat)
                start_repeater(seat, key);

            search_input(
                seat, term, bindings, key, sym, mods, consumed,
                raw_syms, raw_count, serial);
        }
        return;
    }

    else  if (urls_mode_is_active(term)) {
        if (pressed) {
            if (should_repeat)
                start_repeater(seat, key);

            urls_input(
                seat, term, bindings, key, sym, mods, consumed,
                raw_syms, raw_count, serial);
        }
        return;
    }

#if defined(_DEBUG) && defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
    char sym_name[100];
    xkb_keysym_get_name(sym, sym_name, sizeof(sym_name));

    char active_mods_str[256] = {0};
    char consumed_mods_str[256] = {0};
    char locked_mods_str[256] = {0};

    const xkb_mod_mask_t locked =
        xkb_state_serialize_mods(seat->kbd.xkb_state, XKB_STATE_MODS_LOCKED);

    modifier_string(mods, sizeof(active_mods_str), active_mods_str, seat);
    modifier_string(consumed, sizeof(consumed_mods_str), consumed_mods_str, seat);
    modifier_string(locked, sizeof(locked_mods_str), locked_mods_str, seat);

    LOG_DBG("%s: %s (%u/0x%x), seat=%s, term=%p, serial=%u, "
            "mods=%s (0x%08x), consumed=%s (0x%08x), locked=%s (0x%08x), "
            "repeats=%d",
            pressed ? "pressed" : "released", sym_name, sym, sym,
            seat->name, (void *)term, serial,
            active_mods_str, mods, consumed_mods_str, consumed,
            locked_mods_str, locked, should_repeat);
#endif

    /*
     * User configurable bindings
     */
    if (pressed) {
        /* Match untranslated symbols */
        tll_foreach(bindings->key, it) {
            const struct key_binding *bind = &it->item;

            if (bind->mods != mods || bind->mods == 0)
                continue;

            for (size_t i = 0; i < raw_count; i++) {
                if (bind->k.sym == raw_syms[i] &&
                    execute_binding(seat, term, bind, serial, 1))
                {
                    seat->kbd.last_shortcut_sym = sym;
                    goto maybe_repeat;
                }
            }
        }

        /* Match translated symbol */
        tll_foreach(bindings->key, it) {
            const struct key_binding *bind = &it->item;

            if (bind->k.sym == sym &&
                bind->mods == (mods & ~consumed) &&
                execute_binding(seat, term, bind, serial, 1))
            {
                seat->kbd.last_shortcut_sym = sym;
                goto maybe_repeat;
            }
        }

        /* Match raw key code */
        tll_foreach(bindings->key, it) {
            const struct key_binding *bind = &it->item;

            if (bind->mods != mods || bind->mods == 0)
                continue;

            tll_foreach(bind->k.key_codes, code) {
                if (code->item == key &&
                    execute_binding(seat, term, bind, serial, 1))
                {
                    seat->kbd.last_shortcut_sym = sym;
                    goto maybe_repeat;
                }
            }
        }
    }

    if (released && seat->kbd.last_shortcut_sym == sym) {
        /*
         * Don't process a release event, if it corresponds to a
         * triggered shortcut.
         *
         * 1. If we consumed a key (press) event, we shouldn't emit an
         *    escape for its release event.
         * 2. Ignoring the incorrectness of doing so; this also caused
         *    us to reset the viewport.
         *
         * Background: if the kitty keyboard protocol was enabled,
         * then the viewport was instantly reset to the bottom, after
         * scrolling up.
         */
        //seat->kbd.last_shortcut_sym = XKB_KEYSYM_MAX + 1;
        goto maybe_repeat;
    }

    /*
     * Keys generating escape sequences
     */


    /*
     * Compose, and maybe emit "normal" character
     */

    xassert(seat->kbd.xkb_compose_state != NULL || !composed);

    if (compose_status == XKB_COMPOSE_CANCELLED)
        goto maybe_repeat;

    int count = composed
        ? xkb_compose_state_get_utf8(seat->kbd.xkb_compose_state, NULL, 0)
        : xkb_state_key_get_utf8(seat->kbd.xkb_state, key, NULL, 0);

    /* Buffer for translated key. Use a static buffer in most cases,
     * and use a malloc:ed buffer when necessary */
    uint8_t buf[32];
    uint8_t *utf8 = count < sizeof(buf) ? buf : xmalloc(count + 1);
    uint32_t *utf32 = NULL;

    if (composed) {
        xkb_compose_state_get_utf8(
            seat->kbd.xkb_compose_state, (char *)utf8, count + 1);

        if (count > 0)
            utf32 = ambstoc32((const char *)utf8);
    } else {
        xkb_state_key_get_utf8(
            seat->kbd.xkb_state, key, (char *)utf8, count + 1);

        utf32 = xcalloc(2, sizeof(utf32[0]));
        utf32[0] = xkb_state_key_get_utf32(seat->kbd.xkb_state, key);
    }

    struct kbd_ctx ctx = {
        .layout = layout_idx,
        .key = key,
        .sym = sym,
        .level0_syms = {
            .syms = raw_syms,
            .count = raw_count,
        },
        .mods = mods,
        .consumed = consumed,
        .utf8 = {
            .buf = utf8,
            .count = count,
        },
        .utf32 = utf32,
        .compose_status = compose_status,
        .key_state = state,
    };

    bool handled = term->grid->kitty_kbd.flags[term->grid->kitty_kbd.idx] != 0
        ? kitty_kbd_protocol(seat, term, &ctx)
        : legacy_kbd_protocol(seat, term, &ctx);

    if (composed && released)
        xkb_compose_state_reset(seat->kbd.xkb_compose_state);

    if (utf8 != buf)
        free(utf8);

    if (handled && !keysym_is_modifier(sym)) {
        term_reset_view(term);
        selection_cancel(term);
    }

    free(utf32);

maybe_repeat:
    clock_gettime(CLOCK_MONOTONIC, &term->render.input_time);

    if (should_repeat)
        start_repeater(seat, key);
}

void
input_key(struct seat *seat, uint32_t serial, uint32_t key,
          enum key_state state, bool repeat)
{
    if (seat->kbd_focus == NULL)
        return;

    /* Lets the kitty keyboard protocol report it as a repeat */
    seat->kbd.repeat.dont_re_repeat = repeat;
    key_press_release(seat, seat->kbd_focus, serial, key, state);
    seat->kbd.repeat.dont_re_repeat = false;
}

void
input_modifiers(struct seat *seat, uint32_t mods_depressed,
                uint32_t mods_latched, uint32_t mods_locked, uint32_t group)
{
    mods_depressed &= ~seat->kbd.virtual_modifiers;
    mods_latched &= ~seat->kbd.virtual_modifiers;
    mods_locked &= ~seat->kbd.virtual_modifiers;

#if defined(_DEBUG)
    char depressed[256];
    char latched[256];
    char locked[256];

    modifier_string(mods_depressed, sizeof(depressed), depressed, seat);
    modifier_string(mods_latched, sizeof(latched), latched, seat);
    modifier_string(mods_locked, sizeof(locked), locked, seat);

    LOG_DBG(
        "modifiers: depressed=%s (0x%x), latched=%s (0x%x), locked=%s (0x%x), "
        "group=%u",
        depressed, mods_depressed, latched, mods_latched, locked, mods_locked,
        group);
#endif

    if (seat->kbd.xkb_state != NULL) {
        xkb_state_update_mask(
            seat->kbd.xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);

        /* Update state of modifiers we're interested in for e.g mouse events */
        seat->kbd.shift = seat->kbd.mod_shift != XKB_MOD_INVALID
            ? xkb_state_mod_index_is_active(
                seat->kbd.xkb_state, seat->kbd.mod_shift, XKB_STATE_MODS_EFFECTIVE)
            : false;
        seat->kbd.alt = seat->kbd.mod_alt != XKB_MOD_INVALID
            ? xkb_state_mod_index_is_active(
                seat->kbd.xkb_state, seat->kbd.mod_alt, XKB_STATE_MODS_EFFECTIVE)
            : false;
        seat->kbd.ctrl = seat->kbd.mod_ctrl != XKB_MOD_INVALID
            ? xkb_state_mod_index_is_active(
                seat->kbd.xkb_state, seat->kbd.mod_ctrl, XKB_STATE_MODS_EFFECTIVE)
            : false;
        seat->kbd.super = seat->kbd.mod_super != XKB_MOD_INVALID
            ? xkb_state_mod_index_is_active(
                seat->kbd.xkb_state, seat->kbd.mod_super, XKB_STATE_MODS_EFFECTIVE)
            : false;
    }

    if (seat->kbd_focus && seat->kbd_focus->active_surface == TERM_SURF_GRID)
        term_xcursor_update_for_seat(seat->kbd_focus, seat);
}

UNITTEST
{
    int chan[2];
    xassert(pipe2(chan, O_CLOEXEC) == 0);

    xassert(chan[0] >= 0);
    xassert(chan[1] >= 0);

    struct config conf = {0};
    struct grid grid = {0};

    struct terminal term = {
        .conf = &conf,
        .grid = &grid,
        .ptmx = chan[1],
        .selection = {
            .coords = {
                .start = {-1, -1},
                .end = {-1, -1},
            },
            .auto_scroll = {
                .fd = -1,
             },
        },
    };

    struct key_binding_manager *key_binding_manager = key_binding_manager_new();

    struct wayland wayl = {
        .key_binding_manager = key_binding_manager,
        .terms = tll_init(),
    };

    struct seat seat = {
        .wayl = &wayl,
        .name = "unittest",
    };

    tll_push_back(wayl.terms, &term);
    term.wl = &wayl;

    /* There are no XKB data files on macOS; the tests below are skipped */
    seat.kbd.xkb = xkb_context_new(
        XKB_CONTEXT_NO_DEFAULT_INCLUDES | XKB_CONTEXT_NO_ENVIRONMENT_NAMES);
    xassert(seat.kbd.xkb != NULL);
    xkb_context_set_log_level(seat.kbd.xkb, XKB_LOG_LEVEL_CRITICAL);

    grid.kitty_kbd.flags[0] = KITTY_KBD_DISAMBIGUATE | KITTY_KBD_REPORT_ALTERNATE;

    /* Swedish keymap */
    {
        seat.kbd.xkb_keymap = xkb_keymap_new_from_names(
            seat.kbd.xkb, &(struct xkb_rule_names){.layout = "se"}, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (seat.kbd.xkb_keymap == NULL) {
            /* Skip test */
            goto no_keymap;
        }

        seat.kbd.xkb_state = xkb_state_new(seat.kbd.xkb_keymap);
        xassert(seat.kbd.xkb_state != NULL);

        seat.kbd.mod_shift = xkb_keymap_mod_get_index(seat.kbd.xkb_keymap, XKB_MOD_NAME_SHIFT);
        seat.kbd.mod_alt = xkb_keymap_mod_get_index(seat.kbd.xkb_keymap, XKB_MOD_NAME_ALT) ;
        seat.kbd.mod_ctrl = xkb_keymap_mod_get_index(seat.kbd.xkb_keymap, XKB_MOD_NAME_CTRL);
        seat.kbd.mod_super = xkb_keymap_mod_get_index(seat.kbd.xkb_keymap, XKB_MOD_NAME_LOGO);
        seat.kbd.mod_caps = xkb_keymap_mod_get_index(seat.kbd.xkb_keymap, XKB_MOD_NAME_CAPS);
        seat.kbd.mod_num = xkb_keymap_mod_get_index(seat.kbd.xkb_keymap, XKB_MOD_NAME_NUM);

        /* Significant modifiers in the legacy keyboard protocol */
        seat.kbd.legacy_significant = 0;
        if (seat.kbd.mod_shift != XKB_MOD_INVALID)
            seat.kbd.legacy_significant |= 1 << seat.kbd.mod_shift;
        if (seat.kbd.mod_alt != XKB_MOD_INVALID)
            seat.kbd.legacy_significant |= 1 << seat.kbd.mod_alt;
        if (seat.kbd.mod_ctrl != XKB_MOD_INVALID)
            seat.kbd.legacy_significant |= 1 << seat.kbd.mod_ctrl;
        if (seat.kbd.mod_super != XKB_MOD_INVALID)
            seat.kbd.legacy_significant |= 1 << seat.kbd.mod_super;

        /* Significant modifiers in the kitty keyboard protocol */
        seat.kbd.kitty_significant = seat.kbd.legacy_significant;
        if (seat.kbd.mod_caps != XKB_MOD_INVALID)
            seat.kbd.kitty_significant |= 1 << seat.kbd.mod_caps;
        if (seat.kbd.mod_num != XKB_MOD_INVALID)
            seat.kbd.kitty_significant |= 1 << seat.kbd.mod_num;

        key_binding_new_for_seat(key_binding_manager, &seat);
        key_binding_load_keymap(key_binding_manager, &seat);

        {
            xkb_mod_mask_t mods = 1u << seat.kbd.mod_shift | 1u << seat.kbd.mod_ctrl;
            input_modifiers(&seat, mods, 0, 0, 0);
            key_press_release(&seat, &term, 1337, KEY_A + 8, KEY_STATE_PRESSED);

            char escape[64] = {0};
            ssize_t count = read(chan[0], escape, sizeof(escape));

            /* key: 97 = 'a', alternate: 65 = 'A', base: N/A, mods: 6 = ctrl+shift */
            const char expected_ctrl_shift_a[] = "\033[97:65;6u";
            xassert(count == strlen(expected_ctrl_shift_a));
            xassert(streq(escape, expected_ctrl_shift_a));

            key_press_release(&seat, &term, 1337, KEY_A + 8, KEY_STATE_RELEASED);
        }

        {
            xkb_mod_mask_t mods = 1u << seat.kbd.mod_shift | 1u << seat.kbd.mod_alt;
            input_modifiers(&seat, mods, 0, 0, 0);
            key_press_release(&seat, &term, 1337, KEY_2 + 8, KEY_STATE_PRESSED);

            char escape[64] = {0};
            ssize_t count = read(chan[0], escape, sizeof(escape));

            /* key;. 50 = '2', alternate: 34 = '"', base: N/A, 4 = alt+shift */
            const char expected_alt_shift_2[] = "\033[50:34;4u";
            xassert(count == strlen(expected_alt_shift_2));
            xassert(streq(escape, expected_alt_shift_2));

            key_press_release(&seat, &term, 1337, KEY_2 + 8, KEY_STATE_RELEASED);
        }

        {
            xkb_mod_index_t alt_gr = xkb_keymap_mod_get_index(seat.kbd.xkb_keymap, "Mod5");
            xassert(alt_gr != XKB_MOD_INVALID);

            xkb_mod_mask_t mods = 1u << seat.kbd.mod_shift | 1u << seat.kbd.mod_alt | 1u << alt_gr;
            input_modifiers(&seat, mods, 0, 0, 0);
            key_press_release(&seat, &term, 1337, KEY_2 + 8, KEY_STATE_PRESSED);

            char escape[64] = {0};
            ssize_t count = read(chan[0], escape, sizeof(escape));

            /* key; 178 = '²', alternate: N/A, base: 50 = '2', 4 = alt+shift (AltGr not part of the protocol) */
            const char expected_altgr_alt_shift_2[] = "\033[178::50;4u";
            xassert(count == strlen(expected_altgr_alt_shift_2));
            xassert(streq(escape, expected_altgr_alt_shift_2));

            key_press_release(&seat, &term, 1337, KEY_2 + 8, KEY_STATE_RELEASED);
        }

        {
            xkb_mod_mask_t mods = 1u << seat.kbd.mod_alt;
            input_modifiers(&seat, mods, 0, 0, 0);
            key_press_release(&seat, &term, 1337, KEY_BACKSPACE + 8, KEY_STATE_PRESSED);

            char escape[64] = {0};
            ssize_t count = read(chan[0], escape, sizeof(escape));

            /* key; 127 = <backspace>, alternate: N/A, base: N/A, 3 = alt */
            const char expected_alt_backspace[] = "\033[127;3u";
            xassert(count == strlen(expected_alt_backspace));
            xassert(streq(escape, expected_alt_backspace));

            key_press_release(&seat, &term, 1337, KEY_BACKSPACE + 8, KEY_STATE_RELEASED);
        }

        {
            xkb_mod_mask_t mods = 1u << seat.kbd.mod_ctrl;
            input_modifiers(&seat, mods, 0, 0, 0);
            key_press_release(&seat, &term, 1337, KEY_ENTER + 8, KEY_STATE_PRESSED);

            char escape[64] = {0};
            ssize_t count = read(chan[0], escape, sizeof(escape));

            /* key; 13 = <enter>, alternate: N/A, base: N/A, 5 = ctrl */
            const char expected_ctrl_enter[] = "\033[13;5u";
            xassert(count == strlen(expected_ctrl_enter));
            xassert(streq(escape, expected_ctrl_enter));

            key_press_release(&seat, &term, 1337, KEY_ENTER + 8, KEY_STATE_RELEASED);
        }

        {
            xkb_mod_mask_t mods = 1u << seat.kbd.mod_ctrl;
            input_modifiers(&seat, mods, 0, 0, 0);
            key_press_release(&seat, &term, 1337, KEY_TAB + 8, KEY_STATE_PRESSED);

            char escape[64] = {0};
            ssize_t count = read(chan[0], escape, sizeof(escape));

            /* key; 9 = <tab>, alternate: N/A, base: N/A, 5 = ctrl */
            const char expected_ctrl_tab[] = "\033[9;5u";
            xassert(count == strlen(expected_ctrl_tab));
            xassert(streq(escape, expected_ctrl_tab));

            key_press_release(&seat, &term, 1337, KEY_TAB + 8, KEY_STATE_RELEASED);
        }

        {
            xkb_mod_mask_t mods = 1u << seat.kbd.mod_ctrl | 1u << seat.kbd.mod_shift;
            input_modifiers(&seat, mods, 0, 0, 0);
            key_press_release(&seat, &term, 1337, KEY_LEFT + 8, KEY_STATE_PRESSED);

            char escape[64] = {0};
            ssize_t count = read(chan[0], escape, sizeof(escape));

            const char expected_ctrl_shift_left[] = "\033[1;6D";
            xassert(count == strlen(expected_ctrl_shift_left));
            xassert(streq(escape, expected_ctrl_shift_left));

            key_press_release(&seat, &term, 1337, KEY_LEFT + 8, KEY_STATE_RELEASED);
        }
        key_binding_unload_keymap(key_binding_manager, &seat);
        key_binding_remove_seat(key_binding_manager, &seat);

        xkb_state_unref(seat.kbd.xkb_state);
        xkb_keymap_unref(seat.kbd.xkb_keymap);

        seat.kbd.xkb_state = NULL;
        seat.kbd.xkb_keymap = NULL;
    }

    /* de(neo) keymap */
    {
        seat.kbd.xkb_keymap = xkb_keymap_new_from_names(
            seat.kbd.xkb, &(struct xkb_rule_names){.layout = "us,de(neo)"},
            XKB_KEYMAP_COMPILE_NO_FLAGS);

        if (seat.kbd.xkb_keymap == NULL) {
            /* Skip test */
            goto no_keymap;
        }

        seat.kbd.xkb_state = xkb_state_new(seat.kbd.xkb_keymap);
        xassert(seat.kbd.xkb_state != NULL);

        seat.kbd.mod_shift = xkb_keymap_mod_get_index(seat.kbd.xkb_keymap, XKB_MOD_NAME_SHIFT);
        seat.kbd.mod_alt = xkb_keymap_mod_get_index(seat.kbd.xkb_keymap, XKB_MOD_NAME_ALT) ;
        seat.kbd.mod_ctrl = xkb_keymap_mod_get_index(seat.kbd.xkb_keymap, XKB_MOD_NAME_CTRL);
        seat.kbd.mod_super = xkb_keymap_mod_get_index(seat.kbd.xkb_keymap, XKB_MOD_NAME_LOGO);
        seat.kbd.mod_caps = xkb_keymap_mod_get_index(seat.kbd.xkb_keymap, XKB_MOD_NAME_CAPS);
        seat.kbd.mod_num = xkb_keymap_mod_get_index(seat.kbd.xkb_keymap, XKB_MOD_NAME_NUM);

        /* Significant modifiers in the legacy keyboard protocol */
        seat.kbd.legacy_significant = 0;
        if (seat.kbd.mod_shift != XKB_MOD_INVALID)
            seat.kbd.legacy_significant |= 1 << seat.kbd.mod_shift;
        if (seat.kbd.mod_alt != XKB_MOD_INVALID)
            seat.kbd.legacy_significant |= 1 << seat.kbd.mod_alt;
        if (seat.kbd.mod_ctrl != XKB_MOD_INVALID)
            seat.kbd.legacy_significant |= 1 << seat.kbd.mod_ctrl;
        if (seat.kbd.mod_super != XKB_MOD_INVALID)
            seat.kbd.legacy_significant |= 1 << seat.kbd.mod_super;

        /* Significant modifiers in the kitty keyboard protocol */
        seat.kbd.kitty_significant = seat.kbd.legacy_significant;
        if (seat.kbd.mod_caps != XKB_MOD_INVALID)
            seat.kbd.kitty_significant |= 1 << seat.kbd.mod_caps;
        if (seat.kbd.mod_num != XKB_MOD_INVALID)
            seat.kbd.kitty_significant |= 1 << seat.kbd.mod_num;

        key_binding_new_for_seat(key_binding_manager, &seat);
        key_binding_load_keymap(key_binding_manager, &seat);

        {
            /*
             * In the de(neo) layout, the Y key generates 'k'. This
             * means we should get a key+alternate that indicates 'k',
             * but a base key that is 'y'.
             */
            xkb_mod_mask_t mods = 1u << seat.kbd.mod_shift | 1u << seat.kbd.mod_alt;
            input_modifiers(&seat, mods, 0, 0, 1);
            key_press_release(&seat, &term, 1337, KEY_Y + 8, KEY_STATE_PRESSED);

            char escape[64] = {0};
            ssize_t count = read(chan[0], escape, sizeof(escape));

            /* key: 107 = 'k', alternate: 75 = 'K', base: 121 = 'y', mods: 4 = alt+shift */
            const char expected_alt_shift_y[] = "\033[107:75:121;4u";
            xassert(count == strlen(expected_alt_shift_y));
            xassert(streq(escape, expected_alt_shift_y));

            key_press_release(&seat, &term, 1337, KEY_Y + 8, KEY_STATE_RELEASED);
        }

        key_binding_unload_keymap(key_binding_manager, &seat);
        key_binding_remove_seat(key_binding_manager, &seat);

        xkb_state_unref(seat.kbd.xkb_state);
        xkb_keymap_unref(seat.kbd.xkb_keymap);

        seat.kbd.xkb_state = NULL;
        seat.kbd.xkb_keymap = NULL;
    }

    /* us(intl) keymap */
    {
        seat.kbd.xkb_keymap = xkb_keymap_new_from_names(
            seat.kbd.xkb, &(struct xkb_rule_names){.layout = "us", .variant = "intl"},
            XKB_KEYMAP_COMPILE_NO_FLAGS);

        if (seat.kbd.xkb_keymap == NULL) {
            /* Skip test */
            goto no_keymap;
        }

        seat.kbd.xkb_state = xkb_state_new(seat.kbd.xkb_keymap);
        xassert(seat.kbd.xkb_state != NULL);

        seat.kbd.xkb_compose_table = xkb_compose_table_new_from_locale(
            seat.kbd.xkb, setlocale(LC_CTYPE, NULL), XKB_COMPOSE_COMPILE_NO_FLAGS);
        if (seat.kbd.xkb_compose_table == NULL)
            goto no_keymap;

        seat.kbd.xkb_compose_state = xkb_compose_state_new(
            seat.kbd.xkb_compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
        if (seat.kbd.xkb_compose_state == NULL) {
            xkb_compose_table_unref(seat.kbd.xkb_compose_table);
            goto no_keymap;
        }

        seat.kbd.mod_shift = xkb_keymap_mod_get_index(seat.kbd.xkb_keymap, XKB_MOD_NAME_SHIFT);
        seat.kbd.mod_alt = xkb_keymap_mod_get_index(seat.kbd.xkb_keymap, XKB_MOD_NAME_ALT) ;
        seat.kbd.mod_ctrl = xkb_keymap_mod_get_index(seat.kbd.xkb_keymap, XKB_MOD_NAME_CTRL);
        seat.kbd.mod_super = xkb_keymap_mod_get_index(seat.kbd.xkb_keymap, XKB_MOD_NAME_LOGO);
        seat.kbd.mod_caps = xkb_keymap_mod_get_index(seat.kbd.xkb_keymap, XKB_MOD_NAME_CAPS);
        seat.kbd.mod_num = xkb_keymap_mod_get_index(seat.kbd.xkb_keymap, XKB_MOD_NAME_NUM);

        /* Significant modifiers in the legacy keyboard protocol */
        seat.kbd.legacy_significant = 0;
        if (seat.kbd.mod_shift != XKB_MOD_INVALID)
            seat.kbd.legacy_significant |= 1 << seat.kbd.mod_shift;
        if (seat.kbd.mod_alt != XKB_MOD_INVALID)
            seat.kbd.legacy_significant |= 1 << seat.kbd.mod_alt;
        if (seat.kbd.mod_ctrl != XKB_MOD_INVALID)
            seat.kbd.legacy_significant |= 1 << seat.kbd.mod_ctrl;
        if (seat.kbd.mod_super != XKB_MOD_INVALID)
            seat.kbd.legacy_significant |= 1 << seat.kbd.mod_super;

        /* Significant modifiers in the kitty keyboard protocol */
        seat.kbd.kitty_significant = seat.kbd.legacy_significant;
        if (seat.kbd.mod_caps != XKB_MOD_INVALID)
            seat.kbd.kitty_significant |= 1 << seat.kbd.mod_caps;
        if (seat.kbd.mod_num != XKB_MOD_INVALID)
            seat.kbd.kitty_significant |= 1 << seat.kbd.mod_num;

        key_binding_new_for_seat(key_binding_manager, &seat);
        key_binding_load_keymap(key_binding_manager, &seat);

        {
            /*
             * Test the compose sequence "shift+', shift+space"
             *
             * Should result in a double quote, but a regression
             * caused it to instead emit a space. See #1987
             *
             * Note: "shift+', space" also results in a double quote,
             * but never regressed to a space.
             */
            grid.kitty_kbd.flags[0] = KITTY_KBD_DISAMBIGUATE;
            xkb_compose_state_reset(seat.kbd.xkb_compose_state);

            xkb_mod_mask_t mods = 1u << seat.kbd.mod_shift;
            input_modifiers(&seat, mods, 0, 0, 1);

            key_press_release(&seat, &term, 1337, KEY_APOSTROPHE + 8, KEY_STATE_PRESSED);
            key_press_release(&seat, &term, 1337, KEY_APOSTROPHE + 8, KEY_STATE_RELEASED);

            key_press_release(&seat, &term, 1337, KEY_SPACE + 8, KEY_STATE_PRESSED);

            char escape[64] = {0};
            ssize_t count = read(chan[0], escape, sizeof(escape));

            /* key: 34 = '"', alternate: N/A, base: N/A, mods: 2 = shift */
            const char expected_shift_apostrophe[] = "\033[34;2u";
            xassert(count == strlen(expected_shift_apostrophe));
            xassert(streq(escape, expected_shift_apostrophe));

            key_press_release(&seat, &term, 1337, KEY_SPACE + 8, KEY_STATE_RELEASED);

            grid.kitty_kbd.flags[0] = KITTY_KBD_DISAMBIGUATE | KITTY_KBD_REPORT_ALTERNATE;
        }

        key_binding_unload_keymap(key_binding_manager, &seat);
        key_binding_remove_seat(key_binding_manager, &seat);

        xkb_compose_state_unref(seat.kbd.xkb_compose_state);
        xkb_compose_table_unref(seat.kbd.xkb_compose_table);

        xkb_state_unref(seat.kbd.xkb_state);
        xkb_keymap_unref(seat.kbd.xkb_keymap);

        seat.kbd.xkb_state = NULL;
        seat.kbd.xkb_keymap = NULL;
    }

no_keymap:
    xkb_context_unref(seat.kbd.xkb);
    key_binding_manager_destroy(key_binding_manager);

    tll_free(wayl.terms);
    close(chan[0]);
    close(chan[1]);
}

void
input_repeat(struct seat *seat, uint32_t key)
{
    /* Should be cleared as soon as we loose focus */
    xassert(seat->kbd_focus != NULL);
    struct terminal *term = seat->kbd_focus;

    key_press_release(seat, term, seat->kbd.serial, key, KEY_STATE_PRESSED);
}

static void
mouse_button_state_reset(struct seat *seat)
{
    tll_free(seat->mouse.buttons);
    seat->mouse.count = 0;
    seat->mouse.last_released_button = 0;
    memset(&seat->mouse.last_time, 0, sizeof(seat->mouse.last_time));
}

 void
mouse_coord_pixel_to_cell(struct seat *seat, const struct terminal *term,
                          int x, int y)
{
    /*
     * Translate x,y pixel coordinate to a cell coordinate, or clamp
     * if the cursor is outside the grid. I.e. if it is inside the
     * margins.
     */
    if (x < term->margins.left)
        seat->mouse.col = 0;
    else if (x >= term->width - term->margins.right)
        seat->mouse.col = term->cols - 1;
    else
        seat->mouse.col = (x - term->margins.left) / term->cell_width;

    if (y < term->margins.top)
        seat->mouse.row = 0;
    else if (y >= term->height - term->margins.bottom)
        seat->mouse.row = term->rows - 1;
    else
        seat->mouse.row = (y - term->margins.top) / term->cell_height;
}

void
input_pointer_enter(struct seat *seat, struct terminal *term,
                    uint32_t serial, double surface_x, double surface_y)
{
    seat->mouse_focus = term;
    term->active_surface = TERM_SURF_GRID;

    int x = surface_x * term->scale;
    int y = surface_y * term->scale;

    seat->pointer.serial = serial;
    seat->pointer.hidden = false;
    seat->mouse.x = x;
    seat->mouse.y = y;

    LOG_DBG("pointer-enter: serial=%u, new-moused = %p, x=%d, y=%d",
            serial, (void *)term, x, y);

    xassert(tll_length(seat->mouse.buttons) == 0);

    term_xcursor_update_for_seat(term, seat);
    mouse_coord_pixel_to_cell(seat, term, x, y);
}

void
input_pointer_leave(struct seat *seat)
{
    struct terminal *old_moused = seat->mouse_focus;

    LOG_DBG("%s: pointer-leave: old-moused = %p", seat->name, (void *)old_moused);

    seat->pointer.hidden = false;

    /* Reset last-set-xcursor, to ensure we update it on a pointer-enter event */
    seat->pointer.shape = CURSOR_SHAPE_NONE;

    /* Reset mouse state */
    seat->mouse.x = seat->mouse.y = 0;
    seat->mouse.col = seat->mouse.row = 0;
    mouse_button_state_reset(seat);
    for (size_t i = 0; i < ALEN(seat->mouse.aggregated); i++)
        seat->mouse.aggregated[i] = 0.0;
    seat->mouse.have_discrete = false;

    seat->mouse_focus = NULL;
    if (old_moused == NULL)
        return;

    enum term_surface active_surface = old_moused->active_surface;
    old_moused->active_surface = TERM_SURF_NONE;

    if (active_surface == TERM_SURF_GRID)
        selection_finalize(seat, old_moused, seat->pointer.serial);
}

void
input_pointer_motion(struct seat *seat, double surface_x, double surface_y)
{
    struct terminal *term = seat->mouse_focus;

    if (unlikely(term == NULL))
        return;

    int x = surface_x * term->scale;
    int y = surface_y * term->scale;

    LOG_DBG("pointer_motion: x=%d, y=%d", x, y);

    enum term_surface surf_kind = term->active_surface;
    int button = 0;
    bool send_to_client = false;

    seat->pointer.hidden = false;
    seat->mouse.x = x;
    seat->mouse.y = y;

    term_xcursor_update_for_seat(term, seat);

    if (tll_length(seat->mouse.buttons) > 0) {
        const struct button_tracker *tracker = &tll_front(seat->mouse.buttons);
        surf_kind = tracker->surf_kind;
        button = tracker->button;
        send_to_client = tracker->send_to_client;
    }

    if (surf_kind != TERM_SURF_GRID)
        return;

    int old_col = seat->mouse.col;
    int old_row = seat->mouse.row;

    mouse_coord_pixel_to_cell(seat, term, seat->mouse.x, seat->mouse.y);

    xassert(seat->mouse.col >= 0 && seat->mouse.col < term->cols);
    xassert(seat->mouse.row >= 0 && seat->mouse.row < term->rows);

    /* Cursor has moved to a different cell since last time */
    bool cursor_is_on_new_cell
        = old_col != seat->mouse.col || old_row != seat->mouse.row;

    if (cursor_is_on_new_cell) {
        /* Prevent multiple/different mouse bindings from
         * triggering if the mouse has moved "too much" (to
         * another cell) */
        seat->mouse.count = 0;
    }

    /* Cursor is inside the grid, i.e. *not* in the margins */
    const bool cursor_is_on_grid = seat->mouse.col >= 0 && seat->mouse.row >= 0;

    enum selection_scroll_direction auto_scroll_direction
        = term->selection.coords.end.row < 0
            ? SELECTION_SCROLL_NOT
            : y < term->margins.top
                ? SELECTION_SCROLL_UP
                : y > term->height - term->margins.bottom
                    ? SELECTION_SCROLL_DOWN
                    : SELECTION_SCROLL_NOT;

    if (auto_scroll_direction == SELECTION_SCROLL_NOT)
        selection_stop_scroll_timer(term);

    /* Update selection */
    if (!term->is_searching) {
        if (auto_scroll_direction != SELECTION_SCROLL_NOT) {
            /*
             * Start 'selection auto-scrolling'
             *
             * The speed of the scrolling is proportional to the
             * distance between the mouse and the grid; the
             * further away the mouse is, the faster we scroll.
             *
             * Note that the speed is measured in 'intervals (in
             * ns) between each timed scroll of a single line'.
             *
             * Thus, the further away the mouse is, the smaller
             * interval value we use.
             */

            int distance = auto_scroll_direction == SELECTION_SCROLL_UP
                ? term->margins.top - y
                : y - (term->height - term->margins.bottom);

            xassert(distance > 0);
            int divisor
                = distance * term->conf->scrollback.multiplier / term->scale;

            selection_start_scroll_timer(
                term, 400000000 / (divisor > 0 ? divisor : 1),
                auto_scroll_direction, seat->mouse.col);
        }

        if (term->selection.ongoing &&
            (cursor_is_on_new_cell ||
             (term->selection.coords.end.row < 0 &&
              seat->mouse.x >= term->margins.left &&
              seat->mouse.x < term->width - term->margins.right &&
              seat->mouse.y >= term->margins.top &&
              seat->mouse.y < term->height - term->margins.bottom)))
        {
            selection_update(term, seat->mouse.col, seat->mouse.row);
        }
    }

    /* Send mouse event to client application */
    if (!term_mouse_grabbed(term, seat) &&
        (cursor_is_on_new_cell ||
            term->mouse_reporting == MOUSE_SGR_PIXELS) &&
        ((button == 0 && cursor_is_on_grid) ||
         (button != 0 && send_to_client)))
    {
        xassert(seat->mouse.col < term->cols);
        xassert(seat->mouse.row < term->rows);

        term_mouse_motion(
            term, button,
            seat->mouse.row, seat->mouse.col,
            seat->mouse.y - term->margins.top,
            seat->mouse.x - term->margins.left,
            seat->kbd.shift, seat->kbd.alt, seat->kbd.ctrl);
    }
}

static const struct key_binding *
match_mouse_binding(const struct seat *seat, const struct terminal *term,
                    int button)
{
    xassert(seat->kbd.xkb_state != NULL);

    const struct key_binding_set *bindings =
        key_binding_for(term->wl->key_binding_manager, term->conf, seat);
    xassert(bindings != NULL);

    xkb_mod_mask_t mods;
    get_current_modifiers(seat, &mods, NULL, 0, true);

    /* Ignore selection override modifiers when
     * matching modifiers */
    mods &= ~bindings->selection_overrides;

    const struct key_binding *match = NULL;

    tll_foreach(bindings->mouse, it) {
        const struct key_binding *binding = &it->item;

        if (binding->m.button != button) {
            /* Wrong button */
            continue;
        }

        if (binding->mods != mods) {
            /* Modifier mismatch */
            continue;
        }

        if (binding->m.count > seat->mouse.count) {
            /* Not correct click count */
            continue;
        }

        if (match == NULL || binding->m.count > match->m.count)
            match = binding;
    }

    return match;
}

void
input_pointer_button(struct seat *seat, uint32_t serial, int button,
                     enum button_state state)
{
    LOG_DBG("BUTTON: serial=%u, button=%x, state=%u", serial, button, state);

    xassert(serial != 0);

    struct terminal *term = seat->mouse_focus;

    if (unlikely(term == NULL))
        return;

    seat->pointer.serial = serial;
    seat->pointer.hidden = false;

    enum term_surface surf_kind = TERM_SURF_NONE;
    bool send_to_client = false;

    if (state == BUTTON_STATE_PRESSED) {
        /* Time since last click */
        struct timespec now, since_last;
        clock_gettime(CLOCK_MONOTONIC, &now);
        timespec_sub(&now, &seat->mouse.last_time, &since_last);

        if (seat->mouse.last_released_button == button &&
            since_last.tv_sec == 0 && since_last.tv_nsec <= 300 * 1000 * 1000)
        {
            seat->mouse.count++;
        } else
            seat->mouse.count = 1;

        tll_foreach(seat->mouse.buttons, it) {
            if (it->item.button == button) {
                LOG_WARN("multiple button press events for button %d", button);
                tll_remove(seat->mouse.buttons, it);
                break;
            }
        }

        /*
         * Remember which surface "owns" this button, so that we can
         * send motion and button release events to that surface, even
         * if the pointer is no longer over it.
         */
        tll_push_back(
            seat->mouse.buttons,
            ((struct button_tracker){
                .button = button,
                .surf_kind = term->active_surface,
                .send_to_client = false}));

        seat->mouse.last_time = now;

        surf_kind = term->active_surface;
        send_to_client = false;  /* For now, may be set to true if a binding consumes the button */
    } else {
        bool have_button = false;
        tll_foreach(seat->mouse.buttons, it) {
            if (it->item.button == button) {
                have_button = true;
                surf_kind = it->item.surf_kind;
                send_to_client = it->item.send_to_client;
                tll_remove(seat->mouse.buttons, it);
                break;
            }
        }

        if (!have_button) {
            LOG_WARN("stray button release event");
            return;
        }

        seat->mouse.last_released_button = button;
    }

    if (surf_kind != TERM_SURF_GRID)
        return;

    search_cancel(term);
    urls_reset(term);

    bool cursor_is_on_grid = seat->mouse.col >= 0 && seat->mouse.row >= 0;

    switch (state) {
    case BUTTON_STATE_PRESSED: {
        bool consumed = false;

        if (cursor_is_on_grid && term_mouse_grabbed(term, seat)) {
            const struct key_binding *match =
                match_mouse_binding(seat, term, button);

            if (match != NULL)
                consumed = execute_binding(seat, term, match, serial, 1);
        }

        send_to_client = !consumed && cursor_is_on_grid;

        if (send_to_client)
            tll_back(seat->mouse.buttons).send_to_client = true;

        if (send_to_client &&
            !term_mouse_grabbed(term, seat) &&
            cursor_is_on_grid)
        {
            term_mouse_down(
                term, button, seat->mouse.row, seat->mouse.col,
                seat->mouse.y - term->margins.top,
                seat->mouse.x - term->margins.left,
                seat->kbd.shift, seat->kbd.alt, seat->kbd.ctrl);
        }
        break;
    }

    case BUTTON_STATE_RELEASED:
        selection_finalize(seat, term, serial);

        if (send_to_client && !term_mouse_grabbed(term, seat)) {
            term_mouse_up(
                term, button, seat->mouse.row, seat->mouse.col,
                seat->mouse.y - term->margins.top,
                seat->mouse.x - term->margins.left,
                seat->kbd.shift, seat->kbd.alt, seat->kbd.ctrl);
        }
        break;
    }
}

static void
alternate_scroll(struct seat *seat, int amount, int button)
{
    if (seat->kbd.xkb_keymap == NULL)
        return;

    /* Should be cleared in leave event */
    xassert(seat->mouse_focus != NULL);
    struct terminal *term = seat->mouse_focus;

    assert(button == BTN_BACK || button == BTN_FORWARD);

    xkb_keycode_t key = button == BTN_BACK
        ? seat->kbd.key_arrow_up : seat->kbd.key_arrow_down;

    for (int i = 0; i < amount; i++)
        key_press_release(seat, term, seat->kbd.serial, key, KEY_STATE_PRESSED);
    key_press_release(seat, term, seat->kbd.serial, key, KEY_STATE_RELEASED);
}

static void
mouse_scroll(struct seat *seat, int amount, enum scroll_axis axis)
{
    struct terminal *term = seat->mouse_focus;
    xassert(term != NULL);

    int button = axis == SCROLL_AXIS_VERTICAL
        ? amount < 0 ? BTN_WHEEL_BACK : BTN_WHEEL_FORWARD
        : amount < 0 ? BTN_WHEEL_LEFT : BTN_WHEEL_RIGHT;
    amount = abs(amount);

    if (term_mouse_grabbed(term, seat)) {
        seat->mouse.count = 1;

        const struct key_binding *match =
            match_mouse_binding(seat, term, button);

        if (match != NULL)
            execute_binding(seat, term, match, seat->pointer.serial, amount);

        seat->mouse.last_released_button = button;
    }

    else if (seat->mouse.col >= 0 && seat->mouse.row >= 0) {
        xassert(seat->mouse.col < term->cols);
        xassert(seat->mouse.row < term->rows);

        for (int i = 0; i < amount; i++) {
            term_mouse_down(
                term, button, seat->mouse.row, seat->mouse.col,
                seat->mouse.y - term->margins.top,
                seat->mouse.x - term->margins.left,
                seat->kbd.shift, seat->kbd.alt, seat->kbd.ctrl);
        }

        term_mouse_up(
            term, button, seat->mouse.row, seat->mouse.col,
            seat->mouse.y - term->margins.top,
            seat->mouse.x - term->margins.left,
            seat->kbd.shift, seat->kbd.alt, seat->kbd.ctrl);
    }
}

static double
mouse_scroll_multiplier(const struct terminal *term, const struct seat *seat)
{
    return (term->grid == &term->normal ||
            (term_mouse_grabbed(term, seat) && term->alt_scrolling))
        ? term->conf->scrollback.multiplier
        : 1.0;
}

void
input_pointer_scroll(struct seat *seat, enum scroll_axis axis, double value)
{
    if (seat->mouse_focus == NULL)
        return;

    xassert(axis < ALEN(seat->mouse.aggregated));

    const struct terminal *term = seat->mouse_focus;

    /*
     * Aggregate scrolled amount until we get at least one line.
     *
     * Without this, very slow scrolling will never actually scroll
     * anything.
     */
    seat->mouse.aggregated[axis] +=
        mouse_scroll_multiplier(term, seat) * value * term->scale;
    if (fabs(seat->mouse.aggregated[axis]) < term->cell_height)
        return;

    int lines = seat->mouse.aggregated[axis] / term->cell_height;
    mouse_scroll(seat, lines, axis);
    seat->mouse.aggregated[axis] -= (double)lines * term->cell_height;
}

void
input_pointer_scroll_discrete(struct seat *seat, enum scroll_axis axis,
                              int steps)
{
    LOG_DBG("axis_discrete: %d", steps);

    if (seat->mouse_focus == NULL)
        return;

    int amount = steps;

    if (axis == SCROLL_AXIS_HORIZONTAL) {
        /* Treat mouse wheel left/right as regular buttons */
    } else
        amount *= mouse_scroll_multiplier(seat->mouse_focus, seat);

    mouse_scroll(seat, amount, axis);
}

void
input_pointer_scroll_stop(struct seat *seat, enum scroll_axis axis)
{
    xassert(axis < ALEN(seat->mouse.aggregated));
    seat->mouse.aggregated[axis] = 0.;
}
