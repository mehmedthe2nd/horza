#include "overview.hpp"
#include "OverviewPassElement.hpp"
#include <algorithm>
#include <any>
#include <array>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <linux/input-event-codes.h>
#include <limits>
#include <memory>
#include <unordered_map>
#include <drm_fourcc.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define private public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/animation/AnimationManager.hpp>
#include <hyprland/src/managers/animation/DesktopAnimationManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#undef private

static void
damageCallback(WP<Hyprutils::Animation::CBaseAnimatedVariable> var) {
  if (g_pOverview)
    g_pOverview->damage();
}

static SP<Hyprutils::Animation::SAnimationPropertyConfig> makeAnimConfig() {
  return g_pConfigManager->getAnimationPropertyConfig("windowsMove");
}

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
    const bool deadTex = !it->second.tex || it->second.tex->m_size.x <= 0 ||
                         it->second.tex->m_size.y <= 0;
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
  if (!tex || tex->m_size.x <= 0 || tex->m_size.y <= 0)
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
  if (!it->second.tex || it->second.tex->m_size.x <= 0 ||
      it->second.tex->m_size.y <= 0) {
    g_workspaceTileCache.erase(it);
    return false;
  }

  it->second.cachedAt = std::chrono::steady_clock::now();
  outTex = it->second.tex;
  outCapturedAt = it->second.capturedAt;
  return true;
}

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

template <typename T>
static bool extractEventPayloadFromAny(const std::any& param, T& out) {
  if (const auto* direct = std::any_cast<T>(&param)) {
    out = *direct;
    return true;
  }
  if (const auto* directPtr = std::any_cast<T*>(&param)) {
    if (*directPtr) {
      out = **directPtr;
      return true;
    }
  }
  if (const auto* directConstPtr = std::any_cast<const T*>(&param)) {
    if (*directConstPtr) {
      out = **directConstPtr;
      return true;
    }
  }
  if (const auto* mapPtr =
          std::any_cast<std::unordered_map<std::string, std::any>>(&param)) {
    const auto it = mapPtr->find("event");
    if (it != mapPtr->end()) {
      if (const auto* nested = std::any_cast<T>(&it->second)) {
        out = *nested;
        return true;
      }
      if (const auto* nestedPtr = std::any_cast<T*>(&it->second)) {
        if (*nestedPtr) {
          out = **nestedPtr;
          return true;
        }
      }
      if (const auto* nestedConstPtr = std::any_cast<const T*>(&it->second)) {
        if (*nestedConstPtr) {
          out = **nestedConstPtr;
          return true;
        }
      }
    }
  }
  if (const auto* mapDynPtr =
          std::any_cast<std::unordered_map<std::string, std::any>*>(&param)) {
    if (*mapDynPtr) {
      const auto it = (*mapDynPtr)->find("event");
      if (it != (*mapDynPtr)->end()) {
        if (const auto* nested = std::any_cast<T>(&it->second)) {
          out = *nested;
          return true;
        }
        if (const auto* nestedPtr = std::any_cast<T*>(&it->second)) {
          if (*nestedPtr) {
            out = **nestedPtr;
            return true;
          }
        }
      }
    }
  }
  return false;
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

COverview::COverview(PHLWORKSPACE startedOn_, bool transitMode_,
                     PHLWORKSPACE transitDest_)
    : transitMode(transitMode_) {

  const auto PMONITOR = Desktop::focusState()->monitor();
  if (!PMONITOR || !startedOn_) {
    std::cerr << "[horza] cannot open overview: missing focused monitor/workspace\n";
    return;
  }

  pMonitor = PMONITOR;
  directScanoutWasBlocked = g_pHyprRenderer->m_directScanoutBlocked;
  g_pHyprRenderer->m_directScanoutBlocked = true;
  lastActiveWorkspaceID = PMONITOR->m_activeWorkspace
                              ? PMONITOR->m_activeWorkspace->m_id
                              : -1;

  std::vector<PHLWORKSPACE> wsList;
  for (auto &wsWeak : g_pCompositor->m_workspaces) {
    auto ws = wsWeak.lock();
    if (!ws)
      continue;
    if (ws->monitorID() == PMONITOR->m_id && ws->m_id >= 0)
      wsList.push_back(ws);
  }
  std::sort(wsList.begin(), wsList.end(),
            [](const PHLWORKSPACE &a, const PHLWORKSPACE &b) {
              return a->m_id < b->m_id;
            });

  for (size_t i = 0; i < wsList.size(); i++) {
    images.push_back({.pWorkspace = wsList[i], .captured = false});
    if (wsList[i] == startedOn_)
      currentIdx = (int)i;
  }

  if (images.empty()) {
    std::cerr << "[horza] cannot open overview: no normal workspaces on monitor\n";
    return;
  }

  workspaceListDirty = false;
  nextWorkspaceSyncPollAt = std::chrono::steady_clock::now();

  if (g_horzaConfig.freezeAnimationsInOverview)
    suppressGlobalAnimations();

  g_pAnimationManager->createAnimation(1.0f, m_scale, makeAnimConfig(),
                                       AVARDAMAGE_NONE);
  m_scale->setUpdateCallback(damageCallback);

  g_pAnimationManager->createAnimation(0.0f, m_offsetX, makeAnimConfig(),
                                       AVARDAMAGE_NONE);
  m_offsetX->setUpdateCallback(damageCallback);

  g_pAnimationManager->createAnimation(0.0f, m_crossOffset, makeAnimConfig(),
                                       AVARDAMAGE_NONE);
  m_crossOffset->setUpdateCallback(damageCallback);
  m_crossOffset->setValueAndWarp(0.0f);

  if (transitMode) {
    
    
    
    m_scale->setValueAndWarp(1.0f);
    m_offsetX->setValueAndWarp(0.0f);
    m_crossOffset->setValueAndWarp(0.0f);
    openAnimPending = false;

    blockOverviewRendering = true;
    captureWorkspace(currentIdx);
    images[currentIdx].captured = true;

    if (transitDest_ && transitDest_ != startedOn_) {
      for (int i = 0; i < (int)images.size(); ++i) {
        if (images[i].pWorkspace != transitDest_)
          continue;
        if (!images[i].captured)
          captureWorkspace(i);
        images[i].captured = true;
        break;
      }
    }

    blockOverviewRendering = false;
    pendingCapture = false;

    preRenderHook = g_pHookSystem->hookDynamic(
        "preRender", [this](void *self, SCallbackInfo &info, std::any param) {
          onPreRender();
        });

    ready = true;
    return;
  }

  for (int i = 0; i < (int)images.size(); ++i)
    restoreTileFromCache(i);

  blockOverviewRendering = true;
  if (!images[currentIdx].cachedTex) {
    captureWorkspace(currentIdx);
    images[currentIdx].captured = true;
  } else if (images[currentIdx].cachedTex && !images[currentIdx].captured) {
    
    
    damageDirty = true;
  }
  if (g_horzaConfig.hyprpaperBackground)
    captureBackground();
  if (g_horzaConfig.prewarmAll) {
    for (int i = 0; i < (int)images.size(); ++i) {
      if (images[i].captured)
        continue;
      captureWorkspace(i);
      images[i].captured = true;
      images[i].cachedTex.reset();
    }
  }
  blockOverviewRendering = false;
  pendingCapture = !g_horzaConfig.prewarmAll;
  m_scale->setValueAndWarp(1.0f);
  
  
  openAnimPending = true;

  mouseMoveHook = g_pHookSystem->hookDynamic(
      "mouseMove", [this](void *self, SCallbackInfo &info, std::any param) {
        onMouseMove();
      });

  mouseButtonHook = g_pHookSystem->hookDynamic(
      "mouseButton", [this](void *self, SCallbackInfo &info, std::any param) {
        if (closing)
          return;
        info.cancelled = true;
        onMouseButton(param, info);
      });

  mouseAxisHook = g_pHookSystem->hookDynamic(
      "mouseAxis", [this](void *self, SCallbackInfo &info, std::any param) {
        onMouseAxis(param, info);
      });

  keyPressHook = g_pHookSystem->hookDynamic(
      "keyPress", [this](void *self, SCallbackInfo &info, std::any param) {
        onKeyPress(param, info);
      });

  auto markWorkspaceListDirty = [this](void *self, SCallbackInfo &info,
                                       std::any param) {
    requestWorkspaceSync();
  };
  createWorkspaceHook =
      g_pHookSystem->hookDynamic("createWorkspace", markWorkspaceListDirty);
  destroyWorkspaceHook =
      g_pHookSystem->hookDynamic("destroyWorkspace", markWorkspaceListDirty);
  moveWorkspaceHook =
      g_pHookSystem->hookDynamic("moveWorkspace", markWorkspaceListDirty);
  monitorAddedHook =
      g_pHookSystem->hookDynamic("monitorAdded", markWorkspaceListDirty);
  monitorRemovedHook =
      g_pHookSystem->hookDynamic("monitorRemoved", markWorkspaceListDirty);
  configReloadedHook =
      g_pHookSystem->hookDynamic("configReloaded", markWorkspaceListDirty);

  preRenderHook = g_pHookSystem->hookDynamic(
      "preRender", [this](void *self, SCallbackInfo &info, std::any param) {
        onPreRender();
      });

  ready = true;
}

COverview::~COverview() {
  if (closeDropTimer)
    closeDropTimer->cancel();
  closeDropTimer.reset();
  saveTilesToCache();
  preRenderHook.reset();
  mouseButtonHook.reset();
  mouseMoveHook.reset();
  mouseAxisHook.reset();
  keyPressHook.reset();
  createWorkspaceHook.reset();
  destroyWorkspaceHook.reset();
  moveWorkspaceHook.reset();
  monitorAddedHook.reset();
  monitorRemovedHook.reset();
  configReloadedHook.reset();
  g_pHyprRenderer->m_directScanoutBlocked = directScanoutWasBlocked;
  g_pHyprRenderer->makeEGLCurrent();
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

    auto tex = img.captured ? img.fb.getTexture() : img.cachedTex;
    if (!tex)
      continue;

    storeWorkspaceTileInCache(PMONITOR->m_id, img.pWorkspace->m_id, tex,
                              img.lastCaptureAt);
  }
}

