#include "composer/GenerationValidator.h"

#include "composer/Choreography.h"
#include "composer/CompositionPlan.h"
#include "composer/LyricAligner.h"
#include "composer/ModelCapabilities.h"
#include "composer/SequenceWriter.h"
#include "effects/EffectManager.h"
#include "effects/RenderableEffect.h"
#include "media/AudioManager.h"
#include "models/Model.h"
#include "models/ModelGroup.h"
#include "render/Effect.h"
#include "render/EffectLayer.h"
#include "render/Element.h"
#include "render/FSEQFile.h"
#include "render/SequenceElements.h"
#include "render/SequenceFaces.h"
#include "render/SequenceMedia.h"
#include "render/xLightsShowContext.h"
#include "utils/ExternalHooks.h"
#include "utils/FileUtils.h"
#include "utils/UtilClasses.h"
#include "utils/string_utils.h"

#include <pugixml.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace composer {

int ValidationReport::ErrorCount() const {
    return static_cast<int>(std::count_if(issues.begin(), issues.end(), [](const ValidationIssue& i) { return i.severity == IssueSeverity::Error; }));
}

int ValidationReport::WarningCount() const {
    return static_cast<int>(std::count_if(issues.begin(), issues.end(), [](const ValidationIssue& i) { return i.severity == IssueSeverity::Warning; }));
}

const ValidationIssue* ValidationReport::Find(const std::string& code) const {
    const auto it = std::find_if(issues.begin(), issues.end(), [&](const ValidationIssue& i) { return i.code == code; });
    return it == issues.end() ? nullptr : &*it;
}

bool ValidationReport::Has(const std::string& code) const {
    return Find(code) != nullptr;
}

CheckStatus ValidationReport::Status(IssueCategory category) const {
    const bool ran = category == IssueCategory::Structural ? structuralRan : category == IssueCategory::Configuration ? configurationRan : renderRan;
    if (cancelled) {
        return CheckStatus::Cancelled;
    }
    if (!ran) {
        return CheckStatus::NotRun;
    }
    const bool failed = std::any_of(issues.begin(), issues.end(), [&](const ValidationIssue& i) { return i.category == category && i.severity == IssueSeverity::Error; });
    return failed ? CheckStatus::Failed : CheckStatus::Passed;
}

void ValidationReport::Merge(const ValidationReport& other) {
    issues.insert(issues.end(), other.issues.begin(), other.issues.end());
    metrics.insert(metrics.end(), other.metrics.begin(), other.metrics.end());
    cancelled = cancelled || other.cancelled;
    structuralRan = structuralRan || other.structuralRan;
    configurationRan = configurationRan || other.configurationRan;
    renderRan = renderRan || other.renderRan;
}

namespace {

using Args = std::vector<std::pair<std::string, std::string>>;

constexpr int FACE_TYPE_NODE_NAMES = 0;
constexpr int FACE_TYPE_NODE_RANGE = 1;
constexpr int FACE_TYPE_RENDERED = 2;
constexpr int FACE_TYPE_IMAGE = 3;

// Enough to name the problem without burying a report on a badly broken sequence.
constexpr size_t MAX_NAMES_IN_ARG = 20;
constexpr int MAX_MOUTH_MISMATCHES = 5;
constexpr size_t MAX_DISTINCT_FRAMES = 256;

std::string FullTarget(const EffectSpec& effect) {
    return effect.subModel.empty() ? effect.target : effect.target + "/" + effect.subModel;
}

std::string SettingOf(const EffectSpec& effect, const std::string& key) {
    for (const auto& [k, v] : effect.settings) {
        if (k == key) {
            return v;
        }
    }
    return "";
}

std::string JoinNames(const std::vector<std::string>& names) {
    std::string joined;
    for (size_t i = 0; i < names.size() && i < MAX_NAMES_IN_ARG; ++i) {
        joined += (i == 0 ? "" : " | ") + names[i];
    }
    if (names.size() > MAX_NAMES_IN_ARG) {
        joined += fmt::format(" | ... ({} more)", names.size() - MAX_NAMES_IN_ARG);
    }
    return joined;
}

void Raise(ValidationReport& report, const char* code, IssueSeverity severity, IssueCategory category, const std::string& model, const std::string& effect, int startMS, int endMS, Args args = {}) {
    report.issues.push_back({ code, severity, category, model, effect, startMS, endMS, std::move(args) });
}

void RaiseFor(ValidationReport& report, const EffectSpec& effect, const char* code, IssueSeverity severity, IssueCategory category, Args args = {}) {
    Raise(report, code, severity, category, FullTarget(effect), effect.effect, effect.startMS, effect.endMS, std::move(args));
}

bool HasNonAscii(const std::string& s) {
    return std::any_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80; });
}

// Layer method values that exist to combine two effects rather than paint over.
bool IsCompositingLayerMethod(const std::string& method) {
    return method != "Normal" && !method.empty();
}

void AppendModelChannels(const Model& model, std::vector<uint32_t>& out) {
    const int perNode = model.GetChanCountPerNode();
    const uint32_t nodes = model.GetNodeCount();
    for (uint32_t n = 0; n < nodes; ++n) {
        const int32_t first = model.NodeStartChannel(n);
        if (first < 0) {
            continue;
        }
        for (int c = 0; c < perNode; ++c) {
            out.push_back(static_cast<uint32_t>(first + c));
        }
    }
}

void SortUnique(std::vector<uint32_t>& channels) {
    std::sort(channels.begin(), channels.end());
    channels.erase(std::unique(channels.begin(), channels.end()), channels.end());
}

// Read from the layout, never assumed: start channels may chain from other models.
struct TargetChannels {
    std::vector<uint32_t> channels;
    std::vector<std::pair<std::string, std::vector<uint32_t>>> members; // group members only
};

TargetChannels ResolveTargetChannels(const xLightsShowContext& ctx, const std::string& fullName) {
    TargetChannels result;
    const Model* model = ctx.AllModels.GetModel(fullName);
    if (model == nullptr) {
        return result;
    }
    if (const auto* group = dynamic_cast<const ModelGroup*>(model)) {
        for (const Model* member : group->GetFlatModels()) {
            std::vector<uint32_t> channels;
            AppendModelChannels(*member, channels);
            SortUnique(channels);
            result.channels.insert(result.channels.end(), channels.begin(), channels.end());
            result.members.emplace_back(member->GetFullName(), std::move(channels));
        }
    } else {
        AppendModelChannels(*model, result.channels);
    }
    SortUnique(result.channels);
    return result;
}

// The renderer's own lookup, in FacesEffect::RenderFaces, so that this reports the
// face a render would really draw: a missing definition quietly becomes the first
// one on the model rather than an error.
struct FaceResolution {
    const Model* model = nullptr;
    bool onGroup = false;
    bool onSubModel = false;
    bool definitionMissing = false;
    std::string definition;
    std::string type;
    int typeId = FACE_TYPE_IMAGE;
    const std::map<std::string, std::string>* info = nullptr;
    bool fromSequence = false;
};

FaceResolution ResolveFace(const xLightsShowContext& ctx, const SequenceElements& elements, const EffectSpec& effect) {
    FaceResolution face;
    const Model* target = ctx.AllModels.GetModel(FullTarget(effect));
    if (target == nullptr) {
        return face;
    }
    if (target->GetDisplayAs() == DisplayAsType::SubModel) {
        face.onSubModel = true;
        face.model = ctx.AllModels.GetModel(effect.target);
    } else if (const auto* group = dynamic_cast<const ModelGroup*>(target)) {
        face.onGroup = true;
        face.model = group->GetFirstModel();
    } else {
        face.model = target;
    }
    if (face.model == nullptr) {
        return face;
    }

    const auto& faces = face.model->GetFaceInfo();
    std::string definition = SettingOf(effect, "E_CHOICE_Faces_FaceDefinition");
    const bool isDefault = definition.empty() || definition == "Default";
    if (isDefault && !faces.empty() && faces.begin()->first != "") {
        definition = faces.begin()->first;
    }
    auto found = faces.find(definition);
    const std::map<std::string, std::string>* sequenceFace = nullptr;
    if (found == faces.end()) {
        if ((definition.empty() || definition == "Default") && !elements.GetSequenceFaces().empty()) {
            definition = elements.GetSequenceFaces().GetFaces().begin()->first;
        }
        sequenceFace = elements.GetSequenceFaces().GetFace(definition);
    }
    if (found == faces.end() && sequenceFace == nullptr) {
        if (definition == "Coro" && faces.contains("SingleNode")) {
            definition = "SingleNode";
            found = faces.find(definition);
        } else if (definition == "SingleNode" && faces.contains("Coro")) {
            definition = "Coro";
            found = faces.find(definition);
        } else if (definition != "Default" && definition != "Rendered" && definition != "" && definition != "Matrix") {
            face.definitionMissing = true;
            for (auto it = faces.begin(); it != faces.end(); ++it) {
                if (it->first != "") {
                    definition = it->first;
                    found = it;
                    break;
                }
            }
        }
    }

    face.definition = definition;
    if (found != faces.end()) {
        face.info = &found->second;
    } else if (sequenceFace != nullptr) {
        face.info = sequenceFace;
        face.fromSequence = true;
    }
    if (face.info != nullptr && face.info->contains("Type")) {
        face.type = face.info->at("Type");
    }
    if (face.type.empty()) {
        face.type = definition;
    }
    if (face.type == "Coro" || face.type == "SingleNode") {
        face.typeId = FACE_TYPE_NODE_NAMES;
    } else if (face.type == "NodeRange") {
        face.typeId = FACE_TYPE_NODE_RANGE;
    } else if (definition == "Rendered" || definition == "Default" || definition == "") {
        face.typeId = FACE_TYPE_RENDERED;
    }
    return face;
}

const TimingTrackSpec* FindTrack(const SequenceSpec& spec, const std::string& name) {
    const auto it = std::find_if(spec.timingTracks.begin(), spec.timingTracks.end(), [&](const TimingTrackSpec& t) { return t.name == name; });
    return it == spec.timingTracks.end() ? nullptr : &*it;
}

bool IsKnownPhoneme(const std::string& label) {
    static const std::set<std::string> PHONEMES = { "AI", "E", "FV", "L", "MBP", "O", "U", "WQ", "etc", "rest", "(off)" };
    return PHONEMES.contains(BeforeFirst(label, '-'));
}

std::string PhonemeOf(const std::string& label) {
    return BeforeFirst(label, '-');
}

class SpecChecker {
public:
    SpecChecker(xLightsShowContext& ctx, const SequenceSpec& spec, Strictness strictness, const CancelCheck& cancel, ValidationReport& report) :
        _ctx(ctx),
        _spec(spec),
        _strictness(strictness),
        _cancel(cancel),
        _report(report) {}

    void Run() {
        CheckLayout();
        CheckTimeline();
        for (const EffectSpec& effect : _spec.effects) {
            if (IsCancelled()) {
                return;
            }
            CheckTarget(effect);
            CheckEffect(effect);
        }
        if (IsCancelled()) {
            return;
        }
        CheckIncompatibleLayout();
        CheckOverlaps();
        CheckMarkers();
    }

private:
    bool IsCancelled() {
        if (!_report.cancelled && _cancel && _cancel()) {
            _report.cancelled = true;
        }
        return _report.cancelled;
    }

    IssueSeverity Strict() const {
        return _strictness == Strictness::Generated ? IssueSeverity::Error : IssueSeverity::Warning;
    }

    void CheckLayout() {
        if (!_spec.layoutFingerprint.empty() && _spec.layoutFingerprint != LayoutFingerprint(_ctx)) {
            Raise(_report, issue::LayoutIncompatible, IssueSeverity::Error, IssueCategory::Structural, "", "", -1, -1,
                  { { "reason", "fingerprint" }, { "showFolder", _ctx.GetShowDirectory() }, { "missing", "0" }, { "total", std::to_string(_spec.effects.size()) }, { "names", "" } });
        }
    }

    void CheckTimeline() {
        for (const TimingTrackSpec& track : _spec.timingTracks) {
            for (size_t l = 0; l < track.layers.size(); ++l) {
                int previousEnd = 0;
                for (const TimingMark& mark : track.layers[l]) {
                    Args args = { { "track", track.name }, { "layer", std::to_string(l) }, { "label", mark.label } };
                    if (mark.startMS < 0 || mark.endMS <= mark.startMS || (_spec.durationMS > 0 && mark.endMS > _spec.durationMS) || mark.startMS < previousEnd) {
                        Raise(_report, issue::TimingInvalid, IssueSeverity::Error, IssueCategory::Structural, "", "", mark.startMS, mark.endMS, std::move(args));
                    } else if (IsOffGrid(mark.startMS) || IsOffGrid(mark.endMS)) {
                        Raise(_report, issue::TimingOffGrid, IssueSeverity::Warning, IssueCategory::Structural, "", "", mark.startMS, mark.endMS, std::move(args));
                    }
                    previousEnd = std::max(previousEnd, mark.endMS);
                }
            }
        }

        std::map<std::string, std::vector<const EffectSpec*>> byLayer;
        for (const EffectSpec& effect : _spec.effects) {
            if (effect.startMS < 0 || effect.endMS <= effect.startMS || (_spec.durationMS > 0 && effect.endMS > _spec.durationMS)) {
                RaiseFor(_report, effect, issue::EffectIntervalInvalid, IssueSeverity::Error, IssueCategory::Structural, { { "duration", std::to_string(_spec.durationMS) } });
            } else if (IsOffGrid(effect.startMS) || IsOffGrid(effect.endMS)) {
                RaiseFor(_report, effect, issue::TimingOffGrid, IssueSeverity::Warning, IssueCategory::Structural, { { "track", "" }, { "layer", "" }, { "label", effect.effect } });
            }
            byLayer[fmt::format("{}\n{}", FullTarget(effect), effect.layer)].push_back(&effect);
        }
        for (auto& [key, effects] : byLayer) {
            std::sort(effects.begin(), effects.end(), [](const EffectSpec* a, const EffectSpec* b) { return a->startMS < b->startMS; });
            for (size_t i = 1; i < effects.size(); ++i) {
                if (effects[i]->startMS < effects[i - 1]->endMS) {
                    RaiseFor(_report, *effects[i], issue::EffectOverlapInLayer, IssueSeverity::Error, IssueCategory::Structural, { { "layer", std::to_string(effects[i]->layer) } });
                }
            }
        }
    }

    bool IsOffGrid(int ms) const {
        return _spec.frameMS > 0 && ms % _spec.frameMS != 0;
    }

