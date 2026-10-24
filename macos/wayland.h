#pragma once

/*
 * The platform layer.
 *
 * Included from the top-level wayland.h (whose upstream content is left
 * untouched, to keep upstream merges trivial). It keeps upstream foot's
 * names (struct wayland, struct wl_window, struct seat, struct monitor
 * and the wayl_*() functions), so that the terminal core compiles
 * unchanged. On
 * macOS, there is no Wayland involved: everything declared here is
 * implemented natively with AppKit and Core Animation, in macos/.
 *
 *   - struct wayland: the application (NSApplication)
 *   - struct wl_window: a terminal window (NSWindow + FootView)
 *   - struct wayl_surface: a CALayer. The main surface is the view's
 *     grid layer; sub-surfaces are sublayers of it.
 *   - struct seat: the keyboard and mouse
 *   - struct monitor: an NSScreen
 *
 * Buffers are IOSurfaces (see shm.h), set as layer contents.
 */

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <signal.h>
#include <uchar.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

#include <fcft/fcft.h>
#include <tllist.h>

#include "../config.h"
#include "../cursor-shape.h"
#include "../fdm.h"

/* Forward declarations */
struct terminal;
struct buffer;

/* Mime-types we support when dealing with data offers (e.g. copy-paste, or DnD) */
enum data_offer_mime_type {
    DATA_OFFER_MIME_UNSET,
    DATA_OFFER_MIME_TEXT_PLAIN,
    DATA_OFFER_MIME_TEXT_UTF8,
    DATA_OFFER_MIME_URI_LIST,

    DATA_OFFER_MIME_TEXT_TEXT,
    DATA_OFFER_MIME_TEXT_STRING,
    DATA_OFFER_MIME_TEXT_UTF8_STRING,
};

struct wayl_surface {
    void *layer;                /* CALayer (retained) */
    struct buffer *buf;         /* Attached, but not yet committed */
    struct buffer *committed;   /* Currently the layer's contents */
    bool has_pending;           /* Attach since last commit */
    float scale;                /* Buffer scale */
};

struct wayl_sub_surface {
    struct wayl_surface surface;
};

struct wl_window;
struct wl_clipboard {
    char *text;  /* Text we have put on the pasteboard */
    long change_count;  /* NSPasteboard change count when we put it there */
    uint32_t serial;
};

/*
 * There is no primary selection on macOS. foot keeps one internally,
 * shared by all its windows, so that select + middle-click paste
 * works the way it does on Linux.
 */
struct wl_primary {
    char *text;
    uint32_t serial;
};

/* Maps a mouse button to its "owning" surface */
struct button_tracker {
    int button;
    int surf_kind;  /* TODO: this is really an "enum term_surface" */
    bool send_to_client;  /* Only valid when surface is the main grid surface */
};

struct rect {
    int x;
    int y;
    int width;
    int height;
};

struct seat {
    struct wayland *wayl;
    char *name;

    /* Focused terminals */
    struct terminal *kbd_focus;
    struct terminal *mouse_focus;
    struct terminal *ime_focus;

    /* Keyboard state */
    struct {
        uint32_t serial;

        struct xkb_context *xkb;
        struct xkb_keymap *xkb_keymap;
        struct xkb_state *xkb_state;
        struct xkb_compose_table *xkb_compose_table;
        struct xkb_compose_state *xkb_compose_state;
        struct {
            int fd;

            bool dont_re_repeat;
            int32_t delay;
            int32_t rate;
            uint32_t key;
        } repeat;

        xkb_mod_index_t mod_shift;
        xkb_mod_index_t mod_alt;
        xkb_mod_index_t mod_ctrl;
        xkb_mod_index_t mod_super;
        xkb_mod_index_t mod_caps;
        xkb_mod_index_t mod_num;

        xkb_mod_mask_t legacy_significant;  /* Significant modifiers for the legacy keyboard protocol */
        xkb_mod_mask_t kitty_significant;   /* Significant modifiers for the kitty keyboard protocol */

        xkb_mod_mask_t virtual_modifiers;   /* Set of modifiers to completely ignore */

