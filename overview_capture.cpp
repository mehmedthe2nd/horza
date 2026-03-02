// Overview capture pipeline (workspace/background framebuffer capture + tile cache).
#include "overview.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <drm_fourcc.h>
#include <unordered_map>

#define private public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/animation/DesktopAnimationManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#undef private

struct STileCacheKey {
  int monitorID = -1;
  int64_t workspaceID = -1;

  bool operator==(const STileCacheKey& other) const = default;
};

struct STileCacheKeyHash {
  size_t operator()(const STileCacheKey& key) const noexcept {
    const size_t h1 = std::hash<int>{}(key.monitorID);
    const size_t h2 = std::hash<int64_t>{}(key.workspaceID);
    return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6U) + (h1 >> 2U));
  }
};

struct STileCacheEntry {
  SP<CTexture> tex;
  std::chrono::steady_clock::time_point capturedAt{};
  std::chrono::steady_clock::time_point cachedAt{};
};

static std::unordered_map<STileCacheKey, STileCacheEntry, STileCacheKeyHash>
    g_workspaceTileCache;

static bool isRenderableTexture(const SP<CTexture>& tex);

static bool tileCacheEnabled() {
  return g_horzaConfig.persistentCache && g_horzaConfig.cacheTtlMs > 0.0f;
}

static void pruneWorkspaceTileCache() {
  if (!tileCacheEnabled()) {
    g_workspaceTileCache.clear();
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  const auto ttl = std::chrono::duration<float, std::milli>(g_horzaConfig.cacheTtlMs);

  for (auto it = g_workspaceTileCache.begin(); it != g_workspaceTileCache.end();) {
    const bool deadTex = !isRenderableTexture(it->second.tex);
    const bool expired = now - it->second.cachedAt > ttl;
    if (deadTex || expired)
      it = g_workspaceTileCache.erase(it);
    else
      ++it;
  }

  const int maxEntries = std::max(0, g_horzaConfig.cacheMaxEntries);
  while ((int)g_workspaceTileCache.size() > maxEntries) {
    auto oldestIt = g_workspaceTileCache.end();
    for (auto it = g_workspaceTileCache.begin(); it != g_workspaceTileCache.end();
         ++it) {
      if (oldestIt == g_workspaceTileCache.end() ||
          it->second.cachedAt < oldestIt->second.cachedAt)
        oldestIt = it;
    }

    if (oldestIt == g_workspaceTileCache.end())
      break;
    g_workspaceTileCache.erase(oldestIt);
  }
}

static void storeWorkspaceTileInCache(
    int monitorID, int64_t workspaceID, const SP<CTexture>& tex,
    std::chrono::steady_clock::time_point capturedAt) {
  if (!tileCacheEnabled())
    return;
  if (!isRenderableTexture(tex))
    return;

  pruneWorkspaceTileCache();

  const auto now = std::chrono::steady_clock::now();
  g_workspaceTileCache[{monitorID, workspaceID}] = {
      .tex = tex,
      .capturedAt = capturedAt.time_since_epoch().count() == 0 ? now : capturedAt,
      .cachedAt = now,
  };

  pruneWorkspaceTileCache();
}

static bool restoreWorkspaceTileFromCache(
    int monitorID, int64_t workspaceID, SP<CTexture>& outTex,
    std::chrono::steady_clock::time_point& outCapturedAt) {
  if (!tileCacheEnabled())
    return false;

  pruneWorkspaceTileCache();

  const auto it = g_workspaceTileCache.find({monitorID, workspaceID});
  if (it == g_workspaceTileCache.end())
    return false;
  if (!isRenderableTexture(it->second.tex)) {
    g_workspaceTileCache.erase(it);
    return false;
  }

  it->second.cachedAt = std::chrono::steady_clock::now();
  outTex = it->second.tex;
  outCapturedAt = it->second.capturedAt;
  return true;
}

static bool isRenderableTexture(const SP<CTexture>& tex) {
  if (!tex)
    return false;
  if (tex->m_texID != 0)
    return true;
  return tex->m_size.x > 0 && tex->m_size.y > 0;
}

static uint32_t pickSafeRenderFormat(const PHLMONITOR& mon) {
  if (!mon || !mon->m_output || !mon->m_output->state)
    return DRM_FORMAT_ARGB8888;

  const uint32_t fmt = mon->m_output->state->state().drmFormat;
  if (fmt == 0 || fmt == DRM_FORMAT_INVALID)
    return DRM_FORMAT_ARGB8888;

  return fmt;
}

bool COverview::restoreTileFromCache(int idx) {
  if (idx < 0 || idx >= (int)images.size())
    return false;
  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR)
    return false;
  const auto PWORKSPACE = images[idx].pWorkspace;
  if (!PWORKSPACE)
    return false;

  SP<CTexture> cachedTex;
  std::chrono::steady_clock::time_point capturedAt{};
  if (!restoreWorkspaceTileFromCache(PMONITOR->m_id, PWORKSPACE->m_id, cachedTex,
                                     capturedAt))
    return false;

  images[idx].cachedTex = cachedTex;
  images[idx].lastCaptureAt = capturedAt;
  images[idx].captured = false;
  return true;
}

