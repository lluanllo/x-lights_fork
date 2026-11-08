#include "composer/ReferenceStudy.h"

#include "composer/CompositionPlan.h"
#include "composer/VisualDirection.h"
#include "composer/WriterTest.h"
#include "models/Model.h"
#include "models/ModelGroup.h"
#include "models/ModelScreenLocation.h"
#include "models/Node.h"
#include "render/Effect.h"
#include "render/EffectLayer.h"
#include "render/Element.h"
#include "render/HeadlessRenderContext.h"
#include "render/SequenceData.h"
#include "render/SequenceElements.h"
#include "utils/Color.h"
#include "utils/xlImage.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace composer {

namespace {

constexpr int BIN_MS = 100;

struct Placed {
    std::string element; // the row in the sequencer
    std::string kind;    // "model", "group", "submodel", "strand", "node"
    std::string base;    // the model or group the row belongs to
    int layer = 0;
    const Effect* effect = nullptr;
};

bool WriteFile(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
    out.close();
    return !out.fail();
}

double Percentile(std::vector<double> v, double p) {
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    const double at = p * (v.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(at));
    const size_t hi = std::min(v.size() - 1, lo + 1);
    return v[lo] + (v[hi] - v[lo]) * (at - lo);
}

double Round2(double v) {
    return std::round(v * 100.0) / 100.0;
}

bool IsValueCurve(const std::string& value) {
    return value.rfind("Active=TRUE", 0) == 0;
}

// Hue in 12 steps of 30 degrees, or "white" / "black" / "grey".
std::string HueName(const xlColor& c) {
    const int mx = std::max({ c.red, c.green, c.blue });
    const int mn = std::min({ c.red, c.green, c.blue });
    if (mx < 30) {
        return "black";
    }
    const double sat = mx == 0 ? 0 : (mx - mn) / static_cast<double>(mx);
    if (sat < 0.2) {
        return mx > 170 ? "white" : "grey";
    }
    double h = 0;
    const double d = mx - mn;
    if (mx == c.red) {
        h = std::fmod((c.green - c.blue) / d, 6.0);
    } else if (mx == c.green) {
        h = (c.blue - c.red) / d + 2;
    } else {
        h = (c.red - c.green) / d + 4;
    }
    h *= 60;
    if (h < 0) {
        h += 360;
    }
    static const char* const NAMES[] = { "red", "orange", "yellow", "lime", "green", "spring", "cyan", "azure", "blue", "violet", "magenta", "rose" };
    return NAMES[static_cast<int>(std::lround(h / 30.0)) % 12];
}

xlColor ParseHex(const std::string& s) {
    xlColor c(0, 0, 0);
    if (s.size() >= 7 && s[0] == '#') {
        c.red = static_cast<uint8_t>(std::strtol(s.substr(1, 2).c_str(), nullptr, 16));
        c.green = static_cast<uint8_t>(std::strtol(s.substr(3, 2).c_str(), nullptr, 16));
        c.blue = static_cast<uint8_t>(std::strtol(s.substr(5, 2).c_str(), nullptr, 16));
    }
    return c;
}

// The colours an effect actually uses: the palette buttons whose checkbox is on.
std::vector<std::string> ActiveColours(const Effect& e, int& curves) {
    std::vector<std::string> out;
    const SettingsMap& p = e.GetPaletteMap();
    for (int i = 1; i <= 8; ++i) {
        const std::string on = p.Get(fmt::format("C_CHECKBOX_Palette{}", i), "0");
        if (on != "1") {
            continue;
        }
        const std::string value = p.Get(fmt::format("C_BUTTON_Palette{}", i), "");
        if (IsValueCurve(value)) {
            ++curves;
            // A colour curve: its first point's colour stands for it.
            const size_t at = value.find('#');
            if (at != std::string::npos) {
                out.push_back(value.substr(at, 7));
            }
        } else if (!value.empty()) {
            out.push_back(value.substr(0, 7));
        }
    }
    return out;
}

void Collect(Element& element, const std::string& kind, const std::string& base, std::vector<Placed>& out) {
    for (size_t l = 0; l < element.GetEffectLayerCount(); ++l) {
        for (Effect* effect : element.GetEffectLayer(static_cast<int>(l))->GetEffects()) {
            if (!effect->IsRenderDisabled()) {
                out.push_back({ element.GetFullName(), kind, base, static_cast<int>(l), effect });
            }
        }
    }
}

// Every model a group lights, through groups inside groups and submodels.
void Expand(ModelManager& models, const std::string& name, std::set<std::string>& out, int depth = 0) {
    if (depth > 8) {
        return;
    }
    const std::string base = name.substr(0, name.find('/'));
    Model* m = models[base];
    if (m == nullptr) {
        return;
    }
    if (auto* g = dynamic_cast<ModelGroup*>(m)) {
        for (const std::string& member : g->ModelNames()) {
            Expand(models, member, out, depth + 1);
        }
        return;
    }
    out.insert(base);
}

struct NodePoint {
    const NodeBaseClass* node = nullptr;
    float x = 0;
    float y = 0;
    bool wired = true; // on a controller: a model with "No Controller" can never light
};

} // namespace

