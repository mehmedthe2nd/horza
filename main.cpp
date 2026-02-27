#include "config.hpp"
#include "globals.hpp"
#include "overview.hpp"
#include <any>
#include <cmath>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <iostream>

inline CFunctionHook *g_pRenderWorkspaceHook = nullptr;
inline CFunctionHook *g_pAddDamageHookA = nullptr;
inline CFunctionHook *g_pAddDamageHookB = nullptr;
inline SP<HOOK_CALLBACK_FN> g_pMainConfigReloadHook = nullptr;

typedef void (*origRenderWorkspace)(void *, PHLMONITOR, PHLWORKSPACE,
                                    timespec *, const CBox &);
typedef void (*origAddDamageA)(void *, const CBox &);
typedef void (*origAddDamageB)(void *, const pixman_region32_t *);

static bool renderingOverview = false;

namespace {

std::string pluginKey(const std::string& key) { return "plugin:horza:" + key; }

bool addPluginConfigValue(const std::string& key,
                          const Hyprlang::CConfigValue& value) {
  return HyprlandAPI::addConfigValue(PHANDLE, pluginKey(key), value);
}

Hyprlang::CConfigValue* getPluginConfigValue(const std::string& key) {
  return HyprlandAPI::getConfigValue(PHANDLE, pluginKey(key));
}

bool getPluginInt(const std::string& key, Hyprlang::INT& out) {
  auto* cfg = getPluginConfigValue(key);
  if (!cfg || !cfg->m_bSetByUser)
    return false;

  try {
    out = std::any_cast<Hyprlang::INT>(cfg->getValue());
    return true;
  } catch (...) {
    return false;
  }
}

bool getPluginFloat(const std::string& key, Hyprlang::FLOAT& out) {
  auto* cfg = getPluginConfigValue(key);
  if (!cfg || !cfg->m_bSetByUser)
    return false;

  try {
    out = std::any_cast<Hyprlang::FLOAT>(cfg->getValue());
    return true;
  } catch (...) {
    return false;
  }
}

bool getPluginString(const std::string& key, std::string& out) {
  auto* cfg = getPluginConfigValue(key);
  if (!cfg || !cfg->m_bSetByUser)
    return false;

  try {
    const auto raw = std::any_cast<Hyprlang::STRING>(cfg->getValue());
    if (!raw)
      return false;
    out = raw;
    return true;
  } catch (...) {
    return false;
  }
}

const char* boolToToken(bool v) { return v ? "true" : "false"; }

bool parseStrictBool(const std::string& raw, bool& out) {
  const auto token = normalizeHorzaToken(horzaTrim(raw));
  if (token == "true") {
    out = true;
    return true;
  }
  if (token == "false") {
    out = false;
    return true;
  }
  return false;
}

bool getPluginBool(const std::string& key, bool& out) {
  std::string raw;
  if (!getPluginString(key, raw))
    return false;

  bool parsed = false;
  if (!parseStrictBool(raw, parsed)) {
    std::cerr << "[horza] invalid boolean for " << pluginKey(key) << ": '" << raw
              << "' (expected true/false)\n";
    return false;
  }

  out = parsed;
  return true;
}

void registerPluginConfigValues() {
  const char* defaultPreset = "custom";
  const char* defaultOrientation = g_horzaConfig.vertical ? "vertical" : "horizontal";
  const char* defaultBackground =
      g_horzaConfig.hyprpaperBackground ? "hyprpaper" : "black";
  const char* defaultCloseCurve =
      normalizeHorzaToken(g_horzaConfig.asyncCloseFadeCurve) == "linear"
          ? "linear"
          : "ease_out";
  const char* defaultShadowMode =
      normalizeHorzaToken(g_horzaConfig.cardShadowMode) == "texture" ? "texture"
                                                                      : "fast";
  const char* defaultShadowTexture =
      g_horzaConfig.cardShadowTexture.empty()
          ? ""
          : g_horzaConfig.cardShadowTexture.c_str();
  const char* defaultTitleFontFamily =
      g_horzaConfig.titleFontFamily.empty()
          ? "Inter Regular"
          : g_horzaConfig.titleFontFamily.c_str();

  addPluginConfigValue("preset", Hyprlang::CConfigValue{defaultPreset});
  addPluginConfigValue(
      "capture_scale",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.captureScale});
  addPluginConfigValue(
      "display_scale",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.displayScale});
  addPluginConfigValue(
      "overview_gap",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.overviewGap});
  addPluginConfigValue(
      "inactive_tile_size_percent",
      Hyprlang::CConfigValue{
          (Hyprlang::FLOAT)g_horzaConfig.inactiveTileSizePercent});
  
  addPluginConfigValue(
      "inactive_tile_shrink_percent",
      Hyprlang::CConfigValue{
          (Hyprlang::FLOAT)(100.0f - g_horzaConfig.inactiveTileSizePercent)});
  addPluginConfigValue("persistent_cache", Hyprlang::CConfigValue{
                                               boolToToken(g_horzaConfig.persistentCache)});
  addPluginConfigValue(
      "cache_ttl_ms",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.cacheTtlMs});
  addPluginConfigValue("cache_max_entries",
                       Hyprlang::CConfigValue{(Hyprlang::INT)g_horzaConfig.cacheMaxEntries});
  addPluginConfigValue(
      "capture_budget_ms",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.captureBudgetMs});
  addPluginConfigValue(
      "max_captures_per_frame",
      Hyprlang::CConfigValue{(Hyprlang::INT)g_horzaConfig.maxCapturesPerFrame});
  addPluginConfigValue(
      "live_preview_fps",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.livePreviewFps});
  addPluginConfigValue(
      "live_preview_radius",
      Hyprlang::CConfigValue{(Hyprlang::INT)g_horzaConfig.livePreviewRadius});
  addPluginConfigValue("prewarm_all",
                       Hyprlang::CConfigValue{boolToToken(g_horzaConfig.prewarmAll)});
  addPluginConfigValue("background_source", Hyprlang::CConfigValue{defaultBackground});
  addPluginConfigValue(
      "background_blur_radius",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.backgroundBlurRadius});
  addPluginConfigValue("background_blur_passes",
                       Hyprlang::CConfigValue{(Hyprlang::INT)g_horzaConfig.backgroundBlurPasses});
  addPluginConfigValue(
      "background_blur_spread",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.backgroundBlurSpread});
  addPluginConfigValue(
      "background_blur_strength",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.backgroundBlurStrength});
  addPluginConfigValue(
      "background_tint",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.backgroundTint});
  addPluginConfigValue("card_shadow",
                       Hyprlang::CConfigValue{boolToToken(g_horzaConfig.cardShadow)});
  addPluginConfigValue("card_shadow_mode",
                       Hyprlang::CConfigValue{defaultShadowMode});
  addPluginConfigValue("card_shadow_texture",
                       Hyprlang::CConfigValue{defaultShadowTexture});
  addPluginConfigValue(
      "card_shadow_alpha",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.cardShadowAlpha});
  addPluginConfigValue(
      "card_shadow_size",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.cardShadowSize});
  addPluginConfigValue(
      "card_shadow_offset_y",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.cardShadowOffsetY});
  addPluginConfigValue(
      "show_window_titles",
      Hyprlang::CConfigValue{boolToToken(g_horzaConfig.showWindowTitles)});
  addPluginConfigValue(
      "title_font_size",
      Hyprlang::CConfigValue{(Hyprlang::INT)g_horzaConfig.titleFontSize});
  addPluginConfigValue("title_font_family",
                       Hyprlang::CConfigValue{defaultTitleFontFamily});
  addPluginConfigValue(
      "title_background_alpha",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.titleBackgroundAlpha});
  addPluginConfigValue(
      "freeze_animations_in_overview",
      Hyprlang::CConfigValue{
          boolToToken(g_horzaConfig.freezeAnimationsInOverview)});
  addPluginConfigValue("esc_only",
                       Hyprlang::CConfigValue{boolToToken(g_horzaConfig.escOnly)});
  addPluginConfigValue("async_close_handoff", Hyprlang::CConfigValue{
                                                   boolToToken(g_horzaConfig.asyncCloseHandoff)});
  addPluginConfigValue(
      "async_close_fade_start",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.asyncCloseFadeStart});
  addPluginConfigValue("async_close_fade_curve", Hyprlang::CConfigValue{defaultCloseCurve});
  addPluginConfigValue(
      "async_close_min_alpha",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.asyncCloseMinAlpha});
  addPluginConfigValue(
      "close_drop_delay_ms",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.closeDropDelayMs});
  addPluginConfigValue(
      "drag_hover_jump_delay_ms",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.dragHoverJumpDelayMs});
  addPluginConfigValue("orientation", Hyprlang::CConfigValue{defaultOrientation});
  addPluginConfigValue(
      "center_offset",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.centerOffset});
  addPluginConfigValue("corner_radius",
                       Hyprlang::CConfigValue{(Hyprlang::INT)g_horzaConfig.cornerRadius});
}

