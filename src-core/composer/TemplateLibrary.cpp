#include "composer/TemplateLibrary.h"

#include "effects/EffectManager.h"
#include "effects/RenderableEffect.h"
#include "utils/ExternalHooks.h"
#include "utils/FileUtils.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace composer {

namespace {

const std::set<std::string> INTENTS = { "feature", "accompany", "beat-accent", "lyric-text", "face-sing", "ensemble-bed", "ensemble-hit" };
const std::set<std::string> BINDINGS = { "", "lyrics-phrase", "face-track" };
const std::set<std::string> RHYTHMS = { "", "downbeat" };
const std::set<std::string> MEDIA = { "", "image", "video" };
const char* const SECTION_IDS[] = { "intro", "verse", "chorus", "bridge", "instrumental", "outro", "unknown" };

std::string Format(double value, int decimals) {
    std::string s = fmt::format("{:.{}f}", value, decimals);
    return s;
}

bool ReadFile(const fs::path& path, std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream content;
    content << in.rdbuf();
    text = content.str();
    return true;
}

uint64_t Fnv(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

const json* FindProperty(const json& metadata, const std::string& id) {
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

std::string AsText(const json& v) {
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_boolean()) {
        return v.get<bool>() ? "1" : "0";
    }
    if (v.is_number_integer()) {
        return std::to_string(v.get<long long>());
    }
    if (v.is_number()) {
        return fmt::format("{}", v.get<double>());
    }
    return "";
}

// A value as the property expects it, or an error.
std::string Fit(const SettingShape& shape, const json& v, std::string& written) {
    if (shape.boolean) {
        if (!v.is_boolean()) {
            return "expects true or false";
        }
        written = v.get<bool>() ? "1" : "0";
        return "";
    }
    if (shape.numeric) {
        if (!v.is_number()) {
            return "expects a number";
        }
        const double d = v.get<double>();
        if (d < shape.min - 1e-9 || d > shape.max + 1e-9) {
            return fmt::format("{} is outside {}..{}", d, shape.min, shape.max);
        }
        if (shape.integer && std::fabs(d - std::round(d)) > 1e-9) {
            return fmt::format("{} is not a whole number", d);
        }
        written = shape.integer ? std::to_string(static_cast<long long>(std::llround(d))) : Format(d, shape.decimals);
        return "";
    }
    if (!v.is_string()) {
        return "expects text";
    }
    written = v.get<std::string>();
    if (!shape.options.empty() && std::find(shape.options.begin(), shape.options.end(), written) == shape.options.end()) {
        return fmt::format("'{}' is not one of its options", written);
    }
    return "";
}

} // namespace

MetadataLookup MetadataFrom(const EffectManager& manager) {
    return [&manager](const std::string& name) -> const json* {
        RenderableEffect* effect = manager.GetEffect(name);
        return effect != nullptr && effect->HasMetadata() ? &effect->GetMetadata() : nullptr;
    };
}

bool ShapeOf(const json& p, SettingShape& shape, std::string& error) {
    const std::string id = p.value("id", std::string());
    const std::string control = p.value("controlType", std::string());
    const std::string prefix = p.value("settingPrefix", std::string());
    const int divisor = p.contains("divisor") ? p["divisor"].get<int>() : 1;
    shape = SettingShape();
    if (control == "slider") {
        shape.numeric = true;
        const double min = p.value("min", 0.0);
        const double max = p.value("max", 0.0);
        if (divisor > 1 && prefix != "SLIDER") {
            // The text box is the saved control and holds the divided value.
            shape.key = "E_TEXTCTRL_" + id;
            shape.min = min / divisor;
            shape.max = max / divisor;
            shape.decimals = divisor >= 100 ? 2 : 1;
        } else {
            shape.key = (prefix == "TEXTCTRL" ? "E_TEXTCTRL_" : "E_SLIDER_") + id;
            shape.min = min;
            shape.max = max;
            shape.integer = true;
        }
    } else if (control == "spin") {
        shape.key = "E_SPINCTRL_" + id;
        shape.numeric = true;
        shape.integer = true;
        shape.min = p.value("min", 0.0);
        shape.max = p.value("max", 0.0);
    } else if (control == "checkbox") {
        shape.key = "E_CHECKBOX_" + id;
        shape.boolean = true;
    } else if (control == "choice") {
        shape.key = "E_CHOICE_" + id;
        if (p.contains("options") && p["options"].is_array()) {
            for (const json& o : p["options"]) {
                if (o.is_string()) {
                    shape.options.push_back(o.get<std::string>());
                }
            }
        }
        // An empty list is filled by the panel at run time (timing tracks, faces).
        shape.runtimeOnly = shape.options.empty();
    } else if (control == "text") {
        shape.key = "E_TEXTCTRL_" + id;
    } else {
        error = fmt::format("control '{}' cannot be set from a template", control);
        return false;
    }
    return true;
}

std::string CheckSetting(const json& metadata, const std::string& key, const std::string& value) {
    if (key.rfind("E_", 0) != 0) {
        return "";
    }
    const size_t controlEnd = key.find('_', 2);
    if (controlEnd == std::string::npos) {
        return "malformed key";
    }
    const std::string id = key.substr(controlEnd + 1);
    // Rows the effect panel draws itself; their values are checked by the validator.
    if (id == "Faces_TimingTrack" || id == "Text_File" || id == "Pictures_Filename" || id == "State_State") {
        return "";
    }
    const json* p = FindProperty(metadata, id);
    if (p == nullptr) {
        return "unknown property";
    }
    SettingShape shape;
    std::string error;
    if (!ShapeOf(*p, shape, error)) {
        return error;
    }
    if (shape.key != key) {
        return fmt::format("written as {} but xLights saves it as {}", key, shape.key);
    }
    if (shape.boolean) {
        return value == "0" || value == "1" ? "" : "expects 0 or 1";
    }
    if (shape.numeric) {
        char* end = nullptr;
        const double d = std::strtod(value.c_str(), &end);
        if (end == value.c_str() || *end != '\0') {
            return "not a number";
        }
        return d < shape.min - 1e-9 || d > shape.max + 1e-9 ? fmt::format("outside {}..{}", shape.min, shape.max) : "";
    }
    if (!shape.options.empty() && std::find(shape.options.begin(), shape.options.end(), value) == shape.options.end()) {
        return "not one of its options";
    }
    return "";
}

const std::vector<std::string>& TransitionTypes() {
    static const std::vector<std::string> TYPES = { "Blend", "Blinds", "Blobs", "Bow Tie", "Circle Explode", "Circles", "Circular Swirl", "Clock", "Dissolve", "Doorway", "Fade",
                                                    "Fold", "From Middle", "Pinwheel", "Shatter", "Slide Bars", "Slide Checks", "Square Explode", "Star", "Swap", "Wipe", "Zoom" };
    return TYPES;
}

bool ParseTemplate(const std::string& text, const MetadataLookup& metadata, EffectTemplate& t, std::vector<std::string>& errors) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        errors.push_back("not valid JSON");
        return false;
    }
    const auto fail = [&](const std::string& e) { errors.push_back(e); };
    try {
        if (root.value("schemaVersion", 0) != TEMPLATE_SCHEMA_VERSION) {
            fail(fmt::format("schemaVersion {} is not {}", root.value("schemaVersion", 0), TEMPLATE_SCHEMA_VERSION));
        }
        t.id = root.value("id", std::string());
        if (t.id.empty()) {
            fail("no id");
        }
        t.version = root.value("version", 0);
        if (t.version < 1) {
            fail("version must be 1 or more");
        }
        t.effect = root.value("effect", std::string());
        const json* effectMetadata = metadata ? metadata(t.effect) : nullptr;
        if (effectMetadata == nullptr) {
            fail(fmt::format("effect '{}' does not exist in this xLights", t.effect));
        }
        for (const json& r : root.value("roles", json::array())) {
            Role role;
            if (!RoleFromId(r.get<std::string>(), role)) {
                fail(fmt::format("unknown role '{}'", r.get<std::string>()));
            } else {
                t.roles.push_back(role);
            }
        }
        if (t.roles.empty()) {
            fail("no roles");
        }
        for (const json& x : root.value("targetTypes", json::array({ "model" }))) {
            const std::string id = x.get<std::string>();
            if (id == "model") {
                t.targetTypes.push_back(TargetType::Model);
            } else if (id == "group") {
                t.targetTypes.push_back(TargetType::Group);
            } else if (id == "submodel") {
                t.targetTypes.push_back(TargetType::SubModel);
            } else {
                fail(fmt::format("unknown target type '{}'", id));
            }
        }
        for (const json& x : root.value("intents", json::array())) {
            const std::string id = x.get<std::string>();
            if (!INTENTS.contains(id)) {
                fail(fmt::format("unknown intent '{}'", id));
            }
            t.intents.push_back(id);
        }
        if (t.intents.empty()) {
            fail("no intents");
        }
        for (const json& x : root.value("prominence", json::array({ "lead", "support", "accent" }))) {
            const std::string id = x.get<std::string>();
            const Prominence p = id == "lead" ? Prominence::Lead : id == "support" ? Prominence::Support : id == "accent" ? Prominence::Accent : Prominence::Rest;
            if (p == Prominence::Rest) {
                fail(fmt::format("prominence '{}' cannot carry an effect", id));
            }
            t.prominences.push_back(p);
        }
        for (const json& x : root.value("requires", json::array())) {
            bool found = false;
            for (int c = 0; c < CAPABILITY_COUNT; ++c) {
                if (x.get<std::string>() == CapabilityId(static_cast<Capability>(c))) {
                    t.required.push_back(static_cast<Capability>(c));
                    found = true;
                }
            }
            if (!found) {
                fail(fmt::format("unknown capability '{}'", x.get<std::string>()));
            }
        }
        for (const json& x : root.value("sections", json::array())) {
            const std::string id = x.get<std::string>();
            if (std::find(std::begin(SECTION_IDS), std::end(SECTION_IDS), id) == std::end(SECTION_IDS)) {
                fail(fmt::format("unknown section '{}'", id));
            }
            t.sections.push_back(id);
        }
        const json energy = root.value("energy", json::array({ 0.0, 1.0 }));
        t.energyMin = energy.at(0).get<double>();
        t.energyMax = energy.at(1).get<double>();
        if (t.energyMin < 0 || t.energyMax > 1 || t.energyMin > t.energyMax) {
            fail("energy must be [min, max] within 0..1");
        }
        t.intensity = root.value("intensity", 0.7);
        const json duration = root.value("durationMS", json::array({ 2000, 16000 }));
        t.minMS = duration.at(0).get<int>();
        t.maxMS = duration.at(1).get<int>();
        if (t.minMS < 100 || t.maxMS < t.minMS) {
            fail("durationMS must be [min, max] with min of 100 ms or more");
        }
        t.maxPerMinute = root.value("maxPerMinute", 4.0);
        for (const json& x : root.value("transitions", json::array())) {
            const std::string id = x.get<std::string>();
            if (std::find(TransitionTypes().begin(), TransitionTypes().end(), id) == TransitionTypes().end()) {
                fail(fmt::format("unknown transition '{}'", id));
            }
            t.transitions.push_back(id);
        }
        const json palette = root.value("palette", json::object());
        const json colors = palette.value("colors", json::array({ 1, 3 }));
        t.minColors = colors.at(0).get<int>();
        t.maxColors = colors.at(1).get<int>();
        if (t.minColors < 0 || t.maxColors > 8 || t.minColors > t.maxColors) {
            fail("palette colors must be [min, max] within 0..8");
        }
        for (const json& f : palette.value("families", json::array())) {
            t.paletteFamilies.push_back(f.get<std::string>());
        }
        t.rhythm = root.value("rhythm", std::string());
        t.binding = root.value("binding", std::string());
        t.media = root.value("media", std::string());
        if (!RHYTHMS.contains(t.rhythm) || !BINDINGS.contains(t.binding) || !MEDIA.contains(t.media)) {
            fail("unknown rhythm, binding or media");
        }
        const json minBuffer = root.value("minBuffer", json::array({ 0, 0 }));
        t.minBufferWi = minBuffer.at(0).get<int>();
        t.minBufferHt = minBuffer.at(1).get<int>();
        t.weight = root.value("weight", 1.0);
        t.provenance = root.value("provenance", std::string());
        t.visuallyValidated = root.value("visuallyValidated", false);
        t.notes = root.value("notes", std::string());

        std::set<std::string> seen;
        for (const json& p : root.value("parameters", json::array())) {
            TemplateParameter param;
            param.id = p.value("id", std::string());
            if (!seen.insert(param.id).second) {
                fail(fmt::format("parameter '{}' appears twice", param.id));
                continue;
            }
            if (effectMetadata == nullptr) {
                continue;
            }
            const json* property = FindProperty(*effectMetadata, param.id);
            if (property == nullptr) {
                fail(fmt::format("parameter '{}' is not a property of {}", param.id, t.effect));
                continue;
            }
            SettingShape shape;
            std::string error;
            if (!ShapeOf(*property, shape, error)) {
                fail(fmt::format("parameter '{}': {}", param.id, error));
                continue;
            }
            if (shape.runtimeOnly) {
                fail(fmt::format("parameter '{}' is filled at run time and cannot be fixed by a template", param.id));
                continue;
            }
            param.key = shape.key;
            param.integer = shape.integer;
            param.decimals = shape.decimals;
            std::string written;
            if (p.contains("value")) {
                param.kind = TemplateParameter::Kind::Fixed;
                error = Fit(shape, p["value"], written);
                param.value = written;
            } else if (p.contains("energy")) {
                param.kind = TemplateParameter::Kind::Energy;
                if (!shape.numeric || !p["energy"].is_array() || p["energy"].size() != 2) {
                    error = "energy needs a numeric property and [low, high]";
                } else {
                    error = Fit(shape, p["energy"][0], written);
                    if (error.empty()) {
                        error = Fit(shape, p["energy"][1], written);
                    }
                    param.low = p["energy"][0].get<double>();
                    param.high = p["energy"][1].get<double>();
                }
            } else if (p.contains("choices")) {
                param.kind = TemplateParameter::Kind::Choices;
                for (const json& c : p["choices"]) {
                    error = Fit(shape, c, written);
                    if (!error.empty()) {
                        break;
                    }
                    param.choices.push_back(written);
                }
                if (param.choices.empty() && error.empty()) {
                    error = "no choices";
                }
            } else {
                error = "needs value, energy or choices";
            }
            if (!error.empty()) {
                fail(fmt::format("parameter '{}': {}", param.id, error));
                continue;
            }
            t.parameters.push_back(std::move(param));
        }
        // Coherence the metadata cannot check.
        const bool faces = t.effect == "Faces";
        if (faces != (t.binding == "face-track") || (faces && std::find(t.required.begin(), t.required.end(), Capability::Face) == t.required.end())) {
            fail("a Faces template must bind the face track and require the face capability, and only it may");
        }
        if ((t.effect == "Text") != (t.binding == "lyrics-phrase")) {
            fail("a Text template must bind lyric phrases, and only it may");
        }
        if ((t.effect == "Pictures" || t.effect == "Video") && t.media.empty()) {
            fail("a Pictures or Video template must name its media");
        }
    } catch (const json::exception& e) {
        fail(std::string("malformed field: ") + e.what());
    }
    return errors.empty();
}

