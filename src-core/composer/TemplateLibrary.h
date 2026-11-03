#pragma once

#include "composer/CompositionPlan.h"
#include "composer/ModelCapabilities.h"

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <string>
#include <utility>
#include <vector>

class EffectManager;

// Visual techniques the choreography can use: how to drive one native xLights effect
// on one kind of target in one kind of musical moment. Each lives in its own JSON
// file under resources/composer/templates, so the library grows without touching the
// engine, and every parameter is checked against the effect's own metadata when it
// loads. A template that does not check out is rejected with the reason, never turned
// into an effect with empty settings.
namespace composer {

inline constexpr int TEMPLATE_SCHEMA_VERSION = 1;

// The effect's metadata root (resources/effectmetadata/<Effect>.json), or null when
// xLights has no such effect.
using MetadataLookup = std::function<const nlohmann::json*(const std::string& effectName)>;

[[nodiscard]] MetadataLookup MetadataFrom(const EffectManager& manager);

struct TemplateParameter {
    enum class Kind {
        Fixed,  // always `value`
        Energy, // from `low` at the bottom of the template's energy range to `high` at the top
        Choices // one of `choices`, turning from block to block
    };
    std::string id;  // the metadata property id, e.g. "Spirals_Count"
    std::string key; // the setting it becomes, e.g. "E_SLIDER_Spirals_Count"
    Kind kind = Kind::Fixed;
    std::string value;
    double low = 0;
    double high = 0;
    std::vector<std::string> choices;
    bool integer = false;
    int decimals = 0;
};

struct EffectTemplate {
    std::string id;
    int version = 1;
    std::string effect; // as EffectManager names it: "Color Wash", "SingleStrand"
    std::vector<Role> roles;
    std::vector<TargetType> targetTypes;
    std::vector<std::string> intents;      // "feature", "accompany", "beat-accent", "lyric-text", "face-sing"
    std::vector<Prominence> prominences;
    std::vector<Capability> required;
    std::vector<std::string> sections;     // SectionKind ids; empty: any
    double energyMin = 0;
    double energyMax = 1;
    double intensity = 0.7;
    int minMS = 2000;
    int maxMS = 16000;
    double maxPerMinute = 4;               // across the whole song, per target
    std::vector<std::string> transitions;  // native transition types it may enter with
    int minColors = 1;
    int maxColors = 3;
    std::vector<std::string> paletteFamilies;
    std::vector<TemplateParameter> parameters;
    std::string rhythm;  // "" sustained; "downbeat": short events on downbeats
    std::string binding; // "lyrics-phrase", "face-track" or ""
    std::string media;   // "image" or "video": needs a file the library does not have
    int minBufferWi = 0; // below it the target only shows it with limitations
    int minBufferHt = 0;
    double weight = 1;
    std::string provenance;      // "neutral-starter" for the starting set
    bool visuallyValidated = false; // only a person can make this true
    std::string notes;
    std::string file;
};

struct PaletteDef {
    std::string id;
    std::string family;
    std::vector<std::string> colors; // "#RRGGBB"
};

struct RejectedTemplate {
    std::string file;
    std::vector<std::string> errors;
};

struct TemplateLibrary {
    std::vector<EffectTemplate> templates; // valid ones only, by id
    std::vector<PaletteDef> palettes;
    std::vector<RejectedTemplate> rejected;
    std::string hash;

    [[nodiscard]] const EffectTemplate* Find(const std::string& id) const;
    [[nodiscard]] const PaletteDef* Palette(const std::string& id) const;
};

// resources/composer/templates as installed, or in the source tree when run from a build
// that did not copy the resources.
[[nodiscard]] std::string DefaultTemplateDirectory();

// Reads every *.json in `directory` (sorted, so the library is the same on every run)
// and palettes.json beside it.
[[nodiscard]] TemplateLibrary LoadTemplateLibrary(const std::string& directory, const MetadataLookup& metadata);

// One template from its JSON text; the errors say what is wrong, in stable words.
[[nodiscard]] bool ParseTemplate(const std::string& text, const MetadataLookup& metadata, EffectTemplate& out, std::vector<std::string>& errors);

// The setting a metadata property becomes, as the effect panel would save it, and
// the range its value must fall in once written that way.
struct SettingShape {
    std::string key;
    bool numeric = false;
    bool integer = false;
    bool boolean = false;
    double min = 0;
    double max = 0;
    int decimals = 0;
    std::vector<std::string> options; // empty: free or filled at run time
    bool runtimeOnly = false;         // custom rows the engine fills (faces track, text file)
};

[[nodiscard]] bool ShapeOf(const nlohmann::json& property, SettingShape& shape, std::string& error);

// Checks one "E_..." setting against the effect's metadata; empty when it is valid.
[[nodiscard]] std::string CheckSetting(const nlohmann::json& effectMetadata, const std::string& key, const std::string& value);

// The native transition types the renderer knows (PixelBuffer, BlendingPanel).
[[nodiscard]] const std::vector<std::string>& TransitionTypes();

enum class Compatibility {
    Compatible,
    Limited,
    Unknown,
    Incompatible
};

struct CompatibilityResult {
    Compatibility level = Compatibility::Compatible;
    std::vector<std::string> reasons;
};

// Whether a template suits a target, from the target's role, type, buffer and
// capabilities. Unknown is never Incompatible.
[[nodiscard]] CompatibilityResult EvaluateCompatibility(const EffectTemplate& t, const TargetInfo& target);

[[nodiscard]] const char* CompatibilityId(Compatibility c);

} // namespace composer
