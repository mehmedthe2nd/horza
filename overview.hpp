#pragma once
#include "config.hpp"
#include "globals.hpp"
#include <any>
#include <chrono>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/managers/HookSystemManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <memory>
#include <vector>

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
  bool closingHandoffActive() const;
  bool closeUnderlayActive() const;
  bool closeDropPending() const;

  bool ready = false;
  bool closing = false;
  bool transitMode = false;

  SP<HOOK_CALLBACK_FN> preRenderHook;
  SP<HOOK_CALLBACK_FN> mouseButtonHook;
  SP<HOOK_CALLBACK_FN> mouseMoveHook;
  SP<HOOK_CALLBACK_FN> mouseAxisHook;
  SP<HOOK_CALLBACK_FN> keyPressHook;
  SP<HOOK_CALLBACK_FN> createWorkspaceHook;
  SP<HOOK_CALLBACK_FN> destroyWorkspaceHook;
  SP<HOOK_CALLBACK_FN> moveWorkspaceHook;
  SP<HOOK_CALLBACK_FN> monitorAddedHook;
  SP<HOOK_CALLBACK_FN> monitorRemovedHook;
  SP<HOOK_CALLBACK_FN> configReloadedHook;

  bool blockOverviewRendering = false;
  bool blockDamageReporting = false;

  PHLMONITORREF pMonitor;

private:
  void onMouseButton(std::any param, SCallbackInfo& info);
  void onMouseMove();
  void onMouseAxis(std::any param, SCallbackInfo& info);
  void onKeyPress(std::any param, SCallbackInfo& info);
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
  bool isTileOnScreen(const CBox& box) const;
  int pickVisibleLivePreviewWorkspace(
      std::chrono::steady_clock::time_point now) const;
  std::string workspaceTitleFor(const PHLWORKSPACE& ws) const;
  void suppressGlobalAnimations() const;
  void suppressWorkspaceWindowAnimations(const PHLWORKSPACE& ws) const;
  void captureWorkspace(int idx);
  void captureBackground();
  void refreshCardShadowTexture();
  void renderWorkspaceTitle(int idx, const CRegion& dmg, float tileScale);
  float computeCloseOverlayAlpha() const;
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
  bool handoffActive = false;
  bool finalCrossfadeActive = false;
  bool closeDropScheduled = false;
  float closeOverlayAlpha = 1.0f;
  float finalCrossfadeStartAlpha = 1.0f;
  bool passQueuedThisFrame = false;
  std::chrono::steady_clock::time_point closeStartedAt{};
  std::chrono::steady_clock::time_point closeAnimFinishedAt{};
  std::chrono::steady_clock::time_point finalCrossfadeStartedAt{};
  SP<CEventLoopTimer> closeDropTimer;

  PHLANIMVAR<float> m_scale;
  PHLANIMVAR<float> m_offsetX;
  PHLANIMVAR<float> m_crossOffset;

  friend class COverviewPassElement;
};

inline std::unique_ptr<COverview> g_pOverview;