std::string DefaultTemplateDirectory() {
    const fs::path installed = fs::path(FileUtils::GetResourcesDir()) / "composer" / "templates";
    std::error_code ec;
    if (fs::exists(installed, ec)) {
        return installed.string();
    }
    return (fs::path(__FILE__).parent_path().parent_path().parent_path() / "resources" / "composer" / "templates").string();
}

TemplateLibrary LoadTemplateLibrary(const std::string& directory, const MetadataLookup& metadata) {
    TemplateLibrary library;
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fs::path(directory), ec)) {
        if (entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    std::string all;
    std::set<std::string> ids;
    for (const fs::path& file : files) {
        std::string text;
        if (!ReadFile(file, text)) {
            library.rejected.push_back({ file.filename().string(), { "cannot be read" } });
            continue;
        }
        EffectTemplate t;
        std::vector<std::string> errors;
        if (!ParseTemplate(text, metadata, t, errors) || !ids.insert(t.id).second) {
            if (errors.empty()) {
                errors.push_back(fmt::format("id '{}' is already used", t.id));
            }
            library.rejected.push_back({ file.filename().string(), errors });
            continue;
        }
        t.file = file.filename().string();
        all += text;
        library.templates.push_back(std::move(t));
    }
    std::sort(library.templates.begin(), library.templates.end(), [](const EffectTemplate& a, const EffectTemplate& b) { return a.id < b.id; });

    std::string text;
    const fs::path palettes = fs::path(directory).parent_path() / "palettes.json";
    if (ReadFile(palettes, text)) {
        const json root = json::parse(text, nullptr, false);
        if (!root.is_discarded() && root.contains("palettes")) {
            for (const json& p : root["palettes"]) {
                PaletteDef def;
                def.id = p.value("id", std::string());
                def.family = p.value("family", std::string());
                bool valid = !def.id.empty();
                for (const json& c : p.value("colors", json::array())) {
                    const std::string color = c.get<std::string>();
                    valid = valid && color.size() == 7 && color[0] == '#' && color.find_first_not_of("0123456789ABCDEFabcdef", 1) == std::string::npos;
                    def.colors.push_back(color);
                }
                if (valid && !def.colors.empty()) {
                    library.palettes.push_back(std::move(def));
                } else {
                    library.rejected.push_back({ "palettes.json", { fmt::format("palette '{}' has an invalid colour", def.id) } });
                }
            }
            all += text;
        }
    }
    if (library.palettes.empty()) {
        library.rejected.push_back({ "palettes.json", { "no palettes" } });
    }
    for (const EffectTemplate& t : library.templates) {
        for (const std::string& family : t.paletteFamilies) {
            if (std::none_of(library.palettes.begin(), library.palettes.end(), [&](const PaletteDef& p) { return p.family == family; })) {
                library.rejected.push_back({ t.file, { fmt::format("palette family '{}' has no palettes", family) } });
            }
        }
    }
    library.hash = fmt::format("{:016x}", Fnv(all));
    return library;
}

const EffectTemplate* TemplateLibrary::Find(const std::string& id) const {
    for (const EffectTemplate& t : templates) {
        if (t.id == id) {
            return &t;
        }
    }
    return nullptr;
}

const PaletteDef* TemplateLibrary::Palette(const std::string& id) const {
    for (const PaletteDef& p : palettes) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

CompatibilityResult EvaluateCompatibility(const EffectTemplate& t, const TargetInfo& target) {
    CompatibilityResult result;
    const auto worsen = [&](Compatibility c, std::string reason) {
        if (static_cast<int>(c) > static_cast<int>(result.level)) {
            result.level = c;
        }
        result.reasons.push_back(std::move(reason));
    };
    if (std::find(t.targetTypes.begin(), t.targetTypes.end(), target.type) == t.targetTypes.end()) {
        worsen(Compatibility::Incompatible, fmt::format("target-type:{}", TargetTypeId(target.type)));
    }
    const bool role = std::any_of(t.roles.begin(), t.roles.end(), [&](Role r) { return target.HasRole(r); });
    if (!role) {
        worsen(Compatibility::Incompatible, "role");
    }
    if (target.Excluded()) {
        worsen(Compatibility::Incompatible, "excluded");
    }
    for (Capability c : t.required) {
        const Support s = target.Can(c).support;
        if (s == Support::No) {
            worsen(Compatibility::Incompatible, fmt::format("capability-no:{}", CapabilityId(c)));
        } else if (s == Support::Limited) {
            worsen(Compatibility::Limited, fmt::format("capability-limited:{}", CapabilityId(c)));
        } else if (s == Support::Unknown) {
            worsen(Compatibility::Unknown, fmt::format("capability-unknown:{}", CapabilityId(c)));
        }
    }
    if ((t.minBufferWi > 0 && target.bufferWi < t.minBufferWi) || (t.minBufferHt > 0 && target.bufferHt < t.minBufferHt)) {
        worsen(Compatibility::Limited, fmt::format("buffer-below:{}x{}", t.minBufferWi, t.minBufferHt));
    }
    return result;
}

const char* CompatibilityId(Compatibility c) {
    static const char* const IDS[] = { "compatible", "limited", "unknown", "incompatible" };
    return IDS[static_cast<int>(c)];
}

} // namespace composer
