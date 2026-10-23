#pragma once

struct color_theme;

/*
 * foot for macOS' default colors: Grok Night (dark) and Grok Day
 * (light), the same as macos/themes/grok (grok-night, grok-day).
 * Applied to the built-in defaults, before foot.ini is loaded.
 */
void default_colors(struct color_theme *dark, struct color_theme *light);

/*
 * After foot.ini is loaded: a theme with a background of its own, but
 * without cursor or selection colors, gets foot's usual ones (inverse
 * foreground/background) instead of Grok's.
 */
void default_colors_loaded(struct color_theme *dark, struct color_theme *light);
