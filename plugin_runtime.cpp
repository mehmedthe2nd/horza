#include "plugin_runtime.hpp"

#include "config.hpp"
#include "globals.hpp"
#include "overview.hpp"
#include <stdexcept>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

CPluginRuntime::~CPluginRuntime() { shutdown(); }

void CPluginRuntime::init(const std::function<void()>& onConfigReload) {
  if (initialized)
    return;

  onConfigReloadCallback = onConfigReload;

  {
    auto methods = HyprlandAPI::findFunctionsByName(PHANDLE, "renderWorkspace");
    if (methods.empty())
      throw std::runtime_error("[horza] renderWorkspace not found");

    renderWorkspaceHook = HyprlandAPI::createFunctionHook(
        PHANDLE, methods[0].address, (void*)hkRenderWorkspaceBridge);

    if (!renderWorkspaceHook || !renderWorkspaceHook->hook())
      throw std::runtime_error("[horza] failed to hook renderWorkspace");
  }

  {
    auto methods = HyprlandAPI::findFunctionsByName(
        PHANDLE, "_ZN8CMonitor9addDamageERKN9Hyprutils4Math4CBoxE");
    if (!methods.empty()) {
      addDamageHookA = HyprlandAPI::createFunctionHook(
          PHANDLE, methods[0].address, (void*)hkAddDamageABridge);
      if (!addDamageHookA || !addDamageHookA->hook())
        throw std::runtime_error("[horza] failed to hook addDamage(CBox)");
    }
  }

  {
    auto methods = HyprlandAPI::findFunctionsByName(
        PHANDLE, "addDamageEPK15pixman_region32");
    if (!methods.empty()) {
      addDamageHookB = HyprlandAPI::createFunctionHook(
          PHANDLE, methods[0].address, (void*)hkAddDamageBBridge);
      if (!addDamageHookB || !addDamageHookB->hook())
        throw std::runtime_error("[horza] failed to hook addDamage(region)");
    }
  }

  HyprlandAPI::addDispatcherV2(PHANDLE, "horza:toggle", dispatchToggleBridge);
  HyprlandAPI::addDispatcherV2(PHANDLE, "overview:toggle", dispatchToggleBridge);
  HyprlandAPI::addDispatcherV2(PHANDLE, "horza:workspace",
                               dispatchWorkspaceTransitBridge);

  if (auto* bus = Event::bus().get()) {
    if (onConfigReloadCallback) {
      configReloadListener =
          bus->m_events.config.reloaded.listen(
              [this]() { onConfigReloadCallback(); });
    }

    renderStageListener =
        bus->m_events.render.stage.listen([this](eRenderStage stage) {
          if (stage != RENDER_LAST_MOMENT)
            return;

          auto* ov = g_pOverview.get();
          if (!ov || ov->blockOverviewRendering)
            return;
          if (ov->closeDropPending())
            return;

          const auto ovMon = ov->pMonitor.lock();
          if (!ovMon)
            return;

          if (!g_pHyprOpenGL || !g_pHyprOpenGL->m_renderData.pMonitor)
            return;
          const auto stageMon = g_pHyprOpenGL->m_renderData.pMonitor.lock();
          if (!stageMon || stageMon->m_id != ovMon->m_id)
            return;

          ov->render();
        });
    renderViaStage = true;
  } else {
    renderViaStage = false;
  }

  initialized = true;
}

void CPluginRuntime::shutdown() {
  if (addDamageHookA) {
    addDamageHookA->unhook();
    addDamageHookA = nullptr;
  }
  if (addDamageHookB) {
    addDamageHookB->unhook();
    addDamageHookB = nullptr;
  }
  if (renderWorkspaceHook) {
    renderWorkspaceHook->unhook();
    renderWorkspaceHook = nullptr;
  }

  renderStageListener.reset();
  renderViaStage = false;
  configReloadListener.reset();
  onConfigReloadCallback = nullptr;
  initialized = false;
}

void CPluginRuntime::hookRenderWorkspace(void* thisptr, PHLMONITOR monitor,
                                         PHLWORKSPACE workspace,
                                         const Time::steady_tp& now,
                                         const CBox& geometry) {
  auto callOriginal = [&]() {
    if (!renderWorkspaceHook)
      return;
    (*(origRenderWorkspace_t)renderWorkspaceHook->m_original)(
        thisptr, monitor, workspace, now, geometry);
  };

  auto* ov = g_pOverview.get();
  if (!ov || !monitor || renderingOverview) {
    callOriginal();
    return;
  }

  if (ov->closeDropPending()) {
    const auto mon = ov->pMonitor.lock();
    g_pOverview.reset();
    if (mon) {
      g_pHyprRenderer->damageMonitor(mon);
      g_pCompositor->scheduleFrameForMonitor(mon);
    }
    callOriginal();
    return;
  }

  if (ov->blockOverviewRendering) {
    callOriginal();
    return;
  }

  const auto ovMon = ov->pMonitor.lock();
  if (!ovMon || ovMon->m_id != monitor->m_id) {
    callOriginal();
    return;
  }

  if (!renderViaStage)
    ov->render();
}

void CPluginRuntime::hookAddDamageA(void* thisptr, const CBox& box) {
  auto callOriginal = [&]() {
    if (!addDamageHookA)
      return;
    (*(origAddDamageA_t)addDamageHookA->m_original)(thisptr, box);
  };

  const auto mon = reinterpret_cast<CMonitor*>(thisptr);
  auto* ov = g_pOverview.get();
  if (!ov || !mon) {
    callOriginal();
    return;
  }

  const auto ovMon = ov->pMonitor.lock();
  if (!ovMon || ovMon->m_id != mon->m_id || ov->blockDamageReporting) {
    callOriginal();
    return;
  }

  ov->onDamageReported();
}

