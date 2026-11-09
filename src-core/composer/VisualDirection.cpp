#include "composer/VisualDirection.h"

#include "composer/Choreography.h"
#include "composer/ComposerText.h"
#include "composer/MusicAssist.h"
#include "composer/TemplateLibrary.h"
#include "utils/ExternalHooks.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace composer {

namespace {

uint64_t Hash64(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

double Unit(uint64_t seed, const std::string& key) {
    uint64_t z = Hash64(key) ^ (seed * 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return static_cast<double>(z >> 11) / 9007199254740992.0;
}

double Round2(double v) {
    return std::round(v * 100.0) / 100.0;
}

std::string Join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (const std::string& x : v) {
        out += (out.empty() ? "" : sep) + x;
    }
    return out;
}

bool Has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

std::string LowerFolded(const std::string& s) {
    return text::Fold(s);
}

// What every model is, for moving together. Snowflakes are figures by role, but they
// are a family of their own in every reference: round, repeated, and lit as one.
std::string FamilyOfModel(const TargetInfo& t) {
    const RoleAssignment* r = t.PrimaryRole();
    const Role role = r != nullptr ? r->role : Role::Prop;
    const std::string name = LowerFolded(t.name);
    switch (role) {
    case Role::Line:
    case Role::Window:
        return role == Role::Line ? "outline" : "windows";
    case Role::Arch:
        return "arches";
    case Role::SmallTree:
        return "trees";
    case Role::Star:
        return "stars";
    case Role::MegaTree:
        return "mega-tree";
    case Role::Matrix:
        return "matrix";
    case Role::Face:
        return "faces";
    case Role::Figure:
        return name.find("copo") != std::string::npos || name.find("snowflake") != std::string::npos || name.find("flake") != std::string::npos ? "snowflakes" : "figures";
    default:
        return "props";
    }
}

Role RoleOfFamily(const std::string& id) {
    static const std::map<std::string, Role> ROLES = { { "outline", Role::Line }, { "windows", Role::Window }, { "arches", Role::Arch }, { "trees", Role::SmallTree },
                                                       { "stars", Role::Star }, { "mega-tree", Role::MegaTree }, { "matrix", Role::Matrix }, { "faces", Role::Face },
                                                       { "snowflakes", Role::Figure }, { "figures", Role::Figure }, { "props", Role::Prop } };
    const auto it = ROLES.find(id);
    return it != ROLES.end() ? it->second : Role::Prop;
}

// A ring, an outline or an outer edge: what a calm scene lights on its own.
std::string CalmSubModel(const TargetInfo& t, const ModelCapabilities& caps) {
    static const std::vector<std::string> HINTS = { "outline", "outer", "ring", "edge", "border", "contorno", "borde", "aro", "exterior" };
    std::string best;
    for (const std::string& sub : t.subModels) {
        const std::string full = t.name + "/" + sub;
        const TargetInfo* info = caps.Find(full);
        if (info == nullptr || info->Excluded() || info->nodes < 8 || info->nodes * 2 > t.nodes) {
            continue;
        }
        const std::string key = LowerFolded(sub);
        if (std::any_of(HINTS.begin(), HINTS.end(), [&](const std::string& h) { return key.find(h) != std::string::npos; })) {
            if (best.empty() || full < best) {
                best = full;
            }
        }
    }
    return best;
}

std::string Band(const PlanSegment& s, const StyleProfile& p) {
    if (Has(s.flags, "silence")) {
        return "rest";
    }
    if (Has(s.flags, "climax")) {
        return "climax";
    }
    return s.energy >= p.highEnergy ? "high" : s.energy >= p.lowEnergy ? "mid" : "low";
}

json ColorsJson(const std::vector<std::string>& c) {
    return json(c);
}

} // namespace

const ThemeDef* ThemeCatalog::Find(const std::string& id) const {
    for (const ThemeDef& t : themes) {
        if (t.id == id) {
            return &t;
        }
    }
    return nullptr;
}

std::string DefaultThemeCatalogPath() {
    return (fs::path(DefaultTemplateDirectory()).parent_path() / "themes.json").string();
}

bool ParseThemeCatalog(const std::string& text, ThemeCatalog& catalog, std::string& error) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object() || !root.contains("themes") || !root["themes"].is_array()) {
        error = "not a theme catalogue";
        return false;
    }
    const auto validColor = [](const std::string& c) {
        return c.size() == 7 && c[0] == '#' && std::all_of(c.begin() + 1, c.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; });
    };
    ThemeCatalog out;
    std::set<std::string> ids;
    for (const json& t : root["themes"]) {
        ThemeDef d;
        d.id = t.value("id", std::string());
        d.name = t.value("name", d.id);
        d.source = t.value("source", std::string("starter"));
        d.measuredFrom = t.value("measuredFrom", std::vector<std::string>());
        d.accent = t.value("accent", std::string("#FFFFFF"));
        d.characterThemes = t.value("characterThemes", std::vector<std::string>());
        d.mediaCategories = t.value("mediaCategories", std::vector<std::string>());
        for (const std::string& k : t.value("keywords", std::vector<std::string>())) {
            d.keywords.push_back(LowerFolded(k));
        }
        for (const char* list : { "home", "contrast" }) {
            for (const json& p : t.value(list, json::array())) {
                std::vector<std::string> colors;
                for (const json& c : p) {
                    if (c.is_string() && validColor(c.get<std::string>())) {
                        std::string upper = c.get<std::string>();
                        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                        colors.push_back(upper);
                    }
                }
                if (!colors.empty()) {
                    (std::string(list) == "home" ? d.home : d.contrast).push_back(colors);
                }
            }
        }
        if (d.id.empty() || d.home.empty() || !validColor(d.accent) || !ids.insert(d.id).second) {
            error = fmt::format("theme '{}' needs a unique id, a home palette and a #RRGGBB accent", d.id);
            return false;
        }
        out.themes.push_back(std::move(d));
    }
    if (out.themes.empty()) {
        error = "the catalogue has no theme";
        return false;
    }
    out.hash = HashText(text);
    catalog = std::move(out);
    return true;
}

