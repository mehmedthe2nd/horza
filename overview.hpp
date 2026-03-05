#pragma once
#include "config.hpp"
#include <any>
#include <chrono>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <memory>
#include <vector>

using SCallbackInfo = Event::SCallbackInfo;

class COverview {
public:
  
  
  COverview(PHLWORKSPACE startedOn_, bool transitMode_ = false,
            PHLWORKSPACE transitDest_ = nullptr);
  ~COverview();

  void render();
  void fullRender();
  void damage();
  void onDamageReported();
  void onPreRender();
  void close();
  void reopen();
  void requestWorkspaceSync();
  bool closeDropPending() const;

  bool ready = false;
  bool closing = false;
  bool transitMode = false;

  std::any preRenderHook;
  std::any mouseButtonHook;
  std::any mouseMoveHook;
  std::any mouseAxisHook;
  std::any keyPressHook;
  std::any createWorkspaceHook;
  std::any destroyWorkspaceHook;
  std::any moveWorkspaceHook;
  std::any monitorAddedHook;
  std::any monitorRemovedHook;
  std::any configReloadedHook;

  bool blockOverviewRendering = false;
  bool blockDamageReporting = false;

  PHLMONITORREF pMonitor;

private:
  void onMouseButton(const IPointer::SButtonEvent& e, SCallbackInfo& info);
  void onMouseMove();
  void onMouseAxis(const IPointer::SAxisEvent& e, SCallbackInfo& info);
  void onKeyPress(const IKeyboard::SKeyEvent& e, SCallbackInfo& info);
  void onWorkspaceChange();
  bool syncWorkspaces();
  bool needsWorkspaceSync();
  bool shiftCurrentIndexBy(int step);
  int hitTileIndex(const Vector2D& localPos) const;
  Vector2D tileLocalToWorkspacePos(const CBox& tileBox,
                                   const Vector2D& localPos) const;
  PHLWINDOW pickWindowInWorkspace(const PHLWORKSPACE& ws,
                                  const Vector2D& workspacePos) const;
  void clearDragState();
  bool restoreTileFromCache(int idx);
  void saveTilesToCache();
  bool openingAnimInProgress() const;
  bool switchAnimInProgress() const;
  bool shouldDeferCaptures() const;
  bool needsFramePump() const;
  bool framePumpDue(std::chrono::steady_clock::time_point now) const;
  void pumpFrameIfDue(bool force = false);
  bool isTileOnScreen(const CBox& box) const;
  int pickVisibleLivePreviewWorkspace(
      std::chrono::steady_clock::time_point now) const;
  std::string workspaceTitleFor(const PHLWORKSPACE& ws) const;
  void suppressGlobalAnimations() const;
  void suppressWorkspaceWindowAnimations(const PHLWORKSPACE& ws) const;
  bool captureWorkspace(int idx);
  void captureBackground();
  void refreshCardShadowTexture();
  void renderWorkspaceTitle(int idx, const CRegion& dmg, float tileScale);
  void scheduleCloseDrop();

  struct SWorkspaceImage {
    CFramebuffer fb;
    PHLWORKSPACE pWorkspace;
    CBox displayBox;
    bool captured = false;
    std::chrono::steady_clock::time_point lastCaptureAt{};
    SP<CTexture> cachedTex;
    SP<CTexture> titleTex;
    std::string titleTextCached;
    int titleMaxWidthCached = 0;
    int titleFontCached = 0;
    std::string titleFontFamilyCached;
  };

  std::vector<SWorkspaceImage> images;
  int currentIdx = 0;
  bool damageDirty = false;
  bool pendingCapture = false;
  bool workspaceListDirty = false;
  std::chrono::steady_clock::time_point nextWorkspaceSyncPollAt{};
  bool openAnimPending = false;
  bool backgroundCaptured = false;
  bool directScanoutWasBlocked = false;
  int64_t lastActiveWorkspaceID = -1;
  CFramebuffer backgroundFb;
  SP<CTexture> cardShadowTex;
  std::string cardShadowTexConfigPath;
  std::string cardShadowTexResolvedPath;
  bool cardShadowMissingPathLogged = false;
  bool cardShadowLoadErrorLogged = false;
  Vector2D lastMousePosLocal = {};
  Vector2D dragStartPosLocal = {};
  Vector2D dragLastPosLocal = {};
  Vector2D dragWindowPosWorkspace = {};
  Vector2D dragWindowSizeWorkspace = {};
  Vector2D dragWindowGrabOffsetWorkspace = {};
  double scrollGestureAccum = 0.0;
  bool leftButtonDown = false;
  bool draggingWindow = false;
  int dragSourceIdx = -1;
  int dragTargetIdx = -1;
  PHLWINDOW dragWindow = nullptr;
  std::chrono::steady_clock::time_point dragNextHoverJumpAt{};
  bool closeDropScheduled = false;
  bool passQueuedThisFrame = false;
  std::chrono::steady_clock::time_point lastFramePumpAt{};
  std::chrono::steady_clock::time_point closeStartedAt{};
  std::chrono::steady_clock::time_point closeAnimFinishedAt{};

  PHLANIMVAR<float> m_scale;
  PHLANIMVAR<float> m_offsetX;
  PHLANIMVAR<float> m_crossOffset;

  friend class COverviewPassElement;
};

inline std::unique_ptr<COverview> g_pOverview;