int COverview::hitTileIndex(const Vector2D& localPos) const {
  for (int i = 0; i < (int)images.size(); ++i) {
    const auto& b = images[i].displayBox;
    if (localPos.x >= b.x && localPos.x <= b.x + b.w && localPos.y >= b.y &&
        localPos.y <= b.y + b.h)
      return i;
  }
  return -1;
}

Vector2D COverview::tileLocalToWorkspacePos(const CBox& tileBox,
                                            const Vector2D& localPos) const {
  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR)
    return {};

  const double monitorW = std::max(1.0, PMONITOR->m_size.x);
  const double monitorH = std::max(1.0, PMONITOR->m_size.y);
  const double scaleX = tileBox.w / monitorW;
  const double scaleY = tileBox.h / monitorH;
  if (scaleX <= 0.0 || scaleY <= 0.0)
    return {};

  return {(localPos.x - tileBox.x) / scaleX, (localPos.y - tileBox.y) / scaleY};
}

PHLWINDOW COverview::pickWindowInWorkspace(const PHLWORKSPACE& ws,
                                           const Vector2D& workspacePos) const {
  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR || !ws)
    return nullptr;

  const Vector2D globalPos = workspacePos + PMONITOR->m_position;
  for (auto it = g_pCompositor->m_windows.rbegin();
       it != g_pCompositor->m_windows.rend(); ++it) {
    const auto& win = *it;
    if (!win || !win->m_isMapped || win->m_workspace != ws)
      continue;

    const CBox winBox = win->getWindowBoxUnified(
        Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS |
        Desktop::View::ALLOW_FLOATING);
    if (globalPos.x >= winBox.x && globalPos.x <= winBox.x + winBox.w &&
        globalPos.y >= winBox.y && globalPos.y <= winBox.y + winBox.h)
      return win;
  }

  return nullptr;
}

void COverview::clearDragState() {
  leftButtonDown = false;
  draggingWindow = false;
  dragSourceIdx = -1;
  dragTargetIdx = -1;
  dragWindow.reset();
  dragStartPosLocal = {};
  dragLastPosLocal = {};
  dragWindowPosWorkspace = {};
  dragWindowSizeWorkspace = {};
  dragWindowGrabOffsetWorkspace = {};
  dragNextHoverJumpAt = {};
}