        xkb_keycode_t key_arrow_up;
        xkb_keycode_t key_arrow_down;

        /* Enabled modifiers */
        bool shift;
        bool alt;
        bool ctrl;
        bool super;

        xkb_keysym_t last_shortcut_sym;
    } kbd;

    /* Pointer state */
    struct {
        uint32_t serial;

        bool hidden;
        enum cursor_shape shape;
        char *last_custom_xcursor;
    } pointer;

    struct {
        int x;
        int y;
        int col;
        int row;

        /* Mouse buttons currently being pressed, and their "owning" surfaces */
        tll(struct button_tracker) buttons;

        /* Double- and triple click state */
        int count;
        int last_released_button;
        struct timespec last_time;

        /* Scroll deltas not yet converted to whole lines */
        double aggregated[2];
        double aggregated_120[2];
        bool have_discrete;
    } mouse;

    /* Clipboard */
    struct wl_clipboard clipboard;
    struct wl_primary primary;

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    /* Input Method Editor (NSTextInputClient) */
    struct {
        struct {
            struct rect pending;
            struct rect sent;
        } cursor_rect;

        struct {
            struct {
                char *text;
                int32_t cursor_begin;
                int32_t cursor_end;
            } pending;

            char32_t *text;
            struct cell *cells;
            int count;
            struct {
                bool hidden;
                int start;  /* Cell index, inclusive */
                int end;    /* Cell index, exclusive */
            } cursor;
        } preedit;

        struct  {
            struct {
                char *text;
            } pending;
        } commit;

        struct {
            struct {
                uint32_t before_length;
                uint32_t after_length;
            } pending;
        } surrounding;

        uint32_t serial;
    } ime;
#endif
};

enum csd_surface {
    CSD_SURF_TITLE,
    CSD_SURF_LEFT,
    CSD_SURF_RIGHT,
    CSD_SURF_TOP,
    CSD_SURF_BOTTOM,
    CSD_SURF_MINIMIZE,
    CSD_SURF_MAXIMIZE,
    CSD_SURF_CLOSE,
    CSD_SURF_COUNT,
};

/* Subpixel layout of a monitor (always "unknown" on macOS) */
enum monitor_subpixel {
    MONITOR_SUBPIXEL_UNKNOWN,
    MONITOR_SUBPIXEL_NONE,
    MONITOR_SUBPIXEL_HORIZONTAL_RGB,
    MONITOR_SUBPIXEL_HORIZONTAL_BGR,
    MONITOR_SUBPIXEL_VERTICAL_RGB,
    MONITOR_SUBPIXEL_VERTICAL_BGR,
};

struct monitor {
    struct wayland *wayl;
    uint32_t display_id;  /* CGDirectDisplayID */

    int x;
    int y;

    struct {
        /* Physical size, in mm */
        struct {
            int width;
            int height;
        } mm;

        /* Physical size, in pixels */
        struct {
            int width;
            int height;
        } px_real;

        /* Scaled size, in pixels */
        struct {
            int width;
            int height;
        } px_scaled;
    } dim;

    struct {
        /* PPI, based on physical size */
        struct {
            int x;
            int y;
        } real;

        /* PPI, logical, based on scaled size */
        struct {
            int x;
            int y;
        } scaled;
    } ppi;

    struct {
        float scaled;
        float physical;
    } dpi;

    int scale;
    float refresh;
    enum monitor_subpixel subpixel;

    char *make;
    char *model;
    char *name;
    char *description;

    float inch;  /* e.g. 24" */
};

struct wl_url {
    const struct url *url;
    struct wayl_sub_surface surf;
};

enum csd_mode {CSD_UNKNOWN, CSD_NO, CSD_YES};

typedef void (*activation_token_cb_t)(const char *token, void *data);

struct wayland;
struct wl_window {
    struct terminal *term;
    struct wayl_surface surface;

    void *nswindow;  /* NSWindow (retained) */
    void *delegate;  /* The NSWindow's delegate (retained) */
    void *view;      /* FootView, the window's content view */

