// Overview rendering pipeline (layout, tile drawing, overlays, titles, and shadows).
#include "overview.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <drm_fourcc.h>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define private public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#undef private

static std::string expandHomePath(const std::string& path) {
  if (path.empty() || path[0] != '~')
    return path;
  if (path.size() > 1 && path[1] != '/')
    return path;

  const char* home = std::getenv("HOME");
  if (!home || !*home)
    return path;

  if (path.size() == 1)
    return std::string(home);

  return std::string(home) + path.substr(1);
}

static bool loadTextureFromPNG(const std::string& path, SP<CTexture>& outTex) {
  int w = 0, h = 0, channels = 0;
  stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
  if (!pixels || w <= 0 || h <= 0)
    return false;

  const uint32_t stride = (uint32_t)w * 4;
  outTex = makeShared<CTexture>(DRM_FORMAT_ABGR8888, pixels, stride,
                                Vector2D{(double)w, (double)h}, false);
  stbi_image_free(pixels);

  return outTex && outTex->m_size.x > 0 && outTex->m_size.y > 0;
}

static bool loadBuiltInShadowTexture(SP<CTexture>& outTex) {
  constexpr int SHADOW_SIZE = 256;
  constexpr float INNER = 0.42f;
  constexpr float FEATHER = 0.36f;

  std::vector<uint8_t> pixels((size_t)SHADOW_SIZE * (size_t)SHADOW_SIZE * 4U, 0);

  for (int y = 0; y < SHADOW_SIZE; ++y) {
    for (int x = 0; x < SHADOW_SIZE; ++x) {
      const float nx = (((float)x + 0.5f) / (float)SHADOW_SIZE) * 2.0f - 1.0f;
      const float ny = (((float)y + 0.5f) / (float)SHADOW_SIZE) * 2.0f - 1.0f;

      const float ax = std::fabs(nx);
      const float ay = std::fabs(ny);

      float alpha = 0.0f;
      if (ax > INNER || ay > INNER) {
        const float dx = std::max(ax - INNER, 0.0f);
        const float dy = std::max(ay - INNER, 0.0f);
        const float dist = std::sqrt(dx * dx + dy * dy);
        float t = std::clamp(1.0f - dist / FEATHER, 0.0f, 1.0f);
        
        t = t * t * (3.0f - 2.0f * t);
        alpha = std::pow(t, 1.9f) * 0.72f;

        
        const float lowerBias = std::clamp(0.80f + 0.35f * ((ny + 1.0f) * 0.5f),
                                           0.0f, 1.15f);
        alpha = std::clamp(alpha * lowerBias, 0.0f, 1.0f);
      }

      const size_t idx = ((size_t)y * (size_t)SHADOW_SIZE + (size_t)x) * 4U;
      pixels[idx + 0] = 0;
      pixels[idx + 1] = 0;
      pixels[idx + 2] = 0;
      pixels[idx + 3] = (uint8_t)std::round(alpha * 255.0f);
    }
  }

  const uint32_t stride = SHADOW_SIZE * 4U;
  outTex = makeShared<CTexture>(DRM_FORMAT_ABGR8888, pixels.data(), stride,
                                Vector2D{(double)SHADOW_SIZE, (double)SHADOW_SIZE}, false);

  return outTex && outTex->m_size.x > 0 && outTex->m_size.y > 0;
}

static bool isRenderableTexture(const SP<CTexture>& tex) {
  if (!tex)
    return false;
  if (tex->m_texID != 0)
    return true;
  return tex->m_size.x > 0 && tex->m_size.y > 0;
}

static bool endsWithIgnoreCase(const std::string& value,
                               const std::string& suffix) {
  if (suffix.size() > value.size())
    return false;
  for (size_t i = 0; i < suffix.size(); ++i) {
    const char a = (char)std::tolower(
        (unsigned char)value[value.size() - suffix.size() + i]);
    const char b = (char)std::tolower((unsigned char)suffix[i]);
    if (a != b)
      return false;
  }
  return true;
}

bool COverview::openingAnimInProgress() const {
  if (closing || !m_scale)
    return false;

  if (openAnimPending)
    return true;

  const float target = effectiveDisplayScale(g_horzaConfig.displayScale);
  const float delta = std::fabs(m_scale->value() - target);
  return delta > 0.02f;
}

bool COverview::switchAnimInProgress() const {
  if (closing || !m_offsetX)
    return false;
  return m_offsetX->isBeingAnimated();
}

