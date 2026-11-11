#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Shell integration, without the user setting it up: zsh, bash and
 * fish are started in a way that they read foot's script for them
 * (macos/shell-integration/), which marks the prompts and the commands'
 * output (OSC 133), and reports the working directory (OSC 7). That's
 * what jumping to a prompt, piping a command's output, clearing above
 * the prompt, and new windows in the same directory need.
 *
 * - zsh reads ZDOTDIR's .zshenv: foot's, which reads the user's.
 * - bash reads $ENV in POSIX mode (--posix), and nothing else: foot's
 *   script leaves that mode, and reads bash's own startup files. Not
 *   for macOS' /bin/bash (3.2), which doesn't read $ENV then.
 * - fish reads vendor_conf.d in XDG_DATA_DIRS' folders. (fish 4 has
 *   the integration itself; the script is for older ones.)
 *
 * The scripts put the environment back as it was. They are part of the
 * binary (footd on another machine is one file), and are written to
 * foot's folder in Application Support (Linux: the state folder), in a
 * folder named by their contents: another version's are left alone.
 *
 * macos-shell-integration=no turns it off.
 */

#define SHELL_INTEGRATION_MAX_VARS 3

struct shell_integration_var {
    const char *name;
    char *value;  /* malloc:ed */
};

/*
 * Before a terminal's program is started (term_init()): whether its
 * shell gets the integration. Writes the scripts, if they aren't there.
 */
void shell_integration_prepare(bool enabled);

/*
 * In the child (slave_spawn()), with what it's about to execute: the
 * variables to add to its environment (their number), if it's a shell
 * with a script. *argv may be replaced, by one with another argument.
 */
size_t shell_integration_apply(
    char ***argv, char *const *env,
    struct shell_integration_var vars[static SHELL_INTEGRATION_MAX_VARS]);
