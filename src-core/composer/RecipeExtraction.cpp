#include "composer/RecipeLibrary.h"

#include "composer/SongAnalysis.h"
#include "composer/VisualDirection.h"
#include "composer/WriterTest.h"
#include "render/Effect.h"
#include "render/EffectLayer.h"
#include "render/Element.h"
#include "render/HeadlessRenderContext.h"
#include "render/SequenceElements.h"
#include "utils/Color.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <numeric>
#include <set>

namespace fs = std::filesystem;

namespace composer {

namespace {

constexpr int MIN_SCENE_MS = 2000;
constexpr int MAX_SCENE_MS = 16000;
constexpr int HIT_MAX_MS = 700;
constexpr size_t MAX_SURFACES = 5;

// Effects that show a file, a text, a face or a fixture: what they show belongs to that
// show, and the media and lyrics stages make those choices for a new song.
const std::set<std::string> NOT_RECIPES = { "Faces", "Text", "Pictures", "Video", "Shader", "State", "Lyrics", "Off", "Guitar", "Piano", "Servo", "DMX", "Moving Head",
                                            "Glediator", "Duplicate", "Sketch", "Tendril", "Liquid", "Music", "Ripple Image" };

struct Placed {
    std::string row;
    std::string family;
    bool group = false;
    std::string type; // "model", "group", "submodel"
    int layer = 0;
    int bufferWi = 0;
    int bufferHt = 0;
    const Effect* effect = nullptr;
};

double Median(std::vector<double> v) {
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

uint64_t Fnv(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string HueOf(const std::string& hex) {
    if (hex.size() < 7 || hex[0] != '#') {
        return "";
    }
    const int r = static_cast<int>(std::strtol(hex.substr(1, 2).c_str(), nullptr, 16));
    const int g = static_cast<int>(std::strtol(hex.substr(3, 2).c_str(), nullptr, 16));
    const int b = static_cast<int>(std::strtol(hex.substr(5, 2).c_str(), nullptr, 16));
    const int mx = std::max({ r, g, b });
    const int mn = std::min({ r, g, b });
    if (mx < 30) {
        return "black";
    }
    if (mx - mn < 0.2 * mx) {
        return mx > 170 ? "white" : "grey";
    }
    double h = 0;
    const double d = mx - mn;
    if (mx == r) {
        h = std::fmod((g - b) / d, 6.0);
    } else if (mx == g) {
        h = (b - r) / d + 2;
    } else {
        h = (r - g) / d + 4;
    }
    h *= 60;
    if (h < 0) {
        h += 360;
    }
    static const char* const NAMES[] = { "red", "orange", "yellow", "lime", "green", "spring", "cyan", "azure", "blue", "violet", "magenta", "rose" };
    return NAMES[static_cast<int>(std::lround(h / 30.0)) % 12];
}

std::vector<std::string> Colours(const Effect& e) {
    std::vector<std::string> out;
    const SettingsMap& p = e.GetPaletteMap();
    for (int i = 1; i <= 8; ++i) {
        if (p.Get(fmt::format("C_CHECKBOX_Palette{}", i), "0") != "1") {
            continue;
        }
        const std::string value = p.Get(fmt::format("C_BUTTON_Palette{}", i), "");
        const size_t at = value.find('#');
        if (at != std::string::npos && at + 7 <= value.size()) {
            out.push_back(value.substr(at, 7));
        }
    }
    return out;
}

// A group's family is the one most of its lights belong to.
std::string FamilyOfTarget(const ModelCapabilities& caps, const TargetInfo& t) {
    if (t.type == TargetType::SubModel) {
        const TargetInfo* parent = caps.Find(t.parent);
        return parent != nullptr ? FamilyIdOf(*parent) : std::string("props");
    }
    if (t.type == TargetType::Model) {
        return FamilyIdOf(t);
    }
    std::map<std::string, int> nodes;
    for (const std::string& m : t.flatModels) {
        if (const TargetInfo* model = caps.Find(m)) {
            nodes[FamilyIdOf(*model)] += std::max(1, model->nodes);
        }
    }
    std::string best = "props";
    int most = -1;
    for (const auto& [family, n] : nodes) {
        if (n > most) {
            most = n;
            best = family;
        }
    }
    return best;
}

struct LayerDraft {
    std::string effect;
    double time = 0;
    int count = 0;
    std::set<std::string> rows;
    std::vector<double> durations;
    std::map<std::string, std::vector<int>> startsByRow;
    std::map<int, int> orders;
    std::map<std::string, int> blends;
    const Placed* example = nullptr;
};

struct SceneDraft {
    int startMS = 0;
    int endMS = 0;
    double density = 0;
    std::map<std::string, std::map<std::string, LayerDraft>> families; // family -> effect -> draft
    std::vector<std::string> colours;
    std::map<std::string, int> transitions;
    std::vector<std::string> effectIds;
    int colourCount = 0;
};

std::string Describe(const Recipe& r) {
    std::string out;
    for (const RecipeSurface& s : r.surfaces) {
        if (!out.empty()) {
            out += "; ";
        }
        out += s.family + ": ";
        for (size_t i = 0; i < s.layers.size(); ++i) {
            out += (i == 0 ? "" : " + ") + s.layers[i].effect + (s.layers[i].rhythm == "hits" ? " (golpes)" : "");
        }
    }
    return out;
}

} // namespace

int RunRecipeExtraction(const RecipeExtractOptions& o) {
    std::string refused;
    if (!ControllersAreInactive(o.showDir, refused)) {
        printf("composer-recipe-extract: REFUSED: %s\n", refused.c_str());
        return 2;
    }
    HeadlessRenderContext ctx;
    if (!ctx.LoadShowFolder(o.showDir) || !ctx.OpenSequence(o.sequencePath)) {
        printf("composer-recipe-extract: cannot open %s on %s\n", o.sequencePath.c_str(), o.showDir.c_str());
        return 1;
    }
    const MetadataLookup metadata = MetadataFrom(ctx.GetEffectManager());
    const ModelCapabilities caps = ExtractCapabilities(ctx, nullptr);
    const std::string hash = Sha256OfFile(o.sequencePath);
    SequenceElements& elements = ctx.GetSequenceElements();
    const int frameMS = std::max(1, elements.GetFrameMS());
    const int durationMS = elements.GetSequenceEnd();

    std::vector<Placed> placed;
    std::map<std::string, int> skipped;
    const auto collect = [&](Element& element, const std::string& type) {
        const TargetInfo* t = caps.Find(element.GetFullName());
        if (t == nullptr) {
            return;
        }
        const std::string family = FamilyOfTarget(caps, *t);
        for (size_t l = 0; l < element.GetEffectLayerCount(); ++l) {
            for (Effect* e : element.GetEffectLayer(static_cast<int>(l))->GetEffects()) {
                if (e->IsRenderDisabled()) {
                    continue;
                }
                if (NOT_RECIPES.contains(e->GetEffectName())) {
                    skipped[e->GetEffectName()]++;
                    continue;
                }
                placed.push_back({ element.GetFullName(), family, type == "group", type, static_cast<int>(l), t->bufferWi, t->bufferHt, e });
            }
        }
    };
    for (size_t i = 0; i < elements.GetElementCount(); ++i) {
        Element* element = elements.GetElement(i);
        if (element->GetType() == ElementType::ELEMENT_TYPE_TIMING) {
            continue;
        }
        const TargetInfo* t = caps.Find(element->GetName());
        collect(*element, t != nullptr && t->type == TargetType::Group ? "group" : "model");
        if (auto* me = dynamic_cast<ModelElement*>(element)) {
            for (int s = 0; s < me->GetSubModelCount(); ++s) {
                SubModelElement* sub = me->GetSubModel(s);
                if (sub->GetType() == ElementType::ELEMENT_TYPE_SUBMODEL) {
                    collect(*sub, "submodel");
                }
            }
        }
    }
    // What the sequence does, not its bytes: a copy that only points at another audio
    // file is the same show, and must not count twice.
    std::vector<std::string> lines;
    for (const Placed& p : placed) {
        lines.push_back(fmt::format("{}|{}|{}|{}|{}", p.row, p.layer, p.effect->GetEffectName(), p.effect->GetStartTimeMS(), p.effect->GetEndTimeMS()));
    }
    std::sort(lines.begin(), lines.end());
    uint64_t content = 1469598103934665603ULL;
    for (const std::string& l : lines) {
        content = (content ^ Fnv(l)) * 1099511628211ULL;
    }
    const std::string fingerprint = fmt::format("{:016x}", content);
    printf("composer-recipe-extract: %s: %zu effects on %zu targets read, %d left out (files, faces, texts and fixtures)\n", o.package.c_str(), placed.size(), caps.targets.size(),
           [&] { int n = 0; for (const auto& [k, v] : skipped) { n += v; } return n; }());

    // ------------------------------------------------------------------ scenes
    // A scene starts where three or more families start together: the show's cuts.
    std::map<int, std::set<std::string>> startsByFrame;
    for (const Placed& p : placed) {
        startsByFrame[p.effect->GetStartTimeMS() / frameMS].insert(p.family);
    }
    std::vector<int> cuts = { 0 };
    for (const auto& [frame, families] : startsByFrame) {
        const int at = frame * frameMS;
        if (families.size() >= 3 && at - cuts.back() >= MIN_SCENE_MS) {
            cuts.push_back(at);
        }
    }
    cuts.push_back(durationMS);
    std::vector<std::pair<int, int>> scenes;
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        const int a = cuts[i];
        const int b = cuts[i + 1];
        if (b - a < MIN_SCENE_MS / 2) {
            continue;
        }
        const int pieces = std::max(1, static_cast<int>(std::ceil(static_cast<double>(b - a) / MAX_SCENE_MS)));
        for (int k = 0; k < pieces; ++k) {
            scenes.push_back({ a + (b - a) * k / pieces, a + (b - a) * (k + 1) / pieces });
        }
    }

    std::vector<SceneDraft> drafts;
    for (const auto& [a, b] : scenes) {
        SceneDraft d;
        d.startMS = a;
        d.endMS = b;
        std::map<std::string, int> minLayer;
        int starts = 0;
        for (const Placed& p : placed) {
            const int s = p.effect->GetStartTimeMS();
            const int t = p.effect->GetEndTimeMS();
            const int overlap = std::min(t, b) - std::max(s, a);
            if (overlap <= 0 || (s < a && overlap * 2 < b - a)) {
                continue;
            }
            const auto [it, added] = minLayer.try_emplace(p.row, p.layer);
            if (!added) {
                it->second = std::min(it->second, p.layer);
            }
        }
        for (const Placed& p : placed) {
            const int s = p.effect->GetStartTimeMS();
            const int t = p.effect->GetEndTimeMS();
            const int overlap = std::min(t, b) - std::max(s, a);
            if (overlap <= 0 || (s < a && overlap * 2 < b - a)) {
                continue;
            }
            starts += s >= a ? 1 : 0;
            LayerDraft& l = d.families[p.family][p.effect->GetEffectName()];
            l.effect = p.effect->GetEffectName();
            l.time += overlap;
            l.count++;
            l.rows.insert(p.row);
            l.durations.push_back(t - s);
            l.startsByRow[p.row].push_back(s);
            l.orders[p.layer - minLayer[p.row]]++;
            l.blends[p.effect->GetSettings().Get("T_CHOICE_LayerMethod", "Normal")]++;
            if (l.example == nullptr || t - s > l.example->effect->GetEndTimeMS() - l.example->effect->GetStartTimeMS()) {
                l.example = &p;
            }
            const std::vector<std::string> colours = Colours(*p.effect);
            d.colours.insert(d.colours.end(), colours.begin(), colours.end());
            d.colourCount = std::max(d.colourCount, static_cast<int>(colours.size()));
            const SettingsMap& settings = p.effect->GetSettings();
            if (std::strtod(settings.Get("T_TEXTCTRL_Fadein", "0").c_str(), nullptr) > 0) {
                d.transitions[settings.Get("T_CHOICE_In_Transition_Type", "Fade")]++;
            }
            if (d.effectIds.size() < 24) {
                d.effectIds.push_back(fmt::format("{}@{}@{}", p.row, p.layer, s));
            }
        }
        d.density = starts / std::max(1.0, (b - a) / 1000.0);
        if (!d.families.empty()) {
            drafts.push_back(std::move(d));
        }
    }
    std::vector<double> densities;
    for (const SceneDraft& d : drafts) {
        densities.push_back(d.density);
    }
    std::sort(densities.begin(), densities.end());

    // ----------------------------------------------------------------- recipes
    RecipeLibrary extracted;
    extracted.packages.push_back(fmt::format("{} {}", o.package, fingerprint));
    std::map<std::string, size_t> bySignature;
    std::map<std::string, std::vector<int>> durations;
    std::map<std::string, std::vector<double>> energies;
    for (const SceneDraft& d : drafts) {
        Recipe r;
        // The families that carry the scene: a recipe of every figure in a big house
        // would never come back, and a family lit for a moment is not its language.
        std::vector<std::pair<double, std::string>> weight;
        double sceneTotal = 0;
        for (const auto& [family, effects] : d.families) {
            double t = 0;
            for (const auto& [name, l] : effects) {
                t += l.time * static_cast<double>(l.rows.size());
            }
            weight.push_back({ -t, family });
            sceneTotal += t;
        }
        std::sort(weight.begin(), weight.end());
        std::set<std::string> carrying;
        for (size_t k = 0; k < weight.size() && carrying.size() < MAX_SURFACES; ++k) {
            if (-weight[k].first >= 0.06 * sceneTotal) {
                carrying.insert(weight[k].second);
            }
        }
        for (const auto& [family, effects] : d.families) {
            if (!carrying.contains(family)) {
                continue;
            }
            double total = 0;
            for (const auto& [name, l] : effects) {
                total += l.time;
            }
            std::vector<const LayerDraft*> ranked;
            for (const auto& [name, l] : effects) {
                ranked.push_back(&l);
            }
            std::sort(ranked.begin(), ranked.end(), [](const LayerDraft* x, const LayerDraft* y) { return x->time != y->time ? x->time > y->time : x->effect < y->effect; });
            RecipeSurface surface;
            surface.family = family;
            std::set<std::string> types;
            bool hitsOnly = true;
            for (const LayerDraft* l : ranked) {
                const bool hits = Median(l->durations) <= HIT_MAX_MS && l->count >= 6;
                if (surface.layers.size() >= 2 || (!surface.layers.empty() && !hits && l->time < 0.35 * total)) {
                    continue;
                }
                RecipeLayer layer;
                layer.effect = l->effect;
                int order = 0;
                int seen = -1;
                for (const auto& [o2, n] : l->orders) {
                    if (n > seen) {
                        seen = n;
                        order = o2;
                    }
                }
                layer.order = order;
                std::string blend = "Normal";
                seen = -1;
                for (const auto& [b2, n] : l->blends) {
                    if (n > seen) {
                        seen = n;
                        blend = b2;
                    }
                }
                layer.blend = blend;
                layer.share = static_cast<int>(l->rows.size());
                if (hits) {
                    layer.rhythm = "hits";
                    layer.hitMS = static_cast<int>(Median(l->durations));
                    std::vector<double> gaps;
                    for (const auto& [row, list] : l->startsByRow) {
                        std::vector<int> sorted = list;
                        std::sort(sorted.begin(), sorted.end());
                        for (size_t k = 1; k < sorted.size(); ++k) {
                            gaps.push_back(sorted[k] - sorted[k - 1]);
                        }
                    }
                    layer.periodMS = static_cast<int>(Median(gaps));
                } else {
                    hitsOnly = false;
                }
                std::vector<std::pair<std::string, std::string>> settings;
                for (const auto& [k, v] : l->example->effect->GetSettings()) {
                    settings.emplace_back(std::string(k), std::string(v));
                }
                if (!UnportableReason(layer.effect, settings).empty()) {
                    continue;
                }
                layer.settings = PortableSettings(metadata, layer.effect, settings, l->example->group);
                surface.layers.push_back(std::move(layer));
                types.insert(l->example->type);
                surface.minBufferWi = surface.minBufferWi == 0 ? l->example->bufferWi : std::min(surface.minBufferWi, l->example->bufferWi);
                surface.minBufferHt = surface.minBufferHt == 0 ? l->example->bufferHt : std::min(surface.minBufferHt, l->example->bufferHt);
            }
            if (surface.layers.empty()) {
                continue;
            }
            // The stack in drawing order, the top first; same order, the busier first.
            std::stable_sort(surface.layers.begin(), surface.layers.end(), [](const RecipeLayer& x, const RecipeLayer& y) { return x.order < y.order; });
            for (size_t k = 0; k < surface.layers.size(); ++k) {
                surface.layers[k].order = static_cast<int>(k);
            }
            surface.targetTypes.assign(types.begin(), types.end());
            surface.role = hitsOnly ? "accent" : (family == "mega-tree" || family == "matrix") ? "lead" : "complement";
            r.surfaces.push_back(std::move(surface));
        }
        if (r.surfaces.size() < 2) {
            continue;
        }
        if (std::none_of(r.surfaces.begin(), r.surfaces.end(), [](const RecipeSurface& s) { return s.role == "lead"; })) {
            // Without a matrix or a mega tree, the family with the most layers leads.
            auto lead = std::max_element(r.surfaces.begin(), r.surfaces.end(), [](const RecipeSurface& x, const RecipeSurface& y) { return x.layers.size() < y.layers.size(); });
            if (lead->role != "accent") {
                lead->role = "lead";
            }
        }
        const std::string signature = r.Signature();
        const double rank = densities.empty() ? 0.5 : static_cast<double>(std::lower_bound(densities.begin(), densities.end(), d.density) - densities.begin()) / std::max<size_t>(1, densities.size() - 1);
        const RecipeSource source{ o.package, fs::path(o.sequencePath).filename().string(), hash, d.startMS, d.endMS, d.effectIds };
        const double position = durationMS > 0 ? static_cast<double>(d.startMS) / durationMS : 0.5;
        std::vector<std::string> contexts = { rank < 0.3 ? "low" : rank < 0.6 ? "mid" : rank < 0.9 ? "high" : "climax" };
        if (position < 0.1) {
            contexts.push_back("intro");
        } else if (position > 0.88) {
            contexts.push_back("outro");
        }
        const auto found = bySignature.find(signature);
        if (found != bySignature.end()) {
            Recipe& existing = extracted.recipes[found->second];
            existing.support++;
            if (existing.sources.size() < 6) {
                existing.sources.push_back(source);
            }
            for (const std::string& c : contexts) {
                if (std::find(existing.contexts.begin(), existing.contexts.end(), c) == existing.contexts.end()) {
                    existing.contexts.push_back(c);
                }
            }
            durations[signature].push_back(d.endMS - d.startMS);
            energies[signature].push_back(rank);
            continue;
        }
        r.id = fmt::format("{:012x}", Fnv(signature) & 0xFFFFFFFFFFFFULL);
        r.origin = RecipeOrigin::Extracted;
        r.authorship = o.authorship;
        r.sources.push_back(source);
        r.contexts = contexts;
        std::map<std::string, int> hueCount;
        for (const std::string& c : d.colours) {
            hueCount[HueOf(c)]++;
        }
        std::vector<std::pair<int, std::string>> hues;
        for (const auto& [h, n] : hueCount) {
            if (!h.empty()) {
                hues.push_back({ -n, h });
            }
        }
        std::sort(hues.begin(), hues.end());
        for (size_t k = 0; k < hues.size() && k < 3; ++k) {
            r.hues.push_back(hues[k].second);
        }
        r.colors = std::max(1, d.colourCount);
        for (const auto& [tr, n] : d.transitions) {
            r.transitions.push_back(tr);
        }
        r.extractor = RECIPE_EXTRACTOR_VERSION;
        durations[signature].push_back(d.endMS - d.startMS);
        energies[signature].push_back(rank);
        bySignature[signature] = extracted.recipes.size();
        extracted.recipes.push_back(std::move(r));
    }
    for (Recipe& r : extracted.recipes) {
        const std::string signature = r.Signature();
        std::vector<double> lengths(durations[signature].begin(), durations[signature].end());
        r.durationMS = static_cast<int>(Median(lengths));
        const std::vector<double>& e = energies[signature];
        r.energy = std::round(std::accumulate(e.begin(), e.end(), 0.0) / std::max<size_t>(1, e.size()) * 1000.0) / 1000.0;
        int layers = 0;
        int hitSurfaces = 0;
        bool lead = false;
        for (const RecipeSurface& s : r.surfaces) {
            layers += static_cast<int>(s.layers.size());
            hitSurfaces += s.role == "accent" ? 1 : 0;
            lead = lead || s.role == "lead";
            if (s.role == "lead") {
                r.constraints.push_back("lead:" + s.family);
            }
        }
        r.complexity = std::round(std::min(1.0, (layers + static_cast<double>(r.surfaces.size())) / 12.0) * 1000.0) / 1000.0;
        r.function = r.energy < 0.3 ? "calm" : hitSurfaces >= 2 ? "accent" : (lead && r.surfaces.size() <= 3) ? "feature" : "bed";
        r.confidence = std::round(std::clamp(0.3 + 0.12 * r.support + 0.05 * static_cast<double>(r.surfaces.size()), 0.0, 0.95) * 1000.0) / 1000.0;
        r.name = Describe(r);
    }
    std::sort(extracted.recipes.begin(), extracted.recipes.end(), [](const Recipe& a, const Recipe& b) {
        return a.support != b.support ? a.support > b.support : a.surfaces.size() != b.surfaces.size() ? a.surfaces.size() > b.surfaces.size() : a.id < b.id;
    });
    if (static_cast<int>(extracted.recipes.size()) > o.maxRecipes) {
        extracted.recipes.resize(static_cast<size_t>(o.maxRecipes));
    }

    RecipeLibrary library;
    std::string error;
    if (fs::exists(fs::path(o.outPath)) && !ReadRecipeLibrary(o.outPath, library, error)) {
        printf("composer-recipe-extract: %s\n", error.c_str());
        return 1;
    }
    // The same file under another name (a copy in another folder) is not evidence twice.
    for (const std::string& p : library.packages) {
        if (p.size() >= 16 && p.substr(p.size() - 16) == fingerprint) {
            printf("composer-recipe-extract: %s is the same sequence as \"%s\", already read: nothing added\n", o.package.c_str(), p.c_str());
            return 0;
        }
    }
    MergeRecipes(library, extracted);
    if (!SaveRecipeLibrary(o.outPath, library, error)) {
        printf("composer-recipe-extract: %s\n", error.c_str());
        return 1;
    }
    printf("composer-recipe-extract: %s: %zu scenes, %zu recipes (top support %d); library now %zu recipes in %s\n", o.package.c_str(), drafts.size(), extracted.recipes.size(),
           extracted.recipes.empty() ? 0 : extracted.recipes.front().support, library.recipes.size(), o.outPath.c_str());
    for (const Recipe& r : extracted.recipes) {
        printf("  %s x%d e%.2f %s %s\n", r.id.c_str(), r.support, r.energy, r.function.c_str(), r.name.c_str());
    }
    return 0;
}

} // namespace composer
