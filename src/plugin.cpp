/* Hyprland glue for the alt-tab switcher.
 *
 * This file exists because Hyprland's plugin boundary is C++ (pluginAPIVersion
 * returns std::string, pluginInit returns a struct of std::strings) — it cannot
 * be spoken from C. Everything not forced to be C++ lives in switcher.c and
 * altswitch_lua.c.
 *
 * Everything that touches Hyprland internals runs inside guarded(), so an API
 * change that throws takes down the plugin's own feature instead of the whole
 * compositor: the plugin marks itself unhealthy, drops its state and reports
 * back to the config via hl.plugin.altswitch.healthy(), which then falls back
 * to the pure-Lua switcher.
 *
 * What this cannot catch is a segfault — no plugin can. The version guard in
 * pluginInit is what keeps that case away: a plugin built against a different
 * Hyprland refuses to load at all, and Hyprland carries on without it.
 */

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/history/WindowHistoryTracker.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <cairo/cairo.h>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/version.h>

#include <vector>
#include <unordered_map>
#include <string>
#include <cstring>
#include <cmath>

extern "C" {
#include "switcher.h"
#include "altswitch_lua.h"
}

inline HANDLE                                 PHANDLE = nullptr;

/* Palette taken from the user's swaync theme, so the switcher reads as part of
 * the same desktop: flat dark surfaces, hairline borders, muted labels, and a
 * light ring for the selection instead of a coloured block. */
namespace Palette {
    static const CHyprColor PANEL{0.055F, 0.055F, 0.063F, 0.97F};  /* #0e0e10 surface */
    static const CHyprColor DIVIDER{0.263F, 0.263F, 0.302F, 1.00F};/* #43434d strong  */
    static const CHyprColor TILE{0.086F, 0.086F, 0.102F, 1.00F};   /* #16161a hover   */
    static const CHyprColor BORDER{0.173F, 0.173F, 0.200F, 1.00F}; /* #2c2c33 border  */
    static const CHyprColor RING{0.925F, 0.925F, 0.933F, 0.90F};   /* #ececee text    */
}

static bool                                   g_healthy      = true;
static bool                                   g_faultNotified = false;
static std::string                            g_faultReason;

/* Rendered workspace labels, keyed by the text itself. */
static std::unordered_map<std::string, SP<Render::ITexture>> g_labels;

static Hyprutils::Signal::CHyprSignalListener g_renderListener;

/* ------------------------------------------------------------------ */
/* fault handling                                                      */
/* ------------------------------------------------------------------ */

static void notify(const std::string& text, const CHyprColor& color, float ms) {
    /* Even notifying can only be attempted, never relied on. */
    try {
        HyprlandAPI::addNotification(PHANDLE, text, color, ms);
    } catch (...) {}
}

static void fault(const char* where, const std::string& what) {
    g_healthy     = false;
    g_faultReason = std::string{where} + ": " + what;

    /* Drop everything we hold; a broken plugin should not keep textures or a
     * half-built selection alive. */
    sw_reset(&g_switcher);

    if (!g_faultNotified) {
        g_faultNotified = true;
        notify("[hypr-altswitch] disabled after an internal error (" + g_faultReason + ") — rebuild against the running Hyprland", CHyprColor{0.9, 0.4, 0.2, 1.0}, 8000);
    }
}

/* Runs fn, and turns any exception into "the plugin switches itself off"
 * instead of letting it escape into the compositor. */