void COverview::saveTilesToCache() {
  if (!tileCacheEnabled())
    return;
  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR)
    return;

  for (auto& img : images) {
    if (!img.pWorkspace)
      continue;

    auto tex = img.cachedTex;
    if (!isRenderableTexture(tex))
      continue;

    storeWorkspaceTileInCache(PMONITOR->m_id, img.pWorkspace->m_id, tex,
                              img.lastCaptureAt);
  }
}


bool COverview::captureWorkspace(int idx) {
  if (idx < 0 || idx >= (int)images.size())
    return false;
  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR) {
    Log::logger->log(Log::ERR, "[horza] captureWorkspace: no monitor");
    return false;
  }
  if (PMONITOR->m_pixelSize.x <= 0 || PMONITOR->m_pixelSize.y <= 0) {
    Log::logger->log(Log::ERR,
                     "[horza] captureWorkspace: invalid monitor pixel size {}x{}",
                     PMONITOR->m_pixelSize.x, PMONITOR->m_pixelSize.y);
    return false;
  }

  blockDamageReporting = true;

  auto &img = images[idx];
  if (!img.pWorkspace) {
    Log::logger->log(Log::ERR, "[horza] captureWorkspace: null workspace at idx={}",
                     idx);
    blockDamageReporting = false;
    return false;
  }

  img.cachedTex.reset();

  const float captureScale = clampCaptureScale(g_horzaConfig.captureScale);
  const int captureW =
      std::max(1, (int)std::round(PMONITOR->m_pixelSize.x * captureScale));
  const int captureH =
      std::max(1, (int)std::round(PMONITOR->m_pixelSize.y * captureScale));
  CBox monbox = {0.0, 0.0, (double)captureW, (double)captureH};

  g_pHyprRenderer->makeEGLCurrent();

  if (img.fb.m_size != monbox.size()) {
    img.fb.release();
    const uint32_t renderFormat = pickSafeRenderFormat(PMONITOR);
    if (!img.fb.alloc(monbox.w, monbox.h, renderFormat)) {
      Log::logger->log(Log::ERR,
                       "[horza] captureWorkspace: fb.alloc failed idx={} size={}x{} fmt={}",
                       idx, monbox.w, monbox.h, renderFormat);
      blockDamageReporting = false;
      return false;
    }
  }

  CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
  g_pHyprRenderer->beginRender(PMONITOR, fakeDamage, RENDER_MODE_FULL_FAKE,
                               nullptr, &img.fb);

  g_pHyprOpenGL->clear(CHyprColor{0, 0, 0, 1.0});

  const bool oldBlockSurfaceFeedback = g_pHyprRenderer->m_bBlockSurfaceFeedback;
  g_pHyprRenderer->m_bBlockSurfaceFeedback = true;

  const auto oldActiveWorkspace = PMONITOR->m_activeWorkspace;
  const auto oldActiveSpecialWorkspace = PMONITOR->m_activeSpecialWorkspace;
  const bool targetIsOldActive = oldActiveWorkspace == img.pWorkspace;

  const bool oldVisible = img.pWorkspace->m_visible;
  const bool oldForceRendering = img.pWorkspace->m_forceRendering;

  if (PMONITOR->m_activeSpecialWorkspace)
    PMONITOR->m_activeSpecialWorkspace.reset();
  if (oldActiveWorkspace && !targetIsOldActive)
    oldActiveWorkspace->m_visible = false;

  PMONITOR->m_activeWorkspace = img.pWorkspace;
  if (!targetIsOldActive) {
    g_pDesktopAnimationManager->startAnimation(
        img.pWorkspace, CDesktopAnimationManager::ANIMATION_TYPE_IN, true, true);
  }
  img.pWorkspace->m_visible = true;
  img.pWorkspace->m_forceRendering = true;

  g_pHyprRenderer->renderWorkspace(PMONITOR, img.pWorkspace, Time::steadyNow(),
                                   monbox);

  img.pWorkspace->m_forceRendering = oldForceRendering;
  img.pWorkspace->m_visible = oldVisible;
  if (!targetIsOldActive) {
    g_pDesktopAnimationManager->startAnimation(
        img.pWorkspace, CDesktopAnimationManager::ANIMATION_TYPE_OUT, false, true);
  }

  PMONITOR->m_activeWorkspace = oldActiveWorkspace;
  PMONITOR->m_activeSpecialWorkspace = oldActiveSpecialWorkspace;
  if (oldActiveWorkspace && !targetIsOldActive)
    oldActiveWorkspace->m_visible = true;

  g_pHyprRenderer->m_bBlockSurfaceFeedback = oldBlockSurfaceFeedback;

  g_pHyprOpenGL->m_renderData.blockScreenShader = true;
  g_pHyprRenderer->endRender();

  img.lastCaptureAt = std::chrono::steady_clock::now();
  blockDamageReporting = false;
  const auto tex = img.fb.getTexture();
  const bool ok = isRenderableTexture(tex);
  if (!ok) {
    Log::logger->log(Log::ERR,
                     "[horza] captureWorkspace: invalid texture idx={} ws={} fb={}x{} tex={}",
                     idx, img.pWorkspace->m_id, img.fb.m_size.x, img.fb.m_size.y,
                     tex ? (std::to_string((int)tex->m_size.x) + "x" +
                            std::to_string((int)tex->m_size.y))
                         : "null");
  }
  return ok;
}

