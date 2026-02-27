#pragma once
#include <hyprland/src/render/pass/PassElement.hpp>

class COverviewPassElement : public IPassElement {
public:
  COverviewPassElement();

  virtual void draw(const CRegion &damage) override;
  virtual bool needsLiveBlur() override;
  virtual bool needsPrecomputeBlur() override;
  virtual std::optional<CBox> boundingBox() override;
  virtual CRegion opaqueRegion() override;
  virtual const char *passName() override { return "COverviewPassElement"; }
};