    void CheckTarget(const EffectSpec& effect) {
        const Model* model = _ctx.AllModels[effect.target];
        if (model == nullptr) {
            _missingTargets.insert(effect.target);
            RaiseMissing(effect.kind == TargetKind::Group ? issue::GroupNotFound : issue::ModelNotFound, effect);
            return;
        }
        const auto* group = dynamic_cast<const ModelGroup*>(model);
        if ((effect.kind == TargetKind::Group && group == nullptr) || (effect.kind == TargetKind::Model && group != nullptr)) {
            RaiseFor(_report, effect, issue::TargetKindMismatch, IssueSeverity::Error, IssueCategory::Structural,
                     { { "expected", effect.kind == TargetKind::Group ? "group" : "model" } });
        }
        if (group != nullptr && _checkedGroups.insert(effect.target).second) {
            CheckGroupMembers(*group, effect);
        }
        if (!effect.subModel.empty() && model->GetSubModel(effect.subModel) == nullptr) {
            RaiseFor(_report, effect, issue::SubModelNotFound, IssueSeverity::Error, IssueCategory::Structural, { { "parent", effect.target }, { "subModel", effect.subModel } });
        }
    }

    void RaiseMissing(const char* code, const EffectSpec& effect) {
        const auto sameTarget = [&](const ValidationIssue& i) { return i.code == code && i.model == effect.target; };
        if (std::none_of(_report.issues.begin(), _report.issues.end(), sameTarget)) {
            Raise(_report, code, IssueSeverity::Error, IssueCategory::Structural, effect.target, effect.effect, effect.startMS, effect.endMS, { { "showFolder", _ctx.GetShowDirectory() } });
        }
    }

    void CheckGroupMembers(const ModelGroup& group, const EffectSpec& effect) {
        std::vector<std::string> invalid;
        size_t valid = 0;
        for (const std::string& name : group.ModelNames()) {
            if (_ctx.AllModels.GetModel(name) == nullptr) {
                invalid.push_back(name);
            } else {
                ++valid;
            }
        }
        if (!invalid.empty()) {
            RaiseFor(_report, effect, issue::GroupMemberInvalid, IssueSeverity::Warning, IssueCategory::Structural, { { "members", JoinNames(invalid) } });
        }
        if (valid == 0) {
            RaiseFor(_report, effect, issue::GroupEmpty, IssueSeverity::Error, IssueCategory::Structural);
        }
    }

    // A wrong show folder looks like many separate missing models. Said once and
    // plainly it is one mistake, and the names say which layout was expected.
    void CheckIncompatibleLayout() {
        if (_missingTargets.empty() || _report.Has(issue::LayoutIncompatible)) {
            return;
        }
        std::set<std::string> targets;
        for (const EffectSpec& effect : _spec.effects) {
            targets.insert(effect.target);
        }
        if (_missingTargets.size() * 2 >= targets.size()) {
            Raise(_report, issue::LayoutIncompatible, IssueSeverity::Error, IssueCategory::Structural, "", "", -1, -1,
                  { { "reason", "names" }, { "showFolder", _ctx.GetShowDirectory() }, { "missing", std::to_string(_missingTargets.size()) }, { "total", std::to_string(targets.size()) },
                    { "names", JoinNames({ _missingTargets.begin(), _missingTargets.end() }) } });
        }
    }

    void CheckEffect(const EffectSpec& effect) {
        const Model* model = _ctx.AllModels.GetModel(FullTarget(effect));
        RenderableEffect* renderable = _ctx.GetEffectManager().GetEffect(effect.effect);
        if (renderable == nullptr) {
            RaiseFor(_report, effect, issue::EffectUnknown, IssueSeverity::Error, IssueCategory::Structural);
            return;
        }
        for (const auto& [key, value] : effect.settings) {
            CheckSettingAgainstMetadata(*renderable, effect, key, value);
        }
        CheckPalette(effect);
        if (model != nullptr) {
            const std::string style = SettingOf(effect, "B_CHOICE_BufferStyle");
            const auto& styles = model->GetBufferStyles();
            if (!style.empty() && std::find(styles.begin(), styles.end(), style) == styles.end()) {
                RaiseFor(_report, effect, issue::BufferStyleInvalid, Strict(), IssueCategory::Configuration, { { "style", style } });
            }
            CheckMedia(*renderable, *model, effect);
        }
        if (effect.effect == "Faces") {
            CheckFaces(effect);
        } else if (effect.effect == "Text") {
            CheckText(effect);
        }
    }

    // These have no property of their own in the metadata although the effect
    // reads them: the panel draws them in a custom row.
    static bool IsCustomRowSetting(const std::string& id) {
        return id == "Faces_TimingTrack" || id == "State_State" || id == "Text_File" || id == "Pictures_Filename" || id == "Video_Filename";
    }

    void CheckSettingAgainstMetadata(const RenderableEffect& renderable, const EffectSpec& effect, const std::string& key, const std::string& value) {
        if (key.rfind("E_", 0) != 0 || key.rfind("E_NOTEBOOK_", 0) == 0) {
            return;
        }
        const size_t controlEnd = key.find('_', 2);
        if (controlEnd == std::string::npos) {
            RaiseFor(_report, effect, issue::EffectSettingUnknown, Strict(), IssueCategory::Configuration, { { "key", key } });
            return;
        }
        const std::string control = key.substr(2, controlEnd - 2);
        const std::string id = key.substr(controlEnd + 1);
        if (IsCustomRowSetting(id)) {
            return;
        }
        const nlohmann::json* property = renderable.GetPropertyMetadata(id);
        if (property == nullptr) {
            RaiseFor(_report, effect, issue::EffectSettingUnknown, Strict(), IssueCategory::Configuration, { { "key", key } });
            return;
        }
        if (control == "VALUECURVE") {
            return;
        }
        if (property->contains("options") && (*property)["options"].is_array() && !(*property)["options"].empty()) {
            const auto& options = (*property)["options"];
            const bool plain = std::all_of(options.begin(), options.end(), [](const nlohmann::json& o) { return o.is_string(); });
            const bool listed = std::any_of(options.begin(), options.end(), [&](const nlohmann::json& o) { return o.is_string() && o.get<std::string>() == value; });
            if (plain && !listed) {
                RaiseFor(_report, effect, issue::EffectSettingsInvalid, Strict(), IssueCategory::Configuration, { { "key", key }, { "value", value }, { "reason", "not_an_option" } });
            }
            return;
        }
        const bool numeric = property->contains("min") && property->contains("max");
        if (!numeric) {
            return;
        }
        char* end = nullptr;
        const double number = std::strtod(value.c_str(), &end);
        if (end == value.c_str() || *end != '\0') {
            RaiseFor(_report, effect, issue::EffectSettingsInvalid, Strict(), IssueCategory::Configuration, { { "key", key }, { "value", value }, { "reason", "not_a_number" } });
            return;
        }
        double low = (*property)["min"].get<double>();
        double high = (*property)["max"].get<double>();
        if (control == "TEXTCTRL" && property->contains("divisor")) {
            const double divisor = (*property)["divisor"].get<double>();
            low /= divisor;
            high /= divisor;
        }
        if (number < low || number > high) {
            RaiseFor(_report, effect, issue::EffectSettingsInvalid, Strict(), IssueCategory::Configuration,
                     { { "key", key }, { "value", value }, { "reason", fmt::format("outside {}..{}", low, high) } });
        }
    }

    void CheckPalette(const EffectSpec& effect) {
        static const std::set<std::string> NO_PALETTE = { "Off", "Video", "Pictures", "Shader", "Duplicate", "DMX", "Servo", "Moving Head" };
        SettingsMap palette;
        palette.Parse(nullptr, effect.palette, "");
        bool anyLit = false;
        for (int i = 1; i <= 8; ++i) {
            const std::string number = std::to_string(i);
            const std::string color = palette.Get("C_BUTTON_Palette" + number, "");
            const bool active = palette.Get("C_CHECKBOX_Palette" + number, "") == "1";
            if (!color.empty() && color[0] != '#' && color.rfind("Active=", 0) != 0) {
                RaiseFor(_report, effect, issue::PaletteInvalid, Strict(), IssueCategory::Configuration, { { "slot", number }, { "value", color } });
                continue;
            }
            if (color.size() > 1 && color[0] == '#' && (color.size() != 7 || color.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos)) {
                RaiseFor(_report, effect, issue::PaletteInvalid, Strict(), IssueCategory::Configuration, { { "slot", number }, { "value", color } });
                continue;
            }
            if (active && !color.empty() && color != "#000000") {
                anyLit = true;
            }
        }
        if (!anyLit && !NO_PALETTE.contains(effect.effect)) {
            RaiseFor(_report, effect, issue::PaletteEmpty, IssueSeverity::Warning, IssueCategory::Configuration);
        }
    }

    bool MediaExists(const std::string& file) {
        if (FileExists(FileUtils::FixFile(_ctx.GetShowDirectory(), file))) {
            return true;
        }
        // Embedded in the .xsq. HasImage alone is not enough: an image that failed to
        // load has an entry too. Asked only then, because GetImage adds entries.
        SequenceMedia& media = _ctx.GetSequenceElements().GetSequenceMedia();
        if (!media.HasImage(file)) {
            return false;
        }
        const auto image = media.GetImage(file);
        return image != nullptr && image->IsOk();
    }

    void CheckMedia(const RenderableEffect& renderable, const Model& model, const EffectSpec& effect) {
        const auto require = [&](const std::string& file, const char* setting) {
            if (file.empty() || !MediaExists(file)) {
                RaiseFor(_report, effect, issue::MediaFileMissing, IssueSeverity::Error, IssueCategory::Configuration, { { "setting", setting }, { "file", file } });
            }
        };
        if (effect.effect == "Video") {
            if (SettingOf(effect, "E_CHECKBOX_SynchroniseWithAudio") != "1") {
                require(SettingOf(effect, "E_FILEPICKERCTRL_Video_Filename"), "E_FILEPICKERCTRL_Video_Filename");
            }
            return;
        }
        if (effect.effect == "Pictures") {
            require(SettingOf(effect, "E_TEXTCTRL_Pictures_Filename"), "E_TEXTCTRL_Pictures_Filename");
            return;
        }
        if (effect.effect == "Faces") {
            return; // CheckFaceImages knows which of the definitions is really used
        }
        SettingsMap settings;
        for (const auto& [key, value] : effect.settings) {
            settings[key] = value;
        }
        for (const std::string& file : renderable.GetFileReferences(&_ctx, const_cast<Model*>(&model), settings)) {
            if (!file.empty() && !MediaExists(file)) {
                RaiseFor(_report, effect, issue::MediaFileMissing, IssueSeverity::Error, IssueCategory::Configuration, { { "setting", "" }, { "file", file } });
            }
        }
    }

    void CheckFaces(const EffectSpec& effect) {
        CheckFaceTrack(effect);
        const FaceResolution face = ResolveFace(_ctx, _ctx.GetSequenceElements(), effect);
        if (face.model == nullptr) {
            return; // already reported as a missing target or an empty group
        }
        if (face.definitionMissing) {
            RaiseFor(_report, effect, issue::FaceDefinitionNotFound, IssueSeverity::Error, IssueCategory::Configuration,
                     { { "definition", SettingOf(effect, "E_CHOICE_Faces_FaceDefinition") }, { "substitute", face.definition } });
        }
        // FacesEffect returns before drawing in this case: only an image can be
        // spread over a group, and nothing is reported anywhere.
        if (face.onGroup && face.typeId != FACE_TYPE_IMAGE) {
            RaiseFor(_report, effect, issue::FaceDefinitionIncompatible, IssueSeverity::Error, IssueCategory::Configuration, { { "definition", face.definition }, { "type", face.type } });
            return;
        }
        if (face.typeId == FACE_TYPE_NODE_RANGE && face.info != nullptr) {
            const bool anyMouth = std::any_of(face.info->begin(), face.info->end(), [](const auto& kv) { return kv.first.rfind("Mouth-", 0) == 0 && !EndsWith(kv.first, "-Color") && !kv.second.empty(); });
            if (!anyMouth) {
                RaiseFor(_report, effect, issue::FaceDefinitionEmpty, IssueSeverity::Error, IssueCategory::Configuration, { { "definition", face.definition } });
            }
        }
        if (face.typeId == FACE_TYPE_IMAGE) {
            CheckFaceImages(effect, face);
        }
    }

    // Separate from the definition checks: a perfect track on an unusable face is a
    // different problem from an unusable track, and both can be true at once.
    void CheckFaceTrack(const EffectSpec& effect) {
        if (!SettingOf(effect, "E_CHOICE_Faces_Phoneme").empty()) {
            return; // a fixed mouth shape reads no track
        }
        const std::string name = SettingOf(effect, "E_CHOICE_Faces_TimingTrack");
        const TimingTrackSpec* track = FindTrack(_spec, name);
        if (track == nullptr) {
            RaiseFor(_report, effect, issue::FaceTrackMissing, IssueSeverity::Error, IssueCategory::Configuration, { { "track", name } });
            return;
        }
        if (track->layers.size() < 3) {
            RaiseFor(_report, effect, issue::FaceInsufficientLayers, IssueSeverity::Error, IssueCategory::Configuration, { { "track", name }, { "layers", std::to_string(track->layers.size()) } });
            return;
        }
        const auto& phonemes = track->layers[2];
        const auto& words = track->layers[1];
        const auto inEffect = [&](const TimingMark& m) { return m.startMS < effect.endMS && m.endMS > effect.startMS; };
        if (std::none_of(phonemes.begin(), phonemes.end(), inEffect)) {
            RaiseFor(_report, effect, issue::FaceNoPhonemes, IssueSeverity::Error, IssueCategory::Configuration, { { "track", name } });
        }
        // Valid on a vendor track, where only the phoneme layer matters to the render.
        if (words.empty() && _strictness == Strictness::Generated) {
            RaiseFor(_report, effect, issue::FaceNoWords, IssueSeverity::Warning, IssueCategory::Configuration, { { "track", name } });
        }
        for (const TimingMark& mark : phonemes) {
            if (!inEffect(mark)) {
                continue;
            }
            if (!IsKnownPhoneme(mark.label)) {
                Raise(_report, issue::FacePhonemeUnknown, Strict(), IssueCategory::Configuration, FullTarget(effect), effect.effect, mark.startMS, mark.endMS, { { "track", name }, { "label", mark.label } });
            }
            const bool isRest = PhonemeOf(mark.label) == "rest" || mark.label.empty();
            const bool insideWord = std::any_of(words.begin(), words.end(), [&](const TimingMark& w) { return mark.startMS >= w.startMS && mark.startMS < w.endMS; });
            if (!words.empty() && !isRest && !insideWord) {
                Raise(_report, issue::FacePhonemeOutsideWord, Strict(), IssueCategory::Configuration, FullTarget(effect), effect.effect, mark.startMS, mark.endMS, { { "track", name }, { "label", mark.label } });
            }
        }
    }