bool COverview::shiftCurrentIndexBy(int step) {
  if (step == 0 || images.size() < 2)
    return false;

  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR || !m_scale || !m_offsetX)
    return false;

  const int targetIdx =
      std::clamp(currentIdx + step, 0, (int)images.size() - 1);
  if (targetIdx == currentIdx)
    return false;

  const float sMin = std::min(1.0f, g_horzaConfig.displayScale);
  const float sMax = std::max(1.0f, g_horzaConfig.displayScale);
  float s = std::clamp(m_scale->value(), sMin, sMax);
  float ds = std::max(g_horzaConfig.displayScale, 0.0001f);
  float tileW = PMONITOR->m_size.x * s;
  float tileH = PMONITOR->m_size.y * s;
  float baseGap = g_horzaConfig.overviewGap;
  float gap = baseGap * (s / ds);
  bool vertical = g_horzaConfig.vertical;

  const float oldCenter = !vertical
                              ? PMONITOR->m_size.x * 0.5f -
                                    (currentIdx * (tileW + gap) + tileW * 0.5f)
                              : PMONITOR->m_size.y * 0.5f -
                                    (currentIdx * (tileH + gap) + tileH * 0.5f);
  const float newCenter = !vertical
                              ? PMONITOR->m_size.x * 0.5f -
                                    (targetIdx * (tileW + gap) + tileW * 0.5f)
                              : PMONITOR->m_size.y * 0.5f -
                                    (targetIdx * (tileH + gap) + tileH * 0.5f);

  currentIdx = targetIdx;

  m_offsetX->setValueAndWarp(m_offsetX->value() + (oldCenter - newCenter));
  *m_offsetX = 0.0f;

  if (!images[currentIdx].captured && !images[currentIdx].cachedTex) {
    blockOverviewRendering = true;
    captureWorkspace(currentIdx);
    blockOverviewRendering = false;
    images[currentIdx].captured = true;
    images[currentIdx].cachedTex.reset();
  } else if (!images[currentIdx].captured && images[currentIdx].cachedTex) {
    damageDirty = true;
  }

  damage();
  g_pCompositor->scheduleFrameForMonitor(PMONITOR);
  return true;
}

void COverview::onMouseMove() {
  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR)
    return;
  lastMousePosLocal =
      g_pInputManager->getMouseCoordsInternal() - PMONITOR->m_position;

  if (closing || !leftButtonDown)
    return;
  if (!dragWindow || dragSourceIdx < 0 || dragSourceIdx >= (int)images.size())
    return;

  if (!draggingWindow) {
    constexpr double kDragThresholdPx = 10.0;
    const Vector2D delta = lastMousePosLocal - dragStartPosLocal;
    if (std::hypot(delta.x, delta.y) >= kDragThresholdPx)
      draggingWindow = true;
  }

  if (!draggingWindow)
    return;

  int newTargetIdx = hitTileIndex(lastMousePosLocal);
  if (newTargetIdx >= 0 && newTargetIdx != currentIdx) {
    const auto now = std::chrono::steady_clock::now();
    const bool canJump = dragNextHoverJumpAt.time_since_epoch().count() == 0 ||
                         now >= dragNextHoverJumpAt;
    if (canJump && shiftCurrentIndexBy(newTargetIdx - currentIdx)) {
      const float delayMs = std::max(0.0f, g_horzaConfig.dragHoverJumpDelayMs);
      const auto cooldown = std::chrono::milliseconds((int)std::lround(delayMs));
      dragNextHoverJumpAt = now + cooldown;
    }
  }

  newTargetIdx = hitTileIndex(lastMousePosLocal);
  if (newTargetIdx != dragTargetIdx) {
    dragTargetIdx = newTargetIdx;
    damage();
    g_pCompositor->scheduleFrameForMonitor(PMONITOR);
  }
}

void COverview::onMouseButton(std::any param, SCallbackInfo& info) {
  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR)
    return;
  lastMousePosLocal =
      g_pInputManager->getMouseCoordsInternal() - PMONITOR->m_position;

  IPointer::SButtonEvent e;
  if (!extractEventPayloadFromAny(param, e))
    return;

  if (e.button != BTN_LEFT)
    return;

  if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
    clearDragState();
    leftButtonDown = true;
    dragStartPosLocal = lastMousePosLocal;
    dragLastPosLocal = lastMousePosLocal;
    dragSourceIdx = hitTileIndex(lastMousePosLocal);
    dragTargetIdx = dragSourceIdx;

    if (dragSourceIdx >= 0 && dragSourceIdx < (int)images.size()) {
      const auto& hitBox = images[dragSourceIdx].displayBox;
      const auto PHITWS = images[dragSourceIdx].pWorkspace;
      const Vector2D workspacePos =
          tileLocalToWorkspacePos(hitBox, lastMousePosLocal);
      dragWindow = pickWindowInWorkspace(PHITWS, workspacePos);
      if (!dragWindow && PHITWS) {
        dragWindow = PHITWS->getLastFocusedWindow();
        if (!dragWindow)
          dragWindow = PHITWS->getFirstWindow();
        if (dragWindow && dragWindow->m_workspace != PHITWS)
          dragWindow.reset();
      }

      if (dragWindow) {
        const CBox winBox = dragWindow->getWindowBoxUnified(
            Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS |
            Desktop::View::ALLOW_FLOATING);
        dragWindowSizeWorkspace = {
            std::max(1.0, winBox.w),
            std::max(1.0, winBox.h),
        };
        const Vector2D winTopLeftLocal = {
            winBox.x - PMONITOR->m_position.x,
            winBox.y - PMONITOR->m_position.y,
        };
        dragWindowPosWorkspace = winTopLeftLocal;
        const Vector2D grabOffset = workspacePos - winTopLeftLocal;
        dragWindowGrabOffsetWorkspace = {
            std::clamp(grabOffset.x, 0.0, dragWindowSizeWorkspace.x),
            std::clamp(grabOffset.y, 0.0, dragWindowSizeWorkspace.y),
        };
      }
    }

    return;
  }

  if (e.state != WL_POINTER_BUTTON_STATE_RELEASED)
    return;

  const bool wasDragging = draggingWindow;
  const int releaseIdx = hitTileIndex(lastMousePosLocal);
  const int sourceIdx = dragSourceIdx;
  const int targetIdx = releaseIdx == -1 ? dragTargetIdx : releaseIdx;
  auto draggedWindow = dragWindow;

  clearDragState();

  if (wasDragging) {
    if (!draggedWindow || targetIdx < 0 || targetIdx >= (int)images.size())
      return;

    const auto PDSTWS = images[targetIdx].pWorkspace;
    if (!PDSTWS || draggedWindow->m_workspace == PDSTWS)
      return;

    g_pCompositor->moveWindowToWorkspaceSafe(draggedWindow, PDSTWS);

    if (sourceIdx >= 0 && sourceIdx < (int)images.size()) {
      images[sourceIdx].captured = false;
      images[sourceIdx].cachedTex.reset();
      images[sourceIdx].titleTex.reset();
      images[sourceIdx].titleTextCached.clear();
      images[sourceIdx].titleMaxWidthCached = 0;
      images[sourceIdx].titleFontCached = 0;
      images[sourceIdx].titleFontFamilyCached.clear();
    }
    if (targetIdx >= 0 && targetIdx < (int)images.size()) {
      images[targetIdx].captured = false;
      images[targetIdx].cachedTex.reset();
      images[targetIdx].titleTex.reset();
      images[targetIdx].titleTextCached.clear();
      images[targetIdx].titleMaxWidthCached = 0;
      images[targetIdx].titleFontCached = 0;
      images[targetIdx].titleFontFamilyCached.clear();
    }

    workspaceListDirty = true;
    damageDirty = true;
    pendingCapture = true;
    damage();
    g_pCompositor->scheduleFrameForMonitor(PMONITOR);
    return;
  }

  const int hitIdx = releaseIdx;
  if (hitIdx == -1) {
    close();
    return;
  }

  const auto& hitBox = images[hitIdx].displayBox;
  const auto PHITWS = images[hitIdx].pWorkspace;
  if (!PHITWS) {
    close();
    return;
  }

  const Vector2D workspacePos = tileLocalToWorkspacePos(hitBox, lastMousePosLocal);

  
  
  if (PMONITOR->m_activeWorkspace != PHITWS)
    g_pKeybindManager->m_dispatchers.at("workspace")(std::to_string(PHITWS->m_id));

  const auto PWINDOW = pickWindowInWorkspace(PHITWS, workspacePos);

  if (PWINDOW)
    Desktop::focusState()->fullWindowFocus(PWINDOW);

  close();
}

