#import "app.h"

#include <stdlib.h>
#include <string.h>

#include <tllist.h>

#define LOG_MODULE "reload"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "config.h"
#include "key-binding.h"
#include "render.h"
#include "terminal.h"
#include "util.h"
#include "windows.h"
#include "xmalloc.h"

/*
 * foot > Reload Configuration (Cmd+Shift+,, as in Ghostty): the windows
 * that are open take foot.ini as it is now, without restarting foot.
 * (New windows load it anyway.)
 *
 * The file is loaded once. Each configuration in use (every window has
 * its own; the first window's, which has the command line's overrides,
 * is also a server's) gets the contents of a copy of it, in place: what
 * points to a configuration keeps pointing to it. Most options are read
 * from the configuration when they're needed. What a terminal took from
 * it when it started is applied again, if that's what changed: the
 * colors, the fonts, the padding, the cursor.
 *
 * Only new windows get what a terminal starts with (shell, term,
 * login-shell, the environment, the title, the initial size and mode,
 * macos-sessions), scrollback.lines (a session's is footd's), workers
 * and macos-titlebar-style.
 */

/* What a terminal took from its configuration */
enum {
    CHANGED_COLORS = 1 << 0,
    CHANGED_FONTS = 1 << 1,
    CHANGED_PADDING = 1 << 2,
    CHANGED_CURSOR = 1 << 3,
    CHANGED_ALT_SCROLLING = 1 << 4,
    CHANGED_GRAPHEME_SHAPING = 1 << 5,
};

struct replaced {
    struct config *conf;
    unsigned changed;
};

/*
 * The color themes, and which one it is. 'quake': its themes have
 * macos-quake-alpha in place of their own alpha (quake_theme_changed()
 * sees to that).
 */
static bool
themes_equal(const struct config *a, const struct config *b, bool quake)
{
    struct color_theme dark = b->colors_dark;
    struct color_theme light = b->colors_light;

    if (quake) {
        dark.alpha = a->colors_dark.alpha;
        light.alpha = a->colors_light.alpha;
    }

    /* (Padding that differs only applies them again.) With
     * macos-follow-appearance, the initial theme is the appearance's */
    return memcmp(&a->colors_dark, &dark, sizeof(dark)) == 0 &&
        memcmp(&a->colors_light, &light, sizeof(light)) == 0 &&
        a->macos.follow_appearance == b->macos.follow_appearance &&
        (a->macos.follow_appearance ||
         a->initial_color_theme == b->initial_color_theme);
}

static bool
pt_or_px_equal(const struct pt_or_px *a, const struct pt_or_px *b)
{
    return a->px == b->px && a->pt == b->pt;
}

/* The fonts, and what else their glyphs and the cell size are made from */
static bool
fonts_equal(const struct config *a, const struct config *b)
{
    for (size_t i = 0; i < ALEN(a->fonts); i++) {
        if (a->fonts[i].count != b->fonts[i].count)
            return false;

        for (size_t j = 0; j < a->fonts[i].count; j++) {
            const struct config_font *fa = &a->fonts[i].arr[j];
            const struct config_font *fb = &b->fonts[i].arr[j];

            if (strcmp(fa->pattern, fb->pattern) != 0 ||
                fa->pt_size != fb->pt_size || fa->px_size != fb->px_size)
            {
                return false;
            }
        }
    }

    return pt_or_px_equal(&a->line_height, &b->line_height) &&
        pt_or_px_equal(&a->letter_spacing, &b->letter_spacing) &&
        pt_or_px_equal(&a->horizontal_letter_offset, &b->horizontal_letter_offset) &&
        pt_or_px_equal(&a->vertical_letter_offset, &b->vertical_letter_offset) &&
        a->dpi_aware == b->dpi_aware &&
        a->macos.font_smoothing == b->macos.font_smoothing &&
        a->macos.font_smoothing_strength == b->macos.font_smoothing_strength &&
        a->tweak.fcft_filter == b->tweak.fcft_filter &&
        a->tweak.box_drawing_base_thickness == b->tweak.box_drawing_base_thickness &&
        a->tweak.box_drawing_solid_shades == b->tweak.box_drawing_solid_shades;
}