    // A missing image is not a dark model: FacesEffect paints every node pure red.
    void CheckFaceImages(const EffectSpec& effect, const FaceResolution& face) {
        if (face.info == nullptr) {
            return;
        }
        std::vector<std::string> missing;
        for (const auto& [key, value] : *face.info) {
            const bool isImage = face.fromSequence ? SequenceFaces::IsImageKey(key) : (key != "CustomColors" && key != "ImagePlacement" && key != "Type" && key != "Name");
            if (isImage && !value.empty() && !MediaExists(value)) {
                missing.push_back(value);
            }
        }
        if (!missing.empty()) {
            RaiseFor(_report, effect, issue::FaceImageMissing, IssueSeverity::Error, IssueCategory::Configuration,
                     { { "definition", face.definition }, { "count", std::to_string(missing.size()) }, { "files", JoinNames(missing) } });
        }
    }

    void CheckText(const EffectSpec& effect) {
        const std::string text = SettingOf(effect, "E_TEXTCTRL_Text");
        const std::string file = SettingOf(effect, "E_FILEPICKERCTRL_Text_File");
        const std::string lyric = SettingOf(effect, "E_CHOICE_Text_LyricTrack");
        if (text.empty() && file.empty() && lyric.empty()) {
            RaiseFor(_report, effect, issue::TextEmpty, IssueSeverity::Error, IssueCategory::Configuration);
            return;
        }

        std::vector<std::string> shown;
        if (!text.empty()) {
            shown.push_back(text);
        }
        if (!lyric.empty()) {
            const bool phrases = EndsWith(lyric, " - Phrases");
            const bool words = EndsWith(lyric, " - Words");
            const std::string name = phrases ? lyric.substr(0, lyric.size() - 10) : words ? lyric.substr(0, lyric.size() - 8) : "";
            const TimingTrackSpec* track = (phrases || words) ? FindTrack(_spec, name) : nullptr;
            if (!phrases && !words) {
                RaiseFor(_report, effect, issue::TextLyricTrackInvalid, IssueSeverity::Error, IssueCategory::Configuration, { { "value", lyric }, { "reason", "no_suffix" } });
            } else if (track == nullptr) {
                RaiseFor(_report, effect, issue::TextLyricTrackInvalid, IssueSeverity::Error, IssueCategory::Configuration, { { "value", lyric }, { "reason", "no_track" } });
            } else if (track->layers.size() < 2) {
                RaiseFor(_report, effect, issue::TextLyricTrackInvalid, IssueSeverity::Error, IssueCategory::Configuration, { { "value", lyric }, { "reason", "too_few_layers" } });
            } else {
                for (const TimingMark& mark : track->layers[phrases ? 0 : 1]) {
                    if (mark.startMS < effect.endMS && mark.endMS > effect.startMS) {
                        shown.push_back(mark.label);
                    }
                }
            }
        }

        // The bitmap fonts only have glyphs up to 127: anything else draws nothing.
        const std::string font = SettingOf(effect, "E_CHOICE_Text_Font");
        const bool bitmapFont = !font.empty() && font != "Use OS Fonts";
        for (const std::string& value : shown) {
            if (!FileUtils::IsValidUTF8(value)) {
                RaiseFor(_report, effect, issue::TextUtf8Issue, IssueSeverity::Error, IssueCategory::Configuration, { { "reason", "invalid_utf8" }, { "font", font } });
            } else if (bitmapFont && HasNonAscii(value)) {
                RaiseFor(_report, effect, issue::TextUtf8Issue, IssueSeverity::Error, IssueCategory::Configuration, { { "reason", "bitmap_font_ascii_only" }, { "font", font } });
            }
        }
    }

    size_t RowOf(const EffectSpec& effect) const {
        for (size_t i = 0; i < _spec.effects.size(); ++i) {
            if (_spec.effects[i].target == effect.target) {
                return i;
            }
        }
        return _spec.effects.size();
    }

    // Membership, not shared channels: the layout's overlapping models are a wiring
    // matter, and two unrelated props on one channel say nothing about intent.
    bool AreRelated(const EffectSpec& a, const EffectSpec& b) const {
        if (a.target == b.target) {
            return a.subModel != b.subModel;
        }
        const Model* ma = _ctx.AllModels[a.target];
        const Model* mb = _ctx.AllModels[b.target];
        if (ma == nullptr || mb == nullptr) {
            return false;
        }
        const auto* ga = dynamic_cast<const ModelGroup*>(ma);
        const auto* gb = dynamic_cast<const ModelGroup*>(mb);
        if (ga != nullptr && gb == nullptr) {
            return ga->ContainsModelOrSubmodel(mb);
        }
        if (gb != nullptr && ga == nullptr) {
            return gb->ContainsModelOrSubmodel(ma);
        }
        if (ga != nullptr && gb != nullptr) {
            const auto membersB = gb->GetFlatModels();
            for (const Model* member : ga->GetFlatModels()) {
                if (std::find(membersB.begin(), membersB.end(), member) != membersB.end()) {
                    return true;
                }
            }
        }
        return false;
    }

    void CheckOverlaps() {
        std::map<std::string, std::vector<const EffectSpec*>> byTarget;
        for (const EffectSpec& effect : _spec.effects) {
            byTarget[FullTarget(effect)].push_back(&effect);
        }
        for (auto a = byTarget.begin(); a != byTarget.end(); ++a) {
            for (auto b = std::next(a); b != byTarget.end(); ++b) {
                if (IsCancelled()) {
                    return;
                }
                if (AreRelated(*a->second.front(), *b->second.front())) {
                    CheckPairOverlap(a->second, b->second);
                }
            }
        }
    }

    void CheckPairOverlap(const std::vector<const EffectSpec*>& first, const std::vector<const EffectSpec*>& second) {
        std::set<std::string> seen;
        for (const EffectSpec* a : first) {
            for (const EffectSpec* b : second) {
                if (a->startMS >= b->endMS || b->startMS >= a->endMS) {
                    continue;
                }
                const std::string classification = Classify(*a, *b);
                if (seen.insert(classification).second) {
                    RaiseOverlap(*a, *b, classification);
                }
            }
        }
    }

    // A row writes every channel it covers, black included, so without model
    // blending the later row replaces the earlier one on the channels they share.
    // Within a row the submodels are drawn after their parent.
    bool DrawnAfter(const EffectSpec& a, const EffectSpec& b) const {
        if (a.target == b.target) {
            return !a.subModel.empty() && b.subModel.empty();
        }
        return RowOf(a) > RowOf(b);
    }

    std::string Classify(const EffectSpec& a, const EffectSpec& b) const {
        const EffectSpec& later = DrawnAfter(a, b) ? a : b;
        const EffectSpec& earlier = DrawnAfter(a, b) ? b : a;
        if (later.effect == "Off" || earlier.effect == "Off") {
            return overlap::Allowed;
        }
        if (_spec.modelBlending) {
            return IsCompositingLayerMethod(SettingOf(later, "T_CHOICE_LayerMethod")) ? overlap::Intentional : overlap::Possible;
        }
        std::vector<uint32_t> earlierChannels = ResolveTargetChannels(_ctx, FullTarget(earlier)).channels;
        const std::vector<uint32_t> laterChannels = ResolveTargetChannels(_ctx, FullTarget(later)).channels;
        const bool hidden = std::includes(laterChannels.begin(), laterChannels.end(), earlierChannels.begin(), earlierChannels.end());
        return hidden ? overlap::Confirmed : overlap::Intentional;
    }

    void RaiseOverlap(const EffectSpec& a, const EffectSpec& b, const std::string& classification) {
        const EffectSpec& later = DrawnAfter(a, b) ? a : b;
        const EffectSpec& earlier = DrawnAfter(a, b) ? b : a;
        const IssueSeverity severity = classification == overlap::Confirmed ? Strict() : classification == overlap::Possible ? IssueSeverity::Warning : IssueSeverity::Info;
        Raise(_report, issue::GroupMemberConflict, severity, IssueCategory::Configuration, FullTarget(earlier), earlier.effect, std::max(a.startMS, b.startMS), std::min(a.endMS, b.endMS),
              { { "classification", classification }, { "over", FullTarget(later) }, { "overEffect", later.effect }, { "modelBlending", _spec.modelBlending ? "true" : "false" } });
    }

    void CheckMarkers() {
        for (const MarkerSpec& marker : _spec.markers) {
            const Model* model = _ctx.AllModels.GetModel(marker.target);
            if (model == nullptr || marker.startMS < 0 || marker.endMS <= marker.startMS || (_spec.durationMS > 0 && marker.endMS > _spec.durationMS)) {
                Raise(_report, issue::MarkerInvalid, IssueSeverity::Error, IssueCategory::Configuration, marker.target, "", marker.startMS, marker.endMS, { { "id", marker.id } });
                continue;
            }
            const std::vector<uint32_t> channels = ResolveTargetChannels(_ctx, marker.target).channels;
            for (const EffectSpec& effect : _spec.effects) {
                if (effect.startMS >= marker.endMS || effect.endMS <= marker.startMS || effect.effect == "Off") {
                    continue;
                }
                const std::vector<uint32_t> effectChannels = ResolveTargetChannels(_ctx, FullTarget(effect)).channels;
                std::vector<uint32_t> shared;
                std::set_intersection(channels.begin(), channels.end(), effectChannels.begin(), effectChannels.end(), std::back_inserter(shared));
                if (!shared.empty()) {
                    Raise(_report, issue::MarkerProducesLight, IssueSeverity::Error, IssueCategory::Configuration, marker.target, effect.effect, marker.startMS, marker.endMS,
                          { { "id", marker.id }, { "by", FullTarget(effect) } });
                }
            }
        }
    }

    xLightsShowContext& _ctx;
    const SequenceSpec& _spec;
    const Strictness _strictness;
    const CancelCheck& _cancel;
    ValidationReport& _report;
    std::set<std::string> _missingTargets;
    std::set<std::string> _checkedGroups;
};

// The checks xLights already has, translated to issues rather than repeated. Needs
// real Effect objects, so it runs from stage 2 on.
void RunEffectChecks(xLightsShowContext& ctx, const CancelCheck& cancel, ValidationReport& report) {
    SequenceElements& elements = ctx.GetSequenceElements();
    EffectManager& manager = ctx.GetEffectManager();
    const auto checkElement = [&](Element& element, const std::string& fullName) {
        Model* model = ctx.AllModels.GetModel(fullName);
        if (model == nullptr) {
            return;
        }
        for (size_t l = 0; l < element.GetEffectLayerCount(); ++l) {
            for (Effect* effect : element.GetEffectLayer(static_cast<int>(l))->GetEffects()) {
                if (cancel && cancel()) {
                    report.cancelled = true;
                    return;
                }
                RenderableEffect* renderable = manager.GetEffect(effect->GetEffectName());
                if (renderable == nullptr || effect->IsRenderDisabled()) {
                    continue;
                }
                // Same contract as SequenceChecker: the checks walk the sequence through
                // the effect, and Faces resolves a group to its first model.
                Model* checked = model;
                if (auto* group = dynamic_cast<ModelGroup*>(model); group != nullptr && effect->GetEffectName() == "Faces") {
                    checked = group->GetFirstModel();
                }
                if (checked == nullptr) {
                    continue;
                }
                SequenceElements* previous = renderable->GetSequenceElements();
                renderable->SetSequenceElements(&elements);
                const std::list<std::string> findings = renderable->CheckEffectSettings(effect->GetSettings(), ctx.GetCurrentMediaManager(), checked, effect, false);
                renderable->SetSequenceElements(previous);
                for (const std::string& finding : findings) {
                    const bool isError = finding.find("ERR:") != std::string::npos;
                    // Whoever already explained this effect in more detail keeps the error.
                    const bool explained = std::any_of(report.issues.begin(), report.issues.end(), [&](const ValidationIssue& i) {
                        return i.severity == IssueSeverity::Error && i.category == IssueCategory::Configuration && i.model == fullName && i.startMS == effect->GetStartTimeMS();
                    });
                    Raise(report, isError ? issue::EffectSettingsInvalid : issue::EffectSettingsWarning, isError && !explained ? IssueSeverity::Error : IssueSeverity::Warning,
                          IssueCategory::Configuration, fullName, effect->GetEffectName(), effect->GetStartTimeMS(), effect->GetEndTimeMS(), { { "key", "" }, { "value", "" }, { "reason", Trim(finding) } });
                }
            }
        }
    };
    for (size_t i = 0; i < elements.GetElementCount() && !report.cancelled; ++i) {
        Element* element = elements.GetElement(i);
        if (element->GetType() == ElementType::ELEMENT_TYPE_TIMING) {
            continue;
        }
        checkElement(*element, element->GetName());
        if (auto* modelElement = dynamic_cast<ModelElement*>(element)) {
            for (int s = 0; s < modelElement->GetSubModelCount() && !report.cancelled; ++s) {
                SubModelElement* sub = modelElement->GetSubModel(s);
                if (sub->GetType() == ElementType::ELEMENT_TYPE_SUBMODEL) {
                    checkElement(*sub, element->GetName() + "/" + sub->GetName());
                }
            }
        }
    }
}

void AddSpecMismatches(xLightsShowContext& ctx, const SequenceSpec& spec, ValidationReport& report) {
    for (const std::string& detail : VerifySequence(ctx, spec)) {
        Raise(report, issue::SequenceMismatch, IssueSeverity::Error, IssueCategory::Structural, "", "", -1, -1, { { "detail", detail } });
    }
}

