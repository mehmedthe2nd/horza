#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

struct HorzaConfig {
  float captureScale = 0.96f;
  float displayScale = 0.70f;
  float overviewGap = 16.0f;
  float inactiveTileSizePercent = 85.0f;
  bool persistentCache = true;
  float cacheTtlMs = 1500.0f;
  int cacheMaxEntries = 96;
  float captureBudgetMs = 4.0f;
  int maxCapturesPerFrame = 1;
  float livePreviewFps = 6.0f;
  int livePreviewRadius = 2;
  bool prewarmAll = false;
  bool hyprpaperBackground = false;
  float backgroundBlurRadius = 3.0f;
  int backgroundBlurPasses = 1;
  float backgroundBlurSpread = 1.0f;
  float backgroundBlurStrength = 1.0f;
  float backgroundTint = 0.35f;
  bool cardShadow = true;
  std::string cardShadowMode = "fast";
  std::string cardShadowTexture = "";
  float cardShadowAlpha = 0.16f;
  float cardShadowSize = 14.0f;
  float cardShadowOffsetY = 8.0f;
  bool showWindowTitles = false;
  int titleFontSize = 14;
  std::string titleFontFamily = "Inter Regular";
  float titleBackgroundAlpha = 0.35f;
  bool freezeAnimationsInOverview = true;
  bool escOnly = true;
  bool asyncCloseHandoff = false;
  float asyncCloseFadeStart = 0.88f;
  std::string asyncCloseFadeCurve = "ease_out";
  float asyncCloseMinAlpha = 0.0f;
  float closeDropDelayMs = 100.0f;
  float dragHoverJumpDelayMs = 1000.0f;
  bool vertical = false;
  float centerOffset = 0.0f;
  int cornerRadius = 5;
};

inline HorzaConfig g_horzaConfig;

inline float clampCaptureScale(float v) {
  if (!std::isfinite(v) || v <= 0.0f)
    return 1.0f;
  return std::clamp(v, 0.05f, 1.0f);
}

inline float clampDisplayScale(float v) {
  if (!std::isfinite(v))
    return 0.70f;
  return std::clamp(v, 0.05f, 3.0f);
}

inline float clampInactiveTileSizePercent(float v) {
  if (!std::isfinite(v))
    return 85.0f;
  return std::clamp(v, 0.0f, 100.0f);
}

inline float clampInactiveTileShrinkPercent(float v) {
  if (!std::isfinite(v))
    return 15.0f;
  return std::clamp(v, 0.0f, 100.0f);
}

inline std::string horzaTrim(const std::string& s) {
  const size_t start = s.find_first_not_of(" \t\r\n");
  const size_t end = s.find_last_not_of(" \t\r\n");
  return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

inline std::string normalizeHorzaToken(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  std::replace(s.begin(), s.end(), '-', '_');
  return s;
}

inline std::string stripWrappedQuotes(const std::string& s) {
  if (s.size() >= 2 &&
      ((s.front() == '"' && s.back() == '"') ||
       (s.front() == '\'' && s.back() == '\'')))
    return s.substr(1, s.size() - 2);
  return s;
}

inline bool applyHorzaPreset(const std::string& presetRaw, HorzaConfig& cfg) {
  const std::string preset = normalizeHorzaToken(horzaTrim(presetRaw));

  if (preset == "default" || preset == "stock" || preset == "none" ||
      preset == "custom") {
    cfg = HorzaConfig{};
    return true;
  }

  if (preset == "gnome_fast") {
    cfg = HorzaConfig{};
    cfg.captureScale = 0.72f;
    cfg.displayScale = 0.68f;
    cfg.overviewGap = 18.0f;
    cfg.inactiveTileSizePercent = 85.0f;
    cfg.persistentCache = true;
    cfg.cacheTtlMs = 1500.0f;
    cfg.cacheMaxEntries = 96;
    cfg.captureBudgetMs = 4.0f;
    cfg.maxCapturesPerFrame = 1;
    cfg.livePreviewFps = 8.0f;
    cfg.livePreviewRadius = 1;
    cfg.prewarmAll = false;
    cfg.hyprpaperBackground = false;
    cfg.backgroundBlurRadius = 0.0f;
    cfg.backgroundBlurPasses = 0;
    cfg.backgroundBlurSpread = 1.0f;
    cfg.backgroundBlurStrength = 0.0f;
    cfg.backgroundTint = 0.30f;
    cfg.cardShadow = true;
    cfg.cardShadowMode = "fast";
    cfg.cardShadowTexture = "";
    cfg.cardShadowAlpha = 0.16f;
    cfg.cardShadowSize = 14.0f;
    cfg.cardShadowOffsetY = 8.0f;
    cfg.showWindowTitles = false;
    cfg.titleFontSize = 13;
    cfg.titleFontFamily = "Inter Regular";
    cfg.titleBackgroundAlpha = 0.30f;
    cfg.freezeAnimationsInOverview = true;
    cfg.escOnly = true;
    cfg.asyncCloseHandoff = false;
    cfg.asyncCloseFadeStart = 0.88f;
    cfg.asyncCloseFadeCurve = "ease_out";
    cfg.asyncCloseMinAlpha = 0.0f;
    cfg.closeDropDelayMs = 100.0f;
    cfg.dragHoverJumpDelayMs = 1000.0f;
    cfg.vertical = false;
    cfg.centerOffset = 0.0f;
    cfg.cornerRadius = 5;
    return true;
  }

  return false;
}
