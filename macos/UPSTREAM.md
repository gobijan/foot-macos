# Staying in sync with upstream foot

This port tracks [upstream foot](https://codeberg.org/dnkl/foot). The
goal is that merging upstream is routine: almost all upstream work
happens in the terminal core, which this port doesn't touch.

## Branches

- `master`: identical to upstream's `master` (never commit to it).
- `main`: the port; upstream is *merged* into it (never rebased).

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
Upstream files are changed as little as possible, in one of four ways:

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

   footd on Linux (`FOOT_HEADLESS`) takes the macOS side of these (it
   has no window either), and of `slave.c`'s (below).

3. **Adapted in place**, where foot's logic is shared, and only the
   platform specific parts differ:

   | File | What's different |
   |---|---|
   | `meson.build` | A 4 line hook right after `project()`, handing over to `macos/meson.build` (on Linux too: footd only there, see below). New upstream sources/targets must be added there. |
   | `main.c` | `wayl_prepare_environment()`, `wayl_enable_new_windows()`, and `wayl_run()` instead of the `fdm_poll()` loop; SIGUSR1/SIGUSR2 switch all windows' color theme (`wayl_theme_switch_all()`); the command line's overrides are kept (`wayl_keep_config_overrides()`, before `config_load()` takes them apart), for reloading the configuration (`macos/reload.m`). |
   | `input.c`, `input.h` | Wayland keyboard/pointer listeners became `input_key()`, `input_modifiers()`, `input_pointer_*()` etc, called by `macos/view.m`. Touch support removed. Key repeat comes from macOS. Upstream changes to the `keyboard_*`/`wl_pointer_*` listeners must be ported to the corresponding `input_*` function. |
   | `render.c`, `render.h` | `wl_surface_*()` → `wayl_surface_*()`; frame callbacks → `render_frame_callback()`; xcursor themes → `wayl_set_cursor()`; presentation timing removed; the single pixel overlay is a layer color; `tiocswinsz()` hands a persistent session's size to footd (`session_link_resize()`). |
   | `terminal.c`, `terminal.h` | Timer FDs are read with `timerfd_read()` (kqueue timers, see `macos/timerfd.c`); the deferred shutdown uses a pipe (no eventfd); render workers use dispatch semaphores; font sizes use 72 DPI (points); the PTY slave is opened early (macOS quirk); scale = the window's backing scale factor; the font smoothing options are passed to fcft. The initial `TIOCSWINSZ` has the configured size, when it's in cells (footd's sessions: the window's), not 24x80: what runs in a session doesn't start with the wrong size. Persistent sessions (`macos/session.h`): `term_init()` gets footd's socket as `--pty`'s path (`/dev/fd/N`) from `session_link_new()`, the initial `TIOCSWINSZ` is only for a TTY, `term->session` is set there and freed in `term_destroy()`; a failed write to it in `fdm_ptmx()` (footd closed it) is `session_link_write_failed()`'s. |
   | `ime.c`, `ime.h` | text-input-v3 listeners → `ime_enter()`, `ime_preedit()`, `ime_commit()`, `ime_done()` (fed by `NSTextInputClient`). |
   | `config.c`, `config.h` | Font patterns parsed without fontconfig (`macos/font-pattern.c`); `macos-option-as-alt`, `macos-font-smoothing`, `macos-font-smoothing-strength`, `macos-follow-appearance`, `macos-confirm-close`, `macos-sessions` (on by default) and `macos-titlebar-style`, the quake terminal's `macos-quake-*`; the default colors (`macos/default-colors.c`); macOS default commands (`open`, `osascript`), `login-shell=yes` and `pad=8x8`. |
   | `cursor-shape.c`, `cursor-shape.h` | Wayland cursor-shape protocol mapping removed. |
   | `server.c` | No `SOCK_CLOEXEC`/`accept4()` on macOS; `footclient --quake` requests are handed to `wayl_quake()` (`macos/quake.m`), right before a terminal would be created. |
   | `client.c`, `client-protocol.h` | `footclient --quake[=ACTION]`: the action is in `client_data` (3 of its reserved bits), and the server's reply is the quake terminal's state, which is printed. |
   | `search.c` | `quirks.h` include removed. |
   | `slave.c` | `slave_exec()` restores the open files limit before `exec()` (`macos/rlimits.h`: footd raises its own; also on Linux). |
| `url-mode.c`, `url-mode.h` | `quirks.h` include removed; `urls_activate()` exposes `activate_url()`, for Cmd+click (`macos/view.m`). |
   | `notify.c`, `notify.h` | `notify_notify()` and `notify_close()` hand over to `wayl_notify()` and `wayl_notify_close()` (`macos/notifications.m`), and `notify_native_done()` is the end of a notification there; `notify_close()` doesn't `kill()` a PID of -1, i.e. every process. |
| `box-drawing.c` | Rounded corners (`╭╮╯╰`) catch an out of range `sqrt()` with `isnan()` too: macOS' libm doesn't set `errno`. |
   | `fdm.h`, `shm.h` | Additions for run loop integration, and IOSurface buffers (`macos/fdm.c`, `macos/shm.c`). |
   | `.gitignore` | The build directories (`/build/`, `/build-release/`, `/build-x86_64/`, and `-pgo` versions of the last two). |

4. **Upstream bugs, fixed in place.** Found while fuzzing the snapshots
   of persistent sessions (`meson test sessions`); each has a regression
   test in `macos/test-sessions.c` (the underlines' also in the smoke
   test, where they're drawn). These aren't sent upstream. If upstream
   fixes one itself, take upstream's fix and drop the fork's.

   | File | The bug | The fix |
   |---|---|---|
   | `terminal.c` (`term_restore_cursor()`) | With origin mode (DECOM), DECRC, and leaving the alternate screen (1049), could put the cursor outside the scrolling region: the next line feed or reverse index failed an assertion (debug builds), or moved it wrongly. | The row is kept within the region. |
   | `terminal.c`, `terminal.h` (`term_save_cursor()`, `struct grid`) | DECSC saved the underline attribute, but not the underline's style and color: after DECRC, text could be underlined without a style, and drawing it was a `BUG()` (foot exited). | They're saved (`saved_underline`) and restored with the attributes. |
   | `grid.c`, `grid.h` (`grid_row_ranges_move()`), `csi.c` (ICH, DCH), `terminal.c` (`print_insert()`) | Inserting and deleting characters (also in insert mode) moved the cells, but not the row's URI and underline ranges: links and underline styles ended up on other text, and an underlined character moved into a range with an underline color only was a `BUG()` when drawn (foot exited). | The ranges move with the cells. |
   | `grid.c` (`grid_resize_and_reflow()`) | A composed wide character cut off in the last column (printed there without auto-wrap, e.g. 👍🏽, or one that got wide there by composing, as ❤️ does) was copied with its whole width when reflowing: a read past the row's end. | The width is limited to what's left of the row. |
   | `terminal.c` (`term_init()`) | When configuring the PTY, or registering the timers, failed, `term_destroy()` ran on the terminal before it was initialized (a crash; in footd, of all sessions). | Those failures go to `close_fds`; the key bindings are referenced right after them. |

footd (`macos/footd.c`, persistent sessions) runs foot's terminal core
without a window (`macos/headless.c`): what the core calls into is
stubbed there, as in upstream's `pgo/pgo.c`. When upstream's core calls
a new platform function, footd needs a stub too (it fails to link
otherwise); when `render_resize()`'s grid handling changes,
`headless_term_resize()` must follow. New terminal state (modes,
colors) belongs in the snapshot a window gets when it attaches
(`macos/snapshot.c`): `meson test sessions` compares terminals before
and after one, also with random output, and tells what's missing.

footd also builds on Linux, for servers foot reaches over SSH:
`macos/meson.build` builds only footd and its tests there, and
`make linux` builds and tests it in a container (`macos/linux.sh`). The
terminal core is this port's (with its macOS changes), so what it uses
of macOS has stand-ins in `macos/compat-linux/` (libdispatch's
semaphores, `timerfd_read()`, `IOSurfaceRef`), and upstream's `fdm.c`
is used instead of `macos/fdm.c`. When the core starts using more of
macOS, `make linux` fails until there's a stand-in.

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
   config sessions footd`), then `make run` (debug builds run foot's
   unit tests at startup).
5. **Update this file** if the way the port touches upstream changes.

To see the port's complete diff against upstream (`make port-diff`
prints a summary):

```sh
git diff master...main -- . ':!macos' ':!.github' ':!Makefile' ':!AGENTS.md' ':!CLAUDE.md'
```