void COverview::onMouseAxis(std::any param, SCallbackInfo& info) {
  if (closing || images.size() < 2)
    return;

  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR)
    return;
  if (g_pCompositor->getMonitorFromCursor() != PMONITOR)
    return;

  std::unordered_map<std::string, std::any> data;
  try {
    data = std::any_cast<std::unordered_map<std::string, std::any>>(param);
  } catch (...) {
    return;
  }

  const auto evIt = data.find("event");
  if (evIt == data.end())
    return;

  IPointer::SAxisEvent e;
  try {
    e = std::any_cast<IPointer::SAxisEvent>(evIt->second);
  } catch (...) {
    return;
  }

  if (e.axis != WL_POINTER_AXIS_VERTICAL_SCROLL &&
      e.axis != WL_POINTER_AXIS_HORIZONTAL_SCROLL)
    return;

  
  info.cancelled = true;

  int steps = 0;
  const double discrete =
      e.deltaDiscrete != 0.0 ? (double)e.deltaDiscrete : 0.0;
  if (discrete != 0.0) {
    steps = discrete < 0.0 ? -1 : 1;
  } else {
    
    if ((scrollGestureAccum > 0.0 && e.delta < 0.0) ||
        (scrollGestureAccum < 0.0 && e.delta > 0.0))
      scrollGestureAccum = 0.0;
    scrollGestureAccum += e.delta;

    constexpr double kTouchpadStep = 48.0;
    if (std::abs(scrollGestureAccum) < kTouchpadStep)
      return;

    steps = scrollGestureAccum < 0.0 ? -1 : 1;
    scrollGestureAccum = 0.0;
  }

  if (steps == 0)
    return;

  const int targetIdx =
      std::clamp(currentIdx + steps, 0, (int)images.size() - 1);
  if (targetIdx == currentIdx)
    return;

  if (!m_scale || !m_offsetX)
    return;

  const float sMin = std::min(1.0f, g_horzaConfig.displayScale);
  const float sMax = std::max(1.0f, g_horzaConfig.displayScale);
  float s = std::clamp(m_scale->value(), sMin, sMax);
  float ds = std::max(g_horzaConfig.displayScale, 0.0001f);
  float tileW = PMONITOR->m_size.x * s;
  float tileH = PMONITOR->m_size.y * s;
  float baseGap = g_horzaConfig.overviewGap;
  float gap = baseGap * (s / ds);
  bool vertical = g_horzaConfig.vertical;

  const float oldCenter = !vertical
                              ? PMONITOR->m_size.x * 0.5f -
                                    (currentIdx * (tileW + gap) + tileW * 0.5f)
                              : PMONITOR->m_size.y * 0.5f -
                                    (currentIdx * (tileH + gap) + tileH * 0.5f);
  const float newCenter = !vertical
                              ? PMONITOR->m_size.x * 0.5f -
                                    (targetIdx * (tileW + gap) + tileW * 0.5f)
                              : PMONITOR->m_size.y * 0.5f -
                                    (targetIdx * (tileH + gap) + tileH * 0.5f);

  currentIdx = targetIdx;

  m_offsetX->setValueAndWarp(m_offsetX->value() + (oldCenter - newCenter));
  *m_offsetX = 0.0f;

  if (!images[currentIdx].captured && !images[currentIdx].cachedTex) {
    blockOverviewRendering = true;
    captureWorkspace(currentIdx);
    blockOverviewRendering = false;
    images[currentIdx].captured = true;
    images[currentIdx].cachedTex.reset();
  } else if (!images[currentIdx].captured && images[currentIdx].cachedTex) {
    
    damageDirty = true;
  }

  damage();
  g_pCompositor->scheduleFrameForMonitor(PMONITOR);
}

void COverview::onKeyPress(std::any param, SCallbackInfo& info) {
  if (closing)
    return;
  if (!g_horzaConfig.escOnly)
    return;

  std::unordered_map<std::string, std::any> data;
  try {
    data = std::any_cast<std::unordered_map<std::string, std::any>>(param);
  } catch (...) {
    return;
  }

  const auto evIt = data.find("event");
  if (evIt == data.end())
    return;

  IKeyboard::SKeyEvent e;
  try {
    e = std::any_cast<IKeyboard::SKeyEvent>(evIt->second);
  } catch (...) {
    return;
  }

  if (e.keycode != KEY_ESC)
    return;

  
  info.cancelled = true;

  if (e.state == WL_KEYBOARD_KEY_STATE_PRESSED)
    close();
}

void COverview::onWorkspaceChange() {
  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR || images.empty() || !m_scale || !m_offsetX)
    return;

  int newIdx = currentIdx;
  for (int i = 0; i < (int)images.size(); i++) {
    if (images[i].pWorkspace == PMONITOR->m_activeWorkspace) {
      newIdx = i;
      break;
    }
  }

  if (newIdx == currentIdx)
    return;

  const float sMin = std::min(1.0f, g_horzaConfig.displayScale);
  const float sMax = std::max(1.0f, g_horzaConfig.displayScale);
  float s = transitMode ? 1.0f : std::clamp(m_scale->value(), sMin, sMax);
  float ds = transitMode ? 1.0f : std::max(g_horzaConfig.displayScale, 0.0001f);
  float tileW = PMONITOR->m_size.x * s;
  float tileH = PMONITOR->m_size.y * s;
  float baseGap = g_horzaConfig.overviewGap;
  float gap = transitMode ? 0.0f : baseGap * (s / ds);
  bool vertical = transitMode ? false : g_horzaConfig.vertical;

  const float oldCenter = !vertical
                              ? PMONITOR->m_size.x * 0.5f -
                                    (currentIdx * (tileW + gap) + tileW * 0.5f)
                              : PMONITOR->m_size.y * 0.5f -
                                    (currentIdx * (tileH + gap) + tileH * 0.5f);
  const float newCenter = !vertical
                              ? PMONITOR->m_size.x * 0.5f -
                                    (newIdx * (tileW + gap) + tileW * 0.5f)
                              : PMONITOR->m_size.y * 0.5f -
                                    (newIdx * (tileH + gap) + tileH * 0.5f);

  currentIdx = newIdx;

  m_offsetX->setValueAndWarp(m_offsetX->value() + (oldCenter - newCenter));
  *m_offsetX = 0.0f;

  if (transitMode) {
    m_offsetX->setCallbackOnEnd(
        [](WP<Hyprutils::Animation::CBaseAnimatedVariable> var) {
          g_pEventLoopManager->doLater([]() {
            if (g_pOverview && g_pOverview->transitMode)
              g_pOverview->close();
          });
        });
  }

  if (!images[currentIdx].captured) {
    blockOverviewRendering = true;
    captureWorkspace(currentIdx);
    blockOverviewRendering = false;
    images[currentIdx].captured = true;
    images[currentIdx].cachedTex.reset();
  }

  damageDirty = true;
}

