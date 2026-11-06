/*
 * foot's terminal core, without a window; see headless.h.
 *
 * Everything below the core (rendering, the window, fonts, the
 * clipboard, notifications, key bindings) is replaced by functions
 * that do nothing, as in upstream's PGO harness (pgo/pgo.c). What the
 * core does with a window (rendering it, a bell, a notification, the
 * clipboard) is done by the window attached to the session, which
 * parses the same output.
 */

#include "headless.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/socket.h>

#include <tllist.h>

#define LOG_MODULE "headless"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "commands.h"
#include "config.h"
#include "extract.h"
#include "fdm.h"
#include "grid.h"
#include "ime.h"
#include "input.h"
#include "key-binding.h"
#include "notify.h"
#include "render.h"
#include "search.h"
#include "selection.h"
#include "shm.h"
#include "sixel.h"
#include "spawn.h"
#include "terminal.h"
#include "url-mode.h"
#include "util.h"
#include "vt.h"
#include "wayland.h"
#include "xmalloc.h"

/* Not declared in terminal.h; upstream exports it for pgo/pgo.c */
extern bool fdm_ptmx(struct fdm *fdm, int fd, int events, void *data);

static struct {
    struct fdm *fdm;
    struct reaper *reaper;
    struct wayland *wayl;
} headless;

bool
headless_init(struct fdm *fdm, struct reaper *reaper)
{
    struct wayland *wayl = xcalloc(1, sizeof(*wayl));
    wayl->fdm = fdm;

    /* The core takes its initial scale from the first monitor */
    tll_push_back(wayl->monitors, ((struct monitor){
        .wayl = wayl,
        .scale = 1,
        .dpi = {.scaled = 72., .physical = 72.},
        .refresh = 60.,
    }));

    headless.fdm = fdm;
    headless.reaper = reaper;
    headless.wayl = wayl;
    return true;
}

void
headless_fini(void)
{
    struct wayland *wayl = headless.wayl;
    if (wayl == NULL)
        return;

    xassert(tll_length(wayl->terms) == 0);
    tll_free(wayl->terms);
    tll_free(wayl->monitors);
    free(wayl);
    headless.wayl = NULL;
}

/*
 * The terminal's replies (term_to_slave()) go to a socket; writes that
 * don't complete right away are finished by the core (fdm_ptmx()),
 * when the socket is writable again.
 */
static bool
fdm_replies_writable(struct fdm *fdm, int fd, int events, void *data)
{
    struct terminal *term = data;

    if (events & EPOLLOUT)
        return fdm_ptmx(fdm, fd, EPOLLOUT, term);
    return true;
}

static bool
set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