int RunReferenceStudy(const ReferenceStudyOptions& o) {
    std::string refused;
    if (!ControllersAreInactive(o.showDir, refused)) {
        printf("composer-reference-study: REFUSED: %s\n", refused.c_str());
        return 2;
    }
    std::error_code ec;
    const fs::path out(o.outDir);
    fs::create_directories(out / "frames", ec);
    const auto started = std::chrono::steady_clock::now();
    HeadlessRenderContext ctx;
    if (!ctx.LoadShowFolder(o.showDir) || !ctx.OpenSequence(o.sequencePath)) {
        printf("composer-reference-study: cannot open %s on %s\n", o.sequencePath.c_str(), o.showDir.c_str());
        return 1;
    }
    SequenceElements& elements = ctx.GetSequenceElements();
    const int frameMS = std::max(1, elements.GetFrameMS());
    const int durationMS = ctx.IsSequenceLoaded() ? static_cast<int>(ctx.GetSeqData().NumFrames() * ctx.GetSeqData().FrameTime()) : elements.GetSequenceEnd();
    printf("composer-reference-study: %s opened, %d ms at %d ms a frame\n", o.id.c_str(), durationMS, frameMS);

    // ---------------------------------------------------------------- layout
    json layout;
    std::map<std::string, int> nodesOf;
    std::map<std::string, std::string> typeOf;
    int models = 0;
    int groups = 0;
    int submodels = 0;
    long long totalNodes = 0;
    std::map<std::string, int> modelTypes;
    for (const auto& [name, model] : ctx.AllModels) {
        if (dynamic_cast<ModelGroup*>(model) != nullptr) {
            ++groups;
            typeOf[name] = "group";
            continue;
        }
        ++models;
        submodels += static_cast<int>(model->GetSubModels().size());
        nodesOf[name] = static_cast<int>(model->GetNodeCount());
        typeOf[name] = model->GetDisplayAsString();
        totalNodes += model->GetNodeCount();
        modelTypes[model->GetDisplayAsString()]++;
    }
    layout = { { "models", models }, { "groups", groups }, { "submodels", submodels }, { "nodes", totalNodes }, { "modelTypes", modelTypes } };

    // --------------------------------------------------------------- effects
    std::vector<Placed> placed;
    json timing = json::array();
    for (size_t i = 0; i < elements.GetElementCount(); ++i) {
        Element* element = elements.GetElement(i);
        if (element->GetType() == ElementType::ELEMENT_TYPE_TIMING) {
            json layers = json::array();
            for (size_t l = 0; l < element->GetEffectLayerCount(); ++l) {
                layers.push_back(element->GetEffectLayer(static_cast<int>(l))->GetEffectCount());
            }
            timing.push_back({ { "name", element->GetName() }, { "marksPerLayer", layers } });
            continue;
        }
        const std::string name = element->GetName();
        Collect(*element, typeOf[name] == "group" ? "group" : "model", name, placed);
        if (auto* me = dynamic_cast<ModelElement*>(element)) {
            for (int s = 0; s < me->GetSubModelCount(); ++s) {
                SubModelElement* sub = me->GetSubModel(s);
                if (sub->GetType() == ElementType::ELEMENT_TYPE_SUBMODEL) {
                    Collect(*sub, "submodel", name, placed);
                } else if (auto* strand = dynamic_cast<StrandElement*>(sub)) {
                    Collect(*strand, "strand", name, placed);
                    for (int n = 0; n < strand->GetNodeLayerCount(); ++n) {
                        NodeLayer* nl = strand->GetNodeLayer(n);
                        for (Effect* e : nl->GetEffects()) {
                            if (!e->IsRenderDisabled()) {
                                placed.push_back({ strand->GetFullName() + "/node", "node", name, 0, e });
                            }
                        }
                    }
                }
            }
        }
    }

    std::map<std::string, int> byKind;
    std::map<std::string, std::vector<double>> durations;
    std::map<std::string, std::map<std::string, int>> kindsByEffect;
    std::map<std::string, std::map<std::string, std::map<std::string, int>>> choices; // effect -> key -> value -> n
    std::map<std::string, std::map<std::string, std::vector<double>>> numbers;        // effect -> key -> values
    std::map<std::string, std::map<std::string, int>> curvesByEffect;
    std::map<std::string, int> blends, inTransitions, outTransitions, bufferStyles, transforms, hues, colours;
    std::map<std::string, double> hueTime;
    std::map<std::string, int> elementEffects;
    std::map<std::string, std::vector<std::pair<int, int>>> busy;
    std::map<int, int> layerUse;
    int withCurves = 0;
    int colourCurves = 0;
    int withFades = 0;
    int canvas = 0;
    int perPreview = 0;
    std::vector<int> colourCounts;
    std::vector<int> starts;
    std::map<int, std::set<std::string>> startsByFrame;

    for (const Placed& p : placed) {
        const Effect& e = *p.effect;
        const std::string name = e.GetEffectName();
        const int s = e.GetStartTimeMS();
        const int t = e.GetEndTimeMS();
        byKind[p.kind]++;
        durations[name].push_back(t - s);
        kindsByEffect[name][p.kind]++;
        elementEffects[p.element]++;
        busy[p.element].push_back({ s, t });
        layerUse[p.layer]++;
        starts.push_back(s);
        startsByFrame[s / frameMS].insert(p.base);
        bool curve = false;
        const SettingsMap& settings = e.GetSettings();
        for (const auto& [k, v] : settings) {
            const std::string key(k);
            const std::string value(v);
            if (IsValueCurve(value)) {
                curve = true;
                curvesByEffect[name][key]++;
                continue;
            }
            if (key.rfind("E_SLIDER_", 0) == 0 || key.rfind("E_TEXTCTRL_", 0) == 0 || key.rfind("E_SPINCTRL_", 0) == 0) {
                char* end = nullptr;
                const double d = std::strtod(value.c_str(), &end);
                if (end != value.c_str() && *end == '\0') {
                    numbers[name][key].push_back(d);
                    continue;
                }
            }
            if (key.rfind("E_CHOICE_", 0) == 0 || key.rfind("E_CHECKBOX_", 0) == 0 || key.rfind("E_NOTEBOOK", 0) == 0) {
                choices[name][key][value.substr(0, 60)]++;
            }
        }
        withCurves += curve ? 1 : 0;
        blends[settings.Get("T_CHOICE_LayerMethod", "Normal")]++;
        const std::string in = settings.Get("T_CHOICE_In_Transition_Type", "");
        const std::string outT = settings.Get("T_CHOICE_Out_Transition_Type", "");
        const double fadeIn = std::strtod(settings.Get("T_TEXTCTRL_Fadein", "0").c_str(), nullptr);
        const double fadeOut = std::strtod(settings.Get("T_TEXTCTRL_Fadeout", "0").c_str(), nullptr);
        if (fadeIn > 0 || fadeOut > 0) {
            ++withFades;
            inTransitions[fadeIn > 0 ? (in.empty() ? "Fade" : in) : "none"]++;
            outTransitions[fadeOut > 0 ? (outT.empty() ? "Fade" : outT) : "none"]++;
        }
        bufferStyles[settings.Get("B_CHOICE_BufferStyle", "Default")]++;
        transforms[settings.Get("B_CHOICE_BufferTransform", "None")]++;
        canvas += settings.Get("T_CHECKBOX_Canvas", "0") == "1" ? 1 : 0;
        perPreview += settings.Get("B_CHOICE_BufferStyle", "").find("Per Preview") != std::string::npos ? 1 : 0;
        const std::vector<std::string> active = ActiveColours(e, colourCurves);
        colourCounts.push_back(static_cast<int>(active.size()));
        for (const std::string& c : active) {
            colours[c]++;
            const std::string h = HueName(ParseHex(c));
            hues[h]++;
            hueTime[h] += (t - s) / 1000.0 / std::max<size_t>(1, active.size());
        }
    }

    // ------------------------------------------------- coverage and togetherness
    const int bins = std::max(1, durationMS / BIN_MS + 1);
    std::vector<std::set<std::string>> litModels(bins);
    std::vector<int> activeEffects(bins, 0);
    std::map<std::string, std::set<std::string>> expansion;
    for (const Placed& p : placed) {
        auto found = expansion.find(p.base);
        if (found == expansion.end()) {
            std::set<std::string> set;
            Expand(ctx.AllModels, p.base, set);
            found = expansion.emplace(p.base, std::move(set)).first;
        }
        const int a = std::clamp(p.effect->GetStartTimeMS() / BIN_MS, 0, bins - 1);
        const int b = std::clamp((p.effect->GetEndTimeMS() - 1) / BIN_MS, 0, bins - 1);
        for (int i = a; i <= b; ++i) {
            activeEffects[i]++;
            if (p.kind == "model" || p.kind == "group") {
                litModels[i].insert(found->second.begin(), found->second.end());
            }
        }
    }
    std::vector<double> coverage(bins, 0);
    for (int i = 0; i < bins; ++i) {
        long long n = 0;
        for (const std::string& m : litModels[i]) {
            n += nodesOf[m];
        }
        coverage[i] = totalNodes > 0 ? static_cast<double>(n) / totalNodes : 0;
    }
    // Starts shared by many figures at once are the show's cuts.
    int together = 0;
    std::vector<int> cuts;
    for (const auto& [frame, bases] : startsByFrame) {
        std::set<std::string> lit;
        for (const std::string& b : bases) {
            lit.insert(expansion[b].begin(), expansion[b].end());
        }
        long long n = 0;
        for (const std::string& m : lit) {
            n += nodesOf[m];
        }
        if (bases.size() >= 3) {
            together += static_cast<int>(bases.size());
        }
        if (totalNodes > 0 && n * 4 >= totalNodes && bases.size() >= 3) {
            cuts.push_back(frame * frameMS);
        }
    }
    // Scenes between cuts at least 2 s apart.
    std::vector<int> scenes = { 0 };
    for (int c : cuts) {
        if (c - scenes.back() >= 2000) {
            scenes.push_back(c);
        }
    }

    // ---------------------------------------------------------------- render
    json render = { { "ran", false } };
    std::vector<double> litShare;
    std::vector<double> brightness;
    std::vector<double> litWired; // over the nodes that are on a controller
    std::vector<NodePoint> points;
    float minX = 1e9F, maxX = -1e9F, minY = 1e9F, maxY = -1e9F;
    if (o.render) {
        const auto r0 = std::chrono::steady_clock::now();
        const bool ok = ctx.RenderAndWait(0);
        const long long renderMS = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - r0).count();
        render = { { "ran", ok }, { "renderMS", renderMS } };
        printf("composer-reference-study: render %s in %lld ms\n", ok ? "finished" : "FAILED", renderMS);
        if (ok) {
            const SequenceData& data = ctx.GetSeqData();
            for (const auto& [name, model] : ctx.AllModels) {
                if (dynamic_cast<ModelGroup*>(model) != nullptr || model->GetLayoutGroup() != "Default") {
                    continue;
                }
                const bool wired = model->GetControllerName() != "No Controller";
                for (uint32_t n = 0; n < model->GetNodeCount(); ++n) {
                    std::vector<std::tuple<float, float, float>> pts;
                    model->GetNode3DScreenCoords(static_cast<int>(n), pts);
                    const NodeBaseClass* node = model->GetNode(n);
                    if (node->ActChan + node->GetChanCount() > data.NumChannels()) {
                        continue;
                    }
                    for (auto [x, y, z] : pts) {
                        model->GetModelScreenLocation().TranslatePoint(x, y, z);
                        points.push_back({ node, x, y, wired });
                    }
                }
            }
            // Helper models parked far away (a moving head's channels, say) would
            // shrink the house to a corner: the picture frames where nearly all nodes are.
            if (!points.empty()) {
                std::vector<double> xs;
                std::vector<double> ys;
                for (const NodePoint& p : points) {
                    xs.push_back(p.x);
                    ys.push_back(p.y);
                }
                const double x0 = Percentile(xs, 0.01), x1 = Percentile(xs, 0.99);
                const double y0 = Percentile(ys, 0.01), y1 = Percentile(ys, 0.99);
                const double mx = (x1 - x0) * 0.04 + 1, my = (y1 - y0) * 0.04 + 1;
                minX = static_cast<float>(x0 - mx);
                maxX = static_cast<float>(x1 + mx);
                minY = static_cast<float>(y0 - my);
                maxY = static_cast<float>(y1 + my);
                const size_t before = points.size();
                points.erase(std::remove_if(points.begin(), points.end(), [&](const NodePoint& p) { return p.x < minX || p.x > maxX || p.y < minY || p.y > maxY; }), points.end());
                render["nodesOutsidePicture"] = before - points.size();
            }
            // Every node of every model, on or off, once per bin: what the render lights.
            std::set<const NodeBaseClass*> unique;
            std::set<const NodeBaseClass*> wiredNodes;
            for (const NodePoint& p : points) {
                unique.insert(p.node);
                if (p.wired) {
                    wiredNodes.insert(p.node);
                }
            }
            for (int ms = 0; ms < durationMS; ms += BIN_MS) {
                const unsigned frame = std::min<unsigned>(data.NumFrames() - 1, static_cast<unsigned>(ms / frameMS));
                long long lit = 0;
                double light = 0;
                for (const NodeBaseClass* node : unique) {
                    const unsigned char* c = data[frame][node->ActChan];
                    const int chans = node->GetChanCount();
                    int mx = 0;
                    for (int k = 0; k < chans && k < 3; ++k) {
                        mx = std::max<int>(mx, c[k]);
                    }
                    lit += mx > 20 ? 1 : 0;
                    light += mx / 255.0;
                }
                litShare.push_back(unique.empty() ? 0 : static_cast<double>(lit) / unique.size());
                brightness.push_back(unique.empty() ? 0 : light / unique.size());
                litWired.push_back(wiredNodes.empty() ? 0 : static_cast<double>(lit) / wiredNodes.size());
            }
            render["nodesDrawn"] = unique.size();
            render["nodesWired"] = wiredNodes.size();
        }
    }

    // --------------------------------------------------------------- pictures
    std::vector<int> times = o.timesMS;
    if (times.empty() && durationMS > 0) {
        // Half way into each of the longest scenes, plus the brightest moments.
        std::vector<std::pair<int, int>> spans;
        for (size_t i = 0; i < scenes.size(); ++i) {
            const int end = i + 1 < scenes.size() ? scenes[i + 1] : durationMS;
            spans.push_back({ end - scenes[i], scenes[i] + (end - scenes[i]) / 2 });
        }
        std::sort(spans.rbegin(), spans.rend());
        for (size_t i = 0; i < spans.size() && times.size() < 12; ++i) {
            times.push_back(spans[i].second);
        }
        std::vector<std::pair<double, int>> peaks;
        for (size_t i = 0; i < litShare.size(); ++i) {
            peaks.push_back({ brightness[i], static_cast<int>(i) * BIN_MS });
        }
        std::sort(peaks.rbegin(), peaks.rend());
        for (const auto& [b, ms] : peaks) {
            if (times.size() >= 16) {
                break;
            }
            if (std::all_of(times.begin(), times.end(), [ms = ms](int t) { return std::abs(t - ms) > 5000; })) {
                times.push_back(ms);
            }
        }
        for (int k = 0; times.size() < 16 && k < 16; ++k) {
            times.push_back(durationMS * (2 * k + 1) / 32);
        }
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());
    }
    json pictures = json::array();
    std::vector<std::pair<int, xlImage>> thumbs;
    if (!points.empty() && render.value("ran", false)) {
        const SequenceData& data = ctx.GetSeqData();
        const float spanX = std::max(1.0F, maxX - minX);
        const float spanY = std::max(1.0F, maxY - minY);
        const int width = std::max(200, o.imageWidth);
        const int height = std::max(100, static_cast<int>(width * spanY / spanX));
        const int margin = 12;
        for (int ms : times) {
            const unsigned frame = std::min<unsigned>(data.NumFrames() - 1, static_cast<unsigned>(ms / frameMS));
            xlImage image(width + 2 * margin, height + 2 * margin);
            for (int y = 0; y < image.GetHeight(); ++y) {
                for (int x = 0; x < image.GetWidth(); ++x) {
                    image.SetRGBA(x, y, 8, 8, 12, 255);
                }
            }
            // Off nodes first, faint, so the house shows; lit ones over them.
            for (int pass = 0; pass < 2; ++pass) {
                for (const NodePoint& p : points) {
                    NodeBaseClass* node = const_cast<NodeBaseClass*>(p.node);
                    node->SetFromChannels(data[frame][node->ActChan]);
                    xlColor c;
                    node->GetColor(c);
                    const bool on = std::max({ c.red, c.green, c.blue }) > 20;
                    if (on != (pass == 1)) {
                        continue;
                    }
                    const int cx = margin + static_cast<int>((p.x - minX) / spanX * (width - 1));
                    const int cy = margin + height - 1 - static_cast<int>((p.y - minY) / spanY * (height - 1));
                    const int r = on ? 2 : 1;
                    for (int dy = -r; dy <= r; ++dy) {
                        for (int dx = -r; dx <= r; ++dx) {
                            if (dx * dx + dy * dy > r * r + 1) {
                                continue;
                            }
                            const int x = cx + dx;
                            const int y = cy + dy;
                            if (x >= 0 && y >= 0 && x < image.GetWidth() && y < image.GetHeight()) {
                                if (on) {
                                    image.SetRGBA(x, y, c.red, c.green, c.blue, 255);
                                } else {
                                    image.SetRGBA(x, y, 38, 38, 44, 255);
                                }
                            }
                        }
                    }
                }
            }
            std::vector<uint8_t> png;
            const fs::path file = out / "frames" / fmt::format("house_{:06d}ms.png", ms);
            if (image.SaveAsPNG(png) && WriteFile(file, std::string(png.begin(), png.end()))) {
                pictures.push_back({ { "timeMS", ms }, { "file", file.filename().string() } });
            }
            // A quarter-size copy for the sheet.
            const int tw = image.GetWidth() / 3;
            const int th = image.GetHeight() / 3;
            xlImage thumb(tw, th);
            for (int y = 0; y < th; ++y) {
                for (int x = 0; x < tw; ++x) {
                    int r = 0, g = 0, b = 0;
                    for (int k = 0; k < 9; ++k) {
                        const int sx = x * 3 + k % 3;
                        const int sy = y * 3 + k / 3;
                        r = std::max<int>(r, image.GetRed(sx, sy));
                        g = std::max<int>(g, image.GetGreen(sx, sy));
                        b = std::max<int>(b, image.GetBlue(sx, sy));
                    }
                    thumb.SetRGBA(x, y, r, g, b, 255);
                }
            }
            thumbs.push_back({ ms, std::move(thumb) });
        }
        if (!thumbs.empty()) {
            const int columns = 4;
            const int rows = (static_cast<int>(thumbs.size()) + columns - 1) / columns;
            const int tw = thumbs[0].second.GetWidth();
            const int th = thumbs[0].second.GetHeight();
            xlImage sheet(columns * (tw + 4) + 4, rows * (th + 4) + 4);
            for (int y = 0; y < sheet.GetHeight(); ++y) {
                for (int x = 0; x < sheet.GetWidth(); ++x) {
                    sheet.SetRGBA(x, y, 70, 70, 70, 255);
                }
            }
            for (size_t i = 0; i < thumbs.size(); ++i) {
                const int ox = 4 + static_cast<int>(i % columns) * (tw + 4);
                const int oy = 4 + static_cast<int>(i / columns) * (th + 4);
                for (int y = 0; y < th; ++y) {
                    for (int x = 0; x < tw; ++x) {
                        const xlImage& t = thumbs[i].second;
                        sheet.SetRGBA(ox + x, oy + y, t.GetRed(x, y), t.GetGreen(x, y), t.GetBlue(x, y), 255);
                    }
                }
            }
            std::vector<uint8_t> png;
            if (sheet.SaveAsPNG(png)) {
                (void)WriteFile(out / "contact_sheet.png", std::string(png.begin(), png.end()));
            }
        }
    }

    // ---------------------------------------------------------------- report
    json effects = json::object();
    for (const auto& [name, d] : durations) {
        double total = 0;
        for (double v : d) {
            total += v;
        }
        json settings = json::object();
        for (const auto& [key, values] : choices[name]) {
            std::vector<std::pair<int, std::string>> sorted;
            for (const auto& [v, n] : values) {
                sorted.push_back({ n, v });
            }
            std::sort(sorted.rbegin(), sorted.rend());
            json top = json::array();
            for (size_t i = 0; i < sorted.size() && i < 6; ++i) {
                top.push_back({ sorted[i].second, sorted[i].first });
            }
            settings[key] = top;
        }
        for (const auto& [key, values] : numbers[name]) {
            settings[key] = { { "n", values.size() }, { "p10", Round2(Percentile(values, 0.1)) }, { "median", Round2(Percentile(values, 0.5)) }, { "p90", Round2(Percentile(values, 0.9)) } };
        }
        effects[name] = { { "count", d.size() },
                          { "seconds", Round2(total / 1000.0) },
                          { "medianMS", Percentile(d, 0.5) },
                          { "p10MS", Percentile(d, 0.1) },
                          { "p90MS", Percentile(d, 0.9) },
                          { "on", kindsByEffect[name] },
                          { "valueCurves", curvesByEffect[name] },
                          { "settings", settings } };
    }
    std::vector<std::pair<int, std::string>> topElements;
    for (const auto& [n, c] : elementEffects) {
        topElements.push_back({ c, n });
    }
    std::sort(topElements.rbegin(), topElements.rend());
    json elementsJson = json::array();
    for (size_t i = 0; i < topElements.size(); ++i) {
        const std::string& name = topElements[i].second;
        auto spans = busy[name];
        std::sort(spans.begin(), spans.end());
        long long covered = 0;
        int until = -1;
        for (const auto& [a, b] : spans) {
            if (b <= until) {
                continue;
            }
            covered += b - std::max(a, until);
            until = b;
        }
        const std::string base = name.substr(0, name.find('/'));
        elementsJson.push_back({ { "name", name }, { "type", typeOf.count(base) ? typeOf[base] : "?" }, { "effects", topElements[i].first }, { "busyShare", Round2(durationMS > 0 ? covered / static_cast<double>(durationMS) : 0) } });
    }
    std::vector<std::pair<int, std::string>> topColours;
    for (const auto& [c, n] : colours) {
        topColours.push_back({ n, c });
    }
    std::sort(topColours.rbegin(), topColours.rend());
    json coloursJson = json::array();
    for (size_t i = 0; i < topColours.size() && i < 24; ++i) {
        coloursJson.push_back({ topColours[i].second, topColours[i].first });
    }
    std::vector<double> sortedStarts;
    for (int s : starts) {
        sortedStarts.push_back(s);
    }
    std::vector<double> colourCountValues(colourCounts.begin(), colourCounts.end());
    std::vector<double> coverageValues = coverage;
    std::vector<double> activeValues(activeEffects.begin(), activeEffects.end());
    json scenesJson = json::array();
    for (size_t i = 0; i < scenes.size(); ++i) {
        const int end = i + 1 < scenes.size() ? scenes[i + 1] : durationMS;
        std::map<std::string, double> mix;
        std::map<std::string, double> sceneHues;
        std::map<std::string, double> sceneColours;
        for (const Placed& p : placed) {
            const int s = std::max(p.effect->GetStartTimeMS(), scenes[i]);
            const int t = std::min(p.effect->GetEndTimeMS(), end);
            if (t <= s) {
                continue;
            }
            if (p.kind == "model" || p.kind == "group") {
                mix[p.effect->GetEffectName()] += (t - s) / 1000.0;
            }
            int ignored = 0;
            const std::vector<std::string> active = ActiveColours(*p.effect, ignored);
            for (const std::string& c : active) {
                const double w = (t - s) / 1000.0 / active.size();
                sceneHues[HueName(ParseHex(c))] += w;
                sceneColours[c] += w;
            }
        }
        const auto top = [](const std::map<std::string, double>& m, size_t n) {
            std::vector<std::pair<double, std::string>> v;
            double total = 0;
            for (const auto& [k, w] : m) {
                v.push_back({ w, k });
                total += w;
            }
            std::sort(v.rbegin(), v.rend());
            json out = json::array();
            for (size_t k = 0; k < v.size() && k < n; ++k) {
                out.push_back({ v[k].second, Round2(total > 0 ? v[k].first / total : 0) });
            }
            return out;
        };
        std::vector<std::pair<double, std::string>> ranked;
        for (const auto& [n, sec] : mix) {
            ranked.push_back({ sec, n });
        }
        std::sort(ranked.rbegin(), ranked.rend());
        json ranking = json::array();
        for (size_t k = 0; k < ranked.size() && k < 4; ++k) {
            ranking.push_back(ranked[k].second);
        }
        double lit = 0;
        int count = 0;
        for (int b = scenes[i] / BIN_MS; b < end / BIN_MS && b < static_cast<int>(litShare.size()); ++b) {
            lit += litShare[b];
            ++count;
        }
        scenesJson.push_back({ { "startMS", scenes[i] }, { "endMS", end }, { "effects", ranking }, { "hues", top(sceneHues, 4) }, { "colours", top(sceneColours, 5) }, { "litShare", Round2(count > 0 ? lit / count : 0) } });
    }

    const json root = {
        { "kind", "composer-reference-study" },
        { "id", o.id },
        { "measured", true },
        { "sequence", fs::path(o.sequencePath).filename().string() },
        { "durationMS", durationMS },
        { "frameMS", frameMS },
        { "mediaFound", ctx.GetCurrentMediaManager() != nullptr },
        { "layout", layout },
        { "timingTracks", timing },
        { "effectRows", byKind },
        { "effectCount", placed.size() },
        { "effectsPerMinute", Round2(durationMS > 0 ? placed.size() * 60000.0 / durationMS : 0) },
        { "layers", layerUse },
        { "effects", effects },
        { "elements", elementsJson },
        { "blendModes", blends },
        { "fades", { { "effectsWithFades", withFades }, { "in", inTransitions }, { "out", outTransitions } } },
        { "bufferStyles", bufferStyles },
        { "bufferTransforms", transforms },
        { "canvasEffects", canvas },
        { "perPreviewEffects", perPreview },
        { "effectsWithValueCurves", withCurves },
        { "colourCurves", colourCurves },
        { "colours", coloursJson },
        { "hues", hues },
        { "hueSeconds", hueTime },
        { "coloursPerEffect", { { "median", Percentile(colourCountValues, 0.5) }, { "p90", Percentile(colourCountValues, 0.9) } } },
        { "coverage", { { "median", Round2(Percentile(coverageValues, 0.5)) }, { "p10", Round2(Percentile(coverageValues, 0.1)) }, { "p90", Round2(Percentile(coverageValues, 0.9)) } } },
        { "activeEffects", { { "median", Percentile(activeValues, 0.5) }, { "p90", Percentile(activeValues, 0.9) } } },
        { "startsTogether", Round2(placed.empty() ? 0 : together / static_cast<double>(placed.size())) },
        { "cuts", cuts.size() },
        { "scenes", scenesJson },
        { "render", render },
        { "litShare", { { "median", Round2(Percentile(litShare, 0.5)) }, { "p10", Round2(Percentile(litShare, 0.1)) }, { "p90", Round2(Percentile(litShare, 0.9)) } } },
        { "litShareWired", { { "median", Round2(Percentile(litWired, 0.5)) }, { "p10", Round2(Percentile(litWired, 0.1)) }, { "p90", Round2(Percentile(litWired, 0.9)) } } },
        { "pictures", pictures },
        { "visuallyReviewed", false },
    };
    (void)WriteFile(out / "stats.json", root.dump(2));
    std::string csv = "ms,activeEffects,coverage,litShare,brightness\n";
    for (int i = 0; i < bins; ++i) {
        csv += fmt::format("{},{},{:.3f},{:.3f},{:.3f}\n", i * BIN_MS, activeEffects[i], coverage[i], i < static_cast<int>(litShare.size()) ? litShare[i] : 0.0,
                           i < static_cast<int>(brightness.size()) ? brightness[i] : 0.0);
    }
    (void)WriteFile(out / "timeline.csv", csv);
    const long long totalMS = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    printf("composer-reference-study: %s: %zu effects (%s), %zu scenes, %zu pictures, %lld ms\n", o.id.c_str(), placed.size(), json(byKind).dump().c_str(), scenes.size(), pictures.size(), totalMS);
    (void)ctx.CloseSequence();
    return 0;
}

