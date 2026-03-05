#pragma once

#include <any>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

struct pixman_region32;
using pixman_region32_t = struct pixman_region32;

class CPluginRuntime {
public:
  CPluginRuntime() = default;
  ~CPluginRuntime();

  void init(const std::function<void()>& onConfigReload);
  void shutdown();

private:
  using origRenderWorkspace_t =
      void (*)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&,
               const CBox&);
  using origAddDamageA_t = void (*)(void*, const CBox&);
  using origAddDamageB_t = void (*)(void*, const pixman_region32_t*);

  void hookRenderWorkspace(void* thisptr, PHLMONITOR monitor,
                           PHLWORKSPACE workspace, const Time::steady_tp& now,
                           const CBox& geometry);
  void hookAddDamageA(void* thisptr, const CBox& box);
  void hookAddDamageB(void* thisptr, const pixman_region32_t* rg);

  SDispatchResult dispatchToggle(std::string arg);
  SDispatchResult dispatchWorkspaceTransit(std::string arg);
  PHLWORKSPACE resolveWorkspaceFromArg(const std::string& arg,
                                       PHLMONITOR mon) const;

  static void hkRenderWorkspaceBridge(void* thisptr, PHLMONITOR monitor,
                                      PHLWORKSPACE workspace,
                                      const Time::steady_tp& now,
                                      const CBox& geometry);
  static void hkAddDamageABridge(void* thisptr, const CBox& box);
  static void hkAddDamageBBridge(void* thisptr, const pixman_region32_t* rg);
  static SDispatchResult dispatchToggleBridge(std::string arg);
  static SDispatchResult dispatchWorkspaceTransitBridge(std::string arg);

  bool initialized = false;
  bool renderingOverview = false;
  bool renderViaStage = false;

  std::function<void()> onConfigReloadCallback;
  std::any configReloadListener;
  std::any renderStageListener;

  CFunctionHook* renderWorkspaceHook = nullptr;
  CFunctionHook* addDamageHookA = nullptr;
  CFunctionHook* addDamageHookB = nullptr;
};

inline std::unique_ptr<CPluginRuntime> g_pPluginRuntime;