bool COverview::shouldDeferCaptures() const {
  if (closing)
    return false;
  if (openingAnimInProgress())
    return true;
  if (switchAnimInProgress())
    return true;
  return false;
}

bool COverview::needsFramePump() const {
  if (closing)
    return true;
  if (openingAnimInProgress())
    return true;
  if (switchAnimInProgress())
    return true;
  if (m_crossOffset && m_crossOffset->isBeingAnimated())
    return true;
  if (leftButtonDown || draggingWindow)
    return true;
  if (pendingCapture || damageDirty)
    return true;
  return false;
}

bool COverview::closeDropPending() const { return closeDropScheduled; }

void COverview::scheduleCloseDrop() {
  if (closeDropScheduled)
    return;
  closeDropScheduled = true;
  const auto MON = pMonitor;
 
  if (const auto PMON = MON.lock()) {
    g_pHyprRenderer->damageMonitor(PMON);
    g_pCompositor->scheduleFrameForMonitor(PMON);
  }
}

void COverview::refreshCardShadowTexture() {
  const bool shadowEnabled = g_horzaConfig.cardShadow;
  const auto mode = normalizeHorzaToken(horzaTrim(g_horzaConfig.cardShadowMode));
  const bool wantTextureShadow = shadowEnabled && mode == "texture";

  if (!wantTextureShadow) {
    cardShadowTex.reset();
    cardShadowTexConfigPath.clear();
    cardShadowTexResolvedPath.clear();
    cardShadowMissingPathLogged = false;
    cardShadowLoadErrorLogged = false;
    return;
  }

  const std::string configPath = horzaTrim(g_horzaConfig.cardShadowTexture);
  if (configPath.empty()) {
    constexpr const char* BUILTIN = "__builtin__";
    const bool alreadyLoadedBuiltin =
        cardShadowTex && cardShadowTexConfigPath == BUILTIN &&
        cardShadowTexResolvedPath == BUILTIN;
    if (alreadyLoadedBuiltin)
      return;

    cardShadowTex.reset();
    cardShadowTexConfigPath = BUILTIN;
    cardShadowTexResolvedPath = BUILTIN;
    cardShadowMissingPathLogged = false;

    if (!loadBuiltInShadowTexture(cardShadowTex)) {
      if (!cardShadowLoadErrorLogged) {
        std::cerr << "[horza] failed to create built-in card shadow texture; "
                     "falling back to fast shadow\n";
        cardShadowLoadErrorLogged = true;
      }
      return;
    }
    cardShadowLoadErrorLogged = false;
    return;
  }
  cardShadowMissingPathLogged = false;

  const std::string resolvedPath = expandHomePath(configPath);
  const bool pathChanged = configPath != cardShadowTexConfigPath ||
                           resolvedPath != cardShadowTexResolvedPath;

  if (!pathChanged) {
    if (cardShadowTex)
      return;
    if (cardShadowLoadErrorLogged)
      return;
  }

  cardShadowTex.reset();
  cardShadowTexConfigPath = configPath;
  cardShadowTexResolvedPath = resolvedPath;

  if (!loadTextureFromPNG(resolvedPath, cardShadowTex)) {
    if (!cardShadowLoadErrorLogged) {
      std::cerr << "[horza] failed to load card shadow texture: "
                << cardShadowTexResolvedPath
                << " ; using built-in shadow texture fallback\n";
      cardShadowLoadErrorLogged = true;
    }
    if (!loadBuiltInShadowTexture(cardShadowTex))
      cardShadowTex.reset();
    return;
  }

  cardShadowLoadErrorLogged = false;
}

bool COverview::isTileOnScreen(const CBox& box) const {
  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR)
    return false;
  if (box.w <= 1.0 || box.h <= 1.0)
    return false;

  return box.x + box.w > 0.0 && box.y + box.h > 0.0 &&
         box.x < PMONITOR->m_size.x && box.y < PMONITOR->m_size.y;
}