bool LoadThemeCatalog(const std::string& path, ThemeCatalog& catalog, std::string& error) {
    if (!FileExists(path)) {
        error = "no theme catalogue at " + path;
        return false;
    }
    std::ifstream in(fs::path(path), std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return ParseThemeCatalog(text.str(), catalog, error);
}

const SceneLook* ShowVisualDirection::Scene(const std::string& segmentId) const {
    for (const SceneLook& s : scenes) {
        if (s.segmentId == segmentId) {
            return &s;
        }
    }
    return nullptr;
}

std::string FamilyIdOf(const TargetInfo& target) {
    return FamilyOfModel(target);
}

const std::vector<std::string>& MotifEffects(const std::string& motif, const std::string& family) {
    // Measured: what the references run on each kind of figure, by how busy the music is.
    static const std::map<std::string, std::map<std::string, std::vector<std::string>>> TABLE = {
        { "glow",
          { { "outline", { "Color Wash", "Twinkle" } }, { "windows", { "Color Wash" } }, { "arches", { "Color Wash", "Twinkle" } }, { "trees", { "Twinkle", "Color Wash" } },
            { "stars", { "Twinkle", "Color Wash" } }, { "mega-tree", { "Color Wash", "Twinkle" } }, { "matrix", { "Color Wash", "Wave" } }, { "snowflakes", { "Color Wash" } },
            { "figures", { "Color Wash" } }, { "props", { "Color Wash", "Twinkle" } } } },
        { "flow",
          { { "outline", { "SingleStrand", "Marquee" } }, { "windows", { "Color Wash", "SingleStrand" } }, { "arches", { "SingleStrand", "Marquee" } }, { "trees", { "Spirals", "Twinkle" } },
            { "stars", { "Twinkle", "Color Wash" } }, { "mega-tree", { "Spirals", "Wave" } }, { "matrix", { "Wave", "Bars" } }, { "snowflakes", { "Pinwheel", "Color Wash" } },
            { "figures", { "Color Wash", "Twinkle" } }, { "props", { "Pinwheel", "Fan" } } } },
        { "drive",
          { { "outline", { "SingleStrand", "Bars" } }, { "windows", { "Bars", "SingleStrand" } }, { "arches", { "SingleStrand", "Bars" } }, { "trees", { "Spirals", "Bars" } },
            { "stars", { "Color Wash", "Twinkle" } }, { "mega-tree", { "Spirals", "Bars" } }, { "matrix", { "Bars", "Wave" } }, { "snowflakes", { "Fan", "Pinwheel" } },
            { "figures", { "Color Wash", "Bars" } }, { "props", { "Fan", "Pinwheel" } } } },
        { "burst",
          { { "outline", { "SingleStrand", "Bars" } }, { "windows", { "Bars", "Color Wash" } }, { "arches", { "Bars", "SingleStrand" } }, { "trees", { "Spirals", "Bars" } },
            { "stars", { "Color Wash", "Twinkle" } }, { "mega-tree", { "Spirals", "Fan" } }, { "matrix", { "Bars", "Pinwheel" } }, { "snowflakes", { "Fan", "Pinwheel" } },
            { "figures", { "Color Wash", "Bars" } }, { "props", { "Fan", "Pinwheel" } } } },
    };
    static const std::vector<std::string> NONE;
    const auto m = TABLE.find(motif);
    if (m == TABLE.end()) {
        return NONE;
    }
    const auto f = m->second.find(family);
    return f != m->second.end() ? f->second : NONE;
}

ShowVisualDirection DirectShow(const DirectionInput& in) {
    ShowVisualDirection d;
    const StyleProfile neutral = NeutralStarterProfile();
    const StyleProfile& profile = in.profile != nullptr ? *in.profile : neutral;
    d.profileId = profile.id;
    if (in.plan == nullptr || in.capabilities == nullptr) {
        d.warnings.push_back("missing-required-input");
        return d;
    }
    if (!profile.ensemble.enabled) {
        d.warnings.push_back("ensemble-off-in-profile");
        return d;
    }
    if (in.catalog == nullptr || in.catalog->themes.empty()) {
        d.warnings.push_back("no-theme-catalogue");
        return d;
    }
    const EnsembleSettings& e = profile.ensemble;
    const ModelCapabilities& caps = *in.capabilities;
    d.enabled = true;

    // ----------------------------------------------------------- the theme
    const ThemeDef* theme = nullptr;
    if (in.character != nullptr && in.character->suggestedTheme != "uncertain" && in.character->themeConfidence >= 0.5) {
        for (const ThemeDef& t : in.catalog->themes) {
            if (Has(t.characterThemes, in.character->suggestedTheme)) {
                theme = &t;
                d.themeSource = "song-character";
                d.themeConfidence = Round2(in.character->themeConfidence * 0.9);
                d.themeEvidence.push_back(fmt::format("{} said \"{}\" at {:.2f}", in.character->provider, in.character->suggestedTheme, in.character->themeConfidence));
                break;
            }
        }
    }
    if (theme == nullptr) {
        std::string words = " " + LowerFolded(fs::path(in.title).stem().string()) + " ";
        if (in.lyrics != nullptr) {
            for (const LyricLine* l : in.lyrics->Lines()) {
                words += LowerFolded(l->text) + " ";
            }
        }
        for (char& c : words) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && static_cast<unsigned char>(c) < 128) {
                c = ' ';
            }
        }
        int best = 0;
        for (const ThemeDef& t : in.catalog->themes) {
            int hits = 0;
            std::vector<std::string> found;
            for (const std::string& k : t.keywords) {
                int n = 0;
                for (size_t at = words.find(" " + k + " "); at != std::string::npos; at = words.find(" " + k + " ", at + 1)) {
                    ++n;
                }
                if (n > 0) {
                    hits += n;
                    found.push_back(fmt::format("{} x{}", k, n));
                }
            }
            if (hits >= 2 && hits > best) {
                best = hits;
                theme = &t;
                d.themeEvidence = found;
            }
        }
        if (theme != nullptr) {
            d.themeSource = "lyrics";
            d.themeConfidence = Round2(std::min(0.8, 0.3 + 0.05 * best));
        }
    }
    if (theme == nullptr) {
        theme = in.catalog->Find(e.defaultTheme);
        if (theme == nullptr) {
            theme = &in.catalog->themes.front();
            d.warnings.push_back("default-theme-missing:" + e.defaultTheme);
        }
        d.themeSource = "profile-default";
        d.themeConfidence = 0.3;
        d.themeEvidence.push_back("nothing in the lyrics, the title or a song description pointed to a theme");
    }
    d.themeId = theme->id;
    d.themeName = theme->name;
    d.primary = theme->home.front();
    d.secondary = theme->home.size() > 1 ? theme->home[1] : theme->home.front();
    d.accent = { theme->accent };

    // ---------------------------------------------------------- families
    std::map<std::string, VisualFamily> byId;
    std::map<std::string, std::set<std::string>> modelsOf;
    for (const TargetInfo& t : caps.targets) {
        if (t.type != TargetType::Model || t.Excluded() || t.noController || t.nodes <= 0) {
            continue;
        }
        const std::string id = FamilyOfModel(t);
        VisualFamily& f = byId[id];
        f.id = id;
        f.role = RoleOfFamily(id);
        f.protagonist = id == "faces" || id == "matrix";
        f.targets.push_back({ t.name, TargetType::Model, { t.name }, t.nodes, CalmSubModel(t, caps) });
        f.nodes += t.nodes;
        modelsOf[id].insert(t.name);
    }
    // Groups are how the references light a family: one effect across many figures.
    // The largest groups made only of the family's models are taken, without sharing a
    // model, and whatever is left goes model by model. Ties go to the shorter name.
    for (auto& [id, f] : byId) {
        if (f.targets.size() < 2) {
            continue;
        }
        std::vector<const TargetInfo*> candidates;
        for (const TargetInfo& g : caps.targets) {
            if (g.type != TargetType::Group || g.Excluded() || g.noController || g.flatModels.size() < 2) {
                continue;
            }
            if (std::all_of(g.flatModels.begin(), g.flatModels.end(), [&](const std::string& m) { return modelsOf[id].contains(m); })) {
                candidates.push_back(&g);
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const TargetInfo* a, const TargetInfo* b) {
            if (a->flatModels.size() != b->flatModels.size()) {
                return a->flatModels.size() > b->flatModels.size();
            }
            return a->name.size() != b->name.size() ? a->name.size() < b->name.size() : a->name < b->name;
        });
        std::set<std::string> taken;
        std::vector<FamilyTarget> targets;
        for (const TargetInfo* g : candidates) {
            const std::set<std::string> flat(g->flatModels.begin(), g->flatModels.end());
            if (std::any_of(flat.begin(), flat.end(), [&](const std::string& m) { return taken.contains(m); })) {
                continue;
            }
            int nodes = 0;
            for (const FamilyTarget& t : f.targets) {
                nodes += flat.contains(t.target) ? t.nodes : 0;
            }
            targets.push_back({ g->name, TargetType::Group, { flat.begin(), flat.end() }, nodes, "" });
            taken.insert(flat.begin(), flat.end());
        }
        for (const FamilyTarget& t : f.targets) {
            if (!taken.contains(t.target)) {
                targets.push_back(t);
            }
        }
        f.targets = std::move(targets);
    }
    static const std::vector<std::string> ORDER = { "outline", "windows", "arches", "snowflakes", "trees", "stars", "mega-tree", "props", "figures", "matrix", "faces" };
    for (const std::string& id : ORDER) {
        if (byId.contains(id)) {
            d.families.push_back(byId[id]);
        }
    }

    // ------------------------------------------------------------- scenes
    std::map<std::string, int> paletteOfKey;
    std::map<std::string, std::string> motifOfKey;
    int homeTurn = 0;
    int contrastTurn = 0;
    std::set<std::string> looks;
    for (const PlanSegment& s : in.plan->segments) {
        SceneLook look;
        look.segmentId = s.id;
        look.startMS = s.startMS;
        look.endMS = s.endMS;
        look.band = Band(s, profile);
        look.lookKey = s.repeatGroup >= 0 ? fmt::format("g{}", s.repeatGroup) : fmt::format("s{}", s.sectionIndex);
        look.accent = theme->accent;
        if (look.band == "rest") {
            look.motif = "rest";
            look.reasons.push_back({ "silence", "dark, as the references leave a pause" });
            d.scenes.push_back(std::move(look));
            continue;
        }
        look.motif = look.band == "low" ? "glow" : look.band == "mid" ? "flow" : look.band == "high" ? "drive" : "burst";
        // A section that comes back moves as it did the first time, even if its measured
        // energy is a little different; only the climax is allowed its own movement.
        if (const auto first = motifOfKey.find(look.lookKey); first != motifOfKey.end() && look.band != "climax") {
            look.motif = first->second;
        } else if (look.band != "climax") {
            motifOfKey[look.lookKey] = look.motif;
        }
        look.variant = static_cast<int>(Hash64(look.lookKey + "/" + look.motif) % 2);
        look.coverage = look.band == "low" ? e.coverageLow : look.band == "mid" ? e.coverageMid : look.band == "high" ? e.coverageHigh : e.coverageClimax;
        look.hitsPerMinute = look.band == "mid" ? e.hitsPerMinuteMid : look.band == "high" ? e.hitsPerMinuteHigh : look.band == "climax" ? e.hitsPerMinuteClimax : 0;
        const bool repeated = s.repeatGroup >= 0;
        auto known = paletteOfKey.find(look.lookKey);
        if (look.band == "climax") {
            look.paletteKind = "climax";
            look.colors = theme->home.front();
            if (!Has(look.colors, theme->accent)) {
                look.colors.push_back(theme->accent);
            }
            look.reasons.push_back({ "climax", "the home colours with the accent" });
        } else if (known != paletteOfKey.end()) {
            const int k = known->second;
            look.paletteKind = k < 0 ? "contrast" : "home";
            look.colors = k < 0 ? theme->contrast[static_cast<size_t>(-k - 1) % theme->contrast.size()] : theme->home[static_cast<size_t>(k) % theme->home.size()];
            look.reasons.push_back({ "repeat", "the same colours as the first time this section was heard" });
        } else {
            // A section that is heard once may leave the home colours, as the references
            // break a song with a short scene of another hue.
            const bool contrast = !theme->contrast.empty() && !repeated && s.sectionIndex > 0 && Unit(0x5eed, look.lookKey) < e.contrastShare;
            if (contrast) {
                const int k = contrastTurn++;
                paletteOfKey[look.lookKey] = -k - 1;
                look.paletteKind = "contrast";
                look.colors = theme->contrast[static_cast<size_t>(k) % theme->contrast.size()];
                look.reasons.push_back({ "contrast", "a section heard once breaks the home colours" });
            } else {
                const int k = homeTurn++;
                paletteOfKey[look.lookKey] = k;
                look.paletteKind = "home";
                look.colors = theme->home[static_cast<size_t>(k) % theme->home.size()];
                look.reasons.push_back({ "home", fmt::format("home palette {}", k % theme->home.size() + 1) });
            }
        }
        looks.insert(look.motif + "/" + std::to_string(look.variant) + "/" + Join(look.colors, ","));
        d.scenes.push_back(std::move(look));
    }
    for (const std::string& m : { "glow", "flow", "drive", "burst" }) {
        if (std::any_of(d.scenes.begin(), d.scenes.end(), [&](const SceneLook& l) { return l.motif == m; })) {
            d.motifs.push_back(m);
        }
    }
    d.rules = {
        fmt::format("theme {} ({}, confidence {:.2f}); it is an interpretation, the analysis is untouched", d.themeId, d.themeSource, d.themeConfidence),
        "every figure of a scene uses the scene's colours; repeated sections keep their colours and movement",
        fmt::format("calm scenes light about {:.0f}% of the display, busy ones {:.0f}%, the climax {:.0f}%", e.coverageLow * 100, e.coverageHigh * 100, e.coverageClimax * 100),
        "families move together: one effect over a group that holds them, cut at the same times",
        fmt::format("short hits of {} ms on the beat above the base in busy scenes", e.hitMS),
        "faces sing and matrices show the lyrics when the plan gives them that; the rest follows the scene",
        "figures without a controller are left out: the render cannot show them",
        "silences stay dark",
    };
    return d;
}