// The loader only logs what it drops, so the names must be read from the file.
void CheckFileAgainstLayout(xLightsShowContext& ctx, const std::string& xsqPath, Strictness strictness, ValidationReport& report) {
    pugi::xml_document doc;
    if (!doc.load_file(xsqPath.c_str())) {
        Raise(report, issue::SequenceFileUnreadable, IssueSeverity::Error, IssueCategory::Structural, "", "", -1, -1, { { "file", xsqPath } });
        return;
    }
    const pugi::xml_node effects = doc.child("xsequence").child("ElementEffects");

    std::vector<std::string> missing;
    size_t modelElements = 0;
    for (pugi::xml_node element : effects.children("Element")) {
        if (std::string_view(element.attribute("type").as_string()) == "timing") {
            continue;
        }
        const std::string name = element.attribute("name").as_string();
        size_t effectCount = 0;
        for (pugi::xml_node layer : element.children()) {
            for (pugi::xml_node effect : layer.children("Effect")) {
                ++effectCount;
                const std::string effectName = effect.attribute("name").as_string();
                if (ctx.GetEffectManager().GetEffect(effectName) == nullptr) {
                    Raise(report, issue::EffectUnknown, IssueSeverity::Error, IssueCategory::Structural, name, effectName, effect.attribute("startTime").as_int(-1), effect.attribute("endTime").as_int(-1));
                }
            }
        }
        // xLights keeps an empty row for every model of the view it was saved with.
        if (effectCount == 0) {
            continue;
        }
        ++modelElements;
        const Model* model = ctx.AllModels[name];
        if (model == nullptr) {
            missing.push_back(name);
            Raise(report, issue::ModelNotFound, IssueSeverity::Error, IssueCategory::Structural, name, "", -1, -1, { { "showFolder", ctx.GetShowDirectory() }, { "effects", std::to_string(effectCount) } });
            continue;
        }
        for (pugi::xml_node layer : element.children("SubModelEffectLayer")) {
            const std::string subName = layer.attribute("name").as_string();
            if (layer.first_child() && !subName.empty() && model->GetSubModel(subName) == nullptr) {
                Raise(report, issue::SubModelNotFound, IssueSeverity::Error, IssueCategory::Structural, name + "/" + subName, "", -1, -1, { { "parent", name }, { "subModel", subName } });
            }
        }
    }

    if (!missing.empty() && missing.size() * 2 >= modelElements) {
        Raise(report, issue::LayoutIncompatible, IssueSeverity::Error, IssueCategory::Structural, "", "", -1, -1,
              { { "reason", "names" }, { "showFolder", ctx.GetShowDirectory() }, { "missing", std::to_string(missing.size()) }, { "total", std::to_string(modelElements) }, { "names", JoinNames(missing) } });
    }

    // Opening a sequence from one show while another is selected is the same mistake
    // seen from the other side: the file sits next to the layout it was written for.
    std::error_code ec;
    const fs::path show = fs::weakly_canonical(fs::path(ctx.GetShowDirectory()), ec);
    fs::path folder = fs::weakly_canonical(fs::path(xsqPath), ec).parent_path();
    bool insideShow = false;
    fs::path ownLayoutFolder;
    while (!folder.empty()) {
        if (folder == show) {
            insideShow = true;
            break;
        }
        if (ownLayoutFolder.empty() && FileExists((folder / "xlights_rgbeffects.xml").string())) {
            ownLayoutFolder = folder;
        }
        if (folder == folder.parent_path()) {
            break;
        }
        folder = folder.parent_path();
    }
    if (!insideShow && !ownLayoutFolder.empty()) {
        Raise(report, issue::ShowFolderMismatch, strictness == Strictness::Generated || !missing.empty() ? IssueSeverity::Error : IssueSeverity::Warning, IssueCategory::Structural, "", "", -1, -1,
              { { "showFolder", ctx.GetShowDirectory() }, { "sequenceFolder", ownLayoutFolder.string() } });
    }
}

struct EffectProbe {
    const EffectSpec* effect = nullptr;
    TargetChannels target;
    int firstFrame = 0;
    int endFrame = 0;
    int framesWithLight = 0;
    int maxLit = 0;
    int placeholderFrames = 0;
    bool checksPlaceholder = false;
    std::set<uint64_t> distinct;
    std::vector<bool> memberLit;
};

uint64_t HashChannels(const std::vector<uint8_t>& frame, const std::vector<uint32_t>& channels) {
    uint64_t hash = 1469598103934665603ULL;
    for (uint32_t channel : channels) {
        hash ^= frame[channel];
        hash *= 1099511628211ULL;
    }
    return hash;
}

// What a render shows when an image is missing: every node the same single primary.
bool IsPlaceholderFill(const std::vector<uint8_t>& frame, const std::vector<uint32_t>& channels) {
    if (channels.empty() || channels.size() % 3 != 0) {
        return false;
    }
    const std::array<uint8_t, 3> first = { frame[channels[0]], frame[channels[1]], frame[channels[2]] };
    const int full = (first[0] == 255) + (first[1] == 255) + (first[2] == 255);
    const int dark = (first[0] == 0) + (first[1] == 0) + (first[2] == 0);
    if (full != 1 || dark != 2) {
        return false;
    }
    for (size_t i = 0; i < channels.size(); i += 3) {
        if (frame[channels[i]] != first[0] || frame[channels[i + 1]] != first[1] || frame[channels[i + 2]] != first[2]) {
            return false;
        }
    }
    return true;
}

// Compares what the mouth nodes did at chosen instants against what the track and the
// face definition say they should have done.
class FaceRenderCheck {
public:
    FaceRenderCheck(xLightsShowContext& ctx, const SequenceSpec& spec, const EffectSpec& effect) :
        _effect(effect) {
        if (!SettingOf(effect, "E_CHOICE_Faces_Phoneme").empty()) {
            return;
        }
        const TimingTrackSpec* track = FindTrack(spec, SettingOf(effect, "E_CHOICE_Faces_TimingTrack"));
        if (track == nullptr || track->layers.size() < 3) {
            return; // the configuration stage reports this one
        }
        const FaceResolution face = ResolveFace(ctx, ctx.GetSequenceElements(), effect);
        if (face.model == nullptr) {
            return;
        }
        const int frameMS = std::max(1, spec.frameMS);
        int previousEnd = effect.startMS;
        for (const TimingMark& mark : track->layers[2]) {
            if (mark.endMS <= effect.startMS || mark.startMS >= effect.endMS) {
                continue;
            }
            AddGap(previousEnd, mark.startMS, frameMS);
            const int start = std::max(mark.startMS, effect.startMS);
            const int end = std::min(mark.endMS, effect.endMS);
            _samples.push_back({ (start + end) / 2 / frameMS, PhonemeOf(mark.label), start, end });
            previousEnd = end;
        }
        AddGap(previousEnd, effect.endMS, frameMS);

        _target = ResolveTargetChannels(ctx, FullTarget(effect)).channels;
        if (face.typeId != FACE_TYPE_NODE_RANGE || face.onGroup || face.onSubModel) {
            return;
        }
        const auto nodes = face.model->GetFaceInfoNodes().find(face.definition);
        if (nodes == face.model->GetFaceInfoNodes().end()) {
            return;
        }
        _model = face.model;
        std::set<int> ignored;
        for (const auto& [key, list] : nodes->second) {
            if (key.rfind("Eyes-", 0) == 0 || key.rfind("FaceOutline", 0) == 0) {
                ignored.insert(list.begin(), list.end());
            }
        }
        for (const auto& [key, list] : nodes->second) {
            if (key.rfind("Mouth-", 0) != 0) {
                continue;
            }
            std::string phoneme = key.substr(6);
            if (!phoneme.empty() && phoneme.back() == '2') {
                phoneme.pop_back();
            }
            for (int node : list) {
                if (!ignored.contains(node)) {
                    _shapes[phoneme].insert(node);
                    _mouthNodes.insert(node);
                }
            }
        }
    }

    void OnFrame(int frame, const std::vector<uint8_t>& data) {
        for (Sample& sample : _samples) {
            if (sample.frame != frame) {
                continue;
            }
            sample.taken = true;
            if (_model != nullptr) {
                const int perNode = _model->GetChanCountPerNode();
                for (int node : _mouthNodes) {
                    const int32_t first = _model->NodeStartChannel(node);
                    for (int c = 0; c < perNode && first >= 0 && static_cast<size_t>(first) + c < data.size(); ++c) {
                        if (data[static_cast<size_t>(first) + c] != 0) {
                            sample.mouth.insert(node);
                            break;
                        }
                    }
                }
            } else {
                sample.hash = HashChannels(data, _target);
            }
        }
    }

    void Report(ValidationReport& report, IssueSeverity severity) const {
        std::map<std::string, std::set<int>> observed;
        std::set<std::string> labels;
        std::set<uint64_t> hashes;
        int taken = 0;
        for (const Sample& sample : _samples) {
            if (!sample.taken || sample.label == "rest") {
                continue;
            }
            ++taken;
            labels.insert(sample.label);
            hashes.insert(sample.hash);
            observed.emplace(sample.label, sample.mouth);
        }
        if (_model == nullptr) {
            if (labels.size() > 1 && hashes.size() < 2) {
                RaiseFor(report, _effect, issue::FaceMouthStatic, severity, IssueCategory::Render, { { "phonemes", std::to_string(labels.size()) } });
            }
            return;
        }

        std::set<std::set<int>> expectedShapes;
        std::set<std::set<int>> observedShapes;
        for (const std::string& label : labels) {
            const auto shape = _shapes.find(label);
            if (shape != _shapes.end()) {
                expectedShapes.insert(shape->second);
                observedShapes.insert(observed[label]);
            }
        }
        if (expectedShapes.size() > 1 && observedShapes.size() < 2) {
            RaiseFor(report, _effect, issue::FaceMouthStatic, severity, IssueCategory::Render, { { "phonemes", std::to_string(labels.size()) } });
            return;
        }

        int mismatches = 0;
        for (const Sample& sample : _samples) {
            if (!sample.taken || mismatches >= MAX_MOUTH_MISMATCHES) {
                continue;
            }
            const auto shape = _shapes.find(sample.label);
            const std::set<int> expected = shape == _shapes.end() ? std::set<int>() : shape->second;
            if (shape == _shapes.end() && sample.label != "rest") {
                continue;
            }
            if (sample.mouth != expected) {
                ++mismatches;
                Raise(report, sample.label == "rest" ? issue::FaceRestMismatch : issue::FaceMouthMismatch, severity, IssueCategory::Render, FullTarget(_effect), _effect.effect, sample.startMS, sample.endMS,
                      { { "phoneme", sample.label }, { "lit", std::to_string(sample.mouth.size()) }, { "expected", std::to_string(expected.size()) } });
            }
        }
    }

private:
    struct Sample {
        int frame = 0;
        std::string label;
        int startMS = 0;
        int endMS = 0;
        bool taken = false;
        std::set<int> mouth;
        uint64_t hash = 0;
    };

    void AddGap(int start, int end, int frameMS) {
        if (end - start >= 2 * frameMS) {
            _samples.push_back({ (start + end) / 2 / frameMS, "rest", start, end });
        }
    }

    const EffectSpec& _effect;
    const Model* _model = nullptr;
    std::vector<uint32_t> _target;
    std::vector<Sample> _samples;
    std::map<std::string, std::set<int>> _shapes;
    std::set<int> _mouthNodes;
};

struct UnexpectedRange {
    int firstFrame = 0;
    int lastFrame = 0;
    int frames = 0;
};

} // namespace

namespace {

double PhonemeShare(const std::vector<TimingMark>& layer) {
    if (layer.empty()) {
        return 0;
    }
    const auto known = std::count_if(layer.begin(), layer.end(), [](const TimingMark& m) { return IsKnownPhoneme(m.label); });
    return static_cast<double>(known) / static_cast<double>(layer.size());
}

bool Contains(const std::vector<TimingMark>& outer, const TimingMark& inner) {
    return std::any_of(outer.begin(), outer.end(), [&](const TimingMark& o) { return inner.startMS >= o.startMS && inner.endMS <= o.endMS; });
}

} // namespace

ValidationReport ValidateLyricTrack(const TimingTrackSpec& track, int durationMS, int frameMS) {
    ValidationReport report;
    report.configurationRan = true;
    const auto raise = [&](const char* code, IssueSeverity severity, int start, int end, Args args) {
        args.insert(args.begin(), { "track", track.name });
        Raise(report, code, severity, IssueCategory::Configuration, "", "", start, end, std::move(args));
    };
    if (track.layers.size() < 3) {
        raise(issue::LyricsTrackIncomplete, IssueSeverity::Error, -1, -1, { { "missing", track.layers.size() < 2 ? "words" : "phonemes" }, { "layers", std::to_string(track.layers.size()) } });
        return report;
    }
    static const char* const LAYER_NAMES[] = { "phrases", "words", "phonemes" };
    for (size_t l = 0; l < 3; ++l) {
        int previousEnd = 0;
        for (const TimingMark& mark : track.layers[l]) {
            if (mark.startMS < 0 || mark.endMS <= mark.startMS || (durationMS > 0 && mark.endMS > durationMS) || mark.startMS < previousEnd) {
                raise(issue::LyricsTimeInvalid, IssueSeverity::Error, mark.startMS, mark.endMS, { { "layer", LAYER_NAMES[l] }, { "label", mark.label } });
            }
            previousEnd = std::max(previousEnd, mark.endMS);
        }
    }
    const auto& phrases = track.layers[0];
    const auto& words = track.layers[1];
    const auto& phonemes = track.layers[2];
    // A track built in the wrong order still loads, and the face then reads words as
    // mouth shapes and stays at rest.
    if (!phonemes.empty() && PhonemeShare(phonemes) < 0.5 && (PhonemeShare(words) >= 0.5 || PhonemeShare(phrases) >= 0.5)) {
        raise(issue::LyricsLayerOrder, IssueSeverity::Error, -1, -1, {});
        return report;
    }
    if (!phrases.empty() && words.empty()) {
        raise(issue::LyricsTrackIncomplete, IssueSeverity::Error, -1, -1, { { "missing", "words" }, { "layers", "3" } });
    }
    if (!words.empty() && phonemes.empty()) {
        raise(issue::LyricsTrackIncomplete, IssueSeverity::Error, -1, -1, { { "missing", "phonemes" }, { "layers", "3" } });
    }
    for (const TimingMark& mark : phonemes) {
        if (!IsKnownPhoneme(mark.label)) {
            raise(issue::FacePhonemeUnknown, IssueSeverity::Error, mark.startMS, mark.endMS, { { "label", mark.label } });
        }
    }
    for (const TimingMark& word : words) {
        if (!Contains(phrases, word)) {
            raise(issue::LyricsOverlap, IssueSeverity::Error, word.startMS, word.endMS, { { "layer", "words" }, { "label", word.label } });
        }
    }
    for (const TimingMark& phoneme : phonemes) {
        if (PhonemeOf(phoneme.label) != "rest" && !Contains(words, phoneme)) {
            raise(issue::LyricsOverlap, IssueSeverity::Error, phoneme.startMS, phoneme.endMS, { { "layer", "phonemes" }, { "label", phoneme.label } });
        }
    }
    // Between two words the mouth must rest: a shape across the gap keeps it moving in
    // the silence, and a gap with no mark at all relies on the renderer's default.
    for (size_t w = 0; w + 1 < words.size(); ++w) {
        const int gapStart = words[w].endMS;
        const int gapEnd = words[w + 1].startMS;
        if (gapEnd - gapStart < std::max(1, frameMS)) {
            continue;
        }
        bool moving = false;
        bool rested = false;
        for (const TimingMark& phoneme : phonemes) {
            if (phoneme.startMS < gapEnd && phoneme.endMS > gapStart) {
                (PhonemeOf(phoneme.label) == "rest" ? rested : moving) = true;
            }
        }
        if (moving || !rested) {
            raise(issue::LyricsRestMissing, moving ? IssueSeverity::Error : IssueSeverity::Info, gapStart, gapEnd, { { "at", std::to_string(gapStart) } });
        }
    }
    return report;
}