/*
 * 'conf' gets the contents of a copy of 'loaded'; 'initial': with the
 * command line's overrides. Returns what changed, of what its terminals
 * took from it.
 */
static unsigned
replace(struct config *conf, const struct config *loaded, bool initial)
{
    struct config *new = config_clone(loaded);
    if (initial)
        windows_apply_overrides(new);

    /* Not the file's: --hold, and the window of a script (windows.c) */
    new->hold_at_exit = conf->hold_at_exit;

    unsigned changed = 0;

    if (!themes_equal(conf, new, quake_owns_config(conf)))
        changed |= CHANGED_COLORS;
    else {
        /* The appearance's (app_apply_color_theme()), or SIGUSR1/2's */
        new->initial_color_theme = conf->initial_color_theme;
    }

    if (!fonts_equal(conf, new))
        changed |= CHANGED_FONTS;

    if (conf->pad_left != new->pad_left || conf->pad_top != new->pad_top ||
        conf->pad_right != new->pad_right || conf->pad_bottom != new->pad_bottom ||
        conf->center_when != new->center_when)
    {
        changed |= CHANGED_PADDING;
    }

    if (conf->cursor.style != new->cursor.style ||
        conf->cursor.blink.enabled != new->cursor.blink.enabled ||
        conf->cursor.blink.rate_ms != new->cursor.blink.rate_ms)
    {
        changed |= CHANGED_CURSOR;
    }

    if (conf->mouse.alternate_scroll_mode != new->mouse.alternate_scroll_mode)
        changed |= CHANGED_ALT_SCROLLING;
    if (conf->tweak.grapheme_shaping != new->tweak.grapheme_shaping)
        changed |= CHANGED_GRAPHEME_SHAPING;

    /* Swapped: the copy has the old contents now, and frees them */
    const struct config old = *conf;
    *conf = *new;
    *new = old;
    config_free(new);
    free(new);

    return changed;
}

static void
apply(struct terminal *term, unsigned changed)
{
    const struct config *conf = term->conf;

    /* A size for each of the configuration's fonts, as in term_init() */
    if (changed & CHANGED_FONTS) {
        for (size_t i = 0; i < ALEN(term->font_sizes); i++) {
            const struct config_font_list *fonts = &conf->fonts[i];

            free(term->font_sizes[i]);
            term->font_sizes[i] = xmalloc(fonts->count * sizeof(term->font_sizes[i][0]));

            for (size_t j = 0; j < fonts->count; j++) {
                term->font_sizes[i][j] = (struct config_font){
                    .pt_size = fonts->arr[j].pt_size, .px_size = fonts->arr[j].px_size};
            }
        }
    }

    /* Its window is gone, or going */
    if (term->shutdown.in_progress || term->window == NULL)
        return;

    if (changed & CHANGED_ALT_SCROLLING)
        term->alt_scrolling = conf->mouse.alternate_scroll_mode;
    if (changed & CHANGED_GRAPHEME_SHAPING)
        term->grapheme_shaping = conf->tweak.grapheme_shaping;

    if (changed & CHANGED_CURSOR) {
        /* Stopped first: started again, it blinks at the new rate */
        const bool decset = term->cursor_blink.decset;
        term->cursor_blink.decset = term->cursor_blink.deccsusr = false;
        term_cursor_blink_update(term);

        term->cursor_style = conf->cursor.style;
        term->cursor_blink.decset = decset;
        term->cursor_blink.deccsusr = conf->cursor.blink.enabled;
        term_cursor_blink_update(term);
    }

    if (changed & CHANGED_COLORS)
        app_apply_color_theme(term, true);

    bool layout = (changed & CHANGED_PADDING) != 0;

    if (changed & CHANGED_FONTS) {
        const int cell_width = term->cell_width;
        const int cell_height = term->cell_height;

        term_font_dpi_changed(term, term->scale);  /* dpi-aware */
        if (!term_font_size_reset(term))
            LOG_ERR("failed to load the configuration's fonts");

        /*
         * With another cell size, the window fits itself to the grid
         * (resize-keep-grid; see wayl_win_set_resize_increments()), and
         * the padding is the new one already. Else it keeps its size.
         */
        layout = term->cell_width == cell_width && term->cell_height == cell_height;
    }

    if (layout)
        window_layout_changed(term->window);

    /* What's drawn from the configuration (e.g. bold-text-in-bright) */
    if (term->window->is_configured) {
        term_damage_view(term);
        term_damage_margins(term);
        render_refresh(term);
    }
}

