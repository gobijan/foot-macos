#pragma once

/*
 * The configuration file, as new windows (macos/windows.c) load it.
 */

#include <stdbool.h>

struct config;

/* Loads the configuration ('-c', or the default location) */
bool windows_load_config(struct config *conf);

/*
 * The configuration foot was started with: the first window's, or the
 * server's (not a const object: main.c's). Only it has the command
 * line's overrides (-o etc.), which windows_apply_overrides() applies.
 */
struct config *windows_initial_config(void);
void windows_apply_overrides(struct config *conf);

/* The configuration's path; free() it */
char *windows_settings_path(void);

/* Creates the configuration, from the documented defaults, if it doesn't exist */
bool windows_create_settings(const char *path);