ValidationReport ValidateLyrics(const LyricAlignment& alignment, const TimingTrackSpec& track, int durationMS, int frameMS) {
    ValidationReport report = ValidateLyricTrack(track, durationMS, frameMS);
    const auto raise = [&](const char* code, IssueSeverity severity, int start, int end, Args args) {
        args.insert(args.begin(), { "track", track.name });
        Raise(report, code, severity, IssueCategory::Configuration, "", "", start, end, std::move(args));
    };
    if (alignment.language != "es") {
        raise(issue::LyricsLanguageUnsupported, alignment.language == "en" ? IssueSeverity::Warning : IssueSeverity::Info, -1, -1, { { "language", alignment.language.empty() ? "und" : alignment.language } });
    }
    int unsynchronized = 0;
    int uncertain = 0;
    for (const AlignedPhrase& phrase : alignment.phrases) {
        if (phrase.instrumental) {
            continue;
        }
        if (phrase.startMS < 0) {
            ++unsynchronized;
            continue;
        }
        uncertain += phrase.confidence < 0.5 ? 1 : 0;
        const bool hasWords = std::any_of(alignment.words.begin(), alignment.words.end(), [&](const AlignedWord& w) { return w.phraseId == phrase.id; });
        if (!hasWords) {
            raise(issue::LyricsTrackIncomplete, IssueSeverity::Error, phrase.startMS, phrase.endMS, { { "missing", "words" }, { "layers", std::to_string(track.layers.size()) } });
        }
    }
    if (unsynchronized > 0) {
        raise(issue::LyricsNotSynchronized, IssueSeverity::Warning, -1, -1, { { "count", std::to_string(unsynchronized) } });
    }
    if (uncertain > 0) {
        raise(issue::LyricsLowConfidence, IssueSeverity::Warning, -1, -1, { { "count", std::to_string(uncertain) }, { "quality", alignment.quality } });
    }
    for (const AlignedWord& word : alignment.words) {
        const bool shaped = std::any_of(alignment.phonemes.begin(), alignment.phonemes.end(), [&](const AlignedPhoneme& p) { return p.wordId == word.id; });
        if (!shaped) {
            raise(issue::LyricsWordNoPhonemes, IssueSeverity::Warning, word.startMS, word.endMS, { { "word", word.text } });
        }
    }

    // A person's correction is honoured only if the result still says what they said,
    // to the frame.
    const int tolerance = std::max(1, frameMS) / 2;
    const auto near = [&](int value, int wanted) { return wanted < 0 || std::abs(value - wanted) <= tolerance; };
    for (const LyricCorrection& c : alignment.corrections) {
        const auto conflict = std::find_if(alignment.conflicts.begin(), alignment.conflicts.end(), [&](const AlignmentConflict& k) { return k.targetId == c.targetId; });
        if (conflict != alignment.conflicts.end()) {
            raise(issue::LyricsCorrectionConflict, IssueSeverity::Warning, c.startMS, c.endMS, { { "target", c.targetId }, { "reason", conflict->reason } });
            continue;
        }
        bool kept = false;
        if (c.level == LyricCorrection::Level::Phrase) {
            for (const AlignedPhrase& p : alignment.phrases) {
                if (p.id == c.targetId) {
                    kept = p.state == c.state && (c.startMS < 0 || (near(p.startMS, c.startMS) && near(p.endMS, c.endMS) && Contains(track.layers.empty() ? std::vector<TimingMark>() : track.layers[0], { "", p.startMS, p.endMS })));
                }
            }
        } else if (c.level == LyricCorrection::Level::Word) {
            for (const AlignedWord& w : alignment.words) {
                if (w.id == c.targetId) {
                    kept = w.state == c.state && (c.startMS < 0 || (near(w.startMS, c.startMS) && near(w.endMS, c.endMS)));
                }
            }
        } else {
            for (const AlignedPhoneme& p : alignment.phonemes) {
                if (p.id == c.targetId) {
                    kept = p.state == c.state && (c.shape.empty() || p.shape == c.shape) && (c.startMS < 0 || (near(p.startMS, c.startMS) && near(p.endMS, c.endMS)));
                }
            }
        }
        if (!kept) {
            raise(issue::LyricsCorrectionLost, IssueSeverity::Error, c.startMS, c.endMS, { { "target", c.targetId }, { "level", CorrectionLevelId(c.level) } });
        }
    }
    return report;
}

ValidationReport ValidateCompositionPlan(const CompositionPlan& plan, const ModelCapabilities& caps, const StyleProfile* profile) {
    ValidationReport report;
    report.structuralRan = true;
    report.configurationRan = true;
    const StyleProfile neutral = NeutralStarterProfile();
    const StyleProfile& p = profile != nullptr ? *profile : neutral;
    const LevelSettings& level = p.Level(plan.level);
    const auto raise = [&](const char* code, IssueSeverity severity, IssueCategory category, const std::string& target, int start, int end, Args args) {
        Raise(report, code, severity, category, target, "", start, end, std::move(args));
    };

    if (plan.layoutHash != caps.layoutFingerprint) {
        raise(issue::PlanHashMismatch, IssueSeverity::Error, IssueCategory::Structural, "", -1, -1, { { "what", "layout" }, { "expected", caps.layoutFingerprint }, { "found", plan.layoutHash } });
    }
    if (plan.analysisHash.empty() || plan.profileHash.empty()) {
        raise(issue::PlanHashMismatch, IssueSeverity::Error, IssueCategory::Structural, "", -1, -1, { { "what", plan.analysisHash.empty() ? "analysis" : "profile" }, { "expected", "a hash" }, { "found", "none" } });
    }

    int expectedStart = 0;
    for (const PlanSegment& s : plan.segments) {
        const Args where = { { "segment", s.id } };
        if (s.startMS != expectedStart || s.endMS <= s.startMS || s.endMS > plan.durationMS) {
            raise(issue::PlanSegmentInvalid, IssueSeverity::Error, IssueCategory::Structural, "", s.startMS, s.endMS, where);
        }
        expectedStart = s.endMS;
        const bool silence = std::find(s.flags.begin(), s.flags.end(), "silence") != s.flags.end();
        const bool byUser = std::any_of(s.reasons.begin(), s.reasons.end(), [](const Reason& r) { return r.code == "boundary-by-user"; }) || s.state != DecisionState::Automatic;
        const int length = s.endMS - s.startMS;
        if (!silence && !byUser && (length < p.minSegmentMS / 2 || length > p.maxSegmentMS * 3 / 2)) {
            raise(issue::PlanSegmentDuration, IssueSeverity::Warning, IssueCategory::Structural, "", s.startMS, s.endMS, { { "segment", s.id }, { "length", std::to_string(length) } });
        }
        int active = 0;
        bool lead = false;
        std::set<std::string> seen;
        std::string previous;
        for (const PlanAssignment& a : s.assignments) {
            if (!seen.insert(a.target).second || a.target < previous) {
                raise(issue::PlanOrderUnstable, IssueSeverity::Error, IssueCategory::Structural, a.target, s.startMS, s.endMS, where);
            }
            previous = a.target;
            const TargetInfo* t = caps.Find(a.target);
            if (t == nullptr) {
                raise(issue::PlanTargetUnknown, IssueSeverity::Error, IssueCategory::Structural, a.target, s.startMS, s.endMS, where);
                continue;
            }
            if (a.prominence == Prominence::Rest) {
                continue;
            }
            ++active;
            lead = lead || a.prominence == Prominence::Lead;
            if (caps.LightsExcluded(a.target)) {
                raise(issue::PlanTargetExcluded, IssueSeverity::Error, IssueCategory::Configuration, a.target, s.startMS, s.endMS, where);
            }
            const Capability needed = a.intent == "face-sing" ? Capability::Face : a.intent == "lyric-text" ? Capability::Text : a.intent == "media" ? Capability::Image : Capability::Geometric;
            const Support support = t->Can(needed).support;
            if (support == Support::No) {
                raise(issue::PlanRoleIncompatible, IssueSeverity::Error, IssueCategory::Configuration, a.target, s.startMS, s.endMS, { { "segment", s.id }, { "intent", a.intent }, { "capability", CapabilityId(needed) } });
            } else if (support == Support::Unknown || (support == Support::Limited && needed != Capability::Geometric)) {
                raise(issue::PlanCapabilityUnknown, IssueSeverity::Warning, IssueCategory::Configuration, a.target, s.startMS, s.endMS, { { "segment", s.id }, { "intent", a.intent }, { "capability", CapabilityId(needed) } });
            }
        }
        const int limit = static_cast<int>(std::lround(p.maxParticipants * level.densityScale)) + level.maxLeads + level.accents + level.maxFaces + 1;
        if (active > limit || s.density > 1.0) {
            raise(issue::PlanDensityExceeded, IssueSeverity::Error, IssueCategory::Configuration, "", s.startMS, s.endMS, { { "segment", s.id }, { "active", std::to_string(active) }, { "limit", std::to_string(limit) } });
        }
        if (active > 0 && !lead && !silence) {
            raise(issue::PlanNoLead, IssueSeverity::Warning, IssueCategory::Configuration, "", s.startMS, s.endMS, where);
        }
        // Two decisions on the same lights: what the profile allows is intentional,
        // two leads is a confirmed conflict, anything else is possible and reported.
        for (size_t i = 0; i < s.assignments.size(); ++i) {
            for (size_t j = i + 1; j < s.assignments.size(); ++j) {
                const PlanAssignment& a = s.assignments[i];
                const PlanAssignment& b = s.assignments[j];
                if (a.prominence == Prominence::Rest || b.prominence == Prominence::Rest || !caps.Overlap(a.target, b.target)) {
                    continue;
                }
                const bool bothLead = a.prominence == Prominence::Lead && b.prominence == Prominence::Lead;
                const char* classification = p.groupsMayOverlapMembers ? overlap::Intentional : bothLead ? overlap::Confirmed : overlap::Possible;
                const IssueSeverity severity = p.groupsMayOverlapMembers ? IssueSeverity::Info : bothLead ? IssueSeverity::Error : IssueSeverity::Warning;
                raise(issue::PlanGroupConflict, severity, IssueCategory::Configuration, a.target, s.startMS, s.endMS, { { "segment", s.id }, { "over", b.target }, { "classification", classification } });
            }
        }
    }
    if (expectedStart != plan.durationMS) {
        raise(issue::PlanSegmentInvalid, IssueSeverity::Error, IssueCategory::Structural, "", expectedStart, plan.durationMS, { { "segment", "end" } });
    }

    for (const PlanCorrection& c : plan.corrections) {
        if (c.action != PlanCorrection::Action::Lock) {
            continue;
        }
        for (const PlanSegment& s : plan.segments) {
            const int middle = (s.startMS + s.endMS) / 2;
            if (c.startMS >= 0 && (middle < c.startMS || middle >= c.endMS)) {
                continue;
            }
            for (const PlanAssignment& wanted : c.assignments) {
                const bool refused = std::any_of(plan.conflicts.begin(), plan.conflicts.end(), [&](const PlanConflict& k) { return k.target == wanted.target; });
                const bool kept = s.state == DecisionState::Locked && std::any_of(s.assignments.begin(), s.assignments.end(), [&](const PlanAssignment& a) {
                    return a.target == wanted.target && a.prominence == wanted.prominence && a.state == DecisionState::Locked;
                });
                if (!kept && !refused) {
                    raise(issue::PlanLockLost, IssueSeverity::Error, IssueCategory::Configuration, wanted.target, s.startMS, s.endMS, { { "segment", s.id } });
                }
            }
        }
    }
    for (const PlanConflict& c : plan.conflicts) {
        raise(issue::PlanManualConflict, IssueSeverity::Warning, IssueCategory::Configuration, c.target, c.startMS, c.endMS, { { "reason", c.reason } });
    }
    return report;
}

