// Overview workspace-list synchronization (topology detection + image list reconciliation).
#include "overview.hpp"
#include <algorithm>
#include <chrono>

#define private public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#undef private

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
      images[i].captured = captureWorkspace(i);
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
