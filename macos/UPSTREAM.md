# Staying in sync with upstream foot

This port tracks [upstream foot](https://codeberg.org/dnkl/foot). The
goal is that merging upstream is routine: almost all upstream work
happens in the terminal core, which this port doesn't touch.

## Branches

- `master`: identical to upstream's `master` (never commit to it).
- `macos`: the port; upstream is *merged* into it (never rebased).

## Syncing

```sh
make upstream   # what's new upstream
make sync       # macos/sync-upstream.sh
```

The script fetches upstream, prints what's coming (and flags upstream
changes to platform code, see below), merges, builds and runs the
tests. `git rerere` is enabled, so a conflict resolution is recorded
and re-applied automatically if the same conflict shows up again.

In addition, the *Upstream sync* GitHub workflow tries the merge every
Monday. If it is clean, and the result builds and passes the tests, it
opens (or updates) a pull request. Otherwise it opens an issue; resolve
it locally with the script.

Sync often: weekly syncs rarely conflict at all. As a reference, at
the time of writing, merging half a year of upstream (109 commits) in
one go produced 5 conflicts, all one-liners next to code adapted for
macOS.

## How the port touches upstream files

New code lives in `macos/`; the port's other own files are `.github/`,
`Makefile`, `AGENTS.md` and `CLAUDE.md` (none of them exist upstream).
Upstream files are changed as little as possible, in one of three ways:

1. **Not built on macOS, left untouched.** Upstream's Wayland backend
   is kept as is, so it never conflicts: `wayland.c`, `shm.c`,
   `shm-formats.h`, `quirks.c`, `quirks.h`, the `.desktop` files, the
   systemd units and `pgo/`. When upstream changes these, check whether
   the macOS counterpart in `macos/` needs the same change (the sync
   script lists such commits).

2. **Upstream code kept, but compiled out** with
   `#if !defined(__APPLE__)` / `#if defined(__APPLE__)`:
   - `wayland.h`: dispatches to `macos/wayland.h` (the same interface,
     implemented in `macos/*.m`). If upstream appends declarations at
     the end of the file, keep them *above* the final `#endif`. New
     `wayl_*()` functions need a macOS implementation.
   - `selection.c`, `selection.h`: the Wayland clipboard, primary
     selection and drag-and-drop code. macOS: `macos/selection-clipboard.c`.

3. **Adapted in place**, where foot's logic is shared, and only the
   platform specific parts differ:

   | File | What's different |
   |---|---|
   | `meson.build` | A 6 line hook right after `project()`, handing over to `macos/meson.build`. New upstream sources/targets must be added there. |
   | `main.c` | `wayl_prepare_environment()`, `wayl_enable_new_windows()`, and `wayl_run()` instead of the `fdm_poll()` loop; SIGUSR1/SIGUSR2 switch all windows' color theme (`wayl_theme_switch_all()`). |
   | `input.c`, `input.h` | Wayland keyboard/pointer listeners became `input_key()`, `input_modifiers()`, `input_pointer_*()` etc, called by `macos/view.m`. Touch support removed. Key repeat comes from macOS. Upstream changes to the `keyboard_*`/`wl_pointer_*` listeners must be ported to the corresponding `input_*` function. |
   | `render.c`, `render.h` | `wl_surface_*()` → `wayl_surface_*()`; frame callbacks → `render_frame_callback()`; xcursor themes → `wayl_set_cursor()`; presentation timing removed; the single pixel overlay is a layer color. |
   | `terminal.c`, `terminal.h` | Timer FDs are read with `timerfd_read()` (kqueue timers, see `macos/timerfd.c`); the deferred shutdown uses a pipe (no eventfd); render workers use dispatch semaphores; font sizes use 72 DPI (points); the PTY slave is opened early (macOS quirk); scale = the window's backing scale factor; the font smoothing options are passed to fcft. |
   | `ime.c`, `ime.h` | text-input-v3 listeners → `ime_enter()`, `ime_preedit()`, `ime_commit()`, `ime_done()` (fed by `NSTextInputClient`). |
   | `config.c`, `config.h` | Font patterns parsed without fontconfig (`macos/font-pattern.c`); `macos-option-as-alt`, `macos-font-smoothing`, `macos-font-smoothing-strength`, `macos-follow-appearance` (on by default); the default colors (`macos/default-colors.c`); macOS default commands (`open`, `osascript`) and `login-shell=yes`. |
   | `cursor-shape.c`, `cursor-shape.h` | Wayland cursor-shape protocol mapping removed. |
   | `server.c` | No `SOCK_CLOEXEC`/`accept4()` on macOS. |
   | `search.c`, `url-mode.c` | `quirks.h` include removed. |
   | `box-drawing.c` | Rounded corners (`╭╮╯╰`) catch an out of range `sqrt()` with `isnan()` too: macOS' libm doesn't set `errno`. |
   | `fdm.h`, `shm.h` | Additions for run loop integration, and IOSurface buffers (`macos/fdm.c`, `macos/shm.c`). |

Not modified at all: everything else, including `foot.ini`,
`meson_options.txt`, `README.md`, `CONTRIBUTING.md` and `tests/` (this
fork's README and CONTRIBUTING live in `.github/`; macOS specific
defaults for the generated configuration template are applied by
`macos/embed-file.py`).

## Resolving conflicts

For humans and AI assistants alike:

1. **Upstream wins, then re-apply the port.** Take upstream's version of
   the conflicting hunk, and re-apply the macOS adaptation on top of it
   (e.g. a new `wl_surface_commit()` becomes `wayl_surface_commit()`, a
   timer FD `read()` becomes `timerfd_read()`). Don't drop upstream
   changes.
2. **Keep the diff small.** Don't reformat or reorder upstream code. If
   something new is platform specific, put it in `macos/`, and hook it
   in with as few lines as possible.
3. **Mirror platform changes.** If upstream changes Wayland specific
   code (the files in group 1, or code in `#if !defined(__APPLE__)`
   blocks), decide whether the macOS layer needs the equivalent.
4. **Verify.** `make test` (`ninja -C build && meson test -C build
   config`), then `make run` (debug builds run foot's unit tests at
   startup).
5. **Update this file** if the way the port touches upstream changes.

To see the port's complete diff against upstream (`make port-diff`
prints a summary):

```sh
git diff master...macos -- . ':!macos' ':!.github' ':!Makefile' ':!AGENTS.md' ':!CLAUDE.md'
```