void applyPluginConfigOverrides() {
  Hyprlang::FLOAT f = 0.0;
  Hyprlang::INT i = 0;
  std::string s;
  bool b = false;

  if (getPluginString("preset", s))
    applyHorzaPreset(s, g_horzaConfig);

  if (getPluginFloat("capture_scale", f))
    g_horzaConfig.captureScale = clampCaptureScale((float)f);
  if (getPluginFloat("display_scale", f))
    g_horzaConfig.displayScale = clampDisplayScale((float)f);
  if (getPluginFloat("overview_gap", f))
    g_horzaConfig.overviewGap = std::max(0.0f, (float)f);
  
  if (getPluginFloat("inactive_tile_shrink_percent", f))
    g_horzaConfig.inactiveTileSizePercent =
        100.0f - clampInactiveTileShrinkPercent((float)f);
  
  if (getPluginFloat("inactive_tile_size_percent", f))
    g_horzaConfig.inactiveTileSizePercent =
        clampInactiveTileSizePercent((float)f);
  if (getPluginBool("persistent_cache", b))
    g_horzaConfig.persistentCache = b;
  if (getPluginFloat("cache_ttl_ms", f))
    g_horzaConfig.cacheTtlMs = std::max(0.0f, (float)f);
  if (getPluginInt("cache_max_entries", i))
    g_horzaConfig.cacheMaxEntries = std::max(0, (int)i);
  if (getPluginFloat("capture_budget_ms", f))
    g_horzaConfig.captureBudgetMs = std::max(0.0f, (float)f);
  if (getPluginInt("max_captures_per_frame", i))
    g_horzaConfig.maxCapturesPerFrame = std::max(0, (int)i);
  if (getPluginFloat("live_preview_fps", f))
    g_horzaConfig.livePreviewFps = std::max(0.0f, (float)f);
  if (getPluginInt("live_preview_radius", i))
    g_horzaConfig.livePreviewRadius = std::max(0, (int)i);
  if (getPluginBool("prewarm_all", b))
    g_horzaConfig.prewarmAll = b;
  if (getPluginString("background_source", s)) {
    const auto source = normalizeHorzaToken(s);
    if (source == "black")
      g_horzaConfig.hyprpaperBackground = false;
    else if (source == "hyprpaper")
      g_horzaConfig.hyprpaperBackground = true;
  }
  if (getPluginFloat("background_blur_radius", f))
    g_horzaConfig.backgroundBlurRadius = std::max(0.0f, (float)f);
  if (getPluginInt("background_blur_passes", i))
    g_horzaConfig.backgroundBlurPasses = std::max(0, (int)i);
  if (getPluginFloat("background_blur_spread", f))
    g_horzaConfig.backgroundBlurSpread = std::max(0.0f, (float)f);
  if (getPluginFloat("background_blur_strength", f))
    g_horzaConfig.backgroundBlurStrength = std::max(0.0f, (float)f);
  if (getPluginFloat("background_tint", f))
    g_horzaConfig.backgroundTint = std::clamp((float)f, 0.0f, 1.0f);
  if (getPluginBool("card_shadow", b))
    g_horzaConfig.cardShadow = b;
  if (getPluginString("card_shadow_mode", s)) {
    const auto mode = normalizeHorzaToken(horzaTrim(s));
    if (mode == "fast" || mode == "box" || mode == "rect")
      g_horzaConfig.cardShadowMode = "fast";
    else if (mode == "texture" || mode == "png" || mode == "image")
      g_horzaConfig.cardShadowMode = "texture";
  }
  if (getPluginString("card_shadow_texture", s))
    g_horzaConfig.cardShadowTexture = horzaTrim(s);
  if (getPluginFloat("card_shadow_alpha", f))
    g_horzaConfig.cardShadowAlpha = std::clamp((float)f, 0.0f, 1.0f);
  if (getPluginFloat("card_shadow_size", f))
    g_horzaConfig.cardShadowSize = std::max(0.0f, (float)f);
  if (getPluginFloat("card_shadow_offset_y", f))
    g_horzaConfig.cardShadowOffsetY = (float)f;
  if (getPluginBool("show_window_titles", b))
    g_horzaConfig.showWindowTitles = b;
  if (getPluginInt("title_font_size", i))
    g_horzaConfig.titleFontSize = std::max(6, (int)i);
  if (getPluginString("title_font_family", s))
    g_horzaConfig.titleFontFamily = stripWrappedQuotes(horzaTrim(s));
  if (getPluginFloat("title_background_alpha", f))
    g_horzaConfig.titleBackgroundAlpha = std::clamp((float)f, 0.0f, 1.0f);
  if (getPluginBool("freeze_animations_in_overview", b))
    g_horzaConfig.freezeAnimationsInOverview = b;
  if (getPluginBool("esc_only", b))
    g_horzaConfig.escOnly = b;
  if (getPluginBool("async_close_handoff", b))
    g_horzaConfig.asyncCloseHandoff = b;
  if (getPluginFloat("async_close_fade_start", f))
    g_horzaConfig.asyncCloseFadeStart = std::clamp((float)f, 0.0f, 0.999f);
  if (getPluginString("async_close_fade_curve", s)) {
    const auto curve = normalizeHorzaToken(s);
    if (curve == "linear" || curve == "ease_out")
      g_horzaConfig.asyncCloseFadeCurve = curve;
  }
  if (getPluginFloat("async_close_min_alpha", f))
    g_horzaConfig.asyncCloseMinAlpha = std::clamp((float)f, 0.0f, 1.0f);
  if (getPluginFloat("close_drop_delay_ms", f))
    g_horzaConfig.closeDropDelayMs = std::max(0.0f, (float)f);
  if (getPluginFloat("drag_hover_jump_delay_ms", f))
    g_horzaConfig.dragHoverJumpDelayMs = std::max(0.0f, (float)f);
  if (getPluginString("orientation", s)) {
    const auto orientation = normalizeHorzaToken(s);
    if (orientation == "horizontal")
      g_horzaConfig.vertical = false;
    else if (orientation == "vertical")
      g_horzaConfig.vertical = true;
  }
  if (getPluginFloat("center_offset", f))
    g_horzaConfig.centerOffset = (float)f;
  if (getPluginInt("corner_radius", i))
    g_horzaConfig.cornerRadius = std::max(0, (int)i);
}