int COverview::pickVisibleLivePreviewWorkspace(
    std::chrono::steady_clock::time_point now) const {
  if (images.size() < 2)
    return -1;

  const float fps = std::clamp(g_horzaConfig.livePreviewFps, 0.0f, 60.0f);
  if (fps <= 0.0f)
    return -1;
  const auto minVisibleInterval = std::chrono::duration<float>(1.0f / fps);
  const int captureRadius = std::max(0, g_horzaConfig.livePreviewRadius);

  int bestIdx = -1;
  auto bestCaptureTime = std::chrono::steady_clock::time_point::max();

  for (int i = 0; i < (int)images.size(); ++i) {
    if (i == currentIdx || !images[i].captured)
      continue;
    if (captureRadius <= 0 || std::abs(i - currentIdx) > captureRadius)
      continue;
    if (!isTileOnScreen(images[i].displayBox))
      continue;
    if (now - images[i].lastCaptureAt < minVisibleInterval)
      continue;

    if (bestIdx == -1 || images[i].lastCaptureAt < bestCaptureTime) {
      bestIdx = i;
      bestCaptureTime = images[i].lastCaptureAt;
    }
  }

  return bestIdx;
}

std::string COverview::workspaceTitleFor(const PHLWORKSPACE& ws) const {
  if (!ws)
    return "";

  auto PWINDOW = ws->getLastFocusedWindow();
  if (!PWINDOW)
    PWINDOW = ws->getFirstWindow();

  std::string title;
  if (PWINDOW) {
    title = PWINDOW->m_title;
    if (title.empty())
      title = PWINDOW->m_class;
  }

  if (title.empty()) {
    if (!ws->m_name.empty())
      title = ws->m_name;
    else
      title = "Workspace " + std::to_string(ws->m_id);
  }

  return title;
}

void COverview::suppressGlobalAnimations() const {
  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR)
    return;

  
  for (const auto& wsWeak : g_pCompositor->m_workspaces) {
    const auto ws = wsWeak.lock();
    if (!ws || ws->monitorID() != PMONITOR->m_id)
      continue;

    if (ws->m_renderOffset)
      ws->m_renderOffset->setValueAndWarp(ws->m_renderOffset->goal());
    if (ws->m_alpha)
      ws->m_alpha->setValueAndWarp(ws->m_alpha->goal());
  }

  
  for (const auto& win : g_pCompositor->m_windows) {
    if (!win)
      continue;

    bool onOverviewMonitor = false;
    if (win->m_workspace && win->m_workspace->monitorID() == PMONITOR->m_id)
      onOverviewMonitor = true;
    else if (win->m_monitor && win->m_monitor->m_id == PMONITOR->m_id)
      onOverviewMonitor = true;
    if (!onOverviewMonitor)
      continue;
    if (!win->m_isMapped && !win->m_fadingOut)
      continue;

    if (win->m_realPosition)
      win->m_realPosition->setValueAndWarp(win->m_realPosition->goal());
    if (win->m_realSize)
      win->m_realSize->setValueAndWarp(win->m_realSize->goal());
    if (win->m_alpha)
      win->m_alpha->setValueAndWarp(win->m_alpha->goal());
    if (win->m_activeInactiveAlpha)
      win->m_activeInactiveAlpha->setValueAndWarp(win->m_activeInactiveAlpha->goal());
    if (win->m_movingFromWorkspaceAlpha)
      win->m_movingFromWorkspaceAlpha->setValueAndWarp(
          win->m_movingFromWorkspaceAlpha->goal());
    if (win->m_movingToWorkspaceAlpha)
      win->m_movingToWorkspaceAlpha->setValueAndWarp(
          win->m_movingToWorkspaceAlpha->goal());
  }

  
  if (PMONITOR->m_specialFade)
    PMONITOR->m_specialFade->setValueAndWarp(PMONITOR->m_specialFade->goal());
}

void COverview::suppressWorkspaceWindowAnimations(const PHLWORKSPACE& ws) const {
  if (!ws)
    return;

  for (const auto& win : g_pCompositor->m_windows) {
    if (!win || win->m_workspace != ws)
      continue;
    if (!win->m_isMapped && !win->m_fadingOut)
      continue;

    if (win->m_realPosition)
      win->m_realPosition->setValueAndWarp(win->m_realPosition->goal());
    if (win->m_realSize)
      win->m_realSize->setValueAndWarp(win->m_realSize->goal());
    if (win->m_alpha)
      win->m_alpha->setValueAndWarp(win->m_alpha->goal());
    if (win->m_activeInactiveAlpha)
      win->m_activeInactiveAlpha->setValueAndWarp(win->m_activeInactiveAlpha->goal());
    if (win->m_movingFromWorkspaceAlpha)
      win->m_movingFromWorkspaceAlpha->setValueAndWarp(
          win->m_movingFromWorkspaceAlpha->goal());
    if (win->m_movingToWorkspaceAlpha)
      win->m_movingToWorkspaceAlpha->setValueAndWarp(
          win->m_movingToWorkspaceAlpha->goal());
  }
}