std::string ToJson(const ShowVisualDirection& d) {
    json families = json::array();
    for (const VisualFamily& f : d.families) {
        json targets = json::array();
        for (const FamilyTarget& t : f.targets) {
            targets.push_back({ { "target", t.target }, { "type", t.type == TargetType::Group ? "group" : "model" }, { "models", t.models }, { "nodes", t.nodes }, { "calmTarget", t.calmTarget } });
        }
        families.push_back({ { "id", f.id }, { "role", RoleId(f.role) }, { "nodes", f.nodes }, { "protagonist", f.protagonist }, { "targets", targets } });
    }
    json scenes = json::array();
    for (const SceneLook& s : d.scenes) {
        json reasons = json::array();
        for (const Reason& r : s.reasons) {
            reasons.push_back({ { "code", r.code }, { "detail", r.detail } });
        }
        scenes.push_back({ { "segmentId", s.segmentId }, { "startMS", s.startMS }, { "endMS", s.endMS }, { "band", s.band }, { "paletteKind", s.paletteKind }, { "colors", ColorsJson(s.colors) },
                           { "accent", s.accent }, { "motif", s.motif }, { "variant", s.variant }, { "lookKey", s.lookKey }, { "coverage", s.coverage }, { "hitsPerMinute", s.hitsPerMinute }, { "reasons", reasons } });
    }
    const json root = { { "schemaVersion", d.schemaVersion }, { "version", d.version }, { "enabled", d.enabled }, { "profileId", d.profileId },
                        { "interpretation", { { "themeId", d.themeId }, { "themeName", d.themeName }, { "confidence", d.themeConfidence }, { "source", d.themeSource }, { "evidence", d.themeEvidence } } },
                        { "palette", { { "primary", d.primary }, { "secondary", d.secondary }, { "accent", d.accent } } },
                        { "rules", d.rules }, { "motifs", d.motifs }, { "families", families }, { "scenes", scenes }, { "warnings", d.warnings }, { "visuallyValidated", false } };
    return root.dump(1);
}

