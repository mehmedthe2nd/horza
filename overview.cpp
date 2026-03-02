// Overview lifecycle and frame orchestration (constructor, pre-render, state transitions).
#include "overview.hpp"
#include "OverviewPassElement.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <string>

#define private public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/managers/animation/AnimationManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#undef private

static void
damageCallback(WP<Hyprutils::Animation::CBaseAnimatedVariable> var) {
  if (g_pOverview)
    g_pOverview->damage();
}

static SP<Hyprutils::Animation::SAnimationPropertyConfig> makeAnimConfig() {
  auto cfg = g_pConfigManager->getAnimationPropertyConfig("windowsMove");
  if (!cfg)
    cfg = g_pConfigManager->getAnimationPropertyConfig("windowsIn");
  if (!cfg)
    cfg = g_pConfigManager->getAnimationPropertyConfig("windows");
  if (!cfg)
    cfg = g_pConfigManager->getAnimationPropertyConfig("workspaces");
  if (!cfg)
    cfg = g_pConfigManager->getAnimationPropertyConfig("global");
  if (cfg)
    return cfg;

  static SP<Hyprutils::Animation::SAnimationPropertyConfig> fallbackCfg = [] {
    auto c = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
    c->internalEnabled = 1;
    c->internalSpeed = 1.0f;
    c->internalBezier = "default";
    return c;
  }();
  return fallbackCfg;
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
    images[currentIdx].captured = captureWorkspace(currentIdx);

    if (transitDest_ && transitDest_ != startedOn_) {
      for (int i = 0; i < (int)images.size(); ++i) {
        if (images[i].pWorkspace != transitDest_)
          continue;
        if (!images[i].captured)
          images[i].captured = captureWorkspace(i);
        break;
      }
    }

    blockOverviewRendering = false;
    pendingCapture = false;

    if (Event::bus()) {
      preRenderHook = Event::bus()->m_events.render.pre.listen(
          [this](PHLMONITOR mon) {
            const auto PMONITOR = pMonitor.lock();
            if (!PMONITOR || !mon || mon->m_id != PMONITOR->m_id)
              return;
            onPreRender();
          });
    }

    ready = true;
    return;
  }

  for (int i = 0; i < (int)images.size(); ++i)
    restoreTileFromCache(i);

  backgroundCaptured = false;
  blockOverviewRendering = true;
  if (!images[currentIdx].cachedTex) {
    images[currentIdx].captured = captureWorkspace(currentIdx);
    if (!images[currentIdx].captured) {
      Log::logger->log(Log::ERR, "[horza] initial capture failed for ws={}",
                       images[currentIdx].pWorkspace
                           ? std::to_string(images[currentIdx].pWorkspace->m_id)
                           : "null");
    }
  } else if (!images[currentIdx].captured) {
    damageDirty = true;
  }
  if (g_horzaConfig.hyprpaperBackground)
    captureBackground();
  if (g_horzaConfig.prewarmAll) {
    for (int i = 0; i < (int)images.size(); ++i) {
      if (images[i].captured)
        continue;
      images[i].captured = captureWorkspace(i);
      images[i].cachedTex.reset();
    }
  } else {
    const int initialPrewarmRadius = std::clamp(g_horzaConfig.livePreviewRadius, 0, 4);
    for (int d = 1; d <= initialPrewarmRadius; ++d) {
      const int leftIdx = currentIdx - d;
      if (leftIdx >= 0 && leftIdx < (int)images.size() &&
          !images[leftIdx].captured) {
        images[leftIdx].captured = captureWorkspace(leftIdx);
        images[leftIdx].cachedTex.reset();
      }

      const int rightIdx = currentIdx + d;
      if (rightIdx >= 0 && rightIdx < (int)images.size() &&
          !images[rightIdx].captured) {
        images[rightIdx].captured = captureWorkspace(rightIdx);
        images[rightIdx].cachedTex.reset();
      }
    }
  }
  blockOverviewRendering = false;

  damageDirty = true;
  pendingCapture = !g_horzaConfig.prewarmAll;
  m_scale->setValueAndWarp(1.0f);
  
  
  openAnimPending = true;

  if (Event::bus()) {
    mouseMoveHook = Event::bus()->m_events.input.mouse.move.listen(
        [this](Vector2D coords, SCallbackInfo& info) { onMouseMove(); });
    mouseButtonHook = Event::bus()->m_events.input.mouse.button.listen(
        [this](IPointer::SButtonEvent e, SCallbackInfo& info) {
          if (closing)
            return;
          info.cancelled = true;
          onMouseButton(e, info);
        });
    mouseAxisHook = Event::bus()->m_events.input.mouse.axis.listen(
        [this](IPointer::SAxisEvent e, SCallbackInfo& info) {
          onMouseAxis(e, info);
        });
    keyPressHook = Event::bus()->m_events.input.keyboard.key.listen(
        [this](IKeyboard::SKeyEvent e, SCallbackInfo& info) {
          onKeyPress(e, info);
        });
  }

  if (Event::bus()) {
    createWorkspaceHook = Event::bus()->m_events.workspace.created.listen(
        [this](PHLWORKSPACEREF ws) { requestWorkspaceSync(); });
    destroyWorkspaceHook = Event::bus()->m_events.workspace.removed.listen(
        [this](PHLWORKSPACEREF ws) { requestWorkspaceSync(); });
    moveWorkspaceHook = Event::bus()->m_events.workspace.moveToMonitor.listen(
        [this](PHLWORKSPACE ws, PHLMONITOR mon) { requestWorkspaceSync(); });
    monitorAddedHook = Event::bus()->m_events.monitor.added.listen(
        [this](PHLMONITOR mon) { requestWorkspaceSync(); });
    monitorRemovedHook = Event::bus()->m_events.monitor.removed.listen(
        [this](PHLMONITOR mon) { requestWorkspaceSync(); });
    configReloadedHook = Event::bus()->m_events.config.reloaded.listen(
        [this]() { requestWorkspaceSync(); });
    preRenderHook = Event::bus()->m_events.render.pre.listen(
        [this](PHLMONITOR mon) {
          const auto PMONITOR = pMonitor.lock();
          if (!PMONITOR || !mon || mon->m_id != PMONITOR->m_id)
            return;
          onPreRender();
        });
  }

  ready = true;
}