void reloadRuntimeConfig() {
  g_horzaConfig = HorzaConfig{};
  applyPluginConfigOverrides();
}

void refreshOverviewAfterConfig() {
  auto* OV = g_pOverview.get();
  if (!OV)
    return;

  OV->damage();
  if (const auto mon = OV->pMonitor.lock()) {
    g_pHyprRenderer->damageMonitor(mon);
    g_pCompositor->scheduleFrameForMonitor(mon);
  }
}

} 

static void hkRenderWorkspace(void *thisptr, PHLMONITOR pMonitor,
                              PHLWORKSPACE pWorkspace, timespec *now,
                              const CBox &geometry) {
  auto *OV = g_pOverview.get();
  if (OV && OV->closeDropPending()) {
    const auto mon = OV->pMonitor.lock();
    g_pOverview.reset();
    if (mon) {
      g_pHyprRenderer->damageMonitor(mon);
      g_pCompositor->scheduleFrameForMonitor(mon);
    }
    ((origRenderWorkspace)(g_pRenderWorkspaceHook->m_original))(
        thisptr, pMonitor, pWorkspace, now, geometry);
    return;
  }

  if (!OV || renderingOverview || OV->blockOverviewRendering) {
    ((origRenderWorkspace)(g_pRenderWorkspaceHook->m_original))(
        thisptr, pMonitor, pWorkspace, now, geometry);
    return;
  }

  const auto OVMON = OV->pMonitor.lock();
  if (!OVMON || OVMON != pMonitor) {
    ((origRenderWorkspace)(g_pRenderWorkspaceHook->m_original))(
        thisptr, pMonitor, pWorkspace, now, geometry);
    return;
  }

  if (OV->closeUnderlayActive()) {
    ((origRenderWorkspace)(g_pRenderWorkspaceHook->m_original))(
        thisptr, pMonitor, pWorkspace, now, geometry);
    OV->render();
    return;
  }

  OV->render();
}