ValidationReport ValidateChoreography(const Choreography& c, const CompositionPlan& plan, const ModelCapabilities& caps, const TemplateLibrary& library, const std::function<const nlohmann::json*(const std::string&)>& metadata) {
    ValidationReport report;
    report.structuralRan = true;
    report.configurationRan = true;
    const auto raise = [&](const char* code, IssueSeverity severity, IssueCategory category, const ChoreographyEvent* e, Args args) {
        Raise(report, code, severity, category, e != nullptr ? e->target : "", e != nullptr ? e->effect : "", e != nullptr ? e->startMS : -1, e != nullptr ? e->endMS : -1, std::move(args));
    };
    if (c.planHash != HashText(ToJson(plan))) {
        raise(issue::ChoreoPlanMismatch, IssueSeverity::Error, IssueCategory::Structural, nullptr, {});
    }
    std::set<std::string> ids;
    int review = 0;
    int unvalidated = 0;
    for (size_t i = 0; i < c.events.size(); ++i) {
        const ChoreographyEvent& e = c.events[i];
        const Args id = { { "event", e.id } };
        if (!ids.insert(e.id).second || (i > 0 && (c.events[i - 1].startMS > e.startMS || (c.events[i - 1].startMS == e.startMS && c.events[i - 1].target > e.target)))) {
            raise(issue::ChoreoOrderUnstable, IssueSeverity::Error, IssueCategory::Structural, &e, id);
        }
        if (e.startMS < 0 || e.endMS <= e.startMS || e.endMS > c.durationMS || (c.frameMS > 0 && (e.startMS % c.frameMS != 0 || e.endMS % c.frameMS != 0))) {
            raise(issue::ChoreoTimeInvalid, IssueSeverity::Error, IssueCategory::Structural, &e, id);
        }
        const TargetInfo* target = caps.Find(e.target);
        if (target == nullptr) {
            raise(issue::ChoreoTargetUnknown, IssueSeverity::Error, IssueCategory::Structural, &e, id);
            continue;
        }
        if (caps.LightsExcluded(e.target)) {
            raise(issue::ChoreoTargetExcluded, IssueSeverity::Error, IssueCategory::Configuration, &e, id);
        }
        const EffectTemplate* t = library.Find(e.templateId);
        const nlohmann::json* md = metadata ? metadata(e.effect) : nullptr;
        if (t == nullptr || md == nullptr) {
            raise(issue::ChoreoTemplateUnknown, IssueSeverity::Error, IssueCategory::Configuration, &e, { { "event", e.id }, { "template", e.templateId } });
            continue;
        }
        if (t->effect != e.effect) {
            raise(issue::ChoreoEffectMismatch, IssueSeverity::Error, IssueCategory::Configuration, &e, { { "event", e.id }, { "template", t->id }, { "expected", t->effect } });
        }
        unvalidated += t->visuallyValidated ? 0 : 1;
        const CompatibilityResult compat = EvaluateCompatibility(*t, *target);
        if (compat.level == Compatibility::Incompatible) {
            raise(issue::ChoreoIncompatible, IssueSeverity::Error, IssueCategory::Configuration, &e, { { "event", e.id }, { "template", t->id }, { "reason", compat.reasons.empty() ? "" : compat.reasons.front() } });
        } else if (compat.level == Compatibility::Unknown) {
            raise(issue::ChoreoCapabilityUnknown, IssueSeverity::Warning, IssueCategory::Configuration, &e, { { "event", e.id }, { "template", t->id } });
        }
        const int length = e.endMS - e.startMS;
        if (length < t->minMS || length > t->maxMS) {
            raise(issue::ChoreoDuration, IssueSeverity::Warning, IssueCategory::Configuration, &e, { { "event", e.id }, { "length", std::to_string(length) }, { "min", std::to_string(t->minMS) }, { "max", std::to_string(t->maxMS) } });
        }
        std::string faceTrack;
        std::string faceDefinition;
        std::string text;
        bool file = false;
        for (const auto& [key, value] : e.settings) {
            std::string problem;
            if (key.rfind("E_", 0) == 0) {
                problem = CheckSetting(*md, key, value);
            } else if (key == "T_CHOICE_In_Transition_Type" || key == "T_CHOICE_Out_Transition_Type") {
                problem = std::find(TransitionTypes().begin(), TransitionTypes().end(), value) == TransitionTypes().end() ? "unknown transition" : "";
            } else if (key == "T_TEXTCTRL_Fadein" || key == "T_TEXTCTRL_Fadeout") {
                const double seconds = std::strtod(value.c_str(), nullptr);
                problem = seconds < 0 || seconds * 1000 > length ? "fade longer than the event" : "";
            } else {
                problem = "not a setting the choreography may write";
            }
            if (!problem.empty()) {
                raise(issue::ChoreoSettingInvalid, IssueSeverity::Error, IssueCategory::Configuration, &e, { { "event", e.id }, { "key", key }, { "value", value }, { "reason", problem } });
            }
            faceTrack = key == "E_CHOICE_Faces_TimingTrack" ? value : faceTrack;
            faceDefinition = key == "E_CHOICE_Faces_FaceDefinition" ? value : faceDefinition;
            text = key == "E_TEXTCTRL_Text" ? value : text;
            file = file || key == "E_TEXTCTRL_Pictures_Filename" || key == "E_FILEPICKER_Pictures_Filename" || key == "E_FILEPICKERCTRL_Video_Filename";
        }
        for (const std::string& color : e.colors) {
            if (color.size() != 7 || color[0] != '#' || color.find_first_not_of("0123456789ABCDEFabcdef", 1) != std::string::npos) {
                raise(issue::ChoreoPaletteInvalid, IssueSeverity::Error, IssueCategory::Configuration, &e, { { "event", e.id }, { "value", color } });
            }
        }
        if (e.colors.empty() && t->minColors > 0) {
            raise(issue::ChoreoPaletteInvalid, IssueSeverity::Error, IssueCategory::Configuration, &e, { { "event", e.id }, { "value", "none" } });
        }
        if (!t->media.empty() && !file) {
            raise(issue::ChoreoMediaMissing, IssueSeverity::Error, IssueCategory::Configuration, &e, { { "event", e.id }, { "kind", t->media } });
        }
        const bool knownTrack = faceTrack == c.lyricTrack || std::find(c.voiceTracks.begin(), c.voiceTracks.end(), faceTrack) != c.voiceTracks.end();
        if (e.effect == "Faces" && (faceTrack.empty() || !knownTrack || std::find(target->faces.begin(), target->faces.end(), faceDefinition) == target->faces.end())) {
            raise(issue::ChoreoLyricsTrackMissing, IssueSeverity::Error, IssueCategory::Configuration, &e, { { "event", e.id }, { "track", faceTrack } });
        }
        if (e.effect == "Text" && text.empty()) {
            raise(issue::ChoreoLyricsTrackMissing, IssueSeverity::Error, IssueCategory::Configuration, &e, { { "event", e.id }, { "track", "text" } });
        }
        review += e.needsReview || compat.level != Compatibility::Compatible ? 1 : 0;
    }
    // Same target and layer never overlap; different targets on the same lights are a
    // conflict, confirmed when both lead.
    for (size_t i = 0; i < c.events.size(); ++i) {
        for (size_t j = i + 1; j < c.events.size() && c.events[j].startMS < c.events[i].endMS; ++j) {
            const ChoreographyEvent& a = c.events[i];
            const ChoreographyEvent& b = c.events[j];
            if (a.target == b.target && a.layer == b.layer) {
                raise(issue::ChoreoOverlap, IssueSeverity::Error, IssueCategory::Structural, &b, { { "event", b.id }, { "over", a.id } });
            } else if (a.target != b.target && caps.Overlap(a.target, b.target)) {
                const bool lead = a.prominence == Prominence::Lead && b.prominence == Prominence::Lead;
                raise(issue::ChoreoGroupConflict, lead ? IssueSeverity::Error : IssueSeverity::Warning, IssueCategory::Configuration, &b, { { "event", b.id }, { "over", a.target } });
            }
        }
    }
    for (const ChoreographyCorrection& k : c.corrections) {
        if (k.action != ChoreographyCorrection::Action::Lock) {
            continue;
        }
        const bool refused = std::any_of(c.conflicts.begin(), c.conflicts.end(), [&](const ChoreographyConflict& x) { return x.target == k.event.target && x.startMS == k.event.startMS; });
        const bool kept = std::any_of(c.events.begin(), c.events.end(), [&](const ChoreographyEvent& e) {
            return e.state == DecisionState::Locked && e.target == k.event.target && e.templateId == k.event.templateId && e.settings == k.event.settings && e.paletteId == k.event.paletteId && std::abs(e.startMS - k.event.startMS) <= c.frameMS && std::abs(e.endMS - k.event.endMS) <= c.frameMS;
        });
        if (!kept && !refused) {
            Raise(report, issue::ChoreoLockLost, IssueSeverity::Error, IssueCategory::Configuration, k.event.target, k.event.effect, k.event.startMS, k.event.endMS, { { "event", k.event.id } });
        }
    }
    for (const ChoreographyConflict& k : c.conflicts) {
        Raise(report, issue::ChoreoManualConflict, IssueSeverity::Warning, IssueCategory::Configuration, k.target, "", k.startMS, k.endMS, { { "reason", k.reason } });
    }
    if (review > 0) {
        raise(issue::ChoreoNeedsReview, IssueSeverity::Info, IssueCategory::Configuration, nullptr, { { "count", std::to_string(review) } });
    }
    if (unvalidated > 0) {
        raise(issue::ChoreoVisualReview, IssueSeverity::Info, IssueCategory::Configuration, nullptr, { { "count", std::to_string(unvalidated) } });
    }
    return report;
}

ValidationReport ValidatePlan(xLightsShowContext& ctx, const SequenceSpec& spec, const CancelCheck& cancel) {
    ValidationReport report;
    report.structuralRan = true;
    report.configurationRan = true;
    SpecChecker(ctx, spec, Strictness::Generated, cancel, report).Run();
    return report;
}

ValidationReport ValidateBuilt(xLightsShowContext& ctx, const SequenceSpec& spec, const CancelCheck& cancel) {
    ValidationReport report;
    report.structuralRan = true;
    report.configurationRan = true;
    SequenceSpec built = SpecFromSequence(ctx);
    built.markers = spec.markers;
    built.layoutFingerprint = spec.layoutFingerprint;
    SpecChecker(ctx, built, Strictness::Generated, cancel, report).Run();
    if (!report.cancelled) {
        RunEffectChecks(ctx, cancel, report);
    }
    if (!report.cancelled) {
        AddSpecMismatches(ctx, spec, report);
    }
    return report;
}

ValidationReport ValidateSequenceFile(xLightsShowContext& ctx, const std::string& xsqPath, const SequenceSpec* spec, Strictness strictness, const CancelCheck& cancel) {
    ValidationReport report;
    report.structuralRan = true;
    report.configurationRan = true;
    CheckFileAgainstLayout(ctx, xsqPath, strictness, report);
    SequenceSpec loaded = SpecFromSequence(ctx);
    if (spec != nullptr) {
        loaded.markers = spec->markers;
        loaded.layoutFingerprint = spec->layoutFingerprint;
    }
    // CheckFileAgainstLayout already named what the loader dropped.
    SpecChecker(ctx, loaded, strictness, cancel, report).Run();
    if (!report.cancelled) {
        RunEffectChecks(ctx, cancel, report);
    }
    if (!report.cancelled && spec != nullptr) {
        AddSpecMismatches(ctx, *spec, report);
    }
    return report;
}

