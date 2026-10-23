#include "default-colors.h"

#include <string.h>

#include "config.h"

struct palette {
    uint32_t fg, bg;
    uint32_t cursor_text, cursor;
    uint32_t selection_fg, selection_bg;
    uint32_t table[16];  /* regular0-7, bright0-7 */
};

/* Colors from xai-org/grok-build (Apache-2.0) */
static const struct palette grok_night = {
    .fg = 0xe1e1e1,
    .bg = 0x0a0a0a,
    .cursor_text = 0x0a0a0a,
    .cursor = 0xbb9af7,
    .selection_fg = 0xe1e1e1,
    .selection_bg = 0x323237,
    .table = {
        0x111111, 0xf7768e, 0x9ece6a, 0xe0af68,
        0x7aa2f7, 0xbb9af7, 0x1abc9c, 0xc8c8c8,
        0x6c6c6c, 0xf7768e, 0x8bc34a, 0xff9e64,
        0x7aa2f7, 0x9d7cd8, 0x3a95ab, 0xe1e1e1,
    },
};

/* White (7, 15) is dark gray, to stay readable on the light background */
static const struct palette grok_day = {
    .fg = 0x262626,
    .bg = 0xf5f5f5,
    .cursor_text = 0xf5f5f5,
    .cursor = 0x7d4bc6,
    .selection_fg = 0x262626,
    .selection_bg = 0xd4d4d8,
    .table = {
        0x262626, 0xcd3048, 0x378e23, 0xa27612,
        0x2f64d2, 0x7d4bc6, 0x0a8e70, 0x444444,
        0x767676, 0xcd3048, 0x4caf50, 0xc3691e,
        0x2f64d2, 0x6c3eb2, 0x0f87a2, 0x262626,
    },
};

static void
apply(struct color_theme *theme, const struct palette *p)
{
    theme->fg = p->fg;
    theme->bg = p->bg;
    theme->cursor.text = p->cursor_text;
    theme->cursor.cursor = p->cursor;
    theme->use_custom.cursor = true;
    theme->selection_fg = p->selection_fg;
    theme->selection_bg = p->selection_bg;
    memcpy(theme->table, p->table, sizeof(p->table));
}

void
default_colors(struct color_theme *dark, struct color_theme *light)
{
    apply(dark, &grok_night);
    apply(light, &grok_day);
}

static void
loaded(struct color_theme *theme, const struct palette *p)
{
    if (theme->bg == p->bg)
        return;

    if (theme->use_custom.cursor &&
        theme->cursor.text == p->cursor_text && theme->cursor.cursor == p->cursor)
    {
        theme->use_custom.cursor = false;
        theme->cursor.text = theme->cursor.cursor = 0;
    }

    /* foot's defaults: the default background/foreground */
    if (theme->selection_fg == p->selection_fg)
        theme->selection_fg = 0x80000000;
    if (theme->selection_bg == p->selection_bg)
        theme->selection_bg = 0x80000000;
}

void
default_colors_loaded(struct color_theme *dark, struct color_theme *light)
{
    loaded(dark, &grok_night);
    loaded(light, &grok_day);
}