    bool unmapped;
    float scale;

    /* Native window decorations are always used */
    enum csd_mode csd_mode;

    struct {
        struct wayl_sub_surface surface[CSD_SURF_COUNT];
        struct fcft_font *font;
        int move_timeout_fd;
        uint32_t serial;
    } csd;

    struct {
        bool maximize:1;
        bool minimize:1;
    } wm_capabilities;

    struct wayl_sub_surface search;
    struct wayl_sub_surface scrollback_indicator;
    struct wayl_sub_surface render_timer;
    struct wayl_sub_surface overlay;

    /* A frame (display refresh) has been requested */
    bool frame_callback;

    tll(const struct monitor *) on_outputs; /* Outputs we're mapped on */
    tll(struct wl_url) urls;

    bool is_configured;
    bool is_activated;
    bool is_fullscreen;
    bool is_maximized;
    bool is_resizing;
    bool is_suspended;
    bool is_tiled_top;
    bool is_tiled_bottom;
    bool is_tiled_left;
    bool is_tiled_right;
    bool is_tiled;  /* At least one of is_tiled_{top,bottom,left,right} is true */

    int resize_timeout_fd;

    /* The terminal's cell size (pixels), as of the last resize */
    int cell_width;
    int cell_height;
};

struct terminal;
struct wayland {
    struct fdm *fdm;
    struct key_binding_manager *key_binding_manager;

    void *app;  /* FootApp (retained) */

    tll(struct monitor) monitors;  /* All available outputs */
    tll(struct seat) seats;

    tll(struct terminal *) terms;

    bool presentation_timings;

    /* Not available on macOS (yet); see shm.c */
    bool shm_have_argb2101010:1;
    bool shm_have_abgr2101010:1;
    bool shm_have_abgr161616:1;
};

struct wayland *wayl_init(
    struct fdm *fdm, struct key_binding_manager *key_binding_manager,
    bool presentation_timings);
void wayl_destroy(struct wayland *wayl);

/*
 * Runs the application's run loop until the last window closes. Or,
 * with 'keep_running', and when started as an application (Finder,
 * Dock, open(1)), until it quits (Cmd+Q), or '*aborted' is set.
 */
bool wayl_run(struct wayland *wayl, volatile sig_atomic_t *aborted,
              bool keep_running);

/* SIGUSR1/SIGUSR2: all windows switch to the dark/light color theme */
void wayl_theme_switch_all(bool dark);

/*
 * Called first thing in main(): e.g. sets $LANG, which isn't set for
 * applications started from Finder or the Dock.
 */
void wayl_prepare_environment(void);

/* Opens a new terminal window (application menu, dock icon) */
void wayl_set_new_window_handler(
    struct wayland *wayl, bool (*handler)(void *data, const char *cwd), void *data);

/* Opens the configuration in an editor (application menu: Settings) */
void wayl_set_settings_handler(
    struct wayland *wayl, bool (*handler)(void *data), void *data);

/*
 * Enables the above, with foot's implementation (macos/windows.c):
 * new windows re-load the configuration ('conf_path', or the default
 * location); 'conf' is used if that fails.
 */
struct config;
struct reaper;
void wayl_enable_new_windows(
    struct wayland *wayl, const struct config *conf, const char *conf_path,
    struct fdm *fdm, struct reaper *reaper);

void wayl_flush(struct wayland *wayl);
void wayl_roundtrip(struct wayland *wayl);

bool wayl_fractional_scaling(const struct wayland *wayl);
void wayl_surface_scale(
    const struct wl_window *win, struct wayl_surface *surf,
    const struct buffer *buf, float scale);
void wayl_surface_scale_explicit_width_height(
    const struct wl_window *win, struct wayl_surface *surf,
    int width, int height, float scale);

/*
 * Surfaces. These mirror the wl_surface requests foot uses: attach a
 * buffer (NULL hides the surface), mark damage, and commit. Damage is
 * informational; the window server always composites whole layers.
 */
void wayl_surface_attach(struct wayl_surface *surf, struct buffer *buf);
void wayl_surface_damage(
    struct wayl_surface *surf, int x, int y, int width, int height);