void COverview::renderWorkspaceTitle(int idx, const CRegion& dmg, float tileScale) {
  (void)tileScale;

  if (idx < 0 || idx >= (int)images.size())
    return;

  auto& img = images[idx];
  if (!g_horzaConfig.showWindowTitles) {
    
    img.titleTex.reset();
    img.titleTextCached.clear();
    img.titleMaxWidthCached = 0;
    img.titleFontCached = 0;
    img.titleFontFamilyCached.clear();
    return;
  }

  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR)
    return;

  constexpr float overlayA = 1.0f;

  if (img.displayBox.w <= 8.0 || img.displayBox.h <= 8.0)
    return;

  const std::string title = workspaceTitleFor(img.pWorkspace);
  if (title.empty())
    return;

  const int fontPt = std::clamp(g_horzaConfig.titleFontSize, 6, 64);
  std::string fontFamily = horzaTrim(g_horzaConfig.titleFontFamily);
  if (fontFamily.empty())
    fontFamily = "Inter Regular";
  
  
  if (endsWithIgnoreCase(fontFamily, " Regular"))
    fontFamily = horzaTrim(fontFamily.substr(0, fontFamily.size() - 8));
  
  
  const int maxTextPx = std::max(
      64, (int)std::round(std::max(64.0, PMONITOR->m_size.x * 0.90) * PMONITOR->m_scale));

  if (!img.titleTex || img.titleTextCached != title ||
      img.titleFontCached != fontPt || img.titleMaxWidthCached != maxTextPx ||
      img.titleFontFamilyCached != fontFamily) {
    img.titleTex = g_pHyprOpenGL->renderText(title,
                                             CHyprColor{1.0, 1.0, 1.0, 1.0},
                                             fontPt, false, fontFamily, maxTextPx,
                                             400);
    img.titleTextCached = title;
    img.titleFontCached = fontPt;
    img.titleMaxWidthCached = maxTextPx;
    img.titleFontFamilyCached = fontFamily;
  }

  auto textTex = img.titleTex;
  if (!isRenderableTexture(textTex))
    return;

  const float pillPadX = 10.0f;
  const float pillPadY = 4.0f;
  const float belowGap = 12.0f;

  const float textW = textTex->m_size.x / PMONITOR->m_scale;
  const float textH = textTex->m_size.y / PMONITOR->m_scale;
  const float drawTextW = textW;

  float bgW = drawTextW + pillPadX * 2.0f;
  float bgH = textH + pillPadY * 2.0f;
  float bgX = img.displayBox.x + (img.displayBox.w - bgW) * 0.5f;
  float bgY = img.displayBox.y + img.displayBox.h + belowGap;
  
  

  CBox bgbox = {bgX, bgY, bgW, bgH};
  if (bgbox.w <= 0.0 || bgbox.h <= 0.0)
    return;

  CBox textBox = {bgbox.x + pillPadX, bgbox.y + pillPadY, drawTextW, textH};
  if (textBox.w <= 0.0 || textBox.h <= 0.0)
    return;

  bgbox.scale(PMONITOR->m_scale);
  bgbox.round();
  textBox.scale(PMONITOR->m_scale);
  textBox.round();

  if (bgbox.w <= 0 || bgbox.h <= 0 || textBox.w <= 0 || textBox.h <= 0)
    return;

  CHyprOpenGLImpl::SRectRenderData rectData;
  rectData.damage = &dmg;
  rectData.round = std::max(0, (int)std::round(bgbox.h * 0.5f));
  rectData.roundingPower = 2.0f;

  const float bgA = std::clamp(g_horzaConfig.titleBackgroundAlpha * overlayA, 0.0f,
                               1.0f);
  if (bgA > 0.0f)
    g_pHyprOpenGL->renderRect(bgbox, CHyprColor{0.0, 0.0, 0.0, bgA}, rectData);

  CHyprOpenGLImpl::STextureRenderData textData;
  textData.damage = &dmg;
  textData.a = overlayA;
  g_pHyprOpenGL->renderTextureInternal(textTex, textBox, textData);
}