void COverview::requestWorkspaceSync() {
  workspaceListDirty = true;
  nextWorkspaceSyncPollAt = std::chrono::steady_clock::now();
  pendingCapture = true;
  damageDirty = true;

  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR)
    return;

  damage();
  g_pCompositor->scheduleFrameForMonitor(PMONITOR);
}

bool COverview::needsWorkspaceSync() {
  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR)
    return false;

  const auto now = std::chrono::steady_clock::now();
  if (nextWorkspaceSyncPollAt.time_since_epoch().count() != 0 &&
      now < nextWorkspaceSyncPollAt)
    return false;

  
  nextWorkspaceSyncPollAt = now + std::chrono::milliseconds(80);

  std::vector<int64_t> workspaceIDs;
  workspaceIDs.reserve(images.size() + 2);
  for (const auto& wsWeak : g_pCompositor->m_workspaces) {
    const auto ws = wsWeak.lock();
    if (!ws)
      continue;
    if (ws->monitorID() != PMONITOR->m_id || ws->m_id < 0)
      continue;
    workspaceIDs.push_back(ws->m_id);
  }

  std::sort(workspaceIDs.begin(), workspaceIDs.end());
  if (workspaceIDs.size() != images.size())
    return true;

  for (size_t i = 0; i < workspaceIDs.size(); ++i) {
    if (!images[i].pWorkspace || images[i].pWorkspace->m_id != workspaceIDs[i])
      return true;
  }

  return false;
}

bool COverview::syncWorkspaces() {
  const auto PMONITOR = pMonitor.lock();
  workspaceListDirty = false;
  if (!PMONITOR)
    return false;

  std::vector<PHLWORKSPACE> wsList;
  for (auto& wsWeak : g_pCompositor->m_workspaces) {
    auto ws = wsWeak.lock();
    if (!ws)
      continue;
    if (ws->monitorID() == PMONITOR->m_id && ws->m_id >= 0)
      wsList.push_back(ws);
  }

  std::sort(wsList.begin(), wsList.end(),
            [](const PHLWORKSPACE& a, const PHLWORKSPACE& b) {
              return a->m_id < b->m_id;
            });

  bool unchanged = wsList.size() == images.size();
  if (unchanged) {
    for (size_t i = 0; i < wsList.size(); ++i) {
      if (images[i].pWorkspace != wsList[i]) {
        unchanged = false;
        break;
      }
    }
  }

  if (unchanged)
    return false;

  PHLWORKSPACE previousCenter = nullptr;
  if (currentIdx >= 0 && currentIdx < (int)images.size())
    previousCenter = images[currentIdx].pWorkspace;

  auto oldImages = std::move(images);
  const int oldImageCount = (int)oldImages.size();
  images.clear();
  images.reserve(wsList.size());

  for (const auto& ws : wsList) {
    int oldIdx = -1;
    for (int i = 0; i < (int)oldImages.size(); ++i) {
      if (!oldImages[i].pWorkspace)
        continue;
      if (oldImages[i].pWorkspace->m_id == ws->m_id) {
        oldIdx = i;
        break;
      }
    }

    if (oldIdx != -1) {
      auto img = std::move(oldImages[oldIdx]);
      if (img.pWorkspace != ws) {
        img.captured = false;
        img.cachedTex.reset();
        img.titleTex.reset();
        img.titleTextCached.clear();
        img.titleMaxWidthCached = 0;
        img.titleFontCached = 0;
        img.titleFontFamilyCached.clear();
      }
      img.pWorkspace = ws;
      images.push_back(std::move(img));
    } else {
      images.push_back({.pWorkspace = ws, .captured = false});
    }
  }

  if (images.empty())
    return true;

  int newIdx = -1;
  if (previousCenter) {
    for (int i = 0; i < (int)images.size(); ++i) {
      if (images[i].pWorkspace &&
          images[i].pWorkspace->m_id == previousCenter->m_id) {
        newIdx = i;
        break;
      }
    }
  }

  if (newIdx == -1 && PMONITOR->m_activeWorkspace) {
    for (int i = 0; i < (int)images.size(); ++i) {
      if (images[i].pWorkspace &&
          images[i].pWorkspace->m_id == PMONITOR->m_activeWorkspace->m_id) {
        newIdx = i;
        break;
      }
    }
  }

  if (newIdx == -1)
    newIdx = std::clamp(currentIdx, 0, (int)images.size() - 1);
  currentIdx = newIdx;

  for (int i = 0; i < (int)images.size(); ++i) {
    if (images[i].captured || images[i].cachedTex)
      continue;
    restoreTileFromCache(i);
  }

  if (g_horzaConfig.prewarmAll) {
    blockOverviewRendering = true;
    for (int i = 0; i < (int)images.size(); ++i) {
      if (images[i].captured)
        continue;
      captureWorkspace(i);
      images[i].captured = true;
      images[i].cachedTex.reset();
    }
    blockOverviewRendering = false;
    pendingCapture = false;
  } else {
    pendingCapture = true;
  }

  if (currentIdx >= 0 && currentIdx < (int)images.size() &&
      images[currentIdx].cachedTex && !images[currentIdx].captured)
    damageDirty = true;

  if ((int)images.size() != oldImageCount || dragSourceIdx >= (int)images.size() ||
      dragTargetIdx >= (int)images.size())
    clearDragState();

  return true;
}

bool COverview::openingAnimInProgress() const {
  if (closing || !m_scale)
    return false;

  if (openAnimPending)
    return true;

  const float target = clampDisplayScale(g_horzaConfig.displayScale);
  const float delta = std::fabs(m_scale->value() - target);
  return delta > 0.02f;
}

