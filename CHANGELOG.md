# Changelog

## Unreleased

### Mouse

The overlay can be driven with the mouse: hover moves the selection, a click on
a thumbnail focuses that window and closes it. Keyboard and mouse share the same
`index`, so a click just takes whatever is currently ringed.

- Three listeners on `Event::bus()->m_events.input.mouse.{move,button,axis}`.
  These are `Cancellable<...>`, i.e. the handler is handed an
  `Event::SCallbackInfo&` — setting `cancelled` makes `CInputManager` return
  before keybind dispatch, before `PROTO::inputCapture` and before
  `CSeatManager::sendPointerButton`. Nothing reaches the client underneath.
  (`registerCallbackDynamic` is deprecated and inert since 0.56; the event bus is
  the only way in, and no function hook is needed.)
- Hit testing lives in the C core (`sw_hit_test`), against the tile cell rather
  than the letterboxed picture inside it — the cell is what the selection ring
  is drawn around. Renderer and mouse both go through `overlayLayoutFor()`, so
  what a click hits is by construction what was drawn.
- `sw_select` reports whether the selection actually moved, and only then is a
  redraw asked for. `hal_request_redraw` damages the whole screen, so doing it
  per motion event would mean a full recomposition per frame for nothing.
- Commit happens on press, not on release. The overlay hangs off held ALT; if
  ALT comes up between press and release, the watchdog closes the overlay and
  the release would land in the freshly focused window.
- Every press ends the cycle — a tile commits, anything else cancels. No button
  can be swallowed without handing the pointer back.

### Not swallowing the pointer forever

Eating input is the one thing here that can strand a user, so the swallow
condition is deliberately made of state that cannot get stuck:

- It hangs off `g_drawnOn` — the monitors the overlay actually reached the
  screen on — not off `sw_is_active`. With `render:direct_scanout` a fullscreen
  client's buffer bypasses the render pass entirely: `RENDER_LAST_MOMENT` never
  fires, the overlay is invisible, and swallowing clicks there would leave the
  mouse dead until ALT came up. Same for a DPMS-off monitor.
- No separate "pointer is grabbed" flag. `fault()` clears `g_healthy` *and* the
  switcher state, so an unhealthy plugin stops swallowing in the same breath.
- A drag that started before the overlay is left alone
  (`g_pInputManager->hasHeldButtons()`). A press we cancel never enters
  Hyprland's held-buttons list, so Hyprland itself drops the orphaned release —
  no bookkeeping needed on this side.
- All listeners live in one struct that `PLUGIN_EXIT` resets wholesale. A
  forgotten listener is a call into unmapped memory after `dlclose`, which no
  amount of `guarded()` can catch. `PLUGIN_EXIT` and `fault()` now also damage
  the screen, so a stale overlay cannot stay on as a still image.

### Known gaps

- With `cursor:zoom_factor` in play the hit test misses: the zoom scales the
  rendered frame without touching layout coordinates.
- Touch, tablet and touchpad gestures are not swallowed. They are cancellable
  the same way, but for a mouse setup it is weight without a use.
- While motion is cancelled, Hyprland's `m_lastCursorPosFloored` freezes at the
  position the cursor had when the overlay opened. Return to that exact pixel
  and no move event is emitted, so the ring sits still. Cosmetic, and not
  fixable from outside.

## 0.1.0

First working version.

### Switcher

- Alt-tab window switching ordered by Hyprland's own focus history
  (`CWindowHistoryTracker`), so a single alt+tab toggles between the two most
  recent windows.
- Overlay with one tile per window: real window thumbnails, letterboxed to keep
  each window's aspect ratio, selection highlighted.
- Tiles are laid out the way the windows sit on screen (workspace, then left to
  right), while the *selection* still starts on the most recently used window —
  so a single alt+tab keeps toggling, regardless of where that window sits in
  the row or which workspace it is on. Opening the switcher *is* the first step;
  it does not step again on top of the preselection.
- Windows from other workspaces are drawn as their own framed group with the
  workspace name above it, so they read as somewhere else rather than as more
  tiles in the row.
- Labels are rendered through Cairo into a texture; Hyprland has no text pass
  element.
- Tiles draw the window's **own live texture**
  (`window->wlSurface()->resource()->m_current.texture`) — the same one Hyprland
  renders, updated on every client commit. A visible window's tile is live; a
  window on another workspace shows its last committed frame, because the client
  stops drawing when nothing displays it. No snapshots, no cache, nothing to
  invalidate.
- Lua API: `next()`, `prev()`, `commit()`, `cancel()`, `active()`, `healthy()`
  and `stats()` under `hl.plugin.altswitch`.

### Not taking the compositor down

- Version guard in `pluginInit` compares the server hash against
  `__hyprland_api_get_client_hash()` — the commit hash alone would ignore the
  aquamarine/hyprutils/hyprgraphics/hyprcursor/hyprlang versions.
- Every call into Hyprland internals runs inside `guarded()`. An exception marks
  the plugin unhealthy, drops snapshots and selection, and notifies with the
  failing site.
- `healthy()` so a config can fall back to its own switcher per keystroke.

### Layout

- Tiles wrap onto further rows instead of being squeezed into one, the way the
  Windows switcher does. Two levels: tiles wrap inside their workspace card, and
  the cards themselves wrap into further rows. Every row is centred.
- Tiles only start shrinking once the whole block would grow past 80% of the
  screen height, and never below 60px.
- A cell has a floor of 45% of the nominal tile width, so a very tall or very
  flat window cannot collapse into an unrecognisable sliver — the cell keeps its
  size and the picture is letterboxed inside it.
- Windows that the tiling layout squeezed to zero size are still listed (16:9
  fallback cell) instead of being dropped from the switcher.

### Look

- Palette taken from the user's swaync theme: a flat `#0e0e10` surface, hairline
  `#2c2c33` borders via `CBorderPassElement`, muted `#9a9aa2` labels, and a light
  `#ececee` ring for the selection instead of a coloured block.
- **One surface, not a stack of cards.** Workspaces are separated by a `#43434d`
  hairline divider and their name — earlier versions drew a card per workspace
  and a plate per tile inside the panel, which read as card-in-card-in-card. The
  thumbnail *is* the tile now; a plate only stands in when a window has no
  texture yet.
- Workspace names are small (11px at the 1080p reference) and sit above their
  tiles with their own gap, so the selection ring cannot run into them.
- Corner radii and border widths scale with the screen, so the overlay keeps its
  proportions on 4K.
- Compact spacing: the label texture is filled by the text and vertically
  centred, rather than a tall strip with the glyphs parked at the top.

### Why not snapshots

`makeSnapshotFB` was the first approach and is a trap worth documenting:

- It only produces a usable image when called from the normal compositor flow
  (e.g. an exported Lua function). From the event loop — a `wl_event_loop` timer
  or the `window.active` signal — the framebuffer comes back allocated but
  **blank**, because outside a frame the renderer has no monitor set;
  `makeEGLCurrent()` does not help.
- Calling it from inside the render callback **crashes the compositor**
  (`CRenderPass::render` → `needsLiveBlur` → `getBlurTexture`).
- The framebuffer is monitor-sized with the window at its screen position, so
  every tile needed a `clipBox` reconstruction, and a window mid-animation
  produced a half-empty picture.

All of that disappears with the surface texture, which is also live rather than
a still.

### Known gaps

- No labels on the tiles themselves; class and title are collected but only the
  workspace name is drawn.
- Downscaling has no mipmaps, so regular patterns (terminal rows) alias.
- Tile size, gap and padding are hardcoded in `sw_default_cfg()`.
- Every mapped window on every workspace is included, no filtering.