ValidationReport ValidateRender(xLightsShowContext& ctx, const std::string& fseqPath, const SequenceSpec& spec, Strictness strictness, const CancelCheck& cancel) {
    ValidationReport report;
    report.renderRan = true;
    const IssueSeverity strict = strictness == Strictness::Generated ? IssueSeverity::Error : IssueSeverity::Warning;

    ObtainAccessToURL(fseqPath, false);
    std::unique_ptr<FSEQFile> file(FSEQFile::openFSEQFile(fseqPath));
    if (file == nullptr) {
        Raise(report, issue::RenderFileUnreadable, IssueSeverity::Error, IssueCategory::Render, "", "", -1, -1, { { "file", fseqPath } });
        return report;
    }
    const int frameMS = std::max(1, spec.frameMS);
    const int expectedFrames = spec.durationMS / frameMS;
    const int frames = static_cast<int>(file->getNumFrames());
    if (file->getStepTime() != frameMS || frames != expectedFrames) {
        Raise(report, issue::RenderTimingMismatch, IssueSeverity::Error, IssueCategory::Render, "", "", -1, -1,
              { { "frames", std::to_string(frames) }, { "expectedFrames", std::to_string(expectedFrames) }, { "step", std::to_string(file->getStepTime()) }, { "expectedStep", std::to_string(frameMS) } });
    }
    const uint32_t maxChannels = static_cast<uint32_t>(file->getMaxChannel());

    const auto keepInFile = [&](std::vector<uint32_t>& channels) {
        const auto firstOutside = std::lower_bound(channels.begin(), channels.end(), maxChannels);
        const bool lost = firstOutside != channels.end();
        channels.erase(firstOutside, channels.end());
        return lost;
    };

    std::vector<EffectProbe> probes;
    std::vector<FaceRenderCheck> faces;
    probes.reserve(spec.effects.size());
    faces.reserve(spec.effects.size());
    for (const EffectSpec& effect : spec.effects) {
        if (effect.effect == "Off") {
            continue;
        }
        EffectProbe probe;
        probe.effect = &effect;
        probe.target = ResolveTargetChannels(ctx, FullTarget(effect));
        if (probe.target.channels.empty()) {
            continue; // the structural stages name the missing target
        }
        if (keepInFile(probe.target.channels)) {
            RaiseFor(report, effect, issue::RenderChannelsUnavailable, IssueSeverity::Warning, IssueCategory::Render, { { "fileChannels", std::to_string(maxChannels) } });
        }
        for (auto& member : probe.target.members) {
            keepInFile(member.second);
        }
        probe.firstFrame = effect.startMS / frameMS;
        probe.endFrame = effect.endMS / frameMS;
        probe.checksPlaceholder = effect.effect == "Pictures" || effect.effect == "Video" || effect.effect == "Faces";
        probe.memberLit.assign(probe.target.members.size(), false);
        probes.push_back(std::move(probe));
        if (effect.effect == "Faces") {
            faces.emplace_back(ctx, spec, effect);
        }
    }

    struct MarkerProbe {
        const MarkerSpec* marker;
        std::vector<uint32_t> channels;
        int litFrames = 0;
    };
    std::vector<MarkerProbe> markers;
    for (const MarkerSpec& marker : spec.markers) {
        MarkerProbe probe{ &marker, ResolveTargetChannels(ctx, marker.target).channels };
        keepInFile(probe.channels);
        markers.push_back(std::move(probe));
    }

    // Other channels are only legitimate while an effect covers them, so the set
    // is rebuilt whenever an effect starts or ends.
    std::vector<int> boundaries;
    for (const EffectProbe& probe : probes) {
        boundaries.push_back(probe.firstFrame);
        boundaries.push_back(probe.endFrame);
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

    file->setReadPattern(FSEQFile::ReadPattern::Bulk);
    std::vector<std::pair<uint32_t, uint32_t>> ranges{ { 0, maxChannels } };
    file->prepareRead(ranges);

    std::vector<uint8_t> data(maxChannels, 0);
    std::vector<uint8_t> allowed(maxChannels, 0);
    std::map<std::string, UnexpectedRange> unexpected;
    std::map<uint32_t, std::string> ownerOfChannel;
    auto boundary = boundaries.begin();
    for (int frame = 0; frame < frames; ++frame) {
        if (cancel && cancel()) {
            report.cancelled = true;
            return report;
        }
        std::fill(data.begin(), data.end(), 0);
        std::unique_ptr<FSEQFile::FrameData> frameData(file->getFrame(frame));
        if (frameData == nullptr || !frameData->readFrame(data.data(), maxChannels)) {
            Raise(report, issue::RenderFileUnreadable, IssueSeverity::Error, IssueCategory::Render, "", "", frame * frameMS, -1, { { "file", fseqPath } });
            return report;
        }

        if (frame == 0 || (boundary != boundaries.end() && frame >= *boundary)) {
            while (boundary != boundaries.end() && frame >= *boundary) {
                ++boundary;
            }
            std::fill(allowed.begin(), allowed.end(), 0);
            for (const EffectProbe& probe : probes) {
                if (frame >= probe.firstFrame && frame < probe.endFrame) {
                    for (uint32_t channel : probe.target.channels) {
                        allowed[channel] = 1;
                    }
                }
            }
        }

        for (EffectProbe& probe : probes) {
            if (frame < probe.firstFrame || frame >= probe.endFrame) {
                continue;
            }
            int lit = 0;
            for (uint32_t channel : probe.target.channels) {
                lit += data[channel] != 0 ? 1 : 0;
            }
            if (lit > 0) {
                ++probe.framesWithLight;
            }
            probe.maxLit = std::max(probe.maxLit, lit);
            if (probe.distinct.size() < MAX_DISTINCT_FRAMES) {
                probe.distinct.insert(HashChannels(data, probe.target.channels));
            }
            if (probe.checksPlaceholder && IsPlaceholderFill(data, probe.target.channels)) {
                ++probe.placeholderFrames;
            }
            for (size_t m = 0; m < probe.target.members.size(); ++m) {
                if (!probe.memberLit[m]) {
                    const auto& channels = probe.target.members[m].second;
                    probe.memberLit[m] = std::any_of(channels.begin(), channels.end(), [&](uint32_t c) { return data[c] != 0; });
                }
            }
        }
        for (MarkerProbe& probe : markers) {
            if (frame * frameMS >= probe.marker->startMS && frame * frameMS < probe.marker->endMS &&
                std::any_of(probe.channels.begin(), probe.channels.end(), [&](uint32_t c) { return data[c] != 0; })) {
                ++probe.litFrames;
            }
        }
        for (FaceRenderCheck& face : faces) {
            face.OnFrame(frame, data);
        }

        for (uint32_t channel = 0; channel < maxChannels; ++channel) {
            if (data[channel] != 0 && allowed[channel] == 0) {
                auto owner = ownerOfChannel.find(channel);
                if (owner == ownerOfChannel.end()) {
                    std::string names = ctx.AllModels.GetModelsOnChannels(channel + 1, channel + 1, 0);
                    owner = ownerOfChannel.emplace(channel, names.empty() ? fmt::format("channel {}", channel + 1) : std::move(names)).first;
                }
                auto [range, isNew] = unexpected.try_emplace(owner->second, UnexpectedRange{ frame, frame, 0 });
                if (range->second.lastFrame != frame || isNew) {
                    ++range->second.frames;
                }
                range->second.lastFrame = frame;
                break;
            }
        }
    }

    for (const EffectProbe& probe : probes) {
        const EffectSpec& effect = *probe.effect;
        report.metrics.push_back({ FullTarget(effect), effect.effect, effect.startMS, effect.endMS, probe.framesWithLight, static_cast<int>(probe.distinct.size()), probe.maxLit, static_cast<int>(probe.target.channels.size()) });
        if (probe.framesWithLight == 0) {
            RaiseFor(report, effect, issue::RenderTargetSilent, strict, IssueCategory::Render, { { "channels", std::to_string(probe.target.channels.size()) } });
            continue;
        }
        if (probe.placeholderFrames == probe.endFrame - probe.firstFrame) {
            RaiseFor(report, effect, issue::RenderPlaceholderOutput, strict, IssueCategory::Render);
        }
        std::vector<std::string> silent;
        for (size_t m = 0; m < probe.target.members.size(); ++m) {
            if (!probe.memberLit[m] && !probe.target.members[m].second.empty()) {
                silent.push_back(probe.target.members[m].first);
            }
        }
        if (!silent.empty()) {
            RaiseFor(report, effect, issue::RenderMemberSilent, IssueSeverity::Warning, IssueCategory::Render, { { "members", JoinNames(silent) } });
        }
    }
    for (const FaceRenderCheck& face : faces) {
        face.Report(report, strict);
    }
    for (const MarkerProbe& probe : markers) {
        if (probe.litFrames > 0) {
            Raise(report, issue::MarkerProducesLight, IssueSeverity::Error, IssueCategory::Render, probe.marker->target, "", probe.marker->startMS, probe.marker->endMS,
                  { { "id", probe.marker->id }, { "by", fmt::format("{} frames", probe.litFrames) } });
        }
    }
    for (const auto& [owner, range] : unexpected) {
        Raise(report, issue::UnexpectedOutput, strict, IssueCategory::Render, owner, "", range.firstFrame * frameMS, (range.lastFrame + 1) * frameMS, { { "frames", std::to_string(range.frames) } });
    }
    return report;
}

SequenceSpec SpecFromSequence(xLightsShowContext& ctx) {
    SequenceSpec spec;
    SequenceElements& elements = ctx.GetSequenceElements();
    spec.frameMS = elements.GetFrameMS();
    // Only the desktop UI and the writer set the end on the elements. An opened file
    // has it in the sequence data, and whatever the elements hold may be older.
    spec.durationMS = ctx.IsSequenceLoaded() ? static_cast<int>(ctx.GetSeqData().NumFrames() * ctx.GetSeqData().FrameTime()) : elements.GetSequenceEnd();
    spec.modelBlending = elements.SupportsModelBlending();
    if (const AudioManager* media = ctx.GetCurrentMediaManager()) {
        spec.mediaFile = media->FileName();
    }

    const auto addLayers = [&](const Element& element, const std::string& target, const std::string& subModel) {
        for (size_t l = 0; l < element.GetEffectLayerCount(); ++l) {
            for (Effect* effect : element.GetEffectLayer(static_cast<int>(l))->GetEffects()) {
                if (effect->IsRenderDisabled()) {
                    continue;
                }
                EffectSpec out;
                out.target = target;
                out.subModel = subModel;
                out.layer = static_cast<int>(l);
                out.effect = effect->GetEffectName();
                for (const auto& [key, value] : effect->GetSettings()) {
                    out.settings.emplace_back(std::string(key), std::string(value));
                }
                out.palette = effect->GetPaletteAsString();
                out.startMS = effect->GetStartTimeMS();
                out.endMS = effect->GetEndTimeMS();
                spec.effects.push_back(std::move(out));
            }
        }
    };

    for (size_t i = 0; i < elements.GetElementCount(); ++i) {
        Element* element = elements.GetElement(i);
        if (element->GetType() == ElementType::ELEMENT_TYPE_TIMING) {
            TimingTrackSpec track;
            track.name = element->GetName();
            for (size_t l = 0; l < element->GetEffectLayerCount(); ++l) {
                std::vector<TimingMark> marks;
                for (Effect* mark : element->GetEffectLayer(static_cast<int>(l))->GetEffects()) {
                    marks.push_back({ mark->GetEffectName(), mark->GetStartTimeMS(), mark->GetEndTimeMS() });
                }
                track.layers.push_back(std::move(marks));
            }
            spec.timingTracks.push_back(std::move(track));
            continue;
        }
        addLayers(*element, element->GetName(), "");
        if (auto* modelElement = dynamic_cast<ModelElement*>(element)) {
            for (int s = 0; s < modelElement->GetSubModelCount(); ++s) {
                SubModelElement* sub = modelElement->GetSubModel(s);
                if (sub->GetType() == ElementType::ELEMENT_TYPE_SUBMODEL) {
                    addLayers(*sub, element->GetName(), sub->GetName());
                }
            }
        }
    }
    return spec;
}

std::string LayoutFingerprint(const xLightsShowContext& ctx) {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&](const std::string& s) {
        for (unsigned char c : s) {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
    };
    // ModelManager iterates a std::map, so the order is stable for equal layouts.
    for (const auto& [name, model] : ctx.AllModels) {
        mix(name);
        if (model->GetDisplayAs() != DisplayAsType::ModelGroup) {
            mix(fmt::format(":{}:{};", model->GetNodeCount(), model->GetFirstChannel()));
        }
    }
    return fmt::format("{:016x}", hash);
}

namespace {

struct IssueText {
    const char* code;
    const char* message;
    const char* suggestion;
};

// Marks a msgid for ci_scripts/localization/build_catalog.py; the core never translates.
#define COMPOSER_TEXT(text) text

// Worded for someone who does not know xLights' internals. These are the msgids a
// translated UI will look up, so a change of wording here is a change of msgid.
const IssueText ISSUE_TEXTS[] = {
    { issue::ModelNotFound, COMPOSER_TEXT("The model \"{model}\" does not exist in the layout of the selected show folder \"{showFolder}\"."), COMPOSER_TEXT("Open the sequence with the show folder it was created for, or restore this model in the layout.") },
    { issue::GroupNotFound, COMPOSER_TEXT("The group \"{model}\" does not exist in the layout of the selected show folder \"{showFolder}\"."), COMPOSER_TEXT("Choose an existing group or create this one in the layout.") },
    { issue::SubModelNotFound, COMPOSER_TEXT("The model \"{parent}\" has no submodel called \"{subModel}\"."), COMPOSER_TEXT("Choose an existing submodel or create this one on the model.") },
    { issue::TargetKindMismatch, COMPOSER_TEXT("\"{model}\" exists, but it is not a {expected} as the plan expected."), COMPOSER_TEXT("Check that the layout is the one the plan was made for.") },
    { issue::GroupMemberInvalid, COMPOSER_TEXT("The group \"{model}\" refers to members that do not exist: {members}."), COMPOSER_TEXT("Remove those members from the group or restore the models.") },
    { issue::GroupEmpty, COMPOSER_TEXT("The group \"{model}\" has no valid members, so nothing can light up."), COMPOSER_TEXT("Add at least one existing model to the group.") },
    { issue::LayoutIncompatible, COMPOSER_TEXT("This sequence does not match the layout of the show folder \"{showFolder}\" (reason: {reason}). Missing {missing} of {total} targets: {names}."), COMPOSER_TEXT("Select the show folder this sequence was generated for before opening it.") },
    { issue::ShowFolderMismatch, COMPOSER_TEXT("The sequence belongs to the show folder \"{sequenceFolder}\", but the selected show folder is \"{showFolder}\"."), COMPOSER_TEXT("Select \"{sequenceFolder}\" as the show folder and open the sequence again.") },
    { issue::SequenceFileUnreadable, COMPOSER_TEXT("The sequence file \"{file}\" cannot be read."), COMPOSER_TEXT("Generate the sequence again.") },
    { issue::SequenceMismatch, COMPOSER_TEXT("The saved sequence differs from what was planned: {detail}."), COMPOSER_TEXT("Generate the sequence again and report the problem if it repeats.") },
    { issue::EffectUnknown, COMPOSER_TEXT("The effect \"{effect}\" on \"{model}\" does not exist in this version of xLights, and would be dropped."), COMPOSER_TEXT("Choose another effect.") },
    { issue::EffectIntervalInvalid, COMPOSER_TEXT("The effect \"{effect}\" on \"{model}\" has invalid times for a sequence of {duration} ms."), COMPOSER_TEXT("Correct its start and end so that they fall inside the song.") },
    { issue::EffectOverlapInLayer, COMPOSER_TEXT("Two effects overlap on layer {layer} of \"{model}\"."), COMPOSER_TEXT("Move one of them to another layer or shorten it.") },
    { issue::TimingInvalid, COMPOSER_TEXT("The mark \"{label}\" of the track \"{track}\" has invalid times or overlaps the previous one."), COMPOSER_TEXT("Correct the times of that mark.") },
    { issue::TimingOffGrid, COMPOSER_TEXT("\"{label}\" does not start or end on a frame, so xLights will move it slightly."), COMPOSER_TEXT("Align it with the sequence's frames.") },
    { issue::EffectSettingsInvalid, COMPOSER_TEXT("The effect \"{effect}\" on \"{model}\" has an invalid setting {key}={value}: {reason}."), COMPOSER_TEXT("Correct that setting in the effect.") },
    { issue::EffectSettingUnknown, COMPOSER_TEXT("The effect \"{effect}\" on \"{model}\" has a setting xLights does not know: {key}."), COMPOSER_TEXT("Remove that setting.") },
    { issue::EffectSettingsWarning, COMPOSER_TEXT("xLights warns about the effect \"{effect}\" on \"{model}\": {reason}."), COMPOSER_TEXT("Check that it looks as you expect.") },
    { issue::PaletteInvalid, COMPOSER_TEXT("Colour {slot} of the effect \"{effect}\" on \"{model}\" is not valid: {value}."), COMPOSER_TEXT("Choose the colour again.") },
    { issue::PaletteEmpty, COMPOSER_TEXT("The effect \"{effect}\" on \"{model}\" has no visible colour selected, so it may stay dark."), COMPOSER_TEXT("Select at least one colour other than black.") },
    { issue::BufferStyleInvalid, COMPOSER_TEXT("The model \"{model}\" does not support the render style \"{style}\"."), COMPOSER_TEXT("Choose one of the styles the model offers.") },
    { issue::MediaFileMissing, COMPOSER_TEXT("The effect \"{effect}\" on \"{model}\" needs a file that cannot be found: \"{file}\"."), COMPOSER_TEXT("Add the file to the show folder, or remove the effect until you have it.") },
    { issue::FaceTrackMissing, COMPOSER_TEXT("The face on \"{model}\" uses the lyrics track \"{track}\", which does not exist, so the mouth will not move."), COMPOSER_TEXT("Choose an existing lyrics track.") },
    { issue::FaceInsufficientLayers, COMPOSER_TEXT("The face on \"{model}\" uses the lyrics track \"{track}\", which only has {layers} layers. Three layers are needed to move the mouth, so it will stay still."), COMPOSER_TEXT("Generate the phrases, words and phonemes of that track.") },
    { issue::FaceNoPhonemes, COMPOSER_TEXT("The lyrics track \"{track}\" has no phonemes while the face on \"{model}\" is active, so the mouth will not move."), COMPOSER_TEXT("Generate the phonemes for that part of the song.") },
    { issue::FaceNoWords, COMPOSER_TEXT("The lyrics track \"{track}\" has no words layer."), COMPOSER_TEXT("Add the words so that the mouth movements can be reviewed.") },
    { issue::FacePhonemeUnknown, COMPOSER_TEXT("The lyrics track \"{track}\" contains \"{label}\", which is not a mouth shape."), COMPOSER_TEXT("Replace it with a valid phoneme.") },
    { issue::FacePhonemeOutsideWord, COMPOSER_TEXT("The lyrics track \"{track}\" moves the mouth with \"{label}\" outside any word, where it should be resting."), COMPOSER_TEXT("Remove that phoneme or adjust the word's times.") },
    { issue::FaceDefinitionNotFound, COMPOSER_TEXT("The model \"{model}\" has no face called \"{definition}\", and xLights would silently use \"{substitute}\"."), COMPOSER_TEXT("Choose one of the model's faces.") },
    { issue::FaceDefinitionIncompatible, COMPOSER_TEXT("The face \"{definition}\" of type {type} cannot be drawn on the group \"{model}\", so it will not light up."), COMPOSER_TEXT("Apply the face to the model itself, or use an image face on the group.") },
    { issue::FaceDefinitionEmpty, COMPOSER_TEXT("The face \"{definition}\" on \"{model}\" defines no mouth, so nothing will move."), COMPOSER_TEXT("Define the mouth shapes of that face.") },
    { issue::FaceImageMissing, COMPOSER_TEXT("The face \"{definition}\" on \"{model}\" needs {count} images that cannot be found, and xLights would fill the model with red: {files}."), COMPOSER_TEXT("Add the images to the show folder or choose a face that does not need them.") },
    { issue::TextEmpty, COMPOSER_TEXT("The text effect on \"{model}\" has nothing to show."), COMPOSER_TEXT("Write a text or choose a lyrics track.") },
    { issue::TextUtf8Issue, COMPOSER_TEXT("The text effect on \"{model}\" cannot show its text correctly with the font \"{font}\" (reason: {reason})."), COMPOSER_TEXT("Use an operating system font, which can draw accented letters.") },
    { issue::TextLyricTrackInvalid, COMPOSER_TEXT("The text effect on \"{model}\" refers to the lyrics \"{value}\", which will show nothing (reason: {reason})."), COMPOSER_TEXT("Choose the phrases or the words of an existing lyrics track.") },
    { issue::GroupMemberConflict, COMPOSER_TEXT("\"{model}\" and \"{over}\" are active at the same time on the same lights ({classification})."), COMPOSER_TEXT("Check that the result is what you want; if \"{model}\" must be seen, move it or change how they mix.") },
    { issue::MarkerInvalid, COMPOSER_TEXT("The pending resource {id} has invalid times or refers to \"{model}\", which does not exist."), COMPOSER_TEXT("Correct the marker.") },
    { issue::MarkerProducesLight, COMPOSER_TEXT("The pending resource {id} on \"{model}\" should stay dark, but it is lit by {by}."), COMPOSER_TEXT("Remove what lights it, or supply the resource.") },
    { issue::RenderFileUnreadable, COMPOSER_TEXT("The rendered file \"{file}\" cannot be read."), COMPOSER_TEXT("Render the sequence again.") },
    { issue::RenderTimingMismatch, COMPOSER_TEXT("The render has {frames} frames of {step} ms, and {expectedFrames} of {expectedStep} ms were expected."), COMPOSER_TEXT("Render the sequence again.") },
    { issue::RenderChannelsUnavailable, COMPOSER_TEXT("Part of \"{model}\" is beyond the {fileChannels} channels of the rendered file and could not be checked."), COMPOSER_TEXT("Check that the render includes all models.") },
    { issue::RenderTargetSilent, COMPOSER_TEXT("\"{model}\" stayed dark while its effect \"{effect}\" should have been lighting it."), COMPOSER_TEXT("Review the effect's settings and colours, and whether another model covers it.") },
    { issue::RenderMemberSilent, COMPOSER_TEXT("Some members of \"{model}\" stayed dark during \"{effect}\": {members}."), COMPOSER_TEXT("Check the group's render style and those models.") },
    { issue::RenderPlaceholderOutput, COMPOSER_TEXT("\"{model}\" is filled with one pure colour during \"{effect}\", which is what xLights does when an image is missing."), COMPOSER_TEXT("Check the effect's images.") },
    { issue::UnexpectedOutput, COMPOSER_TEXT("\"{model}\" lit up for {frames} frames when no effect should have been lighting it."), COMPOSER_TEXT("Find the effect that reaches those lights.") },
    { issue::FaceMouthStatic, COMPOSER_TEXT("The mouth on \"{model}\" does not change across {phonemes} different phonemes."), COMPOSER_TEXT("Check the lyrics track and the face definition.") },
    { issue::FaceMouthMismatch, COMPOSER_TEXT("The mouth on \"{model}\" does not show \"{phoneme}\" as defined: {lit} lights on instead of {expected}."), COMPOSER_TEXT("Check the face definition, and whether another effect covers it.") },
    { issue::FaceRestMismatch, COMPOSER_TEXT("The mouth on \"{model}\" is not at rest during a pause: {lit} lights on instead of {expected}."), COMPOSER_TEXT("Check the lyrics track around that pause.") },
    { issue::LyricsWordNoPhonemes, COMPOSER_TEXT("The word \"{word}\" in the lyrics track \"{track}\" has no mouth shapes, so the mouth will rest while it is sung."), COMPOSER_TEXT("Write the word in letters, or add its pronunciation as an exception.") },
    { issue::LyricsLanguageUnsupported, COMPOSER_TEXT("The lyrics of \"{track}\" look like language \"{language}\", but mouth shapes are only generated for Spanish."), COMPOSER_TEXT("Check the mouth movements, or use a lyrics track made for that language.") },
    { issue::LyricsNotSynchronized, COMPOSER_TEXT("{count} phrases of the lyrics track \"{track}\" have no times yet, so the mouth will not move during them."), COMPOSER_TEXT("Mark when those phrases start and end while listening to the song.") },
    { issue::LyricsTrackIncomplete, COMPOSER_TEXT("The lyrics track \"{track}\" has no {missing} ({layers} layers), so the mouth cannot follow it."), COMPOSER_TEXT("Generate the words and phonemes of that track again.") },
    { issue::LyricsTimeInvalid, COMPOSER_TEXT("The {layer} mark \"{label}\" of the lyrics track \"{track}\" has invalid times or overlaps the previous one."), COMPOSER_TEXT("Correct the times of that mark.") },
    { issue::LyricsOverlap, COMPOSER_TEXT("The {layer} mark \"{label}\" of the lyrics track \"{track}\" falls outside the phrase or word it belongs to."), COMPOSER_TEXT("Adjust the times so that every word stays inside its phrase and every mouth shape inside its word.") },
    { issue::LyricsRestMissing, COMPOSER_TEXT("In the lyrics track \"{track}\" the mouth does not rest in the pause at {at} ms."), COMPOSER_TEXT("Put a rest in that pause.") },
    { issue::LyricsLayerOrder, COMPOSER_TEXT("The layers of the lyrics track \"{track}\" are in the wrong order: the face would read words as mouth shapes."), COMPOSER_TEXT("The layers must be phrases, words and phonemes, in that order.") },
    { issue::LyricsLowConfidence, COMPOSER_TEXT("{count} phrases of the lyrics track \"{track}\" have uncertain times ({quality})."), COMPOSER_TEXT("Listen to them and confirm or correct their start and end.") },
    { issue::LyricsCorrectionLost, COMPOSER_TEXT("A {level} correction made by hand on \"{target}\" was not kept in the lyrics track \"{track}\"."), COMPOSER_TEXT("Generate the track again; if it repeats, report the problem.") },
    { issue::LyricsCorrectionConflict, COMPOSER_TEXT("A correction made by hand on \"{target}\" could not be applied to the lyrics track \"{track}\" ({reason}). It has been kept, not deleted."), COMPOSER_TEXT("Check whether that part of the lyrics was changed, and mark it again.") },
    { issue::PlanHashMismatch, COMPOSER_TEXT("The composition plan was made for another {what} (expected {expected}, found {found})."), COMPOSER_TEXT("Make the plan again for this show and this song.") },
    { issue::PlanSegmentInvalid, COMPOSER_TEXT("The plan segment {segment} has invalid times or leaves a gap in the song."), COMPOSER_TEXT("Make the plan again; if it repeats, report the problem.") },
    { issue::PlanSegmentDuration, COMPOSER_TEXT("The plan segment {segment} lasts {length} ms, far from what the profile allows."), COMPOSER_TEXT("Check the profile's segment lengths, or move the segment's limits.") },
    { issue::PlanOrderUnstable, COMPOSER_TEXT("The decisions of plan segment {segment} are repeated or out of order, so two plans could not be compared."), COMPOSER_TEXT("Make the plan again; if it repeats, report the problem.") },
    { issue::PlanTargetUnknown, COMPOSER_TEXT("The plan uses \"{model}\" in segment {segment}, which does not exist in this layout."), COMPOSER_TEXT("Make the plan again for the current layout.") },
    { issue::PlanTargetExcluded, COMPOSER_TEXT("The plan uses \"{model}\" in segment {segment}, but it was excluded from the show."), COMPOSER_TEXT("Leave it resting, or include it again in the roles file.") },
    { issue::PlanRoleIncompatible, COMPOSER_TEXT("\"{model}\" is asked to do \"{intent}\" in segment {segment}, but it cannot ({capability})."), COMPOSER_TEXT("Give that action to a figure that can do it.") },
    { issue::PlanCapabilityUnknown, COMPOSER_TEXT("Whether \"{model}\" can do \"{intent}\" in segment {segment} is not known until it is rendered ({capability})."), COMPOSER_TEXT("Check it in the preview once the effects exist.") },
    { issue::PlanDensityExceeded, COMPOSER_TEXT("Segment {segment} has {active} figures active, more than the {limit} the profile allows."), COMPOSER_TEXT("Rest some of them, or raise the profile's density.") },
    { issue::PlanGroupConflict, COMPOSER_TEXT("\"{model}\" and \"{over}\" share lights in segment {segment} ({classification})."), COMPOSER_TEXT("Keep only one of them active, unless the overlap is intended.") },
    { issue::PlanLockLost, COMPOSER_TEXT("A decision locked by hand on \"{model}\" in segment {segment} is no longer in the plan."), COMPOSER_TEXT("Make the plan again; if it repeats, report the problem.") },
    { issue::PlanManualConflict, COMPOSER_TEXT("A manual decision on \"{model}\" could not be applied ({reason}). It was not applied silently."), COMPOSER_TEXT("Change the decision, or the figure's roles, so that they agree.") },
    { issue::PlanNoLead, COMPOSER_TEXT("Segment {segment} has figures active but none leading."), COMPOSER_TEXT("Choose a lead for that segment.") },
    { issue::ChoreoPlanMismatch, COMPOSER_TEXT("The choreography was made from another composition plan."), COMPOSER_TEXT("Make the choreography again from the current plan.") },
    { issue::ChoreoOrderUnstable, COMPOSER_TEXT("The choreography event {event} is repeated or out of order, so two choreographies could not be compared."), COMPOSER_TEXT("Make the choreography again; if it repeats, report the problem.") },
    { issue::ChoreoTargetUnknown, COMPOSER_TEXT("The choreography event {event} uses \"{model}\", which does not exist in this layout."), COMPOSER_TEXT("Make the choreography again for the current layout.") },
    { issue::ChoreoTargetExcluded, COMPOSER_TEXT("The choreography event {event} lights \"{model}\", which was excluded from the show."), COMPOSER_TEXT("Remove that event, or include the figure again in the roles file.") },
    { issue::ChoreoTemplateUnknown, COMPOSER_TEXT("The choreography event {event} uses the template \"{template}\", which is not in the library or whose effect this xLights does not have."), COMPOSER_TEXT("Choose an existing template.") },
    { issue::ChoreoEffectMismatch, COMPOSER_TEXT("The choreography event {event} uses \"{effect}\", but the template \"{template}\" is for \"{expected}\"."), COMPOSER_TEXT("Make the choreography again; if it repeats, report the problem.") },
    { issue::ChoreoIncompatible, COMPOSER_TEXT("The template \"{template}\" cannot be used on \"{model}\" ({reason})."), COMPOSER_TEXT("Choose a template made for that kind of figure.") },
    { issue::ChoreoCapabilityUnknown, COMPOSER_TEXT("Whether the template \"{template}\" works on \"{model}\" is only known once rendered."), COMPOSER_TEXT("Check that part in the preview.") },
    { issue::ChoreoSettingInvalid, COMPOSER_TEXT("The choreography event {event} has an invalid setting {key}={value}: {reason}."), COMPOSER_TEXT("Correct the template that produced it.") },
    { issue::ChoreoPaletteInvalid, COMPOSER_TEXT("The choreography event {event} has an invalid colour: {value}."), COMPOSER_TEXT("Choose another palette.") },
    { issue::ChoreoTimeInvalid, COMPOSER_TEXT("The choreography event {event} has invalid times or does not fall on the sequence's frames."), COMPOSER_TEXT("Correct its start and end.") },
    { issue::ChoreoDuration, COMPOSER_TEXT("The choreography event {event} lasts {length} ms, outside the {min}-{max} ms its template recommends."), COMPOSER_TEXT("Check that it reads well at that length.") },
    { issue::ChoreoOverlap, COMPOSER_TEXT("The choreography event {event} overlaps {over} on the same figure and layer."), COMPOSER_TEXT("Move one of them to another layer or shorten it.") },
    { issue::ChoreoGroupConflict, COMPOSER_TEXT("The choreography event {event} on \"{model}\" shares lights with \"{over}\" at the same time."), COMPOSER_TEXT("Keep only one of them active, unless the overlap is intended.") },
    { issue::ChoreoMediaMissing, COMPOSER_TEXT("The choreography event {event} needs a {kind} file that does not exist."), COMPOSER_TEXT("Add the file, or use the media cue instead of an effect.") },
    { issue::ChoreoLyricsTrackMissing, COMPOSER_TEXT("The choreography event {event} needs lyrics that are not there ({track})."), COMPOSER_TEXT("Synchronize the lyrics first.") },
    { issue::ChoreoLockLost, COMPOSER_TEXT("A choreography event locked by hand on \"{model}\" ({event}) is no longer there."), COMPOSER_TEXT("Make the choreography again; if it repeats, report the problem.") },
    { issue::ChoreoManualConflict, COMPOSER_TEXT("A manual choreography decision on \"{model}\" could not be applied ({reason}). The previous decision was kept."), COMPOSER_TEXT("Change the decision so that it suits the figure.") },
    { issue::ChoreoNeedsReview, COMPOSER_TEXT("{count} choreography events have limitations or unknowns that only a look at the preview can settle."), COMPOSER_TEXT("Review them in the preview.") },
    { issue::ChoreoVisualReview, COMPOSER_TEXT("{count} choreography events use templates nobody has reviewed visually yet."), COMPOSER_TEXT("Watch the result and adjust the templates' starting values.") },
};

const IssueText* FindIssueText(const std::string& code) {
    for (const IssueText& text : ISSUE_TEXTS) {
        if (code == text.code) {
            return &text;
        }
    }
    return nullptr;
}

std::string FillTemplate(std::string text, const ValidationIssue& issue) {
    Replace(text, "{model}", issue.model);
    Replace(text, "{effect}", issue.effect);
    for (const auto& [key, value] : issue.args) {
        Replace(text, "{" + key + "}", value);
    }
    return text;
}

} // namespace

std::string DescribeIssue(const ValidationIssue& issue) {
    const IssueText* text = FindIssueText(issue.code);
    return text == nullptr ? issue.code : FillTemplate(text->message, issue);
}

std::string DescribeSuggestion(const ValidationIssue& issue) {
    const IssueText* text = FindIssueText(issue.code);
    return text == nullptr ? std::string() : FillTemplate(text->suggestion, issue);
}

std::string DescribeIssue(const ValidationIssue& issue, const TranslateText& translate) {
    const IssueText* text = FindIssueText(issue.code);
    return text == nullptr ? issue.code : FillTemplate(translate ? translate(text->message) : std::string(text->message), issue);
}

std::string DescribeSuggestion(const ValidationIssue& issue, const TranslateText& translate) {
    const IssueText* text = FindIssueText(issue.code);
    return text == nullptr ? std::string() : FillTemplate(translate ? translate(text->suggestion) : std::string(text->suggestion), issue);
}

const char* StatusName(CheckStatus status) {
    switch (status) {
    case CheckStatus::NotRun:
        return "NOT_RUN";
    case CheckStatus::Passed:
        return "PASSED";
    case CheckStatus::Failed:
        return "FAILED";
    case CheckStatus::Cancelled:
        return "CANCELLED";
    }
    return "NOT_RUN";
}

} // namespace composer
