# Changelog

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