bool BuildReferenceProfile(const std::vector<std::string>& studyDirs, const std::string& id, const std::string& name, const ThemeCatalog* themes, std::string& profileJson, std::string& error) {
    struct Measured {
        std::string id;
        double coverageP10 = 0, coverageP50 = 0;
        double hitsPerMinute = 0;
        double hitMS = 0;
        double fades = 0;
        double colours = 0;
        double sceneMS = 0;
        double contrast = 0;
        std::map<std::string, double> hues;
    };
    std::vector<Measured> all;
    for (const std::string& dir : studyDirs) {
        std::ifstream in(fs::path(dir) / "stats.json", std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        const json d = json::parse(text.str(), nullptr, false);
        if (d.is_discarded() || d.value("kind", "") != "composer-reference-study") {
            error = "not a reference study: " + dir;
            return false;
        }
        Measured m;
        m.id = d.value("id", fs::path(dir).filename().string());
        const double minutes = std::max(0.1, d.value("durationMS", 0) / 60000.0);
        m.coverageP10 = d["coverage"].value("p10", 0.0);
        m.coverageP50 = d["coverage"].value("median", 0.0);
        // The short effects that make the rhythm: those whose typical length is under 0.6 s.
        double hits = 0;
        double hitTime = 0;
        for (const auto& [effect, e] : d["effects"].items()) {
            const double median = e.value("medianMS", 0.0);
            const int count = e.value("count", 0);
            if (median > 0 && median <= 600 && count >= 10) {
                hits += count;
                hitTime += median * count;
            }
        }
        m.hitsPerMinute = hits / minutes;
        m.hitMS = hits > 0 ? hitTime / hits : 350;
        const int effects = std::max(1, d.value("effectCount", 1));
        m.fades = d["fades"].value("effectsWithFades", 0) / static_cast<double>(effects);
        m.colours = d["coloursPerEffect"].value("median", 2.0);
        std::vector<double> lengths;
        std::map<std::string, int> dominant;
        for (const json& sc : d["scenes"]) {
            lengths.push_back(sc.value("endMS", 0) - sc.value("startMS", 0));
            if (sc.contains("hues") && !sc["hues"].empty()) {
                dominant[sc["hues"][0][0].get<std::string>()]++;
            }
        }
        m.sceneMS = Percentile(lengths, 0.5);
        int top = 0;
        int scenes = 0;
        for (const auto& [h, n] : dominant) {
            top = std::max(top, n);
            scenes += n;
        }
        m.contrast = scenes > 0 ? 1.0 - static_cast<double>(top) / scenes : 0;
        for (const auto& [h, sec] : d["hueSeconds"].items()) {
            m.hues[h] += sec.get<double>();
        }
        all.push_back(std::move(m));
    }
    if (all.empty()) {
        error = "no study given";
        return false;
    }
    const auto mean = [&](auto field) {
        double sum = 0;
        for (const Measured& m : all) {
            sum += field(m);
        }
        return sum / all.size();
    };
    StyleProfile p = NeutralStarterProfile();
    p.id = id;
    p.name = name;
    p.learned = false;
    EnsembleSettings& e = p.ensemble;
    e.enabled = true;
    const double p10 = mean([](const Measured& m) { return m.coverageP10; });
    const double p50 = mean([](const Measured& m) { return m.coverageP50; });
    // The references keep almost the whole display under an effect; calm scenes are
    // lighter in what is lit, which the ensemble does with outlines and soft motifs.
    e.coverageLow = Round2(std::clamp(p10 * 0.7, 0.3, 0.8));
    e.coverageMid = Round2(std::clamp(p50 * 0.9, 0.5, 0.95));
    e.coverageHigh = Round2(std::clamp(p50, 0.6, 1.0));
    e.coverageClimax = 1.0;
    // Hits measured over the whole song happen in its busy parts. Shows differ by a
    // factor of a hundred here, so several shows give their median, not their mean.
    std::vector<double> rates;
    for (const Measured& m : all) {
        rates.push_back(m.hitsPerMinute);
    }
    const double hits = Percentile(rates, 0.5);
    e.hitsPerMinuteMid = Round2(std::clamp(hits * 0.4, 0.0, 600.0));
    e.hitsPerMinuteHigh = Round2(std::clamp(hits * 1.25, 0.0, 600.0));
    e.hitsPerMinuteClimax = Round2(std::clamp(hits * 1.6, 0.0, 600.0));
    e.hitMS = static_cast<int>(std::lround(std::clamp(mean([](const Measured& m) { return m.hitMS; }), 120.0, 900.0) / 25.0)) * 25;
    e.fadeOutShare = Round2(mean([](const Measured& m) { return m.fades; }));
    e.colorsPerEvent = static_cast<int>(std::clamp(std::lround(mean([](const Measured& m) { return m.colours; })), 1L, 3L));
    e.contrastShare = Round2(std::clamp(mean([](const Measured& m) { return m.contrast; }), 0.0, 0.6));
    const double scene = mean([](const Measured& m) { return m.sceneMS; });
    p.minSegmentMS = static_cast<int>(std::clamp(scene * 0.5, 2000.0, 4000.0));
    p.maxSegmentMS = std::max(p.minSegmentMS * 2, static_cast<int>(std::clamp(scene * 1.5, 8000.0, 16000.0)));
    p.maxChangesPerMinute = Round2(std::clamp(60000.0 / std::max(1000.0, scene) * 1.2, 4.0, 20.0));
    for (const Measured& m : all) {
        e.sources.push_back(m.id);
    }
    // The busiest rows of every reference are the matrix and the mega tree, with the
    // faces singing the whole song: those are what the plan picks. Everything else
    // moves as a family in the ensemble, so the plan no longer reaches for groups or
    // a prop's submodel groups, which would leave the rest of that prop dark.
    p.leadPreference = { { Role::Matrix, 1.0 }, { Role::MegaTree, 1.0 }, { Role::Face, 0.8 }, { Role::Star, 0.3 }, { Role::SmallTree, 0.3 }, { Role::Window, 0.25 },
                         { Role::Arch, 0.25 }, { Role::Line, 0.2 }, { Role::Figure, 0.2 }, { Role::Prop, 0.2 }, { Role::Group, 0.1 } };
    p.supportPreference = { { Role::Matrix, 0.6 }, { Role::MegaTree, 0.6 }, { Role::Face, 0.5 }, { Role::Star, 0.2 }, { Role::SmallTree, 0.2 }, { Role::Window, 0.2 },
                            { Role::Arch, 0.2 }, { Role::Line, 0.15 }, { Role::Figure, 0.15 }, { Role::Prop, 0.15 }, { Role::Group, 0.1 } };
    p.maxParticipants = 6;
    // The theme whose home colours are nearest the hues these shows spend most time in.
    std::map<std::string, double> hues;
    for (const Measured& m : all) {
        double total = 0;
        for (const auto& [h, sec] : m.hues) {
            total += sec;
        }
        for (const auto& [h, sec] : m.hues) {
            hues[h] += total > 0 ? sec / total : 0;
        }
    }
    e.defaultTheme = "christmas-classic";
    if (themes != nullptr) {
        double best = -1;
        for (const ThemeDef& t : themes->themes) {
            std::set<std::string> names;
            for (const auto& palette : t.home) {
                for (const std::string& c : palette) {
                    names.insert(HueName(ParseHex(c)));
                }
            }
            // The time the shows spend in the theme's hues, less a little for each hue
            // of the theme they hardly use.
            double score = 0;
            for (const std::string& n : names) {
                const double share = hues.contains(n) ? hues[n] / all.size() : 0;
                score += share;
                score -= share < 0.02 ? 0.05 : 0;
            }
            if (score > best) {
                best = score;
                e.defaultTheme = t.id;
            }
        }
    }
    json root = json::parse(ToJson(p));
    root["note"] = "Measured from reference shows by --composer-reference-profile: coverage from their share of the display under an effect, hits from their short effects, scenes from their cuts, colours from their palettes. A style to start from, not anyone's approval.";
    root["measuredFrom"] = e.sources;
    profileJson = root.dump(1);
    return true;
}

} // namespace composer
