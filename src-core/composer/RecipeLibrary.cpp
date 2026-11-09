#include "composer/RecipeLibrary.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace composer {

namespace {

const char* const ORIGIN_IDS[] = { "extracted", "reviewed", "corrected", "composed" };

bool OriginFromId(const std::string& id, RecipeOrigin& origin) {
    for (int i = 0; i < 4; ++i) {
        if (id == ORIGIN_IDS[i]) {
            origin = static_cast<RecipeOrigin>(i);
            return true;
        }
    }
    return false;
}

json SettingsJson(const std::vector<std::pair<std::string, std::string>>& settings) {
    json out = json::array();
    for (const auto& [k, v] : settings) {
        out.push_back(json::array({ k, v }));
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> SettingsFrom(const json& list) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const json& kv : list) {
        if (kv.is_array() && kv.size() == 2 && kv[0].is_string() && kv[1].is_string()) {
            out.emplace_back(kv[0].get<std::string>(), kv[1].get<std::string>());
        }
    }
    return out;
}

bool ReadAll(const std::string& path, std::string& text) {
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    text = ss.str();
    return true;
}

// Anything that names a file, a timing track or a piece of text belongs to one show,
// not to a way of moving the lights.
bool TiedToOneShow(const std::string& key) {
    static const std::vector<std::string> WORDS = { "File", "Filename", "Timing", "Track", "Lyric", "_Text", "Text_", "Font", "Shader", "Glediator", "Path", "Folder", "Faces_", "State_" };
    return std::any_of(WORDS.begin(), WORDS.end(), [&](const std::string& w) { return key.find(w) != std::string::npos; });
}

const json* PropertyNamed(const json& metadata, const std::string& id) {
    if (!metadata.contains("properties") || !metadata["properties"].is_array()) {
        return nullptr;
    }
    for (const json& p : metadata["properties"]) {
        if (p.value("id", std::string()) == id) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

const RecipeSurface* Recipe::Surface(const std::string& family) const {
    for (const RecipeSurface& s : surfaces) {
        if (s.family == family) {
            return &s;
        }
    }
    return nullptr;
}

std::string Recipe::Signature() const {
    std::vector<std::string> parts;
    for (const RecipeSurface& s : surfaces) {
        std::string part = s.family + ":";
        for (const RecipeLayer& l : s.layers) {
            part += l.effect + (l.rhythm.empty() ? "" : "*" + l.rhythm) + (l.blend == "Normal" ? "" : "+" + l.blend) + ";";
        }
        parts.push_back(part);
    }
    std::sort(parts.begin(), parts.end());
    std::string out;
    for (const std::string& p : parts) {
        out += p + "|";
    }
    return out;
}

Recipe* RecipeLibrary::Find(const std::string& id) {
    for (Recipe& r : recipes) {
        if (r.id == id) {
            return &r;
        }
    }
    return nullptr;
}

const Recipe* RecipeLibrary::Find(const std::string& id) const {
    for (const Recipe& r : recipes) {
        if (r.id == id) {
            return &r;
        }
    }
    return nullptr;
}

const char* RecipeOriginId(RecipeOrigin origin) {
    return ORIGIN_IDS[static_cast<int>(origin)];
}

std::string ToJson(const RecipeLibrary& library) {
    json root;
    root["schemaVersion"] = RECIPE_SCHEMA_VERSION;
    root["packages"] = library.packages;
    json list = json::array();
    for (const Recipe& r : library.recipes) {
        json surfaces = json::array();
        for (const RecipeSurface& s : r.surfaces) {
            json layers = json::array();
            for (const RecipeLayer& l : s.layers) {
                layers.push_back({ { "order", l.order }, { "effect", l.effect }, { "settings", SettingsJson(l.settings) }, { "blend", l.blend }, { "rhythm", l.rhythm },
                                   { "hitMS", l.hitMS }, { "periodMS", l.periodMS }, { "share", l.share } });
            }
            surfaces.push_back({ { "family", s.family }, { "role", s.role }, { "layers", layers }, { "targetTypes", s.targetTypes }, { "minBufferWi", s.minBufferWi }, { "minBufferHt", s.minBufferHt } });
        }
        json sources = json::array();
        for (const RecipeSource& s : r.sources) {
            sources.push_back({ { "package", s.package }, { "sequence", s.sequence }, { "sequenceHash", s.sequenceHash }, { "startMS", s.startMS }, { "endMS", s.endMS }, { "effects", s.effects } });
        }
        list.push_back({ { "id", r.id }, { "version", r.version }, { "name", r.name }, { "origin", RecipeOriginId(r.origin) }, { "authorship", r.authorship }, { "sources", sources },
                         { "function", r.function }, { "contexts", r.contexts }, { "surfaces", surfaces }, { "durationMS", r.durationMS }, { "transitions", r.transitions },
                         { "hues", r.hues }, { "colors", r.colors }, { "energy", r.energy }, { "complexity", r.complexity }, { "constraints", r.constraints },
                         { "confidence", r.confidence }, { "support", r.support }, { "extractor", r.extractor }, { "favorite", r.favorite }, { "excluded", r.excluded },
                         { "reviewer", r.reviewer }, { "reviewedAt", r.reviewedAt }, { "note", r.note } });
    }
    root["recipes"] = list;
    return root.dump(1);
}

bool FromJson(const std::string& text, RecipeLibrary& library, std::string& error) {
    try {
        const json root = json::parse(text);
        const int version = root.value("schemaVersion", 0);
        if (version < 1 || version > RECIPE_SCHEMA_VERSION) {
            error = fmt::format("recipes: schema version {} is not one this program reads", version);
            return false;
        }
        RecipeLibrary out;
        out.packages = root.value("packages", std::vector<std::string>());
        for (const json& r : root.at("recipes")) {
            Recipe recipe;
            recipe.id = r.at("id").get<std::string>();
            recipe.version = r.value("version", 1);
            recipe.name = r.value("name", recipe.id);
            if (!OriginFromId(r.value("origin", std::string("extracted")), recipe.origin)) {
                error = "recipes: unknown origin in " + recipe.id;
                return false;
            }
            recipe.authorship = r.value("authorship", std::string());
            for (const json& s : r.value("sources", json::array())) {
                recipe.sources.push_back({ s.value("package", std::string()), s.value("sequence", std::string()), s.value("sequenceHash", std::string()), s.value("startMS", 0), s.value("endMS", 0),
                                           s.value("effects", std::vector<std::string>()) });
            }
            recipe.function = r.value("function", std::string("bed"));
            recipe.contexts = r.value("contexts", std::vector<std::string>());
            for (const json& s : r.value("surfaces", json::array())) {
                RecipeSurface surface;
                surface.family = s.at("family").get<std::string>();
                surface.role = s.value("role", std::string("complement"));
                surface.targetTypes = s.value("targetTypes", std::vector<std::string>());
                surface.minBufferWi = s.value("minBufferWi", 0);
                surface.minBufferHt = s.value("minBufferHt", 0);
                for (const json& l : s.value("layers", json::array())) {
                    RecipeLayer layer;
                    layer.order = l.value("order", 0);
                    layer.effect = l.at("effect").get<std::string>();
                    layer.settings = SettingsFrom(l.value("settings", json::array()));
                    layer.blend = l.value("blend", std::string("Normal"));
                    layer.rhythm = l.value("rhythm", std::string());
                    layer.hitMS = l.value("hitMS", 0);
                    layer.periodMS = l.value("periodMS", 0);
                    layer.share = l.value("share", 0);
                    surface.layers.push_back(std::move(layer));
                }
                recipe.surfaces.push_back(std::move(surface));
            }
            recipe.durationMS = r.value("durationMS", 0);
            recipe.transitions = r.value("transitions", std::vector<std::string>());
            recipe.hues = r.value("hues", std::vector<std::string>());
            recipe.colors = r.value("colors", 0);
            recipe.energy = r.value("energy", 0.5);
            recipe.complexity = r.value("complexity", 0.0);
            recipe.constraints = r.value("constraints", std::vector<std::string>());
            recipe.confidence = r.value("confidence", 0.0);
            recipe.support = r.value("support", 1);
            recipe.extractor = r.value("extractor", std::string());
            recipe.favorite = r.value("favorite", false);
            recipe.excluded = r.value("excluded", false);
            recipe.reviewer = r.value("reviewer", std::string());
            recipe.reviewedAt = r.value("reviewedAt", std::string());
            recipe.note = r.value("note", std::string());
            out.recipes.push_back(std::move(recipe));
        }
        library = std::move(out);
    } catch (const json::exception& e) {
        error = std::string("recipes: ") + e.what();
        return false;
    }
    return true;
}

std::string RecipeLibraryPath(const std::string& showDir) {
    return (fs::path(showDir) / "ShowComposer" / "recipes.json").string();
}

bool ReadRecipeLibrary(const std::string& path, RecipeLibrary& library, std::string& error) {
    std::string text;
    if (!ReadAll(path, text)) {
        error = "cannot read " + path;
        return false;
    }
    return FromJson(text, library, error);
}

bool SaveRecipeLibrary(const std::string& path, const RecipeLibrary& library, std::string& error) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const fs::path temp = fs::path(path).string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << ToJson(library);
        if (!out) {
            error = "cannot write " + temp.string();
            return false;
        }
    }
    fs::rename(temp, fs::path(path), ec);
    if (ec) {
        error = "cannot replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

void MergeRecipes(RecipeLibrary& into, const RecipeLibrary& extracted) {
    for (const std::string& p : extracted.packages) {
        if (std::find(into.packages.begin(), into.packages.end(), p) == into.packages.end()) {
            into.packages.push_back(p);
        }
    }
    for (const Recipe& r : extracted.recipes) {
        Recipe* existing = into.Find(r.id);
        if (existing == nullptr) {
            into.recipes.push_back(r);
            continue;
        }
        if (existing->origin == RecipeOrigin::Corrected || existing->origin == RecipeOrigin::Composed) {
            // A person's version stays; only the evidence grows.
            for (const RecipeSource& s : r.sources) {
                const bool known = std::any_of(existing->sources.begin(), existing->sources.end(), [&](const RecipeSource& o) { return o.sequenceHash == s.sequenceHash && o.startMS == s.startMS; });
                if (!known) {
                    existing->sources.push_back(s);
                }
            }
            continue;
        }
        Recipe merged = r;
        merged.favorite = existing->favorite;
        merged.excluded = existing->excluded;
        merged.reviewer = existing->reviewer;
        merged.reviewedAt = existing->reviewedAt;
        merged.note = existing->note;
        merged.origin = existing->origin == RecipeOrigin::Reviewed ? RecipeOrigin::Reviewed : RecipeOrigin::Extracted;
        *existing = std::move(merged);
    }
    std::sort(into.recipes.begin(), into.recipes.end(), [](const Recipe& a, const Recipe& b) { return a.id < b.id; });
}

bool SetRecipeFavorite(RecipeLibrary& library, const std::string& id, bool favorite) {
    Recipe* r = library.Find(id);
    if (r == nullptr) {
        return false;
    }
    r->favorite = favorite;
    if (favorite) {
        r->excluded = false;
    }
    return true;
}

bool SetRecipeExcluded(RecipeLibrary& library, const std::string& id, bool excluded) {
    Recipe* r = library.Find(id);
    if (r == nullptr) {
        return false;
    }
    r->excluded = excluded;
    if (excluded) {
        r->favorite = false;
    }
    return true;
}

bool ReviewRecipe(RecipeLibrary& library, const std::string& id, const std::string& reviewer, const std::string& when, const std::string& note) {
    Recipe* r = library.Find(id);
    if (r == nullptr || reviewer.empty()) {
        return false;
    }
    r->reviewer = reviewer;
    r->reviewedAt = when;
    r->note = note;
    if (r->origin == RecipeOrigin::Extracted) {
        r->origin = RecipeOrigin::Reviewed;
    }
    return true;
}

bool CorrectRecipeSetting(RecipeLibrary& library, const MetadataLookup& metadata, const std::string& id, const std::string& family, int order, const std::string& key, const std::string& value,
                          const std::string& reviewer, const std::string& when, std::string& error) {
    Recipe* r = library.Find(id);
    if (r == nullptr || reviewer.empty()) {
        error = r == nullptr ? "no such recipe" : "a correction needs who made it";
        return false;
    }
    for (RecipeSurface& s : r->surfaces) {
        if (s.family != family) {
            continue;
        }
        for (RecipeLayer& l : s.layers) {
            if (l.order != order) {
                continue;
            }
            std::vector<std::string> dropped;
            const auto kept = PortableSettings(metadata, l.effect, { { key, value } }, std::find(s.targetTypes.begin(), s.targetTypes.end(), "group") != s.targetTypes.end(), &dropped);
            if (kept.empty()) {
                error = dropped.empty() ? std::string("not a setting of ") + l.effect : dropped.front();
                return false;
            }
            std::erase_if(l.settings, [&](const auto& kv) { return kv.first == key; });
            l.settings.push_back(kept.front());
            std::sort(l.settings.begin(), l.settings.end());
            r->origin = RecipeOrigin::Corrected;
            r->reviewer = reviewer;
            r->reviewedAt = when;
            ++r->version;
            return true;
        }
    }
    error = "no such layer";
    return false;
}

Role FamilyRole(const std::string& family) {
    static const std::map<std::string, Role> ROLES = { { "outline", Role::Line }, { "windows", Role::Window }, { "arches", Role::Arch }, { "trees", Role::SmallTree },
                                                       { "stars", Role::Star }, { "mega-tree", Role::MegaTree }, { "matrix", Role::Matrix }, { "faces", Role::Face },
                                                       { "snowflakes", Role::Figure }, { "figures", Role::Figure }, { "props", Role::Prop } };
    const auto it = ROLES.find(family);
    return it != ROLES.end() ? it->second : Role::Prop;
}

std::vector<std::pair<std::string, std::string>> PortableSettings(const MetadataLookup& metadata, const std::string& effect, const std::vector<std::pair<std::string, std::string>>& settings, bool group,
                                                                  std::vector<std::string>* dropped) {
    std::vector<std::pair<std::string, std::string>> out;
    const json* meta = metadata ? metadata(effect) : nullptr;
    const auto drop = [&](const std::string& why) {
        if (dropped != nullptr) {
            dropped->push_back(why);
        }
    };
    if (meta == nullptr) {
        drop("no metadata for " + effect);
        return out;
    }
    for (const auto& [key, value] : settings) {
        if (TiedToOneShow(key)) {
            drop(key + ": tied to one show");
            continue;
        }
        if (key == "B_CHOICE_BufferStyle") {
            const std::vector<std::string>& styles = PortableGroupBufferStyles();
            if (group && std::find(styles.begin(), styles.end(), value) != styles.end()) {
                out.emplace_back(key, value);
            } else {
                drop(key + "=" + value + ": not portable");
            }
            continue;
        }
        if (key.rfind("E_VALUECURVE_", 0) == 0) {
            if (value.rfind("Active=TRUE", 0) == 0 && PropertyNamed(*meta, key.substr(13)) != nullptr) {
                out.emplace_back(key, value);
            } else {
                drop(key + ": curve without a property");
            }
            continue;
        }
        if (key.rfind("E_", 0) != 0) {
            continue;
        }
        const std::string problem = CheckSetting(*meta, key, value);
        if (!problem.empty()) {
            drop(key + ": " + problem);
            continue;
        }
        out.emplace_back(key, value);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string UnportableReason(const std::string& effect, const std::vector<std::pair<std::string, std::string>>& settings) {
    if (effect == "VU Meter") {
        for (const auto& [k, v] : settings) {
            // As VUMeterEffect::CheckForErrors: these types follow a timing track.
            if (k == "E_CHOICE_VUMeter_Type" && v.rfind("Timing Event", 0) == 0) {
                return "a VU Meter that follows a timing track";
            }
        }
    }
    return "";
}

const std::vector<std::string>& PortableGroupBufferStyles() {
    static const std::vector<std::string> STYLES = { "Default", "Per Preview", "Per Model Default", "Per Model Per Preview", "Per Model Single Line", "Horizontal Per Model", "Vertical Per Model",
                                                     "Horizontal Per Model/Strand", "Vertical Per Model/Strand", "Single Line", "Overlay - Centered", "Overlay - Scaled" };
    return STYLES;
}

std::string RecipeTemplateId(const std::string& recipe, const std::string& family, int order) {
    return fmt::format("recipe/{}/{}/{}", recipe, family, order);
}

bool ParseRecipeTemplateId(const std::string& templateId, std::string& recipe, std::string& family, int& order) {
    if (templateId.rfind("recipe/", 0) != 0) {
        return false;
    }
    const size_t a = templateId.find('/', 7);
    const size_t b = a == std::string::npos ? a : templateId.find('/', a + 1);
    if (b == std::string::npos) {
        return false;
    }
    recipe = templateId.substr(7, a - 7);
    family = templateId.substr(a + 1, b - a - 1);
    order = static_cast<int>(std::strtol(templateId.substr(b + 1).c_str(), nullptr, 10));
    return true;
}

std::vector<EffectTemplate> RecipeTemplates(const RecipeLibrary& library, const MetadataLookup& metadata) {
    std::vector<EffectTemplate> out;
    const std::vector<std::string>& known = TransitionTypes();
    for (const Recipe& r : library.recipes) {
        if (r.excluded) {
            continue;
        }
        for (const RecipeSurface& s : r.surfaces) {
            // The base is the lowest sustained layer: the one the others are drawn over.
            int base = -1;
            for (const RecipeLayer& l : s.layers) {
                if (l.rhythm != "hits") {
                    base = std::max(base, l.order);
                }
            }
            for (const RecipeLayer& l : s.layers) {
                const json* meta = metadata ? metadata(l.effect) : nullptr;
                if (meta == nullptr || !UnportableReason(l.effect, l.settings).empty()) {
                    continue;
                }
                EffectTemplate t;
                t.id = RecipeTemplateId(r.id, s.family, l.order);
                t.version = r.version;
                t.effect = l.effect;
                t.roles = { FamilyRole(s.family), Role::Group };
                t.targetTypes = { TargetType::Model, TargetType::Group, TargetType::SubModel };
                t.prominences = { Prominence::Lead, Prominence::Support, Prominence::Accent };
                if (l.rhythm == "hits") {
                    t.intents = { "beat-accent" };
                    t.rhythm = "downbeat";
                    t.minMS = std::max(100, l.hitMS / 2);
                    t.maxMS = std::max(400, l.hitMS * 2);
                    t.maxPerMinute = 40;
                } else {
                    // Only the base is chosen by the choreography; the layers over it come with it.
                    t.intents = l.order == base ? std::vector<std::string>{ "feature", "accompany", "ensemble-bed" } : std::vector<std::string>{ "recipe-layer" };
                    t.minMS = 1000;
                    t.maxMS = std::clamp(r.durationMS, 4000, 30000);
                    t.maxPerMinute = 10;
                }
                t.energyMin = std::max(0.0, r.energy - 0.35);
                t.energyMax = std::min(1.0, r.energy + 0.35);
                t.intensity = 0.7;
                for (const std::string& tr : r.transitions) {
                    if (std::find(known.begin(), known.end(), tr) != known.end()) {
                        t.transitions.push_back(tr);
                    }
                }
                if (t.transitions.empty()) {
                    t.transitions = { "Fade" };
                }
                // As many colours as the source used: a recipe of two colours is not one of one.
                t.minColors = std::clamp(r.colors, 1, 3);
                t.maxColors = std::clamp(r.colors, 1, 4);
                t.paletteFamilies = { "festive", "warm", "cool", "soft" };
                // Only the effect's own settings: a group's buffer style stays in the recipe
                // as evidence, because the same template may land on a single figure.
                for (const auto& [key, value] : PortableSettings(metadata, l.effect, l.settings, false)) {
                    TemplateParameter p;
                    p.key = key;
                    const size_t underscore = key.find('_', 2);
                    p.id = underscore == std::string::npos ? key : key.substr(underscore + 1);
                    p.kind = TemplateParameter::Kind::Fixed;
                    p.value = value;
                    t.parameters.push_back(std::move(p));
                }
                t.minBufferWi = 0;
                t.minBufferHt = 0;
                t.weight = 1.0 + 0.1 * std::min(5, r.support);
                t.provenance = fmt::format("recipe:{}:{}", r.id, RecipeOriginId(r.origin));
                t.visuallyValidated = false;
                t.notes = r.name;
                out.push_back(std::move(t));
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const EffectTemplate& a, const EffectTemplate& b) { return a.id < b.id; });
    return out;
}

} // namespace composer