void COverview::captureBackground() {
  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR)
    return;
  if (PMONITOR->m_pixelSize.x <= 0 || PMONITOR->m_pixelSize.y <= 0) {
    backgroundCaptured = false;
    return;
  }

  blockDamageReporting = true;

  CBox monbox = {0.0, 0.0, PMONITOR->m_pixelSize.x, PMONITOR->m_pixelSize.y};

  g_pHyprRenderer->makeEGLCurrent();

  if (backgroundFb.m_size != monbox.size()) {
    backgroundFb.release();
    if (!backgroundFb.alloc(monbox.w, monbox.h, pickSafeRenderFormat(PMONITOR))) {
      blockDamageReporting = false;
      backgroundCaptured = false;
      return;
    }
  }

  CFramebuffer rawBackgroundFb;
  if (!rawBackgroundFb.alloc(monbox.w, monbox.h, pickSafeRenderFormat(PMONITOR))) {
    blockDamageReporting = false;
    backgroundCaptured = false;
    return;
  }

  CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
  g_pHyprRenderer->beginRender(PMONITOR, fakeDamage, RENDER_MODE_FULL_FAKE,
                               nullptr, &rawBackgroundFb);

  g_pHyprOpenGL->clear(CHyprColor{0, 0, 0, 1.0});

  const auto NOW = Time::steadyNow();
  g_pHyprRenderer->renderBackground(PMONITOR);

  constexpr size_t LAYER_BACKGROUND = 0;
  constexpr size_t LAYER_BOTTOM = 1;
  for (const auto &ls : PMONITOR->m_layerSurfaceLayers[LAYER_BACKGROUND]) {
    const auto L = ls.lock();
    if (!L)
      continue;
    g_pHyprRenderer->renderLayer(L, PMONITOR, NOW);
  }
  for (const auto &ls : PMONITOR->m_layerSurfaceLayers[LAYER_BOTTOM]) {
    const auto L = ls.lock();
    if (!L)
      continue;
    g_pHyprRenderer->renderLayer(L, PMONITOR, NOW);
  }

  g_pHyprOpenGL->m_renderData.blockScreenShader = true;
  g_pHyprRenderer->endRender();

  g_pHyprRenderer->beginRender(PMONITOR, fakeDamage, RENDER_MODE_FULL_FAKE,
                               nullptr, &backgroundFb);
  g_pHyprOpenGL->clear(CHyprColor{0, 0, 0, 1.0});

  CBox bgbox = {0, 0, PMONITOR->m_size.x, PMONITOR->m_size.y};
  bgbox.scale(PMONITOR->m_scale);
  bgbox.round();

  CHyprOpenGLImpl::STextureRenderData sampleData;
  sampleData.damage = &fakeDamage;
  sampleData.a = 1.0f;

  const float blurRadiusPx =
      std::max(0.0f, g_horzaConfig.backgroundBlurRadius) * PMONITOR->m_scale;
  const int blurPasses = std::max(0, g_horzaConfig.backgroundBlurPasses);
  const float blurStrength = std::max(0.0f, g_horzaConfig.backgroundBlurStrength);
  const float blurSpread = std::max(0.0f, g_horzaConfig.backgroundBlurSpread);

  const auto rawBgTex = rawBackgroundFb.getTexture();
  if (!isRenderableTexture(rawBgTex)) {
    blockDamageReporting = false;
    backgroundCaptured = false;
    return;
  }

  if (blurRadiusPx <= 0.0f || blurPasses <= 0 || blurStrength <= 0.0f) {
    g_pHyprOpenGL->renderTextureInternal(rawBgTex, bgbox, sampleData);
  } else {
    
    
    static const std::array<Vector2D, 17> SAMPLE_DIRS = {
        Vector2D{0.0, 0.0},
        Vector2D{0.55, 0.0},   Vector2D{-0.55, 0.0},  Vector2D{0.0, 0.55},
        Vector2D{0.0, -0.55},  Vector2D{0.39, 0.39},  Vector2D{-0.39, 0.39},
        Vector2D{0.39, -0.39}, Vector2D{-0.39, -0.39},
        Vector2D{1.0, 0.0},    Vector2D{-1.0, 0.0},   Vector2D{0.0, 1.0},
        Vector2D{0.0, -1.0},   Vector2D{0.7071, 0.7071},
        Vector2D{-0.7071, 0.7071}, Vector2D{0.7071, -0.7071},
        Vector2D{-0.7071, -0.7071},
    };
    static const std::array<float, 17> SAMPLE_WEIGHTS = {
        0.20f, 0.07f, 0.07f, 0.07f, 0.07f, 0.05f, 0.05f, 0.05f, 0.05f,
        0.055f, 0.055f, 0.055f, 0.055f, 0.025f, 0.025f, 0.025f, 0.025f,
    };

    for (int pass = 0; pass < blurPasses; ++pass) {
      const float passRadius =
          blurRadiusPx * (1.0f + pass * std::max(0.25f, blurSpread));
      for (size_t i = 0; i < SAMPLE_DIRS.size(); ++i) {
        CBox sampleBox = bgbox;
        sampleBox.x += SAMPLE_DIRS[i].x * passRadius;
        sampleBox.y += SAMPLE_DIRS[i].y * passRadius;
        sampleData.a = std::clamp(SAMPLE_WEIGHTS[i] * blurStrength, 0.0f, 1.0f);
        g_pHyprOpenGL->renderTextureInternal(rawBgTex, sampleBox, sampleData);
      }
    }
  }

  const float tint = std::clamp(g_horzaConfig.backgroundTint, 0.0f, 1.0f);
  if (tint > 0.0f) {
    CHyprOpenGLImpl::SRectRenderData rectData;
    rectData.damage = &fakeDamage;
    g_pHyprOpenGL->renderRect(bgbox, CHyprColor{0.0, 0.0, 0.0, tint}, rectData);
  }

  g_pHyprOpenGL->m_renderData.blockScreenShader = true;
  g_pHyprRenderer->endRender();

  backgroundCaptured = true;
  blockDamageReporting = false;
}
