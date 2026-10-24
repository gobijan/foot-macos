# foot for macOS

[![Sponsor](https://img.shields.io/badge/Sponsor-gobijan-ea4aaa?logo=githubsponsors&logoColor=white)](https://github.com/sponsors/gobijan)

A native macOS port of [foot](https://codeberg.org/dnkl/foot), the fast,
lightweight and minimalistic terminal emulator. The port is developed
and maintained by [Bijan Rahnema](https://github.com/gobijan); it is
unofficial, and not affiliated with upstream foot.

**Status:** work in progress. Usable day to day, but expect rough edges.

foot's terminal core (VT parser, grid, sixel, search, URL mode etc) is
unchanged. Only the platform layer is new, and it is native - no
Wayland compositor, no X11, no emulation:

| | Upstream (Linux) | macOS |
|---|---|---|
| Windows, input | Wayland | AppKit |
| Presentation | `wl_shm` buffers | IOSurfaces as `CALayer` contents (zero copy) |
| Frame pacing | frame callbacks | `CADisplayLink` (paused when idle or hidden) |
| Event loop | epoll | kqueue, integrated with the main run loop |
| Fonts | fcft (fontconfig, FreeType, HarfBuzz) | fcft API implemented on CoreText |
| Keyboard | xkbcommon keymap from the compositor | xkbcommon keymap generated from the macOS layout |
| Input methods | text-input-v3 | `NSTextInputClient` (dead keys, IMEs) |
| Clipboard | Wayland data devices | `NSPasteboard` |

Rendering is still done on the CPU, with pixman, exactly like upstream
foot; that is what keeps it lean (an idle window uses ~0% CPU, with no
wakeups, and ~20MB of memory).

## Supporting the port

The port is a lot of ongoing work: the native platform layer, signed
and notarized releases, automatic updates, the Homebrew cask, and
keeping up with upstream foot every week. If you use foot on your Mac,
please consider [sponsoring its development](https://github.com/sponsors/gobijan).

Sponsorships go to the macOS port, not to upstream foot.

## Installing

With [Homebrew](https://brew.sh):

```sh
brew install --cask gobijan/tap/foot
```

Or download `foot-<version>.zip` from the
[latest release](https://github.com/gobijan/foot-macos/releases/latest),
and move `foot.app` to Applications. It's a universal app (Apple
silicon and Intel) for macOS 14 and later, signed and notarized.

foot updates itself (foot > Check for Updates…, with
[Sparkle](https://sparkle-project.org)); on the second launch, it asks
whether to check automatically, once a day. Homebrew's cask also puts
`foot` and `footclient` in your `$PATH`.

## Building

Requirements: Xcode (or the command line tools), and
`brew install meson ninja bison` (or `make deps`).

```sh
make install    # builds foot.app, and installs it to /Applications
```

`make` lists all commands: building and running the debug and release
builds, tests, upstream syncs, CI. The Makefile is only a frontend for
meson; without it:

```sh
meson setup build --native-file macos/native.ini --buildtype=release
ninja -C build
cp -R build/macos/foot.app /Applications/
```

This builds `foot.app` (with `foot` and `footclient` inside
`foot.app/Contents/MacOS/`). Without installing it, run
`./build/macos/foot` directly.

All dependencies (pixman, utf8proc, xkbcommon) are built from source,
and linked statically; the resulting binary only depends on system
frameworks.

## macOS specifics

* `Cmd+C` / `Cmd+V` copy and paste, `Cmd+N` opens a new window (in the
  same process, and in the current window's working directory), `Cmd+,`
  opens the configuration in `$EDITOR`, `Cmd+=` / `Cmd+-` / `Cmd+0`
  change the font size, `Ctrl+Cmd+F` toggles fullscreen. foot's own key
  bindings (e.g. `Ctrl+Shift+C`) still work.
* Like other Mac apps, foot keeps running when its last window is
  closed, until `Cmd+Q`; the Dock icon opens a new window. Started from
  a shell, foot exits with its last window, as on Linux.
* The Option key produces characters (like on any Mac). To use it as
  Alt/Meta, set `macos-option-as-alt=left|right|both` in `[main]`.
* The configuration is read from `~/.config/foot/foot.ini`. New windows
  re-read it, so changes apply without restarting foot.
* Font sizes are in points, like in other macOS applications. The
  default font is SF Mono (`monospace`), 13pt. Icons from installed Nerd
  Fonts are used automatically.
* Glyphs are thickened (font smoothing) like macOS' own text rendering,
  which matters most on non-Retina displays. Tune it with
  `macos-font-smoothing-strength=0.0-1.0`, or turn it off with
  `macos-font-smoothing=no`, in `[main]`.
* The default colors are Grok Night (dark) and Grok Day (light), the
  palette of xAI's Grok
  ([grok-build](https://github.com/xai-org/grok-build), Apache-2.0).
* Color themes: pick one in View > Theme. foot's themes (Catppuccin,
  Dracula, Gruvbox, Nord, Solarized, Tokyo Night and 80 more, and Grok)
  are bundled in `foot.app/Contents/Resources/themes/`; the menu writes
  the `include=` line to `foot.ini` (and leaves the rest of the file as
  it is), and all windows switch right away. Default Colors removes it
  again. Colors set in `foot.ini` itself take precedence. Hundreds more
  themes, in foot's format, are in the `foot/` directory of
  [iTerm2-Color-Schemes](https://github.com/mbadolato/iTerm2-Color-Schemes).
* foot follows the system's light/dark mode, with a dark and a light
  theme (View > Theme > Follow System Appearance,
  `macos-follow-appearance=yes` in `[main]`, on by default): a theme
  with dark colors only replaces Grok Night, one with light colors only
  Grok Day. For one theme in both modes, turn it off. Programs that ask
  for theme changes (mode 2031, e.g. Neovim) are notified.
* The shell is started as a login shell, so that it sets up `$PATH`.
* `TERM=foot`; the terminfo entry is bundled, and found through
  `$TERMINFO_DIRS`.

## Everything else

The terminal itself works as in upstream foot: key bindings, scrollback
search, URL mode, server mode (`footclient`), shell integration,
control sequences and all configuration options. See foot's
[README](https://codeberg.org/dnkl/foot/src/branch/master/README.md),
and its man pages: [foot(1)](../doc/foot.1.scd),
[foot.ini(5)](../doc/foot.ini.5.scd),
[footclient(1)](../doc/footclient.1.scd) and
[foot-ctlseqs(7)](../doc/foot-ctlseqs.7.scd). Wayland specific parts
(compositors, fractional scaling, touchscreens) don't apply on macOS.

## Contributing

AI-assisted contributions are explicitly welcome, unlike upstream:
patches, bug reports and comments alike. See
[CONTRIBUTING.md](CONTRIBUTING.md).

## Bugs

Please report bugs to https://github.com/gobijan/foot-macos/issues, not
upstream.

Before you open a new issue, please search existing bug reports, both
open **and** closed ones. Chances are someone else has already
reported the same issue.

The report should contain the following:

- Foot version (`foot --version`).
- Log output from foot (run `foot -d info` from another terminal).
- Which macOS version you are running (`sw_vers`).
- If reporting a crash, please try to provide a `bt full` backtrace
  with symbols.
- Steps to reproduce. The more details the better.

## Staying in sync with upstream

The port is designed to make merging upstream foot routine: new code
lives in `macos/`, and upstream files are touched as little as
possible. A weekly GitHub workflow merges upstream, and opens a pull
request when the result builds and passes the tests. See
[macos/UPSTREAM.md](../macos/UPSTREAM.md).

## Credits

foot is written by [Daniel Eklöf](https://codeberg.org/dnkl) and
contributors. The app icon is based on foot's logo, designed by
[Ordoviz](https://codeberg.org/Ordoviz).

## License

MIT, like upstream foot; see [LICENSE](../LICENSE).
