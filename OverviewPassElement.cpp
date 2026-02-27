#include "OverviewPassElement.hpp"
#include "overview.hpp"
#include <hyprland/src/render/OpenGL.hpp>

COverviewPassElement::COverviewPassElement() {}

void COverviewPassElement::draw(const CRegion &damage) {
  if (!g_pOverview)
    return;
  g_pOverview->fullRender();
}

bool COverviewPassElement::needsLiveBlur() { return false; }
bool COverviewPassElement::needsPrecomputeBlur() { return false; }

std::optional<CBox> COverviewPassElement::boundingBox() {
  if (!g_pOverview)
    return std::nullopt;
  if (!g_pOverview->pMonitor)
    return std::nullopt;
  return CBox{{}, g_pOverview->pMonitor->m_size};
}

CRegion COverviewPassElement::opaqueRegion() {
  if (!g_pOverview)
    return CRegion{};
  if (!g_pOverview->pMonitor)
    return CRegion{};
  return CBox{{}, g_pOverview->pMonitor->m_size};
}
