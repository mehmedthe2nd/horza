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
  const auto mon = g_pOverview->pMonitor.lock();
  if (!mon)
    return std::nullopt;
  return CBox{{}, mon->m_size};
}

CRegion COverviewPassElement::opaqueRegion() {
  if (!g_pOverview)
    return CRegion{};
  const auto mon = g_pOverview->pMonitor.lock();
  if (!mon)
    return CRegion{};
  return CBox{{}, mon->m_size};
}