template <typename F>
static bool guarded(const char* where, F&& fn) {
    if (!g_healthy)
        return false;
    try {
        fn();
        return true;
    } catch (const std::exception& e) {
        fault(where, e.what());
    } catch (...) {
        fault(where, "unknown exception");
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void copyStr(char* dst, size_t cap, const std::string& src) {
    if (!dst || cap == 0)
        return;
    const size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

static PHLWINDOW windowFromId(uint64_t id) {
    for (const auto& w : Desktop::windowState()->windows()) {
        if (w && w->m_stableID == id)
            return w;
    }
    return nullptr;
}

static SP<Render::ITexture> windowTexture(const PHLWINDOW& w);

/* Hyprland keeps focus history oldest-first, so the newest entry is the window
 * in front of you. Flip it so 0 = focused, 1 = the one before, matching what the
 * C core expects. Windows the tracker never saw sort to the back. */
static int32_t historyIndex(const PHLWINDOW& win) {
    const auto& history = Desktop::History::windowTracker()->fullHistory();
    for (size_t i = 0; i < history.size(); ++i) {
        if (history[history.size() - 1 - i].lock() == win)
            return static_cast<int32_t>(i);
    }
    return static_cast<int32_t>(history.size() + 1);
}

/* A label texture via Cairo. Hyprland has no text pass element, but it will
 * turn a Cairo surface into a texture for us. */
static SP<Render::ITexture> labelTexture(const std::string& text, int height) {
    if (text.empty())
        return nullptr;
    if (const auto it = g_labels.find(text); it != g_labels.end())
        return it->second;

    const int width = static_cast<int>(text.size()) * height * 3 / 4 + height / 2;

    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (!surface)
        return nullptr;

    cairo_t* cr = cairo_create(surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, height * 0.82);
    cairo_set_source_rgba(cr, 0.604, 0.604, 0.635, 1.0); /* #9a9aa2 text-muted */

    /* Centre the baseline in the texture instead of parking the text at the top:
     * otherwise the unused lower half reads as padding between the workspace
     * name and the tiles. */
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    cairo_move_to(cr, 0, (height + fe.ascent - fe.descent) / 2.0);
    cairo_show_text(cr, text.c_str());
    cairo_surface_flush(surface);

    SP<Render::ITexture> tex;
    try {
        tex = g_pHyprRenderer->createTexture(surface);
    } catch (...) {}

    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    if (tex)
        g_labels[text] = tex;
    return tex;
}

/* ------------------------------------------------------------------ */
/* HAL: the only entry points the C side calls back into               */
/* ------------------------------------------------------------------ */

extern "C" int hal_collect_windows(sw_window* out, int max) {
    if (!out || max <= 0)
        return 0;

    int n = 0;
    const bool ok = guarded("collect", [&]() {
        const auto focused = Desktop::focusState()->window();

        for (const auto& w : Desktop::windowState()->windows()) {
            if (n >= max)
                break;
            if (!w || !w->m_isMapped || w->isHidden())
                continue;

            /* A window squeezed to nothing by the tiling layout still has to be
             * reachable, so fall back to a 16:9 cell instead of dropping it. */
            const auto size  = w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            const auto srcW  = size.x > 1 ? size.x : 160;
            const auto srcH  = size.y > 1 ? size.y : 90;

            const auto ws  = w->m_workspace;
            const auto pos = w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);

            sw_window entry{};
            entry.id        = w->m_stableID;
            entry.history   = historyIndex(w);
            entry.focused   = (w == focused) ? 1 : 0;
            entry.workspace = ws ? static_cast<int32_t>(ws->m_id) : 0;
            entry.pos_x     = pos.x;
            entry.pos_y     = pos.y;
            entry.src_w     = srcW;
            entry.src_h     = srcH;
            copyStr(entry.class_name, SW_CLASS_LEN, w->m_class);
            copyStr(entry.title, SW_TITLE_LEN, w->m_title);
            copyStr(entry.ws_label, SW_LABEL_LEN, ws ? ws->m_name : std::string{"?"});
            out[n++] = entry;

        }
    });

    return ok ? n : 0;
}

extern "C" void hal_focus_window(uint64_t id) {
    guarded("focus", [&]() {
        if (const auto w = windowFromId(id); w)
            (void)Config::Actions::focus(w);
    });
}

extern "C" void hal_request_redraw() {
    guarded("damage", []() {
        /* Nothing else on screen changed, so without damage the overlay would
         * only show up once something else triggers a frame. A box this large
         * covers every monitor; it only runs while the switcher is on screen. */
        g_pHyprRenderer->damageBox(CBox{-1e5, -1e5, 2e5, 2e5});
    });
}

extern "C" void hal_end_switch() {
    /* Nothing to release: tiles render the window's own live texture, so there
     * is no per-cycle state to hold on to. */
}

extern "C" int hal_thumb_count() {
    /* How many windows currently have something we could draw. */
    int n = 0;
    for (const auto& w : Desktop::windowState()->windows()) {
        if (w && w->m_isMapped && !w->isHidden() && windowTexture(w))
            ++n;
    }
    return n;
}

extern "C" int hal_label_count() {
    return static_cast<int>(g_labels.size());
}

extern "C" int hal_is_healthy() {
    return g_healthy ? 1 : 0;
}

extern "C" void hal_report_unavailable() {
    if (g_healthy)
        return;
    notify("[hypr-altswitch] not available: " + g_faultReason, CHyprColor{0.9, 0.4, 0.2, 1.0}, 4000);
}

/* ------------------------------------------------------------------ */
/* rendering                                                           */
/* ------------------------------------------------------------------ */

static void addRect(const sw_rect& r, const CHyprColor& color, int rounding) {
    CRectPassElement::SRectData data;
    data.box   = CBox{r.x, r.y, r.w, r.h};
    data.color = color;
    data.round = rounding;
    g_pHyprRenderer->currentPass().add(makeUnique<CRectPassElement>(data));
}

/* The window's own live texture. This is what Hyprland itself draws, updated on
 * every client commit — so a tile of a window you can see is live, and a window
 * on another workspace shows its last committed frame (the client stops drawing
 * when nothing displays it). No snapshots, no cache, nothing to invalidate. */
static SP<Render::ITexture> windowTexture(const PHLWINDOW& w) {
    if (!w)
        return nullptr;
    const auto wlSurface = w->wlSurface(); /* m_wlSurface itself is protected */
    if (!wlSurface)
        return nullptr;
    const auto surface = wlSurface->resource();
    if (!surface)
        return nullptr;
    return surface->m_current.texture;
}

/* Hairline border. CRectPassElement cannot outline, so this is a real border
 * element — one draw instead of two nested rects. */
static void addBorder(const sw_rect& r, const CHyprColor& color, int size, int rounding) {
    CBorderPassElement::SBorderData data;
    data.box        = CBox{r.x, r.y, r.w, r.h};
    data.grad1      = Config::CGradientValueData{color};
    data.borderSize = size;
    data.round      = rounding;
    data.a          = 1.F;
    g_pHyprRenderer->currentPass().add(makeUnique<CBorderPassElement>(data));
}

static bool addThumbnail(const sw_rect& r, uint64_t id) {
    const auto w = windowFromId(id);
    if (!w)
        return false;

    const auto tex = windowTexture(w);
    if (!tex)
        return false;

    CTexPassElement::SRenderData data;
    data.tex   = tex;
    data.box   = CBox{r.x, r.y, r.w, r.h};
    data.a     = 1.F;
    data.round = 6;
    g_pHyprRenderer->currentPass().add(makeUnique<CTexPassElement>(data));
    return true;
}

static void addLabel(const sw_rect& box, const std::string& text) {
    const auto tex = labelTexture(text, static_cast<int>(box.h));
    if (!tex)
        return;

    const auto size = tex->m_size;
    if (size.x <= 0 || size.y <= 0)
        return;

    /* Draw at native size, left aligned, clipped to the strip. */
    CTexPassElement::SRenderData data;
    data.tex     = tex;
    data.box     = CBox{box.x, box.y, static_cast<float>(size.x), static_cast<float>(size.y)};
    data.a       = 1.F;
    data.clipBox = CBox{box.x, box.y, box.w, box.h};
    g_pHyprRenderer->currentPass().add(makeUnique<CTexPassElement>(data));
}

static void drawOverlay() {
    const auto monitor = g_pHyprRenderer->renderData().pMonitor.lock();
    if (!monitor)
        return;

    const auto mSize = monitor->m_size;
    if (mSize.x <= 0 || mSize.y <= 0)
        return;

    const auto       cfg = sw_default_cfg();
    sw_layout_result l{};
    sw_layout(&g_switcher, mSize.x, mSize.y, &cfg, &l);
    if (l.tile_count <= 0)
        return;

    const float ui       = mSize.y / 1080.0F;
    const int   panelR   = static_cast<int>(14 * ui);
    const int   tileR    = static_cast<int>(7 * ui);
    const int   hairline = ui > 1.5F ? 2 : 1;
    const float ringPad  = 3.F * ui;

    /* One surface for the whole thing. Workspaces are separated by a divider
     * and their name, not by nested cards — stacking a card inside a card
     * inside a card is what made this look busy. */
    addRect(l.panel, Palette::PANEL, panelR);
    addBorder(l.panel, Palette::BORDER, hairline, panelR);

    if (l.grouped) {
        for (int gi = 0; gi < l.group_count; ++gi) {
            const sw_group& g = l.groups[gi];

            if (!g.first_in_row) {
                /* Hairline between two workspaces, sitting in the gap and
                 * spanning the tiles but not the label line. */
                const float x   = g.box.x - (cfg.group_gap * ui) * 0.5F;
                const float top = g.box.y + (cfg.label_h * ui) * 0.5F;
                addRect(sw_rect{x, top, static_cast<float>(hairline), g.box.h - (cfg.label_h * ui) * 0.5F}, Palette::DIVIDER, 0);
            }

            addLabel(g.label_box, std::string{g.label});
        }
    }

    const int selected = sw_index(&g_switcher);
    for (int i = 0; i < l.tile_count && i < g_switcher.count; ++i) {
        const sw_rect& cell  = l.tiles[i];
        const sw_rect& thumb = l.thumbs[i];

        /* The picture is the tile. Only when a window has no texture yet does a
         * plate stand in for it, so there is no permanent second frame. */
        if (!addThumbnail(thumb, g_switcher.items[i].id))
            addRect(thumb, Palette::TILE, tileR);

        addBorder(thumb, Palette::BORDER, hairline, tileR);

        if (i == selected) {
            const sw_rect ring{cell.x - ringPad, cell.y - ringPad, cell.w + 2 * ringPad, cell.h + 2 * ringPad};
            addBorder(ring, Palette::RING, ui > 1.5F ? 3 : 2, tileR + static_cast<int>(ringPad));
        }
    }
}

static void onRenderStage(eRenderStage stage) {
    if (stage != RENDER_LAST_MOMENT || !g_healthy)
        return;

    if (!sw_is_active(&g_switcher))
        return;

    guarded("render", []() { drawOverlay(); });
}

/* ------------------------------------------------------------------ */
/* plugin entry points                                                 */
/* ------------------------------------------------------------------ */

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    /* The server hash is not just the commit: it carries the aquamarine,
     * hyprutils, hyprgraphics, hyprcursor and hyprlang versions too. Comparing
     * against GIT_COMMIT_HASH alone would let a plugin load into a Hyprland
     * whose dependencies moved underneath it. Throwing here is the documented
     * way to refuse: Hyprland ejects the .so and keeps running. */
    if (std::string{__hyprland_api_get_hash()} != std::string{__hyprland_api_get_client_hash()}) {
        notify("[hypr-altswitch] built against a different Hyprland — not loaded. Rebuild with: make clean && make", CHyprColor{0.9, 0.4, 0.2, 1.0}, 8000);
        throw std::runtime_error("[hypr-altswitch] version mismatch");
    }

    sw_reset(&g_switcher);

    HyprlandAPI::addLuaFunction(PHANDLE, "altswitch", "next", altswitch_lua_next);
    HyprlandAPI::addLuaFunction(PHANDLE, "altswitch", "prev", altswitch_lua_prev);
    HyprlandAPI::addLuaFunction(PHANDLE, "altswitch", "commit", altswitch_lua_commit);
    HyprlandAPI::addLuaFunction(PHANDLE, "altswitch", "cancel", altswitch_lua_cancel);
    HyprlandAPI::addLuaFunction(PHANDLE, "altswitch", "active", altswitch_lua_active);
    HyprlandAPI::addLuaFunction(PHANDLE, "altswitch", "healthy", altswitch_lua_healthy);
    HyprlandAPI::addLuaFunction(PHANDLE, "altswitch", "stats", altswitch_lua_stats);

    g_renderListener = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) { onRenderStage(stage); });

    notify("[hypr-altswitch] loaded", CHyprColor{0.6, 0.7, 0.9, 1.0}, 3000);

    return {"hypr-altswitch", "alt-tab window switcher with window thumbnails", "max", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_renderListener.reset();

    try {
        g_labels.clear();
    } catch (...) {}
    sw_reset(&g_switcher);
}