bool COverview::closingHandoffActive() const {
  return closing && handoffActive && g_horzaConfig.asyncCloseHandoff;
}

bool COverview::closeUnderlayActive() const {
  return closingHandoffActive() || (closing && finalCrossfadeActive);
}

bool COverview::closeDropPending() const { return closeDropScheduled; }

float COverview::computeCloseOverlayAlpha() const {
  if (!closingHandoffActive() || !m_scale)
    return 1.0f;

  const float start = std::clamp(g_horzaConfig.asyncCloseFadeStart, 0.0f, 0.999f);
  const float minAlpha = std::clamp(g_horzaConfig.asyncCloseMinAlpha, 0.0f, 1.0f);

  const float currentScale = std::clamp(m_scale->value(), start, 1.0f);
  float t = (currentScale - start) / std::max(0.001f, 1.0f - start);
  t = std::clamp(t, 0.0f, 1.0f);

  const std::string curve = normalizeHorzaToken(g_horzaConfig.asyncCloseFadeCurve);
  if (curve == "ease_out")
    t = 1.0f - (1.0f - t) * (1.0f - t);

  return std::clamp(1.0f - (1.0f - minAlpha) * t, minAlpha, 1.0f);
}

void COverview::scheduleCloseDrop() {
  if (closeDropScheduled)
    return;
  closeDropScheduled = true;
  const auto MON = pMonitor;

  
  
  if (!closeDropTimer) {
    closeDropTimer = makeShared<CEventLoopTimer>(
        std::optional<Time::steady_dur>{std::chrono::milliseconds(1)},
        [MON](SP<CEventLoopTimer> self, void* data) {
          self->cancel();

          if (g_pOverview && g_pOverview->closeDropPending())
            g_pOverview.reset();

          const auto PMON = MON.lock();
          if (!PMON)
            return;
          g_pHyprRenderer->damageMonitor(PMON);
          g_pCompositor->scheduleFrameForMonitor(PMON);
        },
        nullptr);
    g_pEventLoopManager->addTimer(closeDropTimer);
  } else {
    closeDropTimer->updateTimeout(
        std::optional<Time::steady_dur>{std::chrono::milliseconds(1)});
  }

  
  
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

  int bestIdx = -1;
  auto bestCaptureTime = std::chrono::steady_clock::time_point::max();

  for (int i = 0; i < (int)images.size(); ++i) {
    if (i == currentIdx || !images[i].captured)
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

  
  for (const auto& layerVec : PMONITOR->m_layerSurfaceLayers) {
    for (const auto& lsWeak : layerVec) {
      const auto ls = lsWeak.lock();
      if (!ls)
        continue;

      if (ls->m_realPosition)
        ls->m_realPosition->setValueAndWarp(ls->m_realPosition->goal());
      if (ls->m_realSize)
        ls->m_realSize->setValueAndWarp(ls->m_realSize->goal());
      if (ls->m_alpha)
        ls->m_alpha->setValueAndWarp(ls->m_alpha->goal());
    }
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

  const float overlayA = std::clamp(closeOverlayAlpha, 0.0f, 1.0f);
  if (overlayA <= 0.0f)
    return;

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
  if (!textTex || textTex->m_size.x <= 0 || textTex->m_size.y <= 0)
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

void COverview::onPreRender() {
  passQueuedThisFrame = false;

  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR)
    return;

  if (!closing && g_horzaConfig.freezeAnimationsInOverview)
    suppressGlobalAnimations();

  if (!closing && (workspaceListDirty || needsWorkspaceSync()) &&
      syncWorkspaces()) {
    if (images.empty() || currentIdx < 0 || currentIdx >= (int)images.size()) {
      g_pEventLoopManager->doLater([]() { g_pOverview.reset(); });
      return;
    }
    damage();
    g_pCompositor->scheduleFrameForMonitor(PMONITOR);
    return;
  }

  if (images.empty() || currentIdx < 0 || currentIdx >= (int)images.size()) {
    scheduleCloseDrop();
    return;
  }

  if (closing) {
    const auto now = std::chrono::steady_clock::now();
    closeOverlayAlpha = 1.0f;

    if (g_horzaConfig.asyncCloseHandoff && !handoffActive && m_scale) {
      const float start =
          std::clamp(g_horzaConfig.asyncCloseFadeStart, 0.0f, 0.999f);
      if (m_scale->value() >= start)
        handoffActive = true;
    }

    if (closingHandoffActive())
      closeOverlayAlpha = computeCloseOverlayAlpha();

    const bool scaleAnimFinished =
        !m_scale || (!m_scale->isBeingAnimated() && m_scale->value() >= 0.995f);
    if (scaleAnimFinished && closeAnimFinishedAt.time_since_epoch().count() == 0)
      closeAnimFinishedAt = now;

    const bool animFinished = closeAnimFinishedAt.time_since_epoch().count() != 0;
    if (animFinished) {
      if (!finalCrossfadeActive) {
        finalCrossfadeActive = true;
        finalCrossfadeStartedAt = now;
        finalCrossfadeStartAlpha = std::clamp(closeOverlayAlpha, 0.0f, 1.0f);
      }

      const float fadeMs = std::max(0.0f, g_horzaConfig.closeDropDelayMs);
      const float elapsedMs =
          finalCrossfadeStartedAt.time_since_epoch().count() == 0
              ? fadeMs
              : std::chrono::duration<float, std::milli>(now - finalCrossfadeStartedAt)
                    .count();

      if (fadeMs <= 0.0f) {
        closeOverlayAlpha = 0.0f;
      } else {
        float t = std::clamp(elapsedMs / fadeMs, 0.0f, 1.0f);
        
        t = 1.0f - (1.0f - t) * (1.0f - t);
        closeOverlayAlpha =
            std::clamp(finalCrossfadeStartAlpha * (1.0f - t), 0.0f, 1.0f);
      }

      if (elapsedMs >= fadeMs) {
        scheduleCloseDrop();
        return;
      }
    }

    
    
    if (closeStartedAt.time_since_epoch().count() != 0) {
      const float closeElapsedMs =
          std::chrono::duration<float, std::milli>(now - closeStartedAt).count();
      if (closeElapsedMs >= 900.0f) {
        scheduleCloseDrop();
        return;
      }
    }

    damage();
    g_pCompositor->scheduleFrameForMonitor(PMONITOR);
    return;
  }

  const int64_t activeWSID =
      PMONITOR->m_activeWorkspace ? PMONITOR->m_activeWorkspace->m_id : -1;
  if (activeWSID != lastActiveWorkspaceID) {
    lastActiveWorkspaceID = activeWSID;
    if (PMONITOR->m_activeWorkspace != images[currentIdx].pWorkspace)
      onWorkspaceChange();
  }

  
  
  if (openingAnimInProgress()) {
    g_pCompositor->scheduleFrameForMonitor(PMONITOR);
    return;
  }

  const auto frameCaptureStart = std::chrono::steady_clock::now();
  const int maxCapturesPerFrame = std::max(0, g_horzaConfig.maxCapturesPerFrame);
  const float captureBudgetMs = std::max(0.0f, g_horzaConfig.captureBudgetMs);
  int optionalCapturesThisFrame = 0;
  const auto canDoOptionalCapture = [&]() {
    if (optionalCapturesThisFrame >= maxCapturesPerFrame)
      return false;
    if (captureBudgetMs <= 0.0f)
      return true;

    const auto elapsedMs =
        std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - frameCaptureStart)
            .count();
    return elapsedMs < captureBudgetMs;
  };

  if (pendingCapture) {
    bool capturedAny = false;

    while (canDoOptionalCapture()) {
      int nextIdx = -1;
      int bestDist = std::numeric_limits<int>::max();
      for (int i = 0; i < (int)images.size(); ++i) {
        if (images[i].captured)
          continue;
        if (!isTileOnScreen(images[i].displayBox))
          continue;
        const int dist = std::abs(i - currentIdx);
        if (dist < bestDist) {
          bestDist = dist;
          nextIdx = i;
        }
      }

      pendingCapture = nextIdx != -1;
      if (nextIdx == -1)
        break;

      blockOverviewRendering = true;
      captureWorkspace(nextIdx);
      blockOverviewRendering = false;
      images[nextIdx].captured = true;
      images[nextIdx].cachedTex.reset();
      optionalCapturesThisFrame++;
      capturedAny = true;
    }

    if (capturedAny) {
      pendingCapture = false;
      for (int i = 0; i < (int)images.size(); ++i) {
        const auto& img = images[i];
        if (!img.captured && isTileOnScreen(img.displayBox)) {
          pendingCapture = true;
          break;
        }
      }
      damage();
      return;
    }
  }

  if (damageDirty) {
    damageDirty = false;
    blockOverviewRendering = true;
    captureWorkspace(currentIdx);
    blockOverviewRendering = false;
    images[currentIdx].captured = true;
    images[currentIdx].cachedTex.reset();
    damage();
    return;
  }

  if (!pendingCapture && !closing && canDoOptionalCapture()) {
    const auto now = std::chrono::steady_clock::now();
    const int visibleRefreshIdx = pickVisibleLivePreviewWorkspace(now);
    if (visibleRefreshIdx != -1) {
      blockOverviewRendering = true;
      captureWorkspace(visibleRefreshIdx);
      blockOverviewRendering = false;
      images[visibleRefreshIdx].captured = true;
      images[visibleRefreshIdx].cachedTex.reset();
      damage();
      return;
    }
  }

  g_pCompositor->scheduleFrameForMonitor(pMonitor.lock());
}

void COverview::captureWorkspace(int idx) {
  if (idx < 0 || idx >= (int)images.size())
    return;
  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR)
    return;
  if (PMONITOR->m_pixelSize.x <= 0 || PMONITOR->m_pixelSize.y <= 0)
    return;

  blockDamageReporting = true;

  auto &img = images[idx];
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
    img.fb.alloc(monbox.w, monbox.h,
                 PMONITOR->m_output->state->state().drmFormat);
  }

  CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
  g_pHyprRenderer->beginRender(PMONITOR, fakeDamage, RENDER_MODE_FULL_FAKE,
                               nullptr, &img.fb);

  g_pHyprOpenGL->clear(CHyprColor{0, 0, 0, 1.0});

  for (auto &other : images)
    other.pWorkspace->m_visible = false;

  PMONITOR->m_activeWorkspace = img.pWorkspace;
  g_pDesktopAnimationManager->startAnimation(
      img.pWorkspace, CDesktopAnimationManager::ANIMATION_TYPE_IN, true, true);
  img.pWorkspace->m_visible = true;
  suppressWorkspaceWindowAnimations(img.pWorkspace);

  g_pHyprRenderer->renderWorkspace(PMONITOR, img.pWorkspace, Time::steadyNow(),
                                   monbox);

  img.pWorkspace->m_visible = false;
  g_pDesktopAnimationManager->startAnimation(
      img.pWorkspace, CDesktopAnimationManager::ANIMATION_TYPE_OUT, false,
      true);

  g_pHyprOpenGL->m_renderData.blockScreenShader = true;
  g_pHyprRenderer->endRender();

  images[currentIdx].pWorkspace->m_visible = true;
  PMONITOR->m_activeWorkspace = images[currentIdx].pWorkspace;
  g_pDesktopAnimationManager->startAnimation(
      images[currentIdx].pWorkspace,
      CDesktopAnimationManager::ANIMATION_TYPE_IN, true, true);

  img.lastCaptureAt = std::chrono::steady_clock::now();
  blockDamageReporting = false;
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
    backgroundFb.alloc(monbox.w, monbox.h,
                       PMONITOR->m_output->state->state().drmFormat);
  }

  CFramebuffer rawBackgroundFb;
  rawBackgroundFb.alloc(monbox.w, monbox.h,
                        PMONITOR->m_output->state->state().drmFormat);

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

  if (blurRadiusPx <= 0.0f || blurPasses <= 0 || blurStrength <= 0.0f) {
    g_pHyprOpenGL->renderTextureInternal(rawBackgroundFb.getTexture(), bgbox,
                                         sampleData);
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
        g_pHyprOpenGL->renderTextureInternal(rawBackgroundFb.getTexture(),
                                             sampleBox, sampleData);
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

void COverview::close() {
  if (closing)
    return;
  closing = true;
  closeStartedAt = std::chrono::steady_clock::now();
  closeAnimFinishedAt = {};
  handoffActive = false;
  finalCrossfadeActive = false;
  finalCrossfadeStartAlpha = 1.0f;
  finalCrossfadeStartedAt = {};
  closeDropScheduled = false;
  closeOverlayAlpha = 1.0f;
  if (closeDropTimer)
    closeDropTimer->cancel();
  closeDropTimer.reset();

  if (images.empty() || !m_scale || !m_offsetX || !m_crossOffset) {
    scheduleCloseDrop();
    return;
  }

  m_offsetX->setValueAndWarp(0.0f);

  
  if (transitMode) {
    scheduleCloseDrop();
    return;
  }

  *m_crossOffset = 0.0f;
  *m_scale = 1.0f;
  m_scale->setCallbackOnEnd(
      [](WP<Hyprutils::Animation::CBaseAnimatedVariable> var) {
        if (!g_pOverview)
          return;

        if (g_pOverview->closeAnimFinishedAt.time_since_epoch().count() == 0)
          g_pOverview->closeAnimFinishedAt = std::chrono::steady_clock::now();

        g_pOverview->damage();
        const auto PMONITOR = g_pOverview->pMonitor.lock();
        if (PMONITOR)
          g_pCompositor->scheduleFrameForMonitor(PMONITOR);
      });
}

void COverview::reopen() {
  if (!closing)
    return;

  closing = false;
  closeStartedAt = {};
  closeAnimFinishedAt = {};
  handoffActive = false;
  finalCrossfadeActive = false;
  finalCrossfadeStartAlpha = 1.0f;
  finalCrossfadeStartedAt = {};
  closeDropScheduled = false;
  closeOverlayAlpha = 1.0f;
  if (closeDropTimer)
    closeDropTimer->cancel();
  closeDropTimer.reset();

  if (m_offsetX)
    *m_offsetX = 0.0f;
  if (m_crossOffset)
    *m_crossOffset = g_horzaConfig.centerOffset;
  if (m_scale)
    *m_scale = clampDisplayScale(g_horzaConfig.displayScale);

  openAnimPending = false;
  damageDirty = true;
  damage();

  if (const auto PMONITOR = pMonitor.lock())
    g_pCompositor->scheduleFrameForMonitor(PMONITOR);
}

void COverview::render() {
  if (passQueuedThisFrame)
    return;
  passQueuedThisFrame = true;
  g_pHyprRenderer->m_renderPass.add(makeUnique<COverviewPassElement>());
}

void COverview::fullRender() {
  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR || images.empty())
    return;

  if (openAnimPending && !closing) {
    openAnimPending = false;
    *m_scale = clampDisplayScale(g_horzaConfig.displayScale);
    *m_crossOffset = g_horzaConfig.centerOffset;
  }

  const bool closeUnderlayOverlay = closeUnderlayActive();
  if (closingHandoffActive())
    closeOverlayAlpha = computeCloseOverlayAlpha();
  else if (!finalCrossfadeActive)
    closeOverlayAlpha = 1.0f;
  const float overlayA = std::clamp(closeOverlayAlpha, 0.0f, 1.0f);

  CRegion dmg{0, 0, INT16_MAX, INT16_MAX};

  if (g_horzaConfig.hyprpaperBackground && backgroundCaptured) {
    CBox bgbox = {0, 0, PMONITOR->m_size.x, PMONITOR->m_size.y};
    bgbox.scale(PMONITOR->m_scale);
    bgbox.round();

    CHyprOpenGLImpl::STextureRenderData bgRenderData;
    bgRenderData.damage = &dmg;
    bgRenderData.a = closeUnderlayOverlay ? overlayA : 1.0f;

    auto bgTex = backgroundFb.getTexture();
    if (bgTex)
      g_pHyprOpenGL->renderTextureInternal(bgTex, bgbox, bgRenderData);
    else if (!closeUnderlayOverlay)
      g_pHyprOpenGL->clear(CHyprColor{0, 0, 0, 1.0});
    else {
      CHyprOpenGLImpl::SRectRenderData rectData;
      rectData.damage = &dmg;
      g_pHyprOpenGL->renderRect(bgbox, CHyprColor{0, 0, 0, overlayA}, rectData);
    }
  } else {
    if (!closeUnderlayOverlay)
      g_pHyprOpenGL->clear(CHyprColor{0, 0, 0, 1.0});
    else {
      CBox bgbox = {0, 0, PMONITOR->m_size.x, PMONITOR->m_size.y};
      bgbox.scale(PMONITOR->m_scale);
      bgbox.round();
      CHyprOpenGLImpl::SRectRenderData rectData;
      rectData.damage = &dmg;
      g_pHyprOpenGL->renderRect(bgbox, CHyprColor{0, 0, 0, overlayA}, rectData);
    }
  }

  float ds = transitMode ? 1.0f : std::max(g_horzaConfig.displayScale, 0.0001f);
  const float sMin = std::min(1.0f, g_horzaConfig.displayScale);
  const float sMax = std::max(1.0f, g_horzaConfig.displayScale);
  float s =
      transitMode ? 1.0f : std::clamp(m_scale ? m_scale->value() : 1.0f, sMin, sMax);
  if (!std::isfinite(s))
    s = transitMode ? 1.0f : clampDisplayScale(g_horzaConfig.displayScale);
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

  bool hasVisibleUncaptured = false;
  refreshCardShadowTexture();
  const bool drawCardShadow = !transitMode && g_horzaConfig.cardShadow;
  const auto shadowMode = normalizeHorzaToken(horzaTrim(g_horzaConfig.cardShadowMode));
  const bool preferTextureShadow = shadowMode == "texture";
  const bool useTextureShadow = preferTextureShadow && cardShadowTex &&
                                cardShadowTex->m_size.x > 0 &&
                                cardShadowTex->m_size.y > 0;
  const float shadowAlpha = std::clamp(g_horzaConfig.cardShadowAlpha, 0.0f, 1.0f);
  const float shadowSize = std::max(0.0f, g_horzaConfig.cardShadowSize);
  const float shadowOffsetY = g_horzaConfig.cardShadowOffsetY;
  const int baseCornerPx = std::max(0, (int)(g_horzaConfig.cornerRadius * PMONITOR->m_scale));
  const float inactiveTileSizeScale = transitMode
                                          ? 1.0f
                                          : std::clamp(
                                                g_horzaConfig.inactiveTileSizePercent * 0.01f,
                                                0.0f, 1.0f);
  const float centerPrimary = !vertical ? PMONITOR->m_size.x * 0.5f
                                        : PMONITOR->m_size.y * 0.5f;
  const float tileStep = !vertical ? (tileW + gap) : (tileH + gap);

  for (int i = 0; i < (int)images.size(); i++) {
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
      if (!tex) {
        images[i].captured = false;
        if (tileOnScreen)
          hasVisibleUncaptured = true;
        continue;
      }
    } else {
      tex = images[i].cachedTex;
      if (!tex || tex->m_size.x <= 0 || tex->m_size.y <= 0) {
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
    drawDropTargetHighlight();
    if (!transitMode)
      renderWorkspaceTitle(i, dmg, s * tileScaleFactor);
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
        ghostTex && ghostTex->m_size.x > 0 && ghostTex->m_size.y > 0 &&
        ghostBox.w > 0 && ghostBox.h > 0;

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

void COverview::damage() {
  blockDamageReporting = true;
  g_pHyprRenderer->damageMonitor(pMonitor.lock());
  blockDamageReporting = false;
}

void COverview::onDamageReported() {
  if (blockDamageReporting)
    return;
  damageDirty = true;
  damage();
  g_pCompositor->scheduleFrameForMonitor(pMonitor.lock());
}
