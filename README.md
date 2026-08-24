# hypr-altswitch

Alt-tab window switcher for Hyprland, as a compositor plugin — the tiles show
real window thumbnails, laid out the way the windows sit on screen, with windows
from other workspaces grouped and labelled. The selection still starts on the
most recently used window, so a single alt+tab toggles.

Tiles are the window's own live texture, the same one Hyprland draws — so a
visible window's tile updates in real time. A window on another workspace shows
its last committed frame, since its client stops drawing when nothing displays
it. (Snapshots via `makeSnapshotFB` were tried first and dropped; see CHANGELOG
for why.)

## Build

```
make            # -> build/hypr-altswitch.so
make install    # -> ~/.local/share/hyprland/plugins/
make check      # do the headers match the running Hyprland?
```

Needs the `hyprland` headers (`pkg-config hyprland`) and `luajit`. The plugin is
ABI-bound: after every Hyprland update run `make clean && make`.

## Use

The plugin exports Lua functions instead of dispatchers:

```lua
hl.plugin.load("/home/you/.local/share/hyprland/plugins/hypr-altswitch.so")

hl.bind("ALT + Tab", function() hl.plugin.altswitch.next() end)
hl.bind("ALT + SHIFT + Tab", function() hl.plugin.altswitch.prev() end)
hl.bind("ALT + Alt_L", function() hl.plugin.altswitch.commit() end, { release = true, non_consuming = true })
hl.bind("ALT + Alt_R", function() hl.plugin.altswitch.commit() end, { release = true, non_consuming = true })
```

Also available: `cancel()`, `active()`, `healthy()`.

`healthy()` is worth binding your config around — see below.

## Mouse

While the overlay is up it owns the pointer. Move the mouse and the selection
ring follows the cursor; click a thumbnail and that window is focused and the
overlay closes. Clicking next to the panel closes it without switching, and any
other button does the same — every press ends the cycle, so the overlay can
never hold the pointer for longer than one click.

Nothing reaches the window underneath: motion, buttons and scrolling are all
cancelled at Hyprland's event bus, before keybinds and before anything is sent
to the client. The cursor itself still moves normally.

Two deliberate limits:

- The overlay stays tied to the key that opened it, so this is a "hold ALT, move
  the mouse, click" gesture. There is no mode where it stays open on its own.
- A drag that was already in progress keeps the pointer — the plugin stays out
  of the way rather than freezing it halfway.

## Not crashing your compositor

Three layers, because a plugin fault normally takes the whole session with it:

1. **Refuses to load** into a Hyprland it was not built against (hash guard in
   `pluginInit`, compared against `__hyprland_api_get_client_hash()`).
2. **Disables itself** instead of throwing into the compositor: every call into
   Hyprland internals runs inside `guarded()`, and a fault drops all state and
   notifies.
3. **`healthy()`** lets your config check before each keystroke and fall back to
   its own switcher. Note that `hl.plugin.load()` does not report failures — test
   for `hl.plugin.altswitch ~= nil` afterwards.

A segfault is out of reach of any of this; layer 1 is what keeps that case away.

## Developing

A plugin crash kills the compositor, so work against a nested Hyprland in a
window:

```
make nested                # config: ~/.config/hypr/nested.lua
make load INSTANCE=1       # hyprctl -i 1 talks to the nested one
make reload INSTANCE=1     # rebuild + swap live
```

## Layout

| file | language | job |
|---|---|---|
| `src/switcher.c/.h` | C | ordering, selection, overlay geometry |
| `src/altswitch_lua.c/.h` | C | the `hl.plugin.altswitch.*` callbacks |
| `src/plugin.cpp` | C++ | entry points, window collection, rendering |

Only the C++ file touches Hyprland — its plugin boundary hands out `std::string`
and cannot be spoken from C. The C side reaches back through three `hal_*`
functions.