struct terminal *
headless_term_new(const struct config *conf, const char *cwd,
                  int argc, char *const *argv, const char *const *envp,
                  bool spawn, int *pty, int *replies,
                  void (*shutdown_cb)(void *data, int exit_code),
                  void *shutdown_data)
{
    xassert(headless.wayl != NULL);

    *pty = -1;
    *replies = -1;

    int reply_fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, reply_fds) < 0) {
        LOG_ERRNO("failed to create the reply socket");
        return NULL;
    }

    /* Writing replies nobody reads (any more) is no reason to die */
    const int on = 1;

    if (fcntl(reply_fds[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(reply_fds[1], F_SETFD, FD_CLOEXEC) < 0 ||
        !set_nonblocking(reply_fds[0]) || !set_nonblocking(reply_fds[1]) ||
        setsockopt(reply_fds[0], SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) < 0)
    {
        LOG_ERRNO("failed to configure the reply socket");
        close(reply_fds[0]);
        close(reply_fds[1]);
        return NULL;
    }

    /*
     * Without a command to run, the terminal gets a PTY of its own
     * (the core needs one), that nothing reads from: only the reply
     * socket is used.
     */
    int dummy_master = -1;
    char *dummy_slave = NULL;

    if (!spawn) {
        dummy_master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (dummy_master < 0 || grantpt(dummy_master) < 0 ||
            unlockpt(dummy_master) < 0 || ptsname(dummy_master) == NULL)
        {
            LOG_ERRNO("failed to create a PTY");
            if (dummy_master >= 0)
                close(dummy_master);
            close(reply_fds[0]);
            close(reply_fds[1]);
            return NULL;
        }
        dummy_slave = xstrdup(ptsname(dummy_master));
    }

    struct terminal *term = term_init(
        conf, headless.fdm, headless.reaper, headless.wayl, "foot", cwd, NULL,
        dummy_slave, argc, argv, envp, shutdown_cb, shutdown_data);

    free(dummy_slave);

    if (term == NULL) {
        if (dummy_master >= 0)
            close(dummy_master);
        close(reply_fds[0]);
        close(reply_fds[1]);
        return NULL;
    }

    /* The PTY is the caller's; the terminal writes to the reply socket */
    if (spawn)
        *pty = term->ptmx;
    else {
        close(term->ptmx);
        close(dummy_master);
    }

    term->ptmx = reply_fds[0];
    if (!fdm_add(headless.fdm, term->ptmx, 0, &fdm_replies_writable, term)) {
        /* term_destroy() closes it */
        close(reply_fds[1]);
        if (*pty >= 0)
            close(*pty);
        *pty = -1;
        term->shutdown.in_progress = true;
        term_destroy(term);
        return NULL;
    }

    *replies = reply_fds[1];
    return term;
}

void
headless_term_feed(struct terminal *term, const void *data, size_t len)
{
    if (len > 0)
        vt_from_slave(term, data, len);
}

void
headless_term_resize(struct terminal *term, int cols, int rows,
                     int cell_width, int cell_height)
{
    cols = max(cols, 1);
    rows = max(rows, 1);
    cell_width = max(cell_width, 1);
    cell_height = max(cell_height, 1);

    const bool cell_size_changed =
        term->cell_width != cell_width || term->cell_height != cell_height;

    term->cell_width = cell_width;
    term->cell_height = cell_height;
    term->width = cols * cell_width;
    term->height = rows * cell_height;

    if (cell_size_changed)
        sixel_cell_size_changed(term);

    if (cols == term->cols && rows == term->rows)
        return;

    /* What render_resize() does to the grid; see there */
    term_disable_app_sync_updates(term);

    const int old_rows = term->rows;

    const unsigned max_scrollback = (INT_MAX >> 1) + 1;
    const unsigned scrollback_lines_not_yet_power_of_two =
        min((uint64_t)term->render.scrollback_lines + rows - 1, max_scrollback);

    const int new_normal_grid_rows =
        min(1u << (32 - __builtin_clz(scrollback_lines_not_yet_power_of_two)),
            max_scrollback);
    const int new_alt_grid_rows =
        min(1u << (32 - __builtin_clz(rows)), max_scrollback);

    struct coord *no_tracking_points[1] = {NULL};
    grid_resize_and_reflow(
        &term->normal, term, new_normal_grid_rows, cols, old_rows, rows,
        0, no_tracking_points);
    grid_resize_without_reflow(
        &term->alt, new_alt_grid_rows, cols, old_rows, rows);

    tll_free(term->tab_stops);
    for (int c = 0; c < cols; c += 8)
        tll_push_back(term->tab_stops, c);

    term->cols = cols;
    term->rows = rows;

    sixel_reflow(term);

    if (term->scroll_region.start >= term->rows)
        term->scroll_region.start = 0;
    if (term->scroll_region.end > term->rows ||
        term->scroll_region.end >= old_rows)
    {
        term->scroll_region.end = term->rows;
    }

    term->render.last_cursor.row = NULL;
}

/*
 * The platform layer, as the core sees it: there's a window, but it's
 * never shown.
 */

struct wl_window *
wayl_win_init(struct terminal *term, const char *token)
{
    struct wl_window *win = xcalloc(1, sizeof(*win));
    win->term = term;
    win->csd_mode = CSD_NO;
    win->csd.move_timeout_fd = -1;
    win->resize_timeout_fd = -1;
    win->scale = 1.;

    /* Configured: term_shutdown() unregisters the PTY (the reply socket) */
    win->is_configured = true;
    return win;
}

void
wayl_win_destroy(struct wl_window *win)
{
    free(win);
}

void wayl_win_alpha_changed(struct wl_window *win) {}
bool wayl_win_set_urgent(struct wl_window *win) { return true; }
bool wayl_win_ring_bell(const struct wl_window *win) { return true; }
void wayl_win_set_fullscreen(struct wl_window *win, bool fullscreen) {}
void wayl_win_set_maximized(struct wl_window *win, bool maximized) {}
void wayl_surface_commit(struct wayl_surface *surf) {}

bool
wayl_do_linear_blending(const struct wayland *wayl, const struct config *conf)
{
    return false;
}

/* Rendering */

bool
render_resize(struct terminal *term, int width, int height, uint8_t opts)
{
    return true;
}

void render_refresh(struct terminal *term) {}
void render_refresh_csd(struct terminal *term) {}
void render_refresh_title(struct terminal *term) {}
void render_refresh_app_id(struct terminal *term) {}
void render_refresh_icon(struct terminal *term) {}
void render_overlay(struct terminal *term) {}
void render_buffer_release_callback(struct buffer *buf, void *data) {}

int
render_worker_thread(void *_ctx)
{
    free(_ctx);
    return 0;
}

bool
render_xcursor_is_valid(const struct seat *seat, const char *cursor)
{
    return true;
}

bool
render_xcursor_set(struct seat *seat, struct terminal *term, enum cursor_shape shape)
{
    return true;
}

void shm_unref(struct buffer *buf) {}
void shm_chain_free(struct buffer_chain *chain) {}

enum shm_bit_depth
shm_chain_bit_depth(const struct buffer_chain *chain)
{
    return SHM_BITS_8;
}

struct buffer_chain *
shm_chain_new(struct wayland *wayl, bool scrollable, size_t pix_instances,
              enum shm_bit_depth desired_bit_depth,
              void (*release_cb)(struct buffer *buf, void *data), void *cb_data)
{
    return NULL;
}

/* Input, selections, the clipboard, URL mode, searching */

void
get_current_modifiers(const struct seat *seat, xkb_mod_mask_t *effective,
                      xkb_mod_mask_t *consumed, uint32_t key, bool filter_locked)
{
    if (effective != NULL)
        *effective = 0;
    if (consumed != NULL)
        *consumed = 0;
}

void ime_enable(struct seat *seat) {}
void ime_disable(struct seat *seat) {}
void ime_reset_preedit(struct seat *seat) {}

static struct key_binding_set no_key_bindings;

struct key_binding_set *
key_binding_for(struct key_binding_manager *mgr, const struct config *conf,
                const struct seat *seat)
{
    return &no_key_bindings;
}

void
key_binding_new_for_conf(struct key_binding_manager *mgr,
                         const struct wayland *wayl, const struct config *conf)
{
}

void
key_binding_unref(struct key_binding_manager *mgr, const struct config *conf)
{
}

struct extraction_context *
extract_begin(enum selection_kind kind, bool strip_trailing_empty)
{
    return NULL;
}

bool
extract_one(const struct terminal *term, const struct row *row,
            const struct cell *cell, int col, void *context)
{
    return false;
}

bool
extract_finish(struct extraction_context *context, char **text, size_t *len)
{
    return false;
}

bool selection_clipboard_has_data(const struct seat *seat) { return false; }
bool selection_primary_has_data(const struct seat *seat) { return false; }
void selection_clipboard_unset(struct seat *seat) {}
void selection_primary_unset(struct seat *seat) {}

void
selection_to_clipboard(struct seat *seat, struct terminal *term, uint32_t serial)
{
}

void
selection_to_primary(struct seat *seat, struct terminal *term, uint32_t serial)
{
}

bool
text_to_clipboard(struct seat *seat, struct terminal *term, char *text,
                  uint32_t serial)
{
    free(text);
    return false;
}

bool
text_to_primary(struct seat *seat, struct terminal *term, char *text,
                uint32_t serial)
{
    free(text);
    return false;
}

void
text_from_clipboard(struct seat *seat, struct terminal *term, bool no_strip,
                    void (*cb)(char *data, size_t size, void *user),
                    void (*done)(void *user), void *user)
{
    done(user);
}

void
text_from_primary(struct seat *seat, struct terminal *term, bool no_strip,
                  void (*cb)(char *data, size_t size, void *user),
                  void (*done)(void *user), void *user)
{
    done(user);
}

void cmd_scrollback_up(struct terminal *term, int rows) {}
void cmd_scrollback_down(struct terminal *term, int rows) {}
void urls_reset(struct terminal *term) {}
void search_selection_cancelled(struct terminal *term) {}

/* Notifications are the window's */

bool
notify_notify(struct terminal *term, struct notification *notif)
{
    return true;
}

void notify_close(struct terminal *term, const char *id) {}
void notify_free(struct terminal *term, struct notification *notif) {}

void
notify_icon_add(struct terminal *term, const char *id,
                const char *symbolic_name, const uint8_t *data, size_t data_sz)
{
}

void notify_icon_del(struct terminal *term, const char *id) {}
void notify_icon_free(struct notification_icon *icon) {}

/* Helpers (utmp, the bell's command, new terminals) are the window's too */

pid_t
spawn(struct reaper *reaper, const char *cwd, char *const argv[],
      int stdin_fd, int stdout_fd, int stderr_fd,
      reaper_cb cb, void *cb_data, const char *xdg_activation_token)
{
    return -1;
}

/*
 * Fonts: stand-ins; nothing is rasterized. The cell size is the
 * attached window's (headless_term_resize()). Combining characters
 * aren't pre-composed: the window does that, with its fonts.
 */

struct fake_font {
    struct fcft_font public;
};

bool
fcft_init(enum fcft_log_colorize colorize, bool do_syslog,
          enum fcft_log_class log_level)
{
    return true;
}

void fcft_fini(void) {}

enum fcft_capabilities
fcft_capabilities(void)
{
    /* As macos/fcft.c: the configuration's defaults depend on it */
    return FCFT_CAPABILITY_GRAPHEME_SHAPING | FCFT_CAPABILITY_TEXT_RUN_SHAPING;
}

struct fcft_font_options *
fcft_font_options_create(void)
{
    return xcalloc(1, sizeof(struct fcft_font_options));
}

void
fcft_font_options_destroy(struct fcft_font_options *options)
{
    free(options);
}

static struct fcft_font *
fake_font(void)
{
    struct fake_font *font = xcalloc(1, sizeof(*font));
    font->public = (struct fcft_font){
        .height = 16,
        .ascent = 13,
        .descent = 3,
        .max_advance = {.x = 8, .y = 16},
        .underline = {.position = -2, .thickness = 1},
        .strikeout = {.position = 5, .thickness = 1},
        .antialias = true,
    };
    return &font->public;
}

struct fcft_font *
fcft_from_name(size_t count, const char *names[static count],
               const char *attributes)
{
    return fake_font();
}

struct fcft_font *
fcft_from_name2(size_t count, const char *names[static count],
                const char *attributes, const struct fcft_font_options *options)
{
    return fake_font();
}

struct fcft_font *
fcft_clone(const struct fcft_font *font)
{
    return fake_font();
}

void
fcft_destroy(struct fcft_font *font)
{
    free(font);
}

const struct fcft_glyph *
fcft_rasterize_char_utf32(struct fcft_font *font, uint32_t cp,
                          enum fcft_subpixel subpixel)
{
    return NULL;
}

uint32_t
fcft_precompose(const struct fcft_font *font, uint32_t base, uint32_t comb,
                bool *base_is_from_primary, bool *comb_is_from_primary,
                bool *composed_is_from_primary)
{
    return (uint32_t)-1;
}
