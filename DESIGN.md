# honey-wm — design & plan

**honey** (weld / "wwm") is a tiling Wayland **compositor** built directly on
**wlroots 0.20**, succeeding [lakewm](https://github.com/l3mah/lakewm). lakewm was
a *client* of the river compositor (it spoke `river-window-management-v1`); honey
**is** the compositor. Same window-management brain, no river dependency, no
ceiling on what the WM can control.

## The five goals

1. **Migrate lakewm from a river WM to a full wlroots compositor.** Same policy
   brain, new foundation. lakewm stays intact and frozen as a working river WM.
2. **Simple, clean, DRY, efficient, unbloated — dwl-style.** One `honey.h` header
   + one `honey_server`, the existing module split, a minimal protocol set, no
   effects at first.
3. **Modern wlroots (0.20).** Pinned; nix `wlroots` attr = 0.20.1, matching
   river's link. pkg-config name `wlroots-0.20`.
4. **Retain every lakewm feature, then expand — decorations first.** Full parity
   with lakewm's surface (§4), then the GTK decoration fix and the other things
   river structurally forbade.
5. **Everything stateless.** No stateful layouts (no dwindle tree). This is the
   main anti-spaghetti lever — see §3.

## Why leave river (the trigger)

Diagnosed definitively: GTK3 **and GTK4** link *only*
`org_kde_kwin_server_decoration_manager` (the KDE protocol); alacritty links
`zxdg_decoration_manager_v1`. river 0.4.5 advertises *only* xdg-decoration, so
GTK apps can never get SSD → they draw their own headerbar. A river **client**
cannot advertise a Wayland global, so lakewm cannot fix it. Patching river was
rejected (breaks the "drop-in WM for stock river" shipping story). Becoming the
compositor solves it in one line — advertise **both** decoration managers. The
same client ceiling blocks standard bars, native output config, gamma, tearing,
effects. See the `river-ceiling` memory for the full diagnosis.

## 3. Architecture — how it stays clean

- **`wlr_scene` for all rendering.** Windows are scene trees; borders are
  `wlr_scene_rect`; z-order is a fixed array of scene-tree layers (dwl pattern:
  background / bottom / tiled / floating / top / overlay / fullscreen / lock). No
  hand-rolled render loop or damage tracking.
- **Single source of truth.** The `wlr_xdg_toplevel` / `wlr_output` *is* the
  state. No shadow copies, no resync. This is the rule that prevents state drift.
- **Event-driven.** `wl_listener` signals (`new_xdg_toplevel`, `map`, `commit`,
  `new_output`, `new_input`, cursor motion) call policy synchronously. The
  `manage_start`/`render_start` double-buffer dance is **deleted**.
- **Stateless layouts only.** Every layout is a pure function of
  `(ordered window list + params)`. The layout interface is **just
  `arrange(ctx)`** — no `void *layout_data`, no `on_add`/`on_remove`/`on_swap`/
  `on_drop` lifecycle hooks, no per-workspace tree. master/spiral/grid all fit
  one contract; swap/drop/next/prev come free. A stateful dwindle tree is
  explicitly **out of scope**.
- **Pure-IPC config.** The `honeyctl` command language *is* the config language.
  No compile-time `config.h`, no config-file format, no reload — the `init` file
  is just a shell script of `honeyctl` calls that apply as-is.

## 4. Port / donate / new

### FROM lakewm — port near-verbatim (policy brain; compositor-agnostic)
Output into `wlr_scene` node positions instead of river `propose_dimensions`.

- `layout.c` (~400) — stateless registry + `arrange(ctx)` driver; master / spiral
  / grid. Params namespaced `master-mfact`/`-nmaster`/`-orientation`,
  `spiral-ratio`/`-first-split`, `grid-columns`.
- `workspace.c` (~60) — per-output, unlimited, `output:N`-addressable workspaces
  (NOT dwm tags); `parse_ws_addr`, `prev_ws` for `workspace-back`.
- `action.c` (~500) — every action handler.
- `binding.c` (~450) — keysym + modifier/button token parsing; keybind +
  pointer-bind lists. Drop the per-seat river-object reconciliation → use
  `wlr_keyboard` directly.
- `config.c` (~70) — defaults + `has_<key>` effective-value resolution.
- `ipc.c` + `honeyctl.c` + `status.c` (~1200) — runtime control socket, command
  parser, and the `subscribe` JSON-Lines stream (schema **v:1** — keep identical
  so honey-waybar reuses it).
- window rules (`apply_rules`) — `app-id`/`title`/`initial-title` (+`-re`) →
  `workspace`/`float [W H]`/`tile`/`suppress-maximize`/`no-focus`; replace-on-
  same-match; new-windows-only.
- The whole data model + the locked code style.

### DONATE from dwl — read + borrow, don't fork wholesale
- The `setup()` bootstrap sequence.
- The scene-layer array for z-ordering.
- **XWayland `xwm` glue** — #1 donation, do not reinvent.
- Hard-won correctness fixes: focus-stealing, unmanaged/override-redirect X
  surfaces, activation/urgency, cursor-warp quirks, output hotplug races.

### NET-NEW wlroots glue — replaces the river tax
- `main.c` — wlroots init + event loop.
- `window.c` — `wlr_xdg_toplevel` (+ popups) lifecycle.
- `output.c` — `wlr_output` + `wlr_output_layout` + `wlr_scene_output`.
- `seat.c` — `wlr_seat` + `wlr_cursor` + focus + pointer ops.
- `input.c` — `new_input` + `libinput_device_config_*` **directly** (deletes
  lakewm's ~740-line protocol-client `input.c`). xkb keymap building stays.
- `decoration.c` — advertise `xdg-decoration` AND `wlr_server_decoration_manager`
  (default server) → GTK fixed. The motivating module.
- `layer.c` — implement `wlr_layer_shell_v1` (scene layers + exclusive zones).
- `xwayland.c` — `wlr_xwayland` + the donated `xwm`.
- `output-mgmt.c` — server side of `wlr-output-management-v1`: native in-process
  output config **and** makes `wlr-randr`/`kanshi` work against honey.
- foreign-toplevel + ext-workspace — `wlr_foreign_toplevel_management` +
  `ext-workspace` so stock waybar/taskbar modules work natively (additive to the
  own `subscribe` stream; the titlebar issue is unrelated to bars).

Plus manager one-liners in `setup()`, added as needed: `xdg_shell`, `xdg_output`,
`data_device`/`primary_selection`/`data_control`, `xdg_activation`,
`presentation_time`, `viewporter`, `fractional_scale`, `cursor_shape`,
`relative_pointer`/`pointer_constraints`, `gamma_control`, `session_lock`,
`screencopy`, `tearing_control`, `input_method`/`text_input`.

### DELETED — the river tax (not ported)
- `input.c` ~740 (protocol-client config) → direct libinput.
- `wm.c` manage/render double-buffer → gone (event-driven).
- `window.c`/`output.c`/`seat.c` `river_*_v1` lifecycle + shadow state.

### EXPANDS — ported code gains reach as the compositor
decorations (the fix); scroll-to-switch (axis events); resize-on-border (pointer-
in-window); XWayland `force_zero_scaling`. Effects possible but deferred.

## 5. Bars / IPC — unchanged

The `honeyctl` socket and the `subscribe` JSON stream are honey's own protocol —
nothing to do with river or wlroots. They port over unchanged; honey-waybar keeps
consuming the same v1 schema. Being a compositor only *adds* the option to also
serve `ext-workspace`/`foreign-toplevel` for stock bars (approved — additive).
The GTK decoration issue is separate from waybar.

## Milestones

- **M0** — scaffold: Makefile (nix-shell wlroots-0.20), `main.c` bootstrap
  (backend → renderer → allocator → `wlr_scene` → loop), empty module stubs;
  compiles + runs an empty compositor (nested + headless).
- **M1** — one output, `xdg_shell`, `wlr_scene`, master layout, keybinds, cursor.
- **M2** — workspaces + multi-monitor + `wlr_output_layout` + native output
  config (+ `wlr-output-management` server for wlr-randr/kanshi).
- **M3** — layer-shell + decorations (both protocols) + input (libinput direct) +
  IPC/`honeyctl`/status + config init.
- **M4** — XWayland + foreign-toplevel + ext-workspace + manager long-tail (gamma,
  session-lock, screencopy, activation, clipboard).
- **M5** — polish: rules, floating/fullscreen/maximize/fake-fullscreen, pointer
  drag, drop-at-cursor, spiral/grid; scroll-to-switch, resize-on-border.
- **M6 (optional)** — effects.

## Conventions

- **Code style = lakewm's (verbatim):** factual comments (no first-person, no
  phase tags), explicit names, `name (` space before paren, INLINE for 0–1
  params, MULTI-LINE (one param per line, no trailing comma) for 2+, `) {` on its
  own line; section-banner comments; DRY.
- **Build:** nix-shell (wlroots 0.20, wayland, wayland-protocols, wayland-scanner,
  libxkbcommon, pixman, libinput, libdrm, pkg-config, gcc, gnumake, + xcb/xwayland
  for XWM). Plain Makefile.
- **Headless test:** `WLR_BACKENDS=headless WLR_RENDERER=pixman
  WLR_LIBINPUT_NO_DEVICES=1 WLR_HEADLESS_OUTPUTS=1`; short `XDG_RUNTIME_DIR`
  (<108-byte sun_path); kill test compositor by EXACT PID, never `pkill -f`.
- **Names:** binary `honey`; control client `honeyctl`; config `~/.config/honey/init`
  (shell script of `honeyctl` calls); socket `$XDG_RUNTIME_DIR/honey-$WAYLAND_DISPLAY.sock`;
  bar consumer `honey-waybar`.
- **Commits:** no Claude co-author trailer; author `Maxence Hamel
  <maxence.hc@gmail.com>`.

## Non-goals (initially)

Effects (defer), a config-file format (runtime IPC is the config), dwm-tags (use
workspaces), stateful layouts (dwindle tree), Xorg (Wayland + XWayland only).

## References

- lakewm (`../../lakewm`) — policy code + the `subscribe` schema + lakewm-waybar
  (→ honey-waybar).
- dwl (codeberg.org/dwl/dwl) — compositor skeleton, scene layers, XWayland.
- tinywl (wlroots reference) — minimal `wlr_scene` bootstrap.
- wlroots 0.20 headers — the API of record.
