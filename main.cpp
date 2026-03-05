#include "config.hpp"
#include "globals.hpp"
#include "overview.hpp"
#include "plugin_runtime.hpp"
#include <any>
#include <cmath>
#include <iostream>

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
  const char* defaultBackground =
      g_horzaConfig.hyprpaperBackground ? "hyprpaper" : "black";
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
  addPluginConfigValue("frame_pump",
                       Hyprlang::CConfigValue{boolToToken(g_horzaConfig.framePump)});
  addPluginConfigValue(
      "frame_pump_aggressive",
      Hyprlang::CConfigValue{boolToToken(g_horzaConfig.framePumpAggressive)});
  addPluginConfigValue(
      "frame_pump_fps",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.framePumpFps});
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
  addPluginConfigValue(
      "drag_hover_jump_delay_ms",
      Hyprlang::CConfigValue{(Hyprlang::FLOAT)g_horzaConfig.dragHoverJumpDelayMs});
  addPluginConfigValue("vertical",
                       Hyprlang::CConfigValue{boolToToken(g_horzaConfig.vertical)});
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

  if (getPluginFloat("capture_scale", f))
    g_horzaConfig.captureScale = clampCaptureScale((float)f);
  if (getPluginFloat("display_scale", f))
    g_horzaConfig.displayScale = clampDisplayScale((float)f);
  if (getPluginFloat("overview_gap", f))
    g_horzaConfig.overviewGap = std::max(0.0f, (float)f);
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
  if (getPluginBool("frame_pump", b))
    g_horzaConfig.framePump = b;
  if (getPluginBool("frame_pump_aggressive", b))
    g_horzaConfig.framePumpAggressive = b;
  if (getPluginFloat("frame_pump_fps", f))
    g_horzaConfig.framePumpFps = clampFramePumpFps((float)f);
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
  if (getPluginFloat("drag_hover_jump_delay_ms", f))
    g_horzaConfig.dragHoverJumpDelayMs = std::max(0.0f, (float)f);
  if (getPluginBool("vertical", b))
    g_horzaConfig.vertical = b;
  if (getPluginFloat("center_offset", f))
    g_horzaConfig.centerOffset = (float)f;
  if (getPluginInt("corner_radius", i))
    g_horzaConfig.cornerRadius = std::max(0, (int)i);
}

void reloadRuntimeConfig() {
  g_horzaConfig = HorzaConfig{};
  applyPluginConfigOverrides();
}

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

  g_pPluginRuntime = std::make_unique<CPluginRuntime>();
  g_pPluginRuntime->init(reloadRuntimeConfig);

  HyprlandAPI::addNotification(PHANDLE, "[horza] loaded!",
                               CHyprColor{0.2, 0.8, 0.2, 1.0}, 3000);
  HyprlandAPI::reloadConfig();

  return {"horza", "Workspace overview", "you", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
  g_pPluginRuntime.reset();
  g_pOverview.reset();
}
