#include "composer/FigureSemantics.h"

#include "composer/ComposerText.h"
#include "models/Model.h"
#include "render/xLightsShowContext.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <filesystem>
#include <set>

#define COMPOSER_TEXT(text) text

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace composer {

namespace {

// What a submodel's name suggests, in Spanish and English. Only a suggestion: the person
// confirms it.
const std::vector<std::pair<std::string, std::vector<std::string>>>& NameHints() {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> H = {
        { "mouth", { "boca", "mouth", "labio", "lips" } },
        { "eyes", { "ojo", "eye" } },
        { "face", { "cara", "face", "rostro" } },
        { "head", { "cabeza", "head" } },
        { "trunk", { "tronco", "trunk" } },
        { "foliage", { "follaje", "hoja", "copa", "foliage", "leaves", "leaf" } },
        { "branches", { "rama", "branch" } },
        { "outline", { "contorno", "borde", "outline", "border", "edge" } },
        { "body", { "cuerpo", "body" } },
        { "top", { "estrella", "punta", "star", "topper", "top" } },
    };
    return H;
}

void Unique(std::vector<int>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

FigureRegion Make(const std::string& id, const std::string& source, std::vector<int> nodes, double confidence, bool real, const std::string& family) {
    FigureRegion r;
    r.id = id;
    r.source = source;
    Unique(nodes);
    r.nodes = std::move(nodes);
    r.confidence = confidence;
    r.real = real;
    r.color = DefaultRegionColor(id, family);
    r.colorSource = r.color.empty() ? "" : "default";
    return r;
}

json RegionJson(const FigureRegion& r) {
    return { { "id", r.id }, { "source", r.source }, { "subModel", r.subModel }, { "nodes", NodeRangesText(r.nodes) }, { "count", r.nodes.size() }, { "confidence", r.confidence },
             { "real", r.real }, { "confirmed", r.confirmed }, { "color", r.color }, { "colorSource", r.colorSource } };
}

FigureRegion RegionFrom(const json& j) {
    FigureRegion r;
    r.id = j.value("id", std::string());
    r.source = j.value("source", std::string());
    r.subModel = j.value("subModel", std::string());
    r.nodes = ParseNodeRanges(j.value("nodes", std::string()), 1 << 20);
    r.confidence = j.value("confidence", 0.0);
    r.real = j.value("real", false);
    r.confirmed = j.value("confirmed", false);
    r.color = j.value("color", std::string());
    r.colorSource = j.value("colorSource", std::string());
    return r;
}

} // namespace

const FigureRegion* FigureProfile::Region(const std::string& id) const {
    for (const FigureRegion& r : regions) {
        if (r.id == id) {
            return &r;
        }
    }
    return nullptr;
}

FigureChoice* FigureChoices::Find(const std::string& figure) {
    for (FigureChoice& c : figures) {
        if (c.figure == figure) {
            return &c;
        }
    }
    return nullptr;
}

const FigureChoice* FigureChoices::Find(const std::string& figure) const {
    for (const FigureChoice& c : figures) {
        if (c.figure == figure) {
            return &c;
        }
    }
    return nullptr;
}

const FigureProfile* FigureSemantics::Find(const std::string& figure) const {
    for (const FigureProfile& p : profiles) {
        if (p.figure == figure) {
            return &p;
        }
    }
    return nullptr;
}

std::string DefaultRegionColor(const std::string& region, const std::string& family) {
    if (region == "trunk") {
        return "#6B3E1E";
    }
    if (region == "mouth") {
        return "#D0201A";
    }
    if (region == "eyes") {
        return "#FFE7C2";
    }
    if (region == "foliage" || region == "branches" || (region == "outline" && (family == "trees" || family == "mega-tree"))) {
        return "#1E8C3A";
    }
    if (region == "top") {
        return "#FFC93C";
    }
    return "";
}

std::vector<int> ParseNodeRanges(const std::string& text, int nodeCount) {
    std::vector<int> out;
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = std::min(text.find(',', at), text.size());
        const std::string part = text::Trim(text.substr(at, end - at));
        at = end + 1;
        if (part.empty()) {
            continue;
        }
        const size_t dash = part.find('-');
        long a = std::strtol(part.substr(0, dash).c_str(), nullptr, 10);
        long b = dash == std::string::npos ? a : std::strtol(part.substr(dash + 1).c_str(), nullptr, 10);
        if (a <= 0 || b <= 0) {
            continue;
        }
        if (b < a) {
            std::swap(a, b);
        }
        for (long n = a; n <= b && n <= nodeCount; ++n) {
            out.push_back(static_cast<int>(n - 1));
        }
    }
    Unique(out);
    return out;
}

std::string NodeRangesText(const std::vector<int>& nodes) {
    std::string out;
    for (size_t i = 0; i < nodes.size();) {
        size_t j = i;
        while (j + 1 < nodes.size() && nodes[j + 1] == nodes[j] + 1) {
            ++j;
        }
        out += (out.empty() ? "" : ",") + (i == j ? std::to_string(nodes[i] + 1) : fmt::format("{}-{}", nodes[i] + 1, nodes[j] + 1));
        i = j + 1;
    }
    return out;
}

// A person's decisions over what was found, then what it means: the mouth, the notes and
// what the figure can play. Same result whether the profile was found just now or read
// back from a file.
static FigureProfile FinishProfile(FigureProfile p, const FigureGeometry& g, const FigureChoice* choice) {
    p.notes.clear();
    if (choice != nullptr) {
        std::erase_if(p.regions, [&](const FigureRegion& r) { return std::find(choice->dropped.begin(), choice->dropped.end(), r.id) != choice->dropped.end(); });
        for (FigureRegion r : choice->regions) {
            std::erase_if(p.regions, [&](const FigureRegion& o) { return o.id == r.id; });
            std::erase_if(r.nodes, [&](int n) { return n < 0 || n >= g.nodes; });
            r.real = !r.nodes.empty() || r.source.rfind("face-images", 0) == 0;
            r.confirmed = true;
            r.confidence = 1;
            if (r.color.empty()) {
                r.color = DefaultRegionColor(r.id, g.family);
                r.colorSource = r.color.empty() ? "" : "default";
            }
            p.regions.push_back(r);
        }
        for (FigureRegion& r : p.regions) {
            const auto c = choice->colors.find(r.id);
            if (c != choice->colors.end()) {
                r.color = c->second;
                r.colorSource = "manual";
            }
        }
        p.reviewed = choice->reviewed;
    }
    if (const FigureRegion* m = p.Region("mouth")) {
        p.mouth = m->real && (m->confirmed || m->source.rfind("face-definition", 0) == 0) ? "nodes" : "suggested";
    } else if (const FigureRegion* f = p.Region("face"); f != nullptr && f->source.rfind("face-images", 0) == 0) {
        p.mouth = "images";
    } else {
        p.mouth = "none";
    }
    for (const FigureRegion& r : p.regions) {
        if (!r.real) {
            p.notes.push_back("region-without-lights:" + r.id);
        } else if (!r.confirmed && r.source.rfind("name-hint", 0) == 0) {
            p.notes.push_back("region-to-confirm:" + r.id);
        }
    }
    if (p.mouth == "none") {
        p.notes.push_back("no-mouth: it cannot speak or sing until a mouth is defined");
    }
    if (g.noController) {
        p.notes.push_back("no-controller: only seen in the preview");
    }

    // What it can play.
    std::vector<std::string> roles;
    if (p.CanSpeak()) {
        roles = { "narrator", "character", "lead-singer", "secondary-singer", "chorus-member" };
    }
    if (g.family == "mega-tree" || g.family == "matrix") {
        roles.push_back("instrumental");
        roles.push_back("background");
    }
    if (g.family == "arches" || g.family == "outline" || g.family == "windows") {
        roles.push_back("percussion");
        roles.push_back("accent");
    }
    roles.push_back("decoration");
    p.roles = choice != nullptr && !choice->roles.empty() ? choice->roles : roles;
    return p;
}

FigureSemantics ApplyFigureChoices(const FigureSemantics& found, const FigureCatalog& geometry, const FigureChoices* choices) {
    FigureSemantics out;
    for (const FigureProfile& p : found.profiles) {
        const FigureGeometry* g = geometry.Find(p.figure);
        if (g == nullptr) {
            continue;
        }
        out.profiles.push_back(FinishProfile(p, *g, choices != nullptr ? choices->Find(p.figure) : nullptr));
    }
    return out;
}

FigureSemantics DescribeSemantics(const xLightsShowContext& ctx, const FigureCatalog& geometry, const ModelCapabilities& caps, const FigureChoices* choices) {
    FigureSemantics out;
    for (const FigureGeometry& g : geometry.figures) {
        if (g.kind != "model") {
            continue;
        }
        Model* model = ctx.AllModels[g.name];
        if (model == nullptr) {
            continue;
        }
        FigureProfile p;
        p.figure = g.name;
        p.family = g.family;
        p.mouth = "none";
        std::vector<int> all(static_cast<size_t>(g.nodes));
        for (int n = 0; n < g.nodes; ++n) {
            all[n] = n;
        }

        // Faces: the face definitions xLights itself draws from.
        const auto& faces = model->GetFaceInfo();
        const auto& faceNodes = model->GetFaceInfoNodes();
        for (const auto& [face, info] : faces) {
            const auto type = info.find("Type");
            if (type != info.end() && type->second == "Matrix") {
                const bool drawable = std::any_of(info.begin(), info.end(), [](const auto& kv) { return kv.first.rfind("Mouth-", 0) == 0 && !kv.second.empty(); });
                if (drawable && p.mouth == "none") {
                    // Pictures for each mouth shape: the whole figure is the face.
                    FigureRegion r = Make("face", "face-images:" + face, all, 0.8, true, g.family);
                    p.regions.push_back(r);
                    p.mouth = "images";
                }
                continue;
            }
            const auto nodes = faceNodes.find(face);
            if (nodes == faceNodes.end()) {
                continue;
            }
            std::vector<int> mouth, eyes, outline;
            for (const auto& [key, list] : nodes->second) {
                std::vector<int>& into = key.rfind("Mouth-", 0) == 0 ? mouth : key.rfind("Eyes-", 0) == 0 ? eyes : key.rfind("FaceOutline", 0) == 0 ? outline : mouth;
                if (key.rfind("Mouth-", 0) != 0 && key.rfind("Eyes-", 0) != 0 && key.rfind("FaceOutline", 0) != 0) {
                    continue;
                }
                into.insert(into.end(), list.begin(), list.end());
            }
            if (!mouth.empty() && !p.Region("mouth")) {
                p.regions.push_back(Make("mouth", "face-definition:" + face, mouth, 0.95, true, g.family));
                p.mouth = "nodes";
            }
            if (!eyes.empty() && !p.Region("eyes")) {
                p.regions.push_back(Make("eyes", "face-definition:" + face, eyes, 0.95, true, g.family));
            }
            if (!outline.empty() && !p.Region("outline")) {
                p.regions.push_back(Make("outline", "face-definition:" + face, outline, 0.9, true, g.family));
            }
        }

        // Submodels: real lights; what their name suggests is only a suggestion.
        for (Model* sub : model->GetSubModels()) {
            const std::string folded = text::Fold(sub->GetName());
            const FigureGeometry* sg = geometry.Find(g.name + "/" + sub->GetName());
            std::vector<int> nodes;
            if (sg != nullptr) {
                for (const FigureNode& n : sg->nodeMap) {
                    nodes.push_back(n.index);
                }
            }
            for (const auto& [region, words] : NameHints()) {
                const bool named = std::any_of(words.begin(), words.end(), [&](const std::string& w) { return folded.find(w) != std::string::npos; });
                if (!named || p.Region(region)) {
                    continue;
                }
                FigureRegion r = Make(region, "name-hint:" + sub->GetName(), nodes, 0.4, !nodes.empty(), g.family);
                r.subModel = sub->GetName();
                p.regions.push_back(r);
                if (region == "mouth" && p.mouth == "none") {
                    p.mouth = "suggested";
                }
                break;
            }
        }

        // What the shape itself gives.
        if (g.topology == "matrix") {
            p.regions.push_back(Make("background", "geometry", all, 0.9, true, g.family));
            p.regions.push_back(Make("image", "geometry", all, 0.9, true, g.family));
            if (g.bufferHt >= 7 && g.bufferWi >= 16) {
                p.regions.push_back(Make("text", "geometry", all, 0.8, true, g.family));
            }
        } else if ((g.topology == "tree" || g.topology == "tree-360") && !p.Region("foliage")) {
            p.regions.push_back(Make("foliage", "geometry", all, 0.7, true, g.family));
        } else if ((g.topology == "line" || g.topology == "window" || g.topology == "arch" || g.topology == "circle") && !p.Region("outline")) {
            p.regions.push_back(Make("outline", "geometry", all, 0.8, true, g.family));
        } else if (!p.Region("body")) {
            p.regions.push_back(Make("body", "geometry", all, 0.6, true, g.family));
        }

        out.profiles.push_back(FinishProfile(std::move(p), g, choices != nullptr ? choices->Find(g.name) : nullptr));
    }
    return out;
}

std::string FigureChoicesPath(const std::string& showDir) {
    return (fs::path(showDir) / "ShowComposer" / "figures.json").string();
}

std::string ToJson(const FigureChoices& c) {
    json figures = json::array();
    for (const FigureChoice& f : c.figures) {
        json regions = json::array();
        for (const FigureRegion& r : f.regions) {
            regions.push_back(RegionJson(r));
        }
        figures.push_back({ { "figure", f.figure }, { "roles", f.roles }, { "regions", regions }, { "dropped", f.dropped }, { "colors", f.colors }, { "reviewed", f.reviewed },
                            { "note", f.note } });
    }
    return json({ { "kind", "composer-figure-choices" }, { "schemaVersion", c.schemaVersion }, { "figures", figures } }).dump(1);
}

bool FromJson(const std::string& text, FigureChoices& out, std::string& error) {
    try {
        const json root = json::parse(text);
        if (root.value("schemaVersion", 0) < 1 || root.value("schemaVersion", 0) > FIGURE_SEMANTICS_SCHEMA_VERSION) {
            error = "figure choices: schema not read";
            return false;
        }
        FigureChoices c;
        for (const json& f : root.value("figures", json::array())) {
            FigureChoice k;
            k.figure = f.value("figure", std::string());
            k.roles = f.value("roles", std::vector<std::string>());
            for (const json& r : f.value("regions", json::array())) {
                k.regions.push_back(RegionFrom(r));
            }
            k.dropped = f.value("dropped", std::vector<std::string>());
            k.colors = f.value("colors", std::map<std::string, std::string>());
            k.reviewed = f.value("reviewed", false);
            k.note = f.value("note", std::string());
            c.figures.push_back(std::move(k));
        }
        out = std::move(c);
    } catch (const json::exception& e) {
        error = std::string("figure choices: ") + e.what();
        return false;
    }
    return true;
}

std::string ToJson(const FigureSemantics& s) {
    json profiles = json::array();
    for (const FigureProfile& p : s.profiles) {
        json regions = json::array();
        for (const FigureRegion& r : p.regions) {
            regions.push_back(RegionJson(r));
        }
        profiles.push_back({ { "figure", p.figure }, { "family", p.family }, { "roles", p.roles }, { "mouth", p.mouth }, { "regions", regions }, { "notes", p.notes },
                             { "reviewed", p.reviewed } });
    }
    return json({ { "kind", "composer-figure-semantics" }, { "schemaVersion", FIGURE_SEMANTICS_SCHEMA_VERSION }, { "profiles", profiles } }).dump(1);
}

bool FromJson(const std::string& text, FigureSemantics& out, std::string& error) {
    try {
        const json root = json::parse(text);
        if (root.value("schemaVersion", 0) < 1 || root.value("schemaVersion", 0) > FIGURE_SEMANTICS_SCHEMA_VERSION) {
            error = "figure semantics: schema not read";
            return false;
        }
        FigureSemantics s;
        for (const json& j : root.value("profiles", json::array())) {
            FigureProfile p;
            p.figure = j.value("figure", std::string());
            p.family = j.value("family", std::string());
            p.roles = j.value("roles", std::vector<std::string>());
            p.mouth = j.value("mouth", std::string("none"));
            for (const json& r : j.value("regions", json::array())) {
                p.regions.push_back(RegionFrom(r));
            }
            p.notes = j.value("notes", std::vector<std::string>());
            p.reviewed = j.value("reviewed", false);
            s.profiles.push_back(std::move(p));
        }
        out = std::move(s);
    } catch (const json::exception& e) {
        error = std::string("figure semantics: ") + e.what();
        return false;
    }
    return true;
}

std::string RegionPhrase(const std::string& r) {
    static const std::map<std::string, const char*> P = {
        { "mouth", COMPOSER_TEXT("Mouth") }, { "eyes", COMPOSER_TEXT("Eyes") }, { "face", COMPOSER_TEXT("Face") }, { "head", COMPOSER_TEXT("Head") },
        { "body", COMPOSER_TEXT("Body") }, { "outline", COMPOSER_TEXT("Outline of the figure") }, { "trunk", COMPOSER_TEXT("Trunk") }, { "foliage", COMPOSER_TEXT("Foliage") },
        { "branches", COMPOSER_TEXT("Branches") }, { "top", COMPOSER_TEXT("Top of the figure") }, { "background", COMPOSER_TEXT("Background") }, { "text", COMPOSER_TEXT("Text") },
        { "image", COMPOSER_TEXT("Pictures") }
    };
    const auto it = P.find(r);
    return it != P.end() ? it->second : r;
}

std::string RolePhrase(const std::string& r) {
    static const std::map<std::string, const char*> P = {
        { "narrator", COMPOSER_TEXT("Narrator") }, { "character", COMPOSER_TEXT("A character") }, { "lead-singer", COMPOSER_TEXT("Lead singer") },
        { "secondary-singer", COMPOSER_TEXT("Second singer") }, { "chorus-member", COMPOSER_TEXT("Choir member") }, { "instrumental", COMPOSER_TEXT("Instruments") },
        { "percussion", COMPOSER_TEXT("Percussion") }, { "decoration", COMPOSER_TEXT("Decoration") }, { "background", COMPOSER_TEXT("Background") },
        { "accent", COMPOSER_TEXT("Accents") }
    };
    const auto it = P.find(r);
    return it != P.end() ? it->second : r;
}

} // namespace composer
