#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

struct HorzaConfig {
  float captureScale = 1.0f;
  float displayScale = 0.60f;
  float overviewGap = 20.0f;
  float inactiveTileSizePercent = 85.0f;
  bool persistentCache = true;
  float cacheTtlMs = 5000.0f;
  int cacheMaxEntries = 96;
  float captureBudgetMs = 4.0f;
  int maxCapturesPerFrame = 1;
  float livePreviewFps = 60.0f;
  int livePreviewRadius = 1;
  bool prewarmAll = true;
  bool framePump = true;
  bool framePumpAggressive = true;
  float framePumpFps = 0.0f;
  bool hyprpaperBackground = true;
  float backgroundBlurRadius = 3.0f;
  int backgroundBlurPasses = 1;
  float backgroundBlurSpread = 1.0f;
  float backgroundBlurStrength = 1.0f;
  float backgroundTint = 0.35f;
  bool cardShadow = true;
  std::string cardShadowMode = "fast";
  std::string cardShadowTexture = "";
  float cardShadowAlpha = 0.20f;
  float cardShadowSize = 5.0f;
  float cardShadowOffsetY = 2.0f;
  bool showWindowTitles = true;
  int titleFontSize = 14;
  std::string titleFontFamily = "Inter Regular";
  float titleBackgroundAlpha = 0.35f;
  bool freezeAnimationsInOverview = true;
  bool escOnly = true;
  float dragHoverJumpDelayMs = 1000.0f;
  bool vertical = false;
  float centerOffset = 0.0f;
  int cornerRadius = 0;
};

inline HorzaConfig g_horzaConfig;

inline float clampCaptureScale(float v) {
  if (!std::isfinite(v) || v <= 0.0f)
    return 1.0f;
  return std::clamp(v, 0.05f, 1.0f);
}

inline float clampDisplayScale(float v) {
  if (!std::isfinite(v))
    return 0.60f;
  return std::clamp(v, 0.05f, 3.0f);
}

inline float effectiveDisplayScale(float configured) {
  const float clamped = clampDisplayScale(configured);
  // A configured 1.0 has no geometric headroom for enter/exit animation.
  // Keep it visually near-1 while preserving a smooth close/open path.
  if (std::fabs(clamped - 1.0f) < 0.001f)
    return 0.985f;
  return clamped;
}

inline float clampInactiveTileSizePercent(float v) {
  if (!std::isfinite(v))
    return 85.0f;
  return std::clamp(v, 0.0f, 100.0f);
}

inline float clampFramePumpFps(float v) {
  if (!std::isfinite(v) || v <= 0.0f)
    return 0.0f;
  return std::clamp(v, 1.0f, 240.0f);
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
