// Overview interaction pipeline (pointer/keyboard input, drag, and workspace navigation).
#include "overview.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <linux/input-event-codes.h>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>

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

  const float targetDisplayScale = effectiveDisplayScale(g_horzaConfig.displayScale);
  const float sMin = std::min(1.0f, targetDisplayScale);
  const float sMax = std::max(1.0f, targetDisplayScale);
  float s = std::clamp(m_scale->value(), sMin, sMax);
  float ds = std::max(targetDisplayScale, 0.0001f);
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
  lastSelectionChangeAt = std::chrono::steady_clock::now();

  m_offsetX->setValueAndWarp(m_offsetX->value() + (oldCenter - newCenter));
  *m_offsetX = 0.0f;

  if (!images[currentIdx].captured && !images[currentIdx].cachedTex) {
    blockOverviewRendering = true;
    images[currentIdx].captured = captureWorkspace(currentIdx);
    blockOverviewRendering = false;
    images[currentIdx].cachedTex.reset();
  } else if (!images[currentIdx].captured && images[currentIdx].cachedTex) {
    damageDirty = true;
  }

  pendingCapture = true;
  damage();
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
  }
}

void COverview::onMouseButton(const IPointer::SButtonEvent& e,
                              SCallbackInfo& info) {
  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR)
    return;
  lastMousePosLocal =
      g_pInputManager->getMouseCoordsInternal() - PMONITOR->m_position;

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
    Desktop::focusState()->fullWindowFocus(PWINDOW, Desktop::FOCUS_REASON_CLICK);

  close();
}

void COverview::onMouseAxis(const IPointer::SAxisEvent& e,
                            SCallbackInfo& info) {
  if (closing || images.size() < 2)
    return;

  const auto PMONITOR = pMonitor.lock();
  if (!PMONITOR)
    return;
  if (g_pCompositor->getMonitorFromCursor() != PMONITOR)
    return;

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

  const float targetDisplayScale = effectiveDisplayScale(g_horzaConfig.displayScale);
  const float sMin = std::min(1.0f, targetDisplayScale);
  const float sMax = std::max(1.0f, targetDisplayScale);
  float s = std::clamp(m_scale->value(), sMin, sMax);
  float ds = std::max(targetDisplayScale, 0.0001f);
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
  lastSelectionChangeAt = std::chrono::steady_clock::now();

  m_offsetX->setValueAndWarp(m_offsetX->value() + (oldCenter - newCenter));
  *m_offsetX = 0.0f;

  if (!images[currentIdx].captured && !images[currentIdx].cachedTex) {
    blockOverviewRendering = true;
    images[currentIdx].captured = captureWorkspace(currentIdx);
    blockOverviewRendering = false;
    images[currentIdx].cachedTex.reset();
  } else if (!images[currentIdx].captured && images[currentIdx].cachedTex) {
    damageDirty = true;
  }

  pendingCapture = true;
  damage();
}

void COverview::onKeyPress(const IKeyboard::SKeyEvent& e,
                           SCallbackInfo& info) {
  if (closing)
    return;
  if (!g_horzaConfig.escOnly)
    return;

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

  const float targetDisplayScale = effectiveDisplayScale(g_horzaConfig.displayScale);
  const float sMin = std::min(1.0f, targetDisplayScale);
  const float sMax = std::max(1.0f, targetDisplayScale);
  float s = transitMode ? 1.0f : std::clamp(m_scale->value(), sMin, sMax);
  float ds = transitMode ? 1.0f : std::max(targetDisplayScale, 0.0001f);
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
  lastSelectionChangeAt = std::chrono::steady_clock::now();

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

  if (!images[currentIdx].captured && !images[currentIdx].cachedTex) {
    blockOverviewRendering = true;
    images[currentIdx].captured = captureWorkspace(currentIdx);
    blockOverviewRendering = false;
    images[currentIdx].cachedTex.reset();
  } else if (!images[currentIdx].captured && images[currentIdx].cachedTex) {
    damageDirty = true;
  }

  pendingCapture = true;
  damageDirty = true;
}

void COverview::requestWorkspaceSync() {
  workspaceListDirty = true;
  nextWorkspaceSyncPollAt = std::chrono::steady_clock::now();
  damageRefreshIdx = -1;
  pendingCapture = true;
  damageDirty = true;

  if (!pMonitor.lock())
    return;

  damage();
}