std::string DirectionReport(const ShowVisualDirection& d) {
    std::string out = fmt::format("Visual direction {} — {}\n", d.version, d.enabled ? "on" : "off (the profile does not ask for it)");
    if (!d.enabled) {
        return out;
    }
    out += fmt::format("Theme: {} ({}), from {}, confidence {:.2f}\n", d.themeName, d.themeId, d.themeSource, d.themeConfidence);
    for (const std::string& e : d.themeEvidence) {
        out += "  evidence: " + e + "\n";
    }
    out += fmt::format("Colours: primary {}, secondary {}, accent {}\n", Join(d.primary, " "), Join(d.secondary, " "), Join(d.accent, " "));
    out += "Families:\n";
    for (const VisualFamily& f : d.families) {
        std::string how;
        for (const FamilyTarget& t : f.targets) {
            how += std::string(how.empty() ? "" : ", ") + (t.type == TargetType::Group ? "group " : "") + t.target;
        }
        out += fmt::format("  {:<11} {:>6} nodes  {}\n", f.id, f.nodes, how);
    }
    out += "Scenes:\n";
    for (const SceneLook& s : d.scenes) {
        out += fmt::format("  {:<8} {:>7}-{:<7} {:<6} {:<5} {:<8} {} coverage {:.2f} hits/min {:.0f}\n", s.segmentId, s.startMS, s.endMS, s.band, s.motif, s.paletteKind, Join(s.colors, " "), s.coverage, s.hitsPerMinute);
    }
    out += "Rules:\n";
    for (const std::string& r : d.rules) {
        out += "  - " + r + "\n";
    }
    out += "Nothing of this has been looked at by a person.\n";
    return out;
}