void COverview::fullRender() {
  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR || images.empty())
    return;

  if (openAnimPending && !closing) {
    openAnimPending = false;
    *m_scale = effectiveDisplayScale(g_horzaConfig.displayScale);
    *m_crossOffset = g_horzaConfig.centerOffset;
  }

  constexpr float overlayA = 1.0f;

  CRegion dmg{0, 0, INT16_MAX, INT16_MAX};

  const float targetDisplayScale = effectiveDisplayScale(g_horzaConfig.displayScale);
  float ds = transitMode ? 1.0f : std::max(targetDisplayScale, 0.0001f);
  const float sMin = std::min(1.0f, targetDisplayScale);
  const float sMax = std::max(1.0f, targetDisplayScale);
  float s =
      transitMode ? 1.0f : std::clamp(m_scale ? m_scale->value() : 1.0f, sMin, sMax);
  if (!std::isfinite(s))
    s = transitMode ? 1.0f : targetDisplayScale;
  float tileW = PMONITOR->m_size.x * s;
  float tileH = PMONITOR->m_size.y * s;
  float baseGap = g_horzaConfig.overviewGap;
  float gap = transitMode ? 0.0f : baseGap * (s / ds);
  bool vertical = transitMode ? false : g_horzaConfig.vertical;
  const float centerOffset =
      transitMode
          ? 0.0f
          : (m_crossOffset ? m_crossOffset->value() : g_horzaConfig.centerOffset);

  float startX = 0.0f;
  float startY = 0.0f;

  if (!vertical) {
    float centerX = PMONITOR->m_size.x * 0.5f -
                    (currentIdx * (tileW + gap) + tileW * 0.5f);
    startX = centerX + (m_offsetX ? m_offsetX->value() : 0.0f);
    startY = (PMONITOR->m_size.y - tileH) * 0.5f + centerOffset;
  } else {
    float centerY = PMONITOR->m_size.y * 0.5f -
                    (currentIdx * (tileH + gap) + tileH * 0.5f);
    startY = centerY + (m_offsetX ? m_offsetX->value() : 0.0f);
    startX = (PMONITOR->m_size.x - tileW) * 0.5f + centerOffset;
  }

  const float inactiveTileSizeScale = transitMode
                                          ? 1.0f
                                          : std::clamp(
                                                g_horzaConfig.inactiveTileSizePercent * 0.01f,
                                                0.0f, 1.0f);
  const float centerPrimary = !vertical ? PMONITOR->m_size.x * 0.5f
                                        : PMONITOR->m_size.y * 0.5f;
  const float tileStep = !vertical ? (tileW + gap) : (tileH + gap);
  const int renderRadius = std::max(0, g_horzaConfig.livePreviewRadius);

  if (g_horzaConfig.hyprpaperBackground && backgroundCaptured) {
    CBox bgbox = {0, 0, PMONITOR->m_size.x, PMONITOR->m_size.y};
    bgbox.scale(PMONITOR->m_scale);
    bgbox.round();

    CHyprOpenGLImpl::STextureRenderData bgRenderData;
    bgRenderData.damage = &dmg;
    bgRenderData.a = 1.0f;

    auto bgTex = backgroundFb.getTexture();
    if (isRenderableTexture(bgTex))
      g_pHyprOpenGL->renderTextureInternal(bgTex, bgbox, bgRenderData);
    else
      g_pHyprOpenGL->clear(CHyprColor{0, 0, 0, 1.0});
  } else {
    g_pHyprOpenGL->clear(CHyprColor{0, 0, 0, 1.0});
  }

  bool hasVisibleUncaptured = false;
  int drawnTileCount = 0;
  refreshCardShadowTexture();
  const bool drawCardShadow = !transitMode && g_horzaConfig.cardShadow;
  const auto shadowMode = normalizeHorzaToken(horzaTrim(g_horzaConfig.cardShadowMode));
  const bool preferTextureShadow = shadowMode == "texture";
  const bool useTextureShadow = preferTextureShadow && isRenderableTexture(cardShadowTex);
  const float shadowAlpha = std::clamp(g_horzaConfig.cardShadowAlpha, 0.0f, 1.0f);
  const float shadowSize = std::max(0.0f, g_horzaConfig.cardShadowSize);
  const float shadowOffsetY = g_horzaConfig.cardShadowOffsetY;
  const int baseCornerPx = std::max(0, (int)(g_horzaConfig.cornerRadius * PMONITOR->m_scale));

  for (int i = 0; i < (int)images.size(); i++) {
    if (std::abs(i - currentIdx) > renderRadius) {
      images[i].displayBox = {};
      continue;
    }

    const float baseX = !vertical ? startX + i * (tileW + gap) : startX;
    const float baseY = !vertical ? startY : startY + i * (tileH + gap);
    const float tileCenterPrimary =
        !vertical ? baseX + tileW * 0.5f : baseY + tileH * 0.5f;
    const float normFromCenter =
        tileStep > 0.001f
            ? std::clamp(std::abs(tileCenterPrimary - centerPrimary) / tileStep, 0.0f,
                         1.0f)
            : 1.0f;
    const float tileScaleFactor =
        1.0f - (1.0f - inactiveTileSizeScale) * normFromCenter;
    const float drawW = tileW * tileScaleFactor;
    const float drawH = tileH * tileScaleFactor;
    const float x = baseX - (drawW - tileW) * 0.5f;
    const float y = baseY - (drawH - tileH) * 0.5f;

    images[i].displayBox = {x, y, drawW, drawH};
    const bool tileOnScreen = isTileOnScreen(images[i].displayBox);

    CBox texbox = {x, y, drawW, drawH};
    texbox.scale(PMONITOR->m_scale);
    texbox.round();
    if (texbox.w <= 0 || texbox.h <= 0)
      continue;

    auto drawTileShadow = [&]() {
      if (!drawCardShadow || shadowAlpha <= 0.0f || shadowSize <= 0.0f)
        return;

      if (useTextureShadow) {
        CBox shadowBox = {
            x - shadowSize,
            y - shadowSize + shadowOffsetY,
            drawW + shadowSize * 2.0f,
            drawH + shadowSize * 2.0f,
        };
        shadowBox.scale(PMONITOR->m_scale);
        shadowBox.round();
        if (shadowBox.w <= 0 || shadowBox.h <= 0)
          return;

        CHyprOpenGLImpl::STextureRenderData shadowTexData;
        shadowTexData.damage = &dmg;
        shadowTexData.a = std::clamp(shadowAlpha * overlayA, 0.0f, 1.0f);
        if (shadowTexData.a <= 0.0f)
          return;

        g_pHyprOpenGL->renderTextureInternal(cardShadowTex, shadowBox,
                                             shadowTexData);
        return;
      }

      CHyprOpenGLImpl::SRectRenderData shadowData;
      shadowData.damage = &dmg;
      shadowData.roundingPower = 2.0f;

      auto drawShadowLayer = [&](float spreadMul, float alphaMul) {
        const float spread = shadowSize * spreadMul;
        CBox shadowBox = {
            x - spread,
            y - spread + shadowOffsetY,
            drawW + spread * 2.0f,
            drawH + spread * 2.0f,
        };
        shadowBox.scale(PMONITOR->m_scale);
        shadowBox.round();
        if (shadowBox.w <= 0 || shadowBox.h <= 0)
          return;

        const int spreadPx =
            std::max(0, (int)std::round(spread * PMONITOR->m_scale));
        shadowData.round = baseCornerPx + spreadPx;
        const float layerAlpha =
            std::clamp(shadowAlpha * alphaMul * overlayA, 0.0f, 1.0f);
        if (layerAlpha <= 0.0f)
          return;

        g_pHyprOpenGL->renderRect(shadowBox,
                                  CHyprColor{0.0, 0.0, 0.0, layerAlpha},
                                  shadowData);
      };

      
      
      drawShadowLayer(1.25f, 0.35f);
      drawShadowLayer(0.55f, 1.00f);
    };

    const bool drawDropTarget = draggingWindow && leftButtonDown && dragWindow &&
                                dragTargetIdx == i;
    auto drawDropTargetHighlight = [&]() {
      if (!drawDropTarget)
        return;

      CBox ringBox = {
          x - 2.0f,
          y - 2.0f,
          drawW + 4.0f,
          drawH + 4.0f,
      };
      ringBox.scale(PMONITOR->m_scale);
      ringBox.round();
      if (ringBox.w <= 0 || ringBox.h <= 0)
        return;

      CHyprOpenGLImpl::SRectRenderData ringData;
      ringData.damage = &dmg;
      ringData.roundingPower = 2.0f;
      ringData.round =
          baseCornerPx + std::max(1, (int)std::round(2.0f * PMONITOR->m_scale));

      const float ringAlpha = std::clamp(0.20f * overlayA, 0.0f, 1.0f);
      if (ringAlpha > 0.0f)
        g_pHyprOpenGL->renderRect(ringBox, CHyprColor{1.0, 1.0, 1.0, ringAlpha},
                                  ringData);

      CHyprOpenGLImpl::SRectRenderData fillData;
      fillData.damage = &dmg;
      fillData.roundingPower = 2.0f;
      fillData.round = baseCornerPx;

      const float fillAlpha = std::clamp(0.10f * overlayA, 0.0f, 1.0f);
      if (fillAlpha > 0.0f)
        g_pHyprOpenGL->renderRect(texbox, CHyprColor{1.0, 1.0, 1.0, fillAlpha},
                                  fillData);
    };

    SP<CTexture> tex;
    if (images[i].captured) {
      tex = images[i].fb.getTexture();
      if (!isRenderableTexture(tex)) {
        images[i].captured = false;
        if (tileOnScreen)
          hasVisibleUncaptured = true;
        continue;
      }
    } else {
      tex = images[i].cachedTex;
      if (!isRenderableTexture(tex)) {
        images[i].cachedTex.reset();
        if (tileOnScreen)
          hasVisibleUncaptured = true;
        continue;
      }

      
      
      if (tileOnScreen)
        hasVisibleUncaptured = true;
    }

    drawTileShadow();

    CHyprOpenGLImpl::STextureRenderData renderData;
    renderData.damage = &dmg;
    renderData.a = overlayA;
    renderData.round = (int)(g_horzaConfig.cornerRadius * PMONITOR->m_scale);
    renderData.roundingPower = 2.0f;

    g_pHyprOpenGL->renderTextureInternal(tex, texbox, renderData);
    drawnTileCount++;
    drawDropTargetHighlight();
    if (!transitMode)
      renderWorkspaceTitle(i, dmg, s * tileScaleFactor);
  }

  if (drawnTileCount == 0) {
    static auto lastNoTilesLog = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (lastNoTilesLog.time_since_epoch().count() == 0 ||
        now - lastNoTilesLog > std::chrono::milliseconds(750)) {
      lastNoTilesLog = now;
      Log::logger->log(
          Log::ERR,
          "[horza] fullRender: drew 0 tiles images={} currentIdx={} pendingCapture={} damageDirty={}",
          images.size(), currentIdx, pendingCapture ? "true" : "false",
          damageDirty ? "true" : "false");
    }
  }

  const bool drawDragGhost = draggingWindow && leftButtonDown && dragWindow &&
                             dragWindowPosWorkspace.x >= 0.0 &&
                             dragWindowPosWorkspace.y >= 0.0 &&
                             dragWindowSizeWorkspace.x > 0.0 &&
                             dragWindowSizeWorkspace.y > 0.0;
  if (drawDragGhost) {
    int refIdx = dragTargetIdx;
    if (refIdx < 0 || refIdx >= (int)images.size())
      refIdx = dragSourceIdx;
    if (refIdx < 0 || refIdx >= (int)images.size())
      refIdx = currentIdx;

    float tileScaleX = 1.0f;
    float tileScaleY = 1.0f;
    if (refIdx >= 0 && refIdx < (int)images.size()) {
      const auto& refBox = images[refIdx].displayBox;
      tileScaleX =
          (float)(refBox.w / std::max(1.0f, (float)PMONITOR->m_size.x));
      tileScaleY =
          (float)(refBox.h / std::max(1.0f, (float)PMONITOR->m_size.y));
    }

    const float ghostW =
        std::max(24.0f, (float)(dragWindowSizeWorkspace.x * tileScaleX));
    const float ghostH =
        std::max(18.0f, (float)(dragWindowSizeWorkspace.y * tileScaleY));
    const float ghostX =
        (float)(lastMousePosLocal.x - dragWindowGrabOffsetWorkspace.x * tileScaleX);
    const float ghostY =
        (float)(lastMousePosLocal.y - dragWindowGrabOffsetWorkspace.y * tileScaleY);

    const float ringInset = 1.5f;
    CBox ghostOuterBox = {
        ghostX - ringInset,
        ghostY - ringInset,
        ghostW + ringInset * 2.0f,
        ghostH + ringInset * 2.0f,
    };
    ghostOuterBox.scale(PMONITOR->m_scale);
    ghostOuterBox.round();

    CBox ghostBox = {ghostX, ghostY, ghostW, ghostH};
    ghostBox.scale(PMONITOR->m_scale);
    ghostBox.round();

    const int ghostRoundPx =
        std::max(baseCornerPx, (int)std::round(8.0f * PMONITOR->m_scale));
    SP<CTexture> ghostTex;
    int ghostSrcIdx = dragSourceIdx;
    if (ghostSrcIdx < 0 || ghostSrcIdx >= (int)images.size())
      ghostSrcIdx = currentIdx;
    if (ghostSrcIdx >= 0 && ghostSrcIdx < (int)images.size()) {
      if (images[ghostSrcIdx].captured)
        ghostTex = images[ghostSrcIdx].fb.getTexture();
      else
        ghostTex = images[ghostSrcIdx].cachedTex;
    }

    const bool drawSnapshotGhost =
        isRenderableTexture(ghostTex) && ghostBox.w > 0 && ghostBox.h > 0;

    if (drawSnapshotGhost) {
      const double monW = std::max(1.0, PMONITOR->m_size.x);
      const double monH = std::max(1.0, PMONITOR->m_size.y);

      Vector2D uvTL = {dragWindowPosWorkspace.x / monW,
                       dragWindowPosWorkspace.y / monH};
      Vector2D uvBR = {(dragWindowPosWorkspace.x + dragWindowSizeWorkspace.x) / monW,
                       (dragWindowPosWorkspace.y + dragWindowSizeWorkspace.y) / monH};

      uvTL.x = std::clamp(uvTL.x, 0.0, 1.0);
      uvTL.y = std::clamp(uvTL.y, 0.0, 1.0);
      uvBR.x = std::clamp(uvBR.x, 0.0, 1.0);
      uvBR.y = std::clamp(uvBR.y, 0.0, 1.0);

      const auto lastTL = g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft;
      const auto lastBR = g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight;
      g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft = uvTL;
      g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight = uvBR;

      CHyprOpenGLImpl::STextureRenderData ghostTexData;
      ghostTexData.damage = &dmg;
      ghostTexData.a = std::clamp(0.90f * overlayA, 0.0f, 1.0f);
      ghostTexData.round = ghostRoundPx;
      ghostTexData.roundingPower = 2.0f;
      ghostTexData.allowCustomUV = true;
      ghostTexData.allowDim = false;

      g_pHyprOpenGL->renderTextureInternal(ghostTex, ghostBox, ghostTexData);

      g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft = lastTL;
      g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight = lastBR;

      
      CHyprOpenGLImpl::SRectRenderData ghostOutlineData;
      ghostOutlineData.damage = &dmg;
      ghostOutlineData.round =
          ghostRoundPx + std::max(1, (int)std::round(ringInset * PMONITOR->m_scale));
      ghostOutlineData.roundingPower = 2.0f;
      const float dragGhostRingA = std::clamp(0.08f * overlayA, 0.0f, 1.0f);
      if (dragGhostRingA > 0.0f && ghostOuterBox.w > 0 && ghostOuterBox.h > 0)
        g_pHyprOpenGL->renderRect(ghostOuterBox,
                                  CHyprColor{1.0, 1.0, 1.0, dragGhostRingA},
                                  ghostOutlineData);
    } else {
      
      CHyprOpenGLImpl::SRectRenderData ghostOuterData;
      ghostOuterData.damage = &dmg;
      ghostOuterData.round =
          ghostRoundPx + std::max(1, (int)std::round(ringInset * PMONITOR->m_scale));
      ghostOuterData.roundingPower = 2.0f;

      CHyprOpenGLImpl::SRectRenderData ghostData;
      ghostData.damage = &dmg;
      ghostData.round = ghostRoundPx;
      ghostData.roundingPower = 2.0f;

      const float dragGhostRingA = std::clamp(0.24f * overlayA, 0.0f, 1.0f);
      if (dragGhostRingA > 0.0f && ghostOuterBox.w > 0 && ghostOuterBox.h > 0)
        g_pHyprOpenGL->renderRect(
            ghostOuterBox, CHyprColor{1.0, 1.0, 1.0, dragGhostRingA}, ghostOuterData);

      const float dragGhostFillA = std::clamp(0.12f * overlayA, 0.0f, 1.0f);
      if (dragGhostFillA > 0.0f && ghostBox.w > 0 && ghostBox.h > 0)
        g_pHyprOpenGL->renderRect(
            ghostBox, CHyprColor{1.0, 1.0, 1.0, dragGhostFillA}, ghostData);
    }
  }

  pendingCapture = hasVisibleUncaptured;
}