static void hkAddDamageA(void *thisptr, const CBox &box) {
  const auto PMONITOR = (CMonitor *)thisptr;
  auto *OV = g_pOverview.get();
  if (!OV || OV->blockDamageReporting) {
    ((origAddDamageA)g_pAddDamageHookA->m_original)(thisptr, box);
    return;
  }

  const auto OVMON = OV->pMonitor.lock();
  if (!OVMON || OVMON != PMONITOR->m_self) {
    ((origAddDamageA)g_pAddDamageHookA->m_original)(thisptr, box);
    return;
  }

  OV->onDamageReported();
}

static void hkAddDamageB(void *thisptr, const pixman_region32_t *rg) {
  const auto PMONITOR = (CMonitor *)thisptr;
  auto *OV = g_pOverview.get();
  if (!OV || OV->blockDamageReporting) {
    ((origAddDamageB)g_pAddDamageHookB->m_original)(thisptr, rg);
    return;
  }

  const auto OVMON = OV->pMonitor.lock();
  if (!OVMON || OVMON != PMONITOR->m_self) {
    ((origAddDamageB)g_pAddDamageHookB->m_original)(thisptr, rg);
    return;
  }

  OV->onDamageReported();
}


static SDispatchResult onToggle(std::string arg) {
  if (g_pOverview) {
    if (!g_pOverview->ready) {
      g_pOverview.reset();
      return {};
    }
    if (g_pOverview->closing)
      g_pOverview->reopen();
    else
      g_pOverview->close();
    return {};
  }

  const auto PMONITOR = Desktop::focusState()->monitor();
  if (!PMONITOR || !PMONITOR->m_activeWorkspace)
    return {};

  renderingOverview = true;
  g_pOverview = std::make_unique<COverview>(PMONITOR->m_activeWorkspace);
  renderingOverview = false;

  if (!g_pOverview || !g_pOverview->ready) {
    g_pOverview.reset();
    return {};
  }

  auto mon = g_pOverview->pMonitor.lock();
  if (mon) {
    g_pHyprRenderer->damageMonitor(mon);
    g_pCompositor->scheduleFrameForMonitor(mon);
  }

  return {};
}



