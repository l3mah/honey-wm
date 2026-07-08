# honey

A lean tiling Wayland compositor built directly on [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots).
Master-stack workflow, unlimited per-monitor workspaces, and a runtime control
socket that doubles as the entire configuration system. Written in C, roughly
6,500 lines, no dependencies beyond wlroots and what it already pulls in.

honey is a window manager, not a desktop environment. It tiles windows, routes
input, draws borders, and stays out of the way. The one comfort it ships beyond
strict window management is a built-in night light, because gamma control
belongs in the compositor.

**Status:** feature-complete and in daily use, pre-1.0. Version numbers track
the wlroots line honey is built against: honey 0.20.x requires wlroots 0.20.

---

**Contents** —
[About](#about) ·
[Features](#features) ·
[Installation](#installation) ·
[Configuration](#configuration) ·
[Command reference](#command-reference) ·
[Status stream](#the-status-stream) ·
[Protocol support](#wayland-protocol-support) ·
[XWayland & HiDPI](#xwayland--hidpi) ·
[Waybar](#waybar-modules) ·
[Acknowledgements](#acknowledgements) ·
[License](#license)

---

## About

honey succeeds [lakewm](https://github.com/l3mah/lakewm), a window manager that
ran as a *client* of the river compositor. That design had a ceiling: a river
client cannot advertise Wayland globals, so whole classes of fixes were
structurally impossible — most painfully server-side decorations for GTK.
GTK3 and GTK4 only speak the KDE `server-decoration` protocol, river only
offers `xdg-decoration`, and so every GTK app drew its own headerbar forever.

honey removes the ceiling by being the compositor. It advertises both
decoration protocols (GTK apps lose their headerbars, as intended), owns
output configuration, gamma, tearing, and everything else a WM built on
someone else's compositor has to beg for.

The architecture keeps three rules, inherited from the best parts of
[dwl](https://codeberg.org/dwl/dwl) and enforced everywhere:

- **The wlroots object is the state.** No shadow copies, no reconciliation.
- **Layouts are stateless.** Every layout is a pure function of the window
  list and its parameters — no layout trees, no lifecycle hooks. Swapping,
  cycling, and drop-at-cursor come for free.
- **The command language is the config language.** There is no configuration
  file format. The config is a shell script of `honeyctl` calls, and every
  single one of them also works at runtime, live.

## Features

- **Layouts:** master-stack (adjustable ratio, count, and orientation — master
  area on any of the four sides), spiral (Fibonacci), and grid. Per-workspace
  layout and parameter overrides.
- **Workspaces:** unlimited, created on demand, per-monitor, addressable as
  `output:N` from any command. Nameable. Back-and-forth switching.
- **Multi-monitor:** native output configuration (mode, position, scale,
  transform, adaptive-sync, DPMS), plus `wlr-output-management` so `wlr-randr`
  and `kanshi` work too. Directional focus and window movement across
  monitors.
- **Decorations:** both `xdg-decoration` and KDE `server-decoration`, so GTK,
  Qt, and everything else gets consistent server-side decoration — honey draws
  a border, nothing more.
- **Window states:** floating, fullscreen, maximize, fake-fullscreen; client
  fullscreen requests honored; maximize requests from tiled windows ignored
  (the layout owns them), with a rule to suppress even the attempt.
- **Window rules:** match `app-id`, `title`, or `initial-title`, exact or
  POSIX regex → send to workspace, float (optionally sized), force tile,
  suppress maximize, or map unfocused.
- **Pointer workflow:** click-to-focus and focus-follows-mouse, mouse-follows-
  keyboard-focus warping, super+drag move, super+right-drag resize,
  drag-a-border resize, drop-at-cursor re-tiling, super+scroll workspace
  cycling — each individually toggleable.
- **Effects, minimal by intent:** per-window active/inactive opacity and an
  inactive-dim overlay. No animations, no blur, no rounded corners.
- **Night light:** built-in gamma control — color temperature and brightness
  per hardware LUT, with clamping, relative adjustments, and a live event on
  the status stream so bars stay in sync no matter what changed it.
- **XWayland:** rootless, started lazily on the first X11 client. Dialogs,
  menus, and fixed-size windows float automatically. Optional per-monitor
  HiDPI scaling — see [XWayland & HiDPI](#xwayland--hidpi).
- **Bars:** works with stock waybar out of the box (`ext/workspaces`,
  `wlr/taskbar`), and ships [native waybar modules](#waybar-modules) for
  occupied/empty workspace state, the focused title, and the night light.
- **Config & tooling:** everything above is set over one Unix socket with a
  one-word-per-concept grammar; `honeyctl reload` re-runs your init and shows
  errors in a floating terminal; a JSON status stream feeds custom tooling.

## Installation

### Packages

Not packaged anywhere yet — distribution packages will land after the first
tagged release. This section will fill in as they do.

#### Void Linux

No binary package yet. A ready-to-build [xbps template](contrib/void/template)
ships in this repo: copy it into a `void-packages` checkout as
`srcpkgs/honey/template` and run `./xbps-src pkg honey`.

#### Arch Linux

*Not yet available.*

#### Debian / Ubuntu (.deb)

*Not yet available.*

#### Fedora / openSUSE (.rpm)

*Not yet available.*

### From a release tarball

```sh
curl -LO https://github.com/l3mah/honey-wm/archive/refs/tags/v0.20.0.tar.gz
tar xf v0.20.0.tar.gz && cd honey-wm-0.20.0
make
sudo make install PREFIX=/usr/local    # or PREFIX=$HOME/.local, no sudo
```

### From git

```sh
git clone https://github.com/l3mah/honey-wm.git
cd honey-wm
make
make install    # installs to ~/.local/bin by default
```

`make install` honors `PREFIX` (default `$HOME/.local`) and `DESTDIR`.

### Build dependencies

| Library | Notes |
|---|---|
| wlroots 0.20.x | pkg-config name `wlroots-0.20` |
| wayland (server) | `wayland-server` |
| wayland-protocols + wayland-scanner | generates the two vendored protocol headers |
| libxkbcommon | keyboard layouts |
| pixman | |
| libinput | direct device configuration |
| libdrm | |
| libxcb + xcb-util-wm | `xcb`, `xcb-ewmh`, `xcb-icccm` — XWayland window management |
| pkg-config, a C11 compiler, make | |

Runtime: `xkeyboard-config` (keymap data), and the `Xwayland` binary if you
want X11 apps — it's optional and only started when an X11 client appears.

On a system with Nix, the whole toolchain can be ephemeral:

```sh
nix-shell -p wlroots wayland wayland-protocols wayland-scanner libxkbcommon \
  pixman libinput libdrm libxcb xcbutilwm pkg-config gcc gnumake --run make
```

### Running it

Start `honey` from a TTY (a seat manager — systemd-logind, elogind, or seatd —
must be available, as with any wlroots compositor), or nested inside another
Wayland session for testing. Two binaries are involved:

- `honey` — the compositor. `honey -c <path>` uses an alternate config; the
  path is remembered, so `reload` re-runs the same file.
- `honeyctl` — the control client. Talks to
  `$XDG_RUNTIME_DIR/honey-$WAYLAND_DISPLAY.sock`.

honey sets `XDG_CURRENT_DESKTOP=honey`, `XDG_SESSION_TYPE=wayland`, and
`DISPLAY` (once XWayland starts) for everything it spawns. Set
`HONEY_DEBUG=1` for verbose logging to stderr.

## Configuration

There is no config file format. At startup honey runs
`~/.config/honey/init` — a plain shell script of `honeyctl` calls:

```sh
#!/bin/sh

# outputs
honeyctl output DP-3 mode 2560x2880@60 pos 0,0 scale 1.25
honeyctl output DP-1 mode 3840x2160@60 pos 2048,0 scale 1.5
honeyctl output DP-2 mode 2560x1440@60 pos 4608,0

# appearance
honeyctl set gaps-in 5
honeyctl set gaps-out 8
honeyctl set border-size 2
honeyctl set border-color-active 0xBD93F9FF

# input
honeyctl kb-layout us
honeyctl kb-repeat 25 600
honeyctl input "*" natural-scroll on

# keys
honeyctl map super+Return "spawn alacritty"
honeyctl map super+shift+q close
honeyctl map super+j focus-next
honeyctl map super+k focus-prev
honeyctl map super+0 "workspace 10"

# rules
honeyctl rule app-id mpv float
honeyctl rule title-re "^Picture-in-Picture$" float 30% 30%

# autostart
waybar &
swaybg -i ~/wallpaper.png &
```

Three properties fall out of this design:

- **Live reconfiguration is not a feature, it's the default.** Every line of
  the config is a runtime command. Change a gap width, remap a key, add a rule
  — from a terminal, a script, or a bar widget, and it applies immediately.
- **`reload` is trivial.** `honeyctl reload` just re-runs the script. If any
  line fails, a floating terminal pops up listing the errors (`set
  error-window false` sends them to
  `$XDG_RUNTIME_DIR/honey-init-errors.log` instead). The same happens at
  startup, so a typo in your init never fails silently.
- **Tools are easy to build on.** Anything that can write to a Unix socket can
  drive honey; anything that can read one can follow it (see
  [the status stream](#the-status-stream)). The
  [waybar modules](#waybar-modules) are ~700 lines total for that reason.

Since it's a shell script, loops, conditionals, and external tools all work —
your "config format" is whatever your shell can do.

If no init file exists, honey loads a
[default set of keybindings](#default-keybindings) so it's usable out of the
box.

`honeyctl` prints nothing on success, query results to stdout, and errors to
stderr with a non-zero exit code.

## Command reference

Every command below is a `honeyctl` invocation. Multi-word arguments (like a
`spawn` command line) can be quoted at the shell level.

### Settings — `set` and `get`

`set <key> <value>` changes a global setting and re-arranges; `get <key>`
prints the current value. Booleans accept `true/false`, `on/off`, `yes/no`,
`1/0`. Colors are `0xRRGGBBAA`.

#### Layout

| Key | Default | Meaning |
|---|---|---|
| `layout` | `master` | global layout: `master`, `spiral`, or `grid` |
| `master-mfact` | `0.55` | master area fraction (0.05–0.95) |
| `master-nmaster` | `1` | number of windows in the master area |
| `master-orientation` | `left` | master side: `left`, `right`, `top`, `bottom` |
| `spiral-ratio` | `0.5` | split ratio per spiral step (0.1–0.9) |
| `spiral-first-split` | `horizontal` | first split direction: `horizontal` or `vertical` |
| `grid-columns` | `0` | grid columns; `0` = automatic (square-ish) |
| `gaps-in` | `5` | pixels between windows |
| `gaps-out` | `8` | pixels at the screen edge |
| `smart-gaps` | `true` | drop gaps and border when one window is alone |

#### Appearance

| Key | Default | Meaning |
|---|---|---|
| `border-size` | `1` | border width in pixels |
| `border-color-active` | `0xBD93F9FF` | focused window border |
| `border-color-inactive` | `0x595959AA` | unfocused window border |
| `active-opacity` | `1.0` | focused window opacity (0–1) |
| `inactive-opacity` | `1.0` | unfocused window opacity (0–1) |
| `dim-inactive` | `0.0` | darken unfocused windows by this strength (0–1) |
| `cursor-theme` | *(system)* | xcursor theme; also exported as `XCURSOR_THEME` |
| `cursor-size` | `24` | cursor size; also exported as `XCURSOR_SIZE` |

#### Floating

| Key | Default | Meaning |
|---|---|---|
| `float-width` | `0.33` | default float width — fraction of the output, values > 1 read as percent |
| `float-height` | `0.5` | default float height |
| `float-app-size` | `false` | let apps pick their own floating size instead |

#### Behavior

| Key | Default | Meaning |
|---|---|---|
| `follow-mouse` | `true` | keyboard focus follows the pointer |
| `mouse-follows-focus` | `true` | warp the pointer to keyboard-focus changes |
| `warp-on-workspace-switch` | `false` | also warp when switching workspaces (off so a bar click doesn't yank the cursor) |
| `new-window-master` | `false` | new windows become master instead of joining the stack tail |
| `focus-new` | `true` | focus newly opened windows |
| `mouse-focus-new` | `false` | warp the pointer to newly opened windows |
| `exit-fullscreen-on-new` | `true` | leave fullscreen when another window opens |
| `drop-at-cursor` | `true` | dragging a tiled window re-tiles it where dropped |
| `resize-on-border` | `true` | dragging a window border resizes without a modifier |
| `scroll-workspace` | `true` | super+scroll cycles workspaces |
| `allow-tearing` | `false` | honor tearing hints for fullscreen clients (gaming) |
| `error-window` | `true` | show init/reload errors in a floating terminal |
| `xwayland-scale` | `off` | X11 HiDPI mode: `off`, `auto`, or a scale 1.0–4.0 — see [XWayland & HiDPI](#xwayland--hidpi) |

### Per-workspace overrides — `set-ws`

```
set-ws <output:N|N> <key> <value>
```

Overrides `layout`, `master-mfact`, `master-nmaster`, or `master-orientation`
for one workspace; everything else stays global. A bare `N` means workspace N
on the focused output. The live-tweak actions (`master-mfact 0.05` on a key,
for instance) write the focused workspace's override, so each workspace
remembers its own shape.

### Actions

Actions are single verbs usable in three places interchangeably: bound to a
key (`map super+j focus-next`), sent directly (`honeyctl focus-next`), or run
from a script. Directions are `left`, `right`, `up`, `down`.

| Action | Effect |
|---|---|
| `spawn <command...>` | run a command (inherits honey's environment) |
| `close` | close the focused window |
| `exit` | quit honey |
| `focus-next` / `focus-prev` | cycle focus within the workspace |
| `focus-dir <dir>` | focus the nearest window in a direction, crossing monitors; falls back to the adjacent output |
| `focus-output <name>` | focus a monitor by connector name and warp the cursor to it |
| `swap-next` / `swap-prev` | swap the focused tiled window along the stack |
| `swap-master` | swap the focused window with the master |
| `swap-dir <dir>` | swap with the nearest tiled window in a direction |
| `move-to-output <dir>` | send the focused window to the adjacent monitor |
| `workspace <N>` | switch to workspace N on the focused output |
| `move-to-workspace <N>` | send the focused window to workspace N |
| `workspace-back` | switch to the previously active workspace |
| `workspace-next` / `workspace-prev` | cycle through existing workspaces |
| `toggle-float` | toggle floating |
| `fullscreen` | toggle fullscreen |
| `maximize` | toggle maximize (fills the usable area, keeps bars visible) |
| `fake-fullscreen` | the client renders fullscreen UI but stays tiled |
| `master-mfact <±delta>` | grow/shrink the master area (e.g. `0.05`, `-0.05`) |
| `master-nmaster <±delta>` | add/remove master slots |
| `master-orientation <dir>` | set the master side |
| `master-orientationcycle` | rotate the master side |

### Keybindings — `map`, `unmap`, `binds`

```
map <combo> <action>
unmap <combo>
binds
```

A combo is modifiers plus one key, joined with `+`: `super+shift+q`,
`ctrl+alt+F1`. Modifier names: `super` (also `logo`/`mod4`), `shift`, `ctrl`
(`control`), `alt` (`mod1`). The key is any XKB keysym name, matched
case-insensitively (`Return`, `Escape`, `F11`, `comma`, `0`–`9`).
Re-mapping an existing combo replaces it. `binds` lists everything currently
mapped.

#### Default keybindings

Loaded **only when no init file exists**, as a usable starting point:

| Combo | Action |
|---|---|
| `super+Return` | `spawn alacritty` |
| `super+shift+q` | `close` |
| `super+j` / `super+k` | `focus-next` / `focus-prev` |
| `super+shift+j` / `super+shift+k` | `swap-next` / `swap-prev` |
| `super+shift+Return` | `swap-master` |
| `super+h` / `super+l` | `master-mfact 0.05` / `-0.05` |
| `super+Left/Right/Up/Down` | `focus-dir` that way |
| `super+shift+Left/Right` | `move-to-output` that way |
| `super+1` … `super+9`, `super+0` | `workspace 1`–`10` |
| `super+shift+1` … `super+shift+0` | `move-to-workspace 1`–`10` |
| `super+Tab` | `workspace-back` |
| `super+shift+f` | `toggle-float` |
| `super+e` | `fullscreen` |
| `super+m` | `maximize` |
| `super+shift+Escape` | `exit` |

### Mouse

All of these are on by default; each has a `set` toggle:

- **super + left-drag** — move a window (a tiled window floats in place first)
- **super + right-drag** — resize a window from whichever quadrant you grabbed
- **left-drag on a border** — resize, no modifier needed (`resize-on-border`)
- **drop a tiled window** — it re-tiles at the drop position, including onto
  another monitor (`drop-at-cursor`)
- **super + scroll** — cycle the focused output's workspaces (`scroll-workspace`)
- **click** — focuses the window under the cursor

### Window rules — `rule`, `windows`

```
rule <app-id|title|initial-title>[-re] <pattern...> <action>
```

Rules run once, when a window maps. The field suffix `-re` makes the pattern a
case-insensitive POSIX extended regex; otherwise it's an exact match
(`initial-title` matches the title at map time, before apps rewrite it).
Adding a rule with the same field, pattern, and action replaces the old one —
so re-running the init doesn't stack duplicates. Actions:

| Action | Effect |
|---|---|
| `workspace <output:N\|N>` | open on that workspace |
| `float` | float at the default size |
| `float <W> <H>` | float at a size — `40%`, `600p`/`600px`, or a bare fraction |
| `float default` | float at the app's own requested size |
| `tile` | force tiling (overrides the automatic dialog-float policy) |
| `suppress-maximize` | ignore the app's self-maximize requests |
| `no-focus` | map without taking focus |

```sh
honeyctl rule app-id mpv float 50% 50%
honeyctl rule app-id-re "^org\.pwmt\." workspace 2
honeyctl rule title "Steam - News" no-focus
honeyctl rule app-id jetbrains-idea workspace DP-1:2
```

`windows` lists every mapped window with its app-id, title, workspace, and
state flags — useful for finding the right pattern.

### Input devices — `kb-layout`, `kb-repeat`, `input`

```
kb-layout <layout> [variant] [model] [options] [rules]
kb-repeat <rate> <delay>
input <device|*> <option> <value>
```

`kb-layout` takes XKB RMLVO pieces (`kb-layout ca multix`, `kb-layout us ,
, grp:alt_shift_toggle`); it validates by compiling the keymap before applying
to every keyboard, so a typo can't leave you with a dead keyboard.
`kb-repeat` is characters per second and delay in ms (default 25/600).

`input` rules configure pointer devices through libinput directly. The device
selector is a case-insensitive substring of the device name, or `*` for all;
rules apply to present and future (hotplugged) devices:

| Option | Values |
|---|---|
| `natural-scroll` | bool |
| `tap` | bool (tap-to-click) |
| `accel-speed` | −1.0 … 1.0 |
| `accel-profile` | `flat` or `adaptive` |
| `scroll-method` | `2fg`, `edge`, `button`, `none` |
| `disable-while-typing` | bool |
| `left-handed` | bool |
| `middle-emulation` | bool |

### Outputs — `output`, `outputs`

```
output <name> [mode WxH[@Hz]] [scale F] [pos X,Y] [transform 0-7]
              [adaptive-sync on|off] [on|off]
```

Options combine in one call. `mode` picks the closest advertised mode or sets
a custom one; `pos` places the output in the global layout (omitted = auto,
to the right); `transform` is the Wayland transform enum (1 = 90°, …, 4 =
flipped); `off` disables the output (DPMS — pair with
[swayidle](https://github.com/swaywm/swayidle) and honey's idle-notify support
for an idle blanker); `on` re-enables it.

`outputs` lists connectors with resolution, position, scale, active workspace,
and focus. Because honey also serves `wlr-output-management`, `wlr-randr` and
`kanshi` work as alternatives.

### Night light — `gamma`

Color temperature (Kelvin) and brightness (percent) applied through each
output's hardware gamma LUT. Not a backlight control — for external monitors
over DisplayPort/HDMI it's the practical equivalent (the same mechanism as
gammastep/hyprsunset), and it's what the bar module rides.

| Command | Effect |
|---|---|
| `gamma` | print `temperature <K> brightness <pct>` |
| `gamma <1000–10000> [brightness%]` | set both absolutely (`gamma 4000 60`) |
| `gamma temperature <+N\|-N\|N>` | adjust temperature only; relative from off starts at 6500 K |
| `gamma brightness <+N\|-N\|N>` | adjust brightness only (`gamma brightness +5`) |
| `gamma min <N>` / `gamma max <N>` | clamp brightness — applies to every source: hotkeys, bar scroll, direct calls |
| `gamma off` (or `reset`) | back to neutral |

Every change, from any source, is broadcast as a `gamma` event on the status
stream, so a bar widget is always in sync. External tools that speak
`wlr-gamma-control` (wlsunset, gammastep) also work, if you'd rather not use
the built-in.

### Workspace names — `workspace-name`

```
workspace-name <N> [name]
```

Labels workspace N on the focused output (omit the name to clear it). Names
ride the status stream for bars; addressing stays numeric.

### Environment — `env`

```
env <KEY> <VALUE>
```

Sets a variable in honey's own environment, inherited by everything spawned
afterward. This is the correct place for toolkit hints — a plain `export` in
the init only reaches the init's own shell:

```sh
honeyctl env QT_QPA_PLATFORM wayland
honeyctl env _JAVA_AWT_WM_NONREPARENTING 1
```

### Session — `reload`, `ping`, `exit`

`reload` re-runs the init file (the one from `-c` if given); `ping` answers
`pong`; `exit` terminates the compositor cleanly.

## The status stream

`honeyctl subscribe` holds its connection open and receives newline-delimited
JSON: a snapshot on connect, then a line whenever something changes (diffed —
unchanged state is never re-sent). Three event types, all `"v":1`:

```json
{"ev":"workspaces","v":1,"output":"DP-1","active":2,"occupied":[1,2,5],"focused":true,"layout":"master","count":3,"names":{"1":"web"}}
{"ev":"window","v":1,"output":"DP-1","focused":true,"app_id":"org.gnome.Nautilus","title":"Home"}
{"ev":"gamma","v":1,"temperature":4000,"brightness":60}
```

- `workspaces` — per output: the active workspace, which workspaces have
  windows, whether this output has focus, the active layout, the window count,
  and any names.
- `window` — per output: the focused window's app-id and title (or the
  workspace's first window on unfocused outputs).
- `gamma` — global: current temperature and brightness.

The stream costs nothing when nobody listens (formatting is skipped entirely),
and extra keys will only ever be added, not changed — consumers should ignore
what they don't recognize. This is the feed the
[waybar modules](#waybar-modules) are built on; `honeyctl subscribe | jq` is a
fine way to prototype your own.

For generic consumers, the standard `ext-workspace` and
`foreign-toplevel-management` protocols expose overlapping information — the
stream exists for what those can't say (occupied/empty per workspace, gamma).

## Wayland protocol support

Everything honey advertises, grouped by what it's for:

| Area | Protocols |
|---|---|
| Core | `wl_compositor` v5, `wl_subcompositor`, `wl_shm`, `wl_seat`, `wl_output`, `wl_data_device_manager` |
| Window shells | `xdg-shell` v3, `wlr-layer-shell` v5 (bars, launchers, wallpapers, lock-style overlays), XWayland (rootless, lazy) |
| Decoration | `xdg-decoration`, KDE `server-decoration` (default server-side — the GTK fix) |
| Output | `wlr-output-management`, `xdg-output` v2 (in-tree implementation), `wlr-gamma-control`, `fractional-scale`, `viewporter`, `presentation-time`, `tearing-control`, `content-type`, `alpha-modifier`, `single-pixel-buffer`, `linux-dmabuf` v5 |
| Input | `pointer-constraints` + `relative-pointer` (gaming/VMs), `cursor-shape`, `virtual-keyboard`, `virtual-pointer`, `keyboard-shortcuts-inhibit`, `idle-notify`, `idle-inhibit`, `xdg-activation` |
| Clipboard | `primary-selection`, `wlr-data-control` (clipboard managers) |
| Capture | `wlr-screencopy`, `ext-image-copy-capture` (+ output capture source), `export-dmabuf`, `security-context` |
| Bars & taskbars | `wlr-foreign-toplevel-management` (with correct per-workspace visibility, so `wlr/taskbar` with `all-outputs: false` shows the current workspace), `ext-workspace` |

Deliberately not implemented (so far): `ext-session-lock` (no screen-locker
support yet), `input-method`/`text-input` (IMEs), `drm-lease` (VR), and an
internal idle timeout — idle policy composes fine externally via `idle-notify`
(swayidle calling `honeyctl output <name> off`).

## XWayland & HiDPI

X11 apps on a scaled Wayland output are normally rendered small and upscaled
by the compositor — functional, but blurry. That's the standard wlroots
behavior (sway documents it as a known limitation). honey implements the
technique Hyprland uses (`force_zero_scaling`) natively, controlled by one
setting:

```
set xwayland-scale off|auto|<scale>
```

- **`off`** (default) — standard passthrough behavior, with one improvement:
  X11 surfaces on scaled outputs are sampled nearest-neighbor, so the upscale
  is pixel-sharp rather than smeared.
- **`auto`** — every X11 window renders at its monitor's *physical*
  resolution. Crisp on every output, per-monitor: mixed-scale multi-head
  setups work, and a window crossing monitors is reconfigured for the one
  it's on.
- **`<scale>`** — a fixed scale (1.0–4.0) if you'd rather target one
  monitor's density.

How it works, briefly: Xwayland sizes its X11 screen from `xdg-output`, which
wlroots serves in logical coordinates. honey implements `xdg-output` in-tree
so that the Xwayland client — and only that client — sees the output layout at
physical size, packed contiguously. Windows are configured at physical
resolution, pointer coordinates are transformed per-monitor, and
override-redirect surfaces (menus, tooltips) are mapped back. Regular Wayland
clients never see any of this.

One caveat is inherent to X11 and identical on Hyprland: X11 has a single
global DPI, so apps render their UI at 96 dpi — crisp but small on a HiDPI
screen — unless told otherwise. Tell them per toolkit: `Xft.dpi` via `xrdb`
for most, `-Dsun.java2d.uiScale=2.0` in `.vmoptions` for JetBrains IDEs
(integer scales only).

The whole feature is runtime-gated and isolated: `off` makes it dormant, every
scaling site is tagged `xwayland-scale (removable)`, and
[src/xdg-output.c](src/xdg-output.c) carries a step-by-step removal manifest
— it can be excised without archaeology if you want a minimal tree.

## Waybar modules

[**honey-waybar**](https://github.com/l3mah/honey-waybar) ships three native
waybar plugins (CFFI modules — real GTK widgets, not scripts polling text),
all fed by the status stream over the control socket, reconnecting
automatically:

- **honey-workspaces** — one button per workspace with `active` / `occupied` /
  `empty` classes, clickable to switch. The state stock modules can't show.
- **honey-window** — the focused window's title, following focus across
  outputs, with app-id fallback and a full-title tooltip.
- **honey-gamma** — a night-light toggle: click to switch day/night presets,
  scroll to adjust brightness, separately styleable icon and value labels.

A small CLI adapter (`honey-waybar`) is included for text-mode setups without
CFFI. And stock waybar works with no extra software at all: `ext/workspaces`
and `wlr/taskbar` (set `all-outputs: false` to see the current workspace's
windows) talk to honey's standard protocols.

## Acknowledgements

honey stands on other people's work, gratefully:

- [**wlroots**](https://gitlab.freedesktop.org/wlroots/wlroots) — the
  foundation. Half of every feature above is "advertise the manager wlroots
  already implements correctly".
- [**dwl**](https://codeberg.org/dwl/dwl) and
  [**tinywl**](https://gitlab.freedesktop.org/wlroots/wlroots/-/tree/master/tinywl)
  — the reference points for how much compositor you actually need. dwl's
  scene-layer z-ordering and its hard-won XWayland correctness fixes saved
  this project from re-learning them the slow way.
- [**Hyprland**](https://github.com/hyprwm/Hyprland) — the XWayland HiDPI
  approach (the per-client `xdg-output` special-case and physical monitor
  packing) was learned by reading their source. honey's implementation is
  independent, but they charted the territory.
- [**river**](https://codeberg.org/river/river) — honey's predecessor
  [lakewm](https://github.com/l3mah/lakewm) grew up as a river client, and the
  workflow honey preserves was shaped there.
- [**sway**](https://github.com/swaywm/sway) — the encyclopedia of wlroots
  usage every compositor author ends up consulting.
- [**waybar**](https://github.com/Alexays/Waybar) — for the bar, and for the
  CFFI interface that makes native modules possible.

## License

[GPL-3.0-or-later](LICENSE).