// The hue of a colour in the twelve steps the reference study measured, white, grey and
// black apart: shades of one hue are one hue, as the references' "two hues" counted them.
static std::string HueStep(const std::string& hex) {
    if (hex.size() != 7 || hex[0] != '#') {
        return hex;
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
    const double d = mx - mn;
    double h = mx == r ? std::fmod((g - b) / d, 6.0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
    h *= 60;
    if (h < 0) {
        h += 360;
    }
    return std::to_string(static_cast<int>(std::lround(h / 30.0)) % 12);
}

DirectionMetrics MeasureDirection(const ShowVisualDirection& d, const Choreography& c, const ModelCapabilities& caps) {
    DirectionMetrics m;
    if (c.events.empty()) {
        return m;
    }
    double inScene = 0;
    double colored = 0;
    std::map<std::string, double> hueTime;
    std::map<int, int> startCount;
    std::set<int> layers;
    for (const ChoreographyEvent& e : c.events) {
        const double len = (e.endMS - e.startMS) / 1000.0;
        startCount[e.startMS]++;
        layers.insert(e.layer);
        m.hits += e.intent == "ensemble-hit" ? 1 : 0;
        m.groupEvents += e.type == TargetType::Group ? 1 : 0;
        m.subModelEvents += e.type == TargetType::SubModel ? 1 : 0;
        if (e.colors.empty()) {
            continue;
        }
        colored += len;
        const SceneLook* s = d.Scene(e.segmentId);
        if (s != nullptr && std::all_of(e.colors.begin(), e.colors.end(), [&](const std::string& col) { return Has(s->colors, col) || col == s->accent; })) {
            inScene += len;
        }
        for (const std::string& col : e.colors) {
            hueTime[HueStep(col)] += len / e.colors.size();
        }
    }
    m.layersUsed = static_cast<int>(layers.size());
    m.paletteCoherence = colored > 0 ? Round2(inScene / colored) : 0;
    std::vector<double> hues;
    double total = 0;
    for (const auto& [h, t] : hueTime) {
        hues.push_back(t);
        total += t;
    }
    std::sort(hues.rbegin(), hues.rend());
    m.hueConcentration = total > 0 ? Round2((hues[0] + (hues.size() > 1 ? hues[1] : 0)) / total) : 0;
    int together = 0;
    for (const ChoreographyEvent& e : c.events) {
        together += startCount[e.startMS] >= 3 ? 1 : 0;
    }
    m.startsTogether = Round2(static_cast<double>(together) / c.events.size());
    m.eventsPerMinute = c.durationMS > 0 ? Round2(c.events.size() * 60000.0 / c.durationMS) : 0;

    // Looks: sections heard once should not all look alike; repeated ones should.
    std::map<std::string, std::set<std::string>> looksByKey;
    std::set<std::string> onceLooks;
    int once = 0;
    for (const SceneLook& s : d.scenes) {
        // The climax may look its own way even inside a section that repeats.
        if (s.motif == "rest" || s.band == "climax") {
            continue;
        }
        const std::string look = s.motif + "/" + std::to_string(s.variant) + "/" + Join(s.colors, ",");
        looksByKey[s.lookKey].insert(look);
        if (s.lookKey.rfind("s", 0) == 0) {
            ++once;
            onceLooks.insert(look);
        }
    }
    int repeatedKeys = 0;
    int consistent = 0;
    for (const auto& [key, set] : looksByKey) {
        if (key.rfind("g", 0) == 0) {
            ++repeatedKeys;
            consistent += set.size() == 1 ? 1 : 0;
        }
    }
    m.lookDiversity = once > 0 ? Round2(static_cast<double>(onceLooks.size()) / once) : 0;
    m.repeatConsistency = repeatedKeys > 0 ? Round2(static_cast<double>(consistent) / repeatedKeys) : 1;

    // Coverage: nodes under an effect, sampled every 250 ms over the scenes with music.
    long long totalNodes = 0;
    std::map<std::string, std::vector<std::string>> flat;
    for (const TargetInfo& t : caps.targets) {
        if (t.type == TargetType::Model && !t.Excluded() && !t.noController) {
            totalNodes += t.nodes;
        }
    }
    const auto modelsOf = [&](const ChoreographyEvent& e) -> const std::vector<std::string>& {
        auto it = flat.find(e.target);
        if (it != flat.end()) {
            return it->second;
        }
        std::vector<std::string> models;
        const TargetInfo* t = caps.Find(e.target);
        if (t != nullptr) {
            if (t->type == TargetType::Group) {
                models = t->flatModels;
            } else if (t->type == TargetType::SubModel) {
                models = { t->parent };
            } else {
                models = { t->name };
            }
        }
        return flat.emplace(e.target, std::move(models)).first->second;
    };
    double sum = 0;
    int samples = 0;
    for (const SceneLook& s : d.scenes) {
        if (s.motif == "rest") {
            continue;
        }
        for (int t = s.startMS; t < s.endMS; t += 250) {
            std::set<std::string> lit;
            for (const ChoreographyEvent& e : c.events) {
                if (e.startMS <= t && e.endMS > t) {
                    for (const std::string& model : modelsOf(e)) {
                        lit.insert(model);
                    }
                }
            }
            long long n = 0;
            for (const std::string& model : lit) {
                const TargetInfo* info = caps.Find(model);
                n += info != nullptr && !info->noController ? info->nodes : 0;
            }
            sum += totalNodes > 0 ? static_cast<double>(n) / totalNodes : 0;
            ++samples;
        }
    }
    m.coverageMean = samples > 0 ? Round2(sum / samples) : 0;
    return m;
}

std::string ToJson(const DirectionMetrics& m) {
    const json root = { { "paletteCoherence", m.paletteCoherence }, { "hueConcentration", m.hueConcentration }, { "lookDiversity", m.lookDiversity }, { "repeatConsistency", m.repeatConsistency },
                        { "coverageMean", m.coverageMean }, { "startsTogether", m.startsTogether }, { "eventsPerMinute", m.eventsPerMinute }, { "hits", m.hits },
                        { "groupEvents", m.groupEvents }, { "subModelEvents", m.subModelEvents }, { "layersUsed", m.layersUsed } };
    return root.dump(1);
}

} // namespace composer