void wayl_surface_commit(struct wayl_surface *surf);
void wayl_surface_set_opaque(struct wayl_surface *surf, bool opaque);

/* Position (in logical coordinates) relative to the main surface */
void wayl_sub_surface_set_position(struct wayl_sub_surface *surf, int x, int y);

/* Single color sub-surface, covering width x height logical pixels */
void wayl_sub_surface_set_color(
    struct wayl_sub_surface *surf, uint32_t argb, int width, int height);

struct wl_window *wayl_win_init(struct terminal *term, const char *token);
void wayl_win_destroy(struct wl_window *win);

void wayl_win_scale(struct wl_window *win, const struct buffer *buf);
void wayl_win_alpha_changed(struct wl_window *win);
bool wayl_win_set_urgent(struct wl_window *win);
bool wayl_win_ring_bell(const struct wl_window *win);

/* Requests a frame callback, i.e. a call to render's frame handler
 * at the next display refresh */
void wayl_win_request_frame(struct wl_window *win);

void wayl_win_set_title(struct wl_window *win, const char *title);
void wayl_win_set_app_id(struct wl_window *win, const char *app_id);
void wayl_win_set_fullscreen(struct wl_window *win, bool fullscreen);
void wayl_win_set_maximized(struct wl_window *win, bool maximized);
void wayl_win_minimize(struct wl_window *win);
void wayl_win_close(struct wl_window *win);

/* Asks the window to resize itself to fit width x height (logical) */
void wayl_win_set_content_size(struct wl_window *win, int width, int height);

/* Cell size, for window resize increments; when it changes (font size,
 * scale), the window adopts the terminal's size (resize-keep-grid) */
void wayl_win_set_resize_increments(
    struct wl_window *win, int cell_width, int cell_height);

bool wayl_win_csd_titlebar_visible(const struct wl_window *win);
bool wayl_win_csd_borders_visible(const struct wl_window *win);

bool wayl_win_subsurface_new(
    struct wl_window *win, struct wayl_sub_surface *surf,
    bool allow_pointer_input);
bool wayl_win_subsurface_new_with_custom_parent(
    struct wl_window *win, struct wayl_surface *parent,
    struct wayl_sub_surface *surf, bool allow_pointer_input);
void wayl_win_subsurface_destroy(struct wayl_sub_surface *surf);

bool wayl_get_activation_token(
    struct wayland *wayl, struct seat *seat, uint32_t serial,
    struct wl_window *win, activation_token_cb_t cb, void *cb_data);
void wayl_activate(struct wayland *wayl, struct wl_window *win, const char *token);

bool wayl_do_linear_blending(const struct wayland *wayl, const struct config *conf);

/*
 * Clipboard (NSPasteboard). wayl_clipboard_set() returns the
 * pasteboard's change count after setting the text, or -1 on error;
 * wayl_clipboard_clear() only clears the pasteboard if its change
 * count still matches. wayl_clipboard_get() returns a malloc:ed
 * buffer, either text/plain;charset=utf-8, or a text/uri-list of
 * file URLs.
 */
long wayl_clipboard_set(struct wayland *wayl, const char *text);
void wayl_clipboard_clear(struct wayland *wayl, long change_count);
bool wayl_clipboard_has_data(const struct wayland *wayl);
char *wayl_clipboard_get(
    struct wayland *wayl, enum data_offer_mime_type *mime_type, size_t *len);

/* Input method: discard any composition in progress */
void wayl_win_ime_reset(struct wl_window *win);
/* Input method: the cursor rectangle (seat->ime.cursor_rect) changed */
void wayl_win_ime_cursor_rect_changed(struct wl_window *win);

/*
 * Sets the mouse cursor. 'custom_name' is a CSS/XCursor name (e.g.
 * "pointer"), used with CURSOR_SHAPE_CUSTOM.
 */
void wayl_set_cursor(
    struct seat *seat, enum cursor_shape shape, const char *custom_name);
bool wayl_cursor_is_valid(const char *name);