COverview::~COverview() {
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


void COverview::onPreRender() {
  passQueuedThisFrame = false;

  if (blockOverviewRendering)
    return;

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

    const bool scaleAnimFinished =
        !m_scale || (!m_scale->isBeingAnimated() && m_scale->value() >= 0.995f);
    if (scaleAnimFinished && closeAnimFinishedAt.time_since_epoch().count() == 0)
      closeAnimFinishedAt = now;

    if (closeAnimFinishedAt.time_since_epoch().count() != 0) {
      scheduleCloseDrop();
      return;
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
  const int captureRadius = std::max(0, g_horzaConfig.livePreviewRadius);
  const auto inCaptureRadius = [&](int idx) {
    if (idx == currentIdx)
      return true;
    if (captureRadius <= 0)
      return false;
    return std::abs(idx - currentIdx) <= captureRadius;
  };
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
        if (!inCaptureRadius(i))
          continue;
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
      images[nextIdx].captured = captureWorkspace(nextIdx);
      blockOverviewRendering = false;
      images[nextIdx].cachedTex.reset();
      optionalCapturesThisFrame++;
      if (images[nextIdx].captured)
        capturedAny = true;
    }

    if (capturedAny) {
      pendingCapture = false;
      for (int i = 0; i < (int)images.size(); ++i) {
        if (!inCaptureRadius(i))
          continue;
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
    images[currentIdx].captured = captureWorkspace(currentIdx);
    blockOverviewRendering = false;
    images[currentIdx].cachedTex.reset();
    damage();
    return;
  }

  if (!pendingCapture && !closing && canDoOptionalCapture()) {
    const auto now = std::chrono::steady_clock::now();
    const int visibleRefreshIdx = pickVisibleLivePreviewWorkspace(now);
    if (visibleRefreshIdx != -1) {
      blockOverviewRendering = true;
      images[visibleRefreshIdx].captured = captureWorkspace(visibleRefreshIdx);
      blockOverviewRendering = false;
      images[visibleRefreshIdx].cachedTex.reset();
      damage();
      return;
    }
  }

  if (const auto PMONITOR = pMonitor.lock())
    g_pCompositor->scheduleFrameForMonitor(PMONITOR);
}


void COverview::close() {
  if (closing)
    return;

  closing = true;
  closeStartedAt = std::chrono::steady_clock::now();
  closeAnimFinishedAt = {};
  closeDropScheduled = false;

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
  closeDropScheduled = false;

  if (m_offsetX)
    *m_offsetX = 0.0f;
  if (m_crossOffset)
    *m_crossOffset = g_horzaConfig.centerOffset;
  if (m_scale)
    *m_scale = effectiveDisplayScale(g_horzaConfig.displayScale);

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


void COverview::damage() {
  blockDamageReporting = true;
  if (const auto PMONITOR = pMonitor.lock())
    g_pHyprRenderer->damageMonitor(PMONITOR);
  blockDamageReporting = false;
}

void COverview::onDamageReported() {
  if (blockDamageReporting)
    return;
  damageDirty = true;
  damage();
  if (const auto PMONITOR = pMonitor.lock())
    g_pCompositor->scheduleFrameForMonitor(PMONITOR);
}