/*
 * What's wrong with the file (a new window prints it too). What's
 * valid in it was applied, as when foot starts. Deprecated options
 * alone aren't worth an alert each time.
 */
static void
show_problems(const user_notifications_t *notifications)
{
    static const enum user_notification_kind kinds[] = {
        USER_NOTIFICATION_ERROR, USER_NOTIFICATION_WARNING, USER_NOTIFICATION_DEPRECATED,
    };
    const NSUInteger max_lines = 8;

    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    size_t counts[ALEN(kinds)] = {0};

    for (size_t i = 0; i < ALEN(kinds); i++) {
        tll_foreach(*notifications, it) {
            if (it->item.kind != kinds[i])
                continue;

            counts[i]++;
            NSString *line = [NSString stringWithUTF8String:it->item.text];
            if (lines.count < max_lines && line != nil) {
                [lines addObject:[line stringByReplacingOccurrencesOfString:NSHomeDirectory()
                                                                 withString:@"~"]];
            }
        }
    }

    if (counts[0] + counts[1] == 0)
        return;

    const size_t more = tll_length(*notifications) - lines.count;
    if (more > 0)
        [lines addObject:[NSString stringWithFormat:@"… and %zu more", more]];

    NSAlert *alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = counts[0] > 0
        ? @"The configuration has errors" : @"The configuration has warnings";
    alert.informativeText = [NSString stringWithFormat:@"%@\n\nThe rest of it was applied.",
                             [lines componentsJoinedByString:@"\n"]];
    [alert runModal];
}

void
app_reload_config(void)
{
    struct wayland *wayl = app_wayland();

    struct config loaded;
    if (!windows_load_config(&loaded)) {
        LOG_ERR("failed to load the configuration");
        config_free(&loaded);
        return;
    }

    /* The bindings point into their configurations */
    tll_foreach(wayl->seats, it)
        key_binding_unload_keymap(wayl->key_binding_manager, &it->item);

    tll(struct replaced) replaced = tll_init();

    /* Also without a window of its own: it's what new windows fall back to */
    struct config *initial = windows_initial_config();
    tll_push_back(replaced, ((struct replaced){initial, replace(initial, &loaded, true)}));

    tll_foreach(wayl->terms, it) {
        struct config *conf = (struct config *)it->item->conf;

        bool done = false;
        tll_foreach(replaced, r)
            done = done || r->item.conf == conf;

        if (!done)
            tll_push_back(replaced, ((struct replaced){conf, replace(conf, &loaded, false)}));
    }

    tll_foreach(wayl->seats, it)
        key_binding_load_keymap(wayl->key_binding_manager, &it->item);

    tll_foreach(replaced, r) {
        if (quake_owns_config(r->item.conf))
            quake_theme_changed();

        tll_foreach(wayl->terms, it) {
            if (it->item->conf == r->item.conf)
                apply(it->item, r->item.changed);
        }
    }

    tll_free(replaced);

    app_set_quake_hotkey(initial->macos.quake.hotkey.sym, initial->macos.quake.hotkey.mods);

    LOG_INFO("the configuration was reloaded");
    app_run_hooks();

    show_problems(&loaded.notifications);
    config_free(&loaded);
}
