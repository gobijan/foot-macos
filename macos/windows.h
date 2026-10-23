#pragma once

/*
 * The configuration file, as new windows (macos/windows.c) load it.
 */

#include <stdbool.h>

struct config;

/* Loads the configuration ('-c', or the default location) */
bool windows_load_config(struct config *conf);

/* The configuration's path; free() it */
char *windows_settings_path(void);

/* Creates the configuration, from the documented defaults, if it doesn't exist */
bool windows_create_settings(const char *path);