static PHLWORKSPACE resolveWorkspaceFromArg(const std::string& arg,
                                            PHLMONITOR mon) {
  if (!mon || !mon->m_activeWorkspace)
    return nullptr;

  const std::string trimmed = horzaTrim(arg);
  if (trimmed.empty())
    return nullptr;

  int targetID = -1;
  if (trimmed[0] == '+' || trimmed[0] == '-') {
    try {
      targetID = mon->m_activeWorkspace->m_id + std::stoi(trimmed);
    } catch (...) {
      return nullptr;
    }
  } else {
    try {
      targetID = std::stoi(trimmed);
    } catch (...) {
      return nullptr;
    }
  }

  if (targetID <= 0 || mon->m_activeWorkspace->m_id == targetID)
    return nullptr;

  const auto ws = g_pCompositor->getWorkspaceByID(targetID);
  if (!ws || ws->monitorID() != mon->m_id)
    return nullptr;

  return ws;
}



static SDispatchResult onWorkspaceTransit(std::string arg) {
  const auto workspaceDispatcher =
      g_pKeybindManager->m_dispatchers.find("workspace");
  if (workspaceDispatcher == g_pKeybindManager->m_dispatchers.end())
    return {};

  const auto dispatchWorkspace = [&](const std::string& val) {
    workspaceDispatcher->second(val);
  };

  const auto PMONITOR = Desktop::focusState()->monitor();
  if (!PMONITOR || !PMONITOR->m_activeWorkspace) {
    dispatchWorkspace(arg);
    return {};
  }

  
  if (g_pOverview) {
    dispatchWorkspace(arg);
    return {};
  }

  
  
  const auto dest = resolveWorkspaceFromArg(arg, PMONITOR);
  if (!dest) {
    dispatchWorkspace(arg);
    return {};
  }

  renderingOverview = true;
  g_pOverview =
      std::make_unique<COverview>(PMONITOR->m_activeWorkspace, true, dest);
  renderingOverview = false;

  if (!g_pOverview || !g_pOverview->ready) {
    g_pOverview.reset();
    dispatchWorkspace(arg);
    return {};
  }

  if (auto mon = g_pOverview->pMonitor.lock()) {
    g_pHyprRenderer->damageMonitor(mon);
    g_pCompositor->scheduleFrameForMonitor(mon);
  }

  dispatchWorkspace(arg);
  return {};
}

APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
  PHANDLE = handle;

  g_horzaConfig = HorzaConfig{};
  registerPluginConfigValues();
  applyPluginConfigOverrides();

  const std::string HASH = __hyprland_api_get_hash();
  const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

  if (HASH != CLIENT_HASH) {
    HyprlandAPI::addNotification(PHANDLE, "[horza] Version mismatch!",
                                 CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
    throw std::runtime_error("[horza] Version mismatch");
  }

  auto FNS = HyprlandAPI::findFunctionsByName(PHANDLE, "renderWorkspace");
  if (FNS.empty())
    throw std::runtime_error("[horza] renderWorkspace not found");
  g_pRenderWorkspaceHook = HyprlandAPI::createFunctionHook(
      PHANDLE, FNS[0].address, (void *)hkRenderWorkspace);

  FNS = HyprlandAPI::findFunctionsByName(PHANDLE,
                                         "addDamageEPK15pixman_region32");
  if (FNS.empty())
    throw std::runtime_error("[horza] addDamageB not found");
  g_pAddDamageHookB = HyprlandAPI::createFunctionHook(PHANDLE, FNS[0].address,
                                                      (void *)hkAddDamageB);

  FNS = HyprlandAPI::findFunctionsByName(
      PHANDLE, "_ZN8CMonitor9addDamageERKN9Hyprutils4Math4CBoxE");
  if (FNS.empty())
    throw std::runtime_error("[horza] addDamageA not found");
  g_pAddDamageHookA = HyprlandAPI::createFunctionHook(PHANDLE, FNS[0].address,
                                                      (void *)hkAddDamageA);

  bool success = g_pRenderWorkspaceHook->hook();
  success = success && g_pAddDamageHookA->hook();
  success = success && g_pAddDamageHookB->hook();

  if (!success)
    throw std::runtime_error("[horza] Failed to activate hooks");

  HyprlandAPI::addDispatcherV2(PHANDLE, "horza:toggle", ::onToggle);
  HyprlandAPI::addDispatcherV2(PHANDLE, "horza:workspace",
                               ::onWorkspaceTransit);
  g_pMainConfigReloadHook = g_pHookSystem->hookDynamic(
      "configReloaded", [](void* self, SCallbackInfo& info, std::any param) {
        reloadRuntimeConfig();
        refreshOverviewAfterConfig();
      });

  HyprlandAPI::addNotification(PHANDLE, "[horza] loaded!",
                               CHyprColor{0.2, 0.8, 0.2, 1.0}, 3000);

  return {"horza", "Workspace overview", "you", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
  g_pMainConfigReloadHook.reset();
  g_pHyprRenderer->m_renderPass.removeAllOfType("COverviewPassElement");
  g_pOverview.reset();
}
