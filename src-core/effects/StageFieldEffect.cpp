/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "StageFieldEffect.h"

#include "../composer/ProceduralField.h"
#include "../models/Node.h"
#include "../render/RenderBuffer.h"
#include "../utils/UtilClasses.h"

#include "../../include/shockwave-16.xpm"
#include "../../include/shockwave-24.xpm"
#include "../../include/shockwave-32.xpm"
#include "../../include/shockwave-48.xpm"
#include "../../include/shockwave-64.xpm"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

// What the field needs per light, worked out once for a buffer and a setting.
class StageFieldCache : public EffectRenderCache {
public:
    std::string text;
    bool ok = false;
    composer::FieldSpec spec;
    std::vector<float> x;
    std::vector<float> y;
    std::vector<char> lit; // the lights it may light
};

StageFieldCache* CacheFor(RenderBuffer& buffer, int id, const std::string& text) {
    auto* cache = static_cast<StageFieldCache*>(buffer.infoCache[id]);
    if (cache == nullptr) {
        cache = new StageFieldCache();
        buffer.infoCache[id] = cache;
    }
    const auto& nodes = buffer.GetNodes();
    if (cache->text == text && cache->x.size() == nodes.size()) {
        return cache;
    }
    cache->text = text;
    std::string error;
    cache->ok = composer::FromText(text, cache->spec, error);
    cache->x.assign(nodes.size(), 0.0F);
    cache->y.assign(nodes.size(), 0.0F);
    const int count = static_cast<int>(nodes.size());
    std::vector<char> only = composer::LightSet(cache->spec.only, count, true);
    const std::vector<char> skip = composer::LightSet(cache->spec.skip, count, false);
    for (size_t n = 0; n < nodes.size(); ++n) {
        if (!nodes[n]->Coords.empty()) {
            const auto& c = nodes[n]->Coords[0];
            const auto [sx, sy] = composer::StagePoint(cache->spec, c.screenX, c.screenY, c.screenZ);
            cache->x[n] = static_cast<float>(sx);
            cache->y[n] = static_cast<float>(sy);
        }
        only[n] = only[n] && !skip[n];
    }
    cache->lit = std::move(only);
    return cache;
}

} // namespace

StageFieldEffect::StageFieldEffect(int id) :
    RenderableEffect(id, "Stage Field", shockwave_16, shockwave_24, shockwave_32, shockwave_48, shockwave_64) {
}

StageFieldEffect::~StageFieldEffect() = default;

void StageFieldEffect::Render(Effect* effect, const SettingsMap& settings, RenderBuffer& buffer) {
    (void)effect;
    StageFieldCache* cache = CacheFor(buffer, GetId(), settings.Get("TEXTCTRL_StageField_Spec", ""));
    if (!cache->ok) {
        return; // an unreadable field lights nothing rather than guessing
    }
    const double strength = std::clamp(settings.GetInt("SLIDER_StageField_Intensity", 100), 0, 100) / 100.0;
    // Gesture time: from this effect's start, plus how late it joined the gesture.
    const double t = (buffer.curPeriod - buffer.curEffStartPer) * static_cast<double>(buffer.frameTimeInMs) / 1000.0 + cache->spec.at;
    const auto& nodes = buffer.GetNodes();
    for (size_t n = 0; n < nodes.size(); ++n) {
        if (!cache->lit[n]) {
            continue;
        }
        const composer::FieldSample s = composer::EvaluateField(cache->spec, cache->x[n], cache->y[n], t);
        const double i = s.intensity * strength;
        if (i <= 1.0 / 255.0) {
            continue; // transparent: the layers below show
        }
        xlColor c;
        buffer.GetMultiColorBlend(s.position, false, c);
        if (buffer.allowAlpha) {
            c.alpha = static_cast<uint8_t>(std::lround(i * 255.0));
        } else {
            c.red = static_cast<uint8_t>(std::lround(c.red * i));
            c.green = static_cast<uint8_t>(std::lround(c.green * i));
            c.blue = static_cast<uint8_t>(std::lround(c.blue * i));
        }
        buffer.SetNodePixel(static_cast<int>(n), c);
    }
}
