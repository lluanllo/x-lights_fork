#pragma once

/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "RenderableEffect.h"

// A light field shared by several models: each light asks the field (a wave, a sweep, a
// pulse...) what it shows at its own place on the stage, so one gesture can cross a tree,
// an arch and a matrix as one movement. The Show Composer writes it; it can be edited like
// any effect. Where the field gives nothing the light is left transparent, so in the
// "Normal" layer method the layers below (a figure's own colours, a face) show through.
class StageFieldEffect : public RenderableEffect {
public:
    explicit StageFieldEffect(int id);
    ~StageFieldEffect() override;
    void Render(Effect* effect, const SettingsMap& settings, RenderBuffer& buffer) override;
    bool CanRenderPartialTimeInterval() const override { return true; }
};