void CPluginRuntime::hookAddDamageB(void* thisptr, const pixman_region32_t* rg) {
  auto callOriginal = [&]() {
    if (!addDamageHookB)
      return;
    (*(origAddDamageB_t)addDamageHookB->m_original)(thisptr, rg);
  };

  const auto mon = reinterpret_cast<CMonitor*>(thisptr);
  auto* ov = g_pOverview.get();
  if (!ov || !mon) {
    callOriginal();
    return;
  }

  const auto ovMon = ov->pMonitor.lock();
  if (!ovMon || ovMon->m_id != mon->m_id || ov->blockDamageReporting) {
    callOriginal();
    return;
  }

  ov->onDamageReported();
}

SDispatchResult CPluginRuntime::dispatchToggle(std::string arg) {
  (void)arg;
  if (g_pOverview) {
    if (!g_pOverview->ready) {
      g_pOverview.reset();
      return {};
    }
    if (g_pOverview->closing)
      g_pOverview->reopen();
    else
      g_pOverview->close();
    return {};
  }

  const auto mon = Desktop::focusState()->monitor();
  if (!mon || !mon->m_activeWorkspace)
    return {};

  renderingOverview = true;
  g_pOverview = std::make_unique<COverview>(mon->m_activeWorkspace);
  renderingOverview = false;

  if (!g_pOverview || !g_pOverview->ready) {
    g_pOverview.reset();
    return {};
  }

  if (auto ovMon = g_pOverview->pMonitor.lock()) {
    g_pHyprRenderer->damageMonitor(ovMon);
    g_pCompositor->scheduleFrameForMonitor(ovMon);
  }

  return {};
}

PHLWORKSPACE CPluginRuntime::resolveWorkspaceFromArg(const std::string& arg,
                                                     PHLMONITOR mon) const {
  if (!mon || !mon->m_activeWorkspace)
    return nullptr;

  const std::string trimmed = horzaTrim(arg);
  if (trimmed.empty())
    return nullptr;

  int targetID = -1;
  if (trimmed[0] == '+' || trimmed[0] == '-') {
    try {
      targetID = mon->m_activeWorkspace->m_id + std::stoi(trimmed);
    } catch (...) {
      return nullptr;
    }
  } else {
    try {
      targetID = std::stoi(trimmed);
    } catch (...) {
      return nullptr;
    }
  }

  if (targetID <= 0 || mon->m_activeWorkspace->m_id == targetID)
    return nullptr;

  const auto ws = g_pCompositor->getWorkspaceByID(targetID);
  if (!ws || ws->monitorID() != mon->m_id)
    return nullptr;

  return ws;
}

SDispatchResult CPluginRuntime::dispatchWorkspaceTransit(std::string arg) {
  const auto workspaceDispatcher =
      g_pKeybindManager->m_dispatchers.find("workspace");
  if (workspaceDispatcher == g_pKeybindManager->m_dispatchers.end())
    return {};

  const auto dispatchWorkspace = [&](const std::string& val) {
    workspaceDispatcher->second(val);
  };

  const auto mon = Desktop::focusState()->monitor();
  if (!mon || !mon->m_activeWorkspace) {
    dispatchWorkspace(arg);
    return {};
  }

  if (g_pOverview) {
    dispatchWorkspace(arg);
    return {};
  }

  const auto dest = resolveWorkspaceFromArg(arg, mon);
  if (!dest) {
    dispatchWorkspace(arg);
    return {};
  }

  renderingOverview = true;
  g_pOverview = std::make_unique<COverview>(mon->m_activeWorkspace, true, dest);
  renderingOverview = false;

  if (!g_pOverview || !g_pOverview->ready) {
    g_pOverview.reset();
    dispatchWorkspace(arg);
    return {};
  }

  if (auto ovMon = g_pOverview->pMonitor.lock()) {
    g_pHyprRenderer->damageMonitor(ovMon);
    g_pCompositor->scheduleFrameForMonitor(ovMon);
  }

  dispatchWorkspace(arg);
  return {};
}

void CPluginRuntime::hkRenderWorkspaceBridge(void* thisptr, PHLMONITOR monitor,
                                             PHLWORKSPACE workspace,
                                             const Time::steady_tp& now,
                                             const CBox& geometry) {
  if (!g_pPluginRuntime)
    return;
  g_pPluginRuntime->hookRenderWorkspace(thisptr, monitor, workspace, now,
                                        geometry);
}

void CPluginRuntime::hkAddDamageABridge(void* thisptr, const CBox& box) {
  if (!g_pPluginRuntime)
    return;
  g_pPluginRuntime->hookAddDamageA(thisptr, box);
}

void CPluginRuntime::hkAddDamageBBridge(void* thisptr,
                                        const pixman_region32_t* rg) {
  if (!g_pPluginRuntime)
    return;
  g_pPluginRuntime->hookAddDamageB(thisptr, rg);
}

SDispatchResult CPluginRuntime::dispatchToggleBridge(std::string arg) {
  if (!g_pPluginRuntime)
    return {};
  return g_pPluginRuntime->dispatchToggle(arg);
}

SDispatchResult CPluginRuntime::dispatchWorkspaceTransitBridge(std::string arg) {
  if (!g_pPluginRuntime)
    return {};
  return g_pPluginRuntime->dispatchWorkspaceTransit(arg);
}
