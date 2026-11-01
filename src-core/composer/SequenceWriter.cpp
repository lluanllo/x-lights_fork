#include "composer/SequenceWriter.h"

#include "effects/EffectManager.h"
#include "effects/RenderableEffect.h"
#include "models/Model.h"
#include "render/Effect.h"
#include "render/EffectLayer.h"
#include "render/Element.h"
#include "render/HeadlessRenderContext.h"
#include "render/SequenceElements.h"
#include "render/SequenceFile.h"
#include "utils/ExternalHooks.h"
#include "utils/UtilClasses.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <list>
#include <memory>
#include <system_error>

namespace fs = std::filesystem;

namespace composer {

namespace {

std::string FullTargetName(const EffectSpec& spec) {
    return spec.subModel.empty() ? spec.target : spec.target + "/" + spec.subModel;
}

Element* FindEffectHolder(SequenceElements& elements, const EffectSpec& spec) {
    Element* el = elements.GetElement(spec.target);
    if (el == nullptr || spec.subModel.empty()) {
        return el;
    }
    auto* me = dynamic_cast<ModelElement*>(el);
    return me != nullptr ? me->GetSubModel(spec.subModel) : nullptr;
}

bool AddTimingTracks(SequenceElements& elements, const SequenceSpec& spec, std::string& error) {
    for (const auto& track : spec.timingTracks) {
        Element* el = elements.AddElement(track.name, "timing", true, false, false, false, false);
        if (el == nullptr) {
            error = fmt::format("no se pudo crear la pista '{}'", track.name);
            return false;
        }
        for (const auto& marks : track.layers) {
            EffectLayer* layer = el->AddEffectLayer();
            for (const auto& mark : marks) {
                if (layer->AddEffect(0, mark.label, "", "", mark.startMS, mark.endMS, EFFECT_NOT_SELECTED, false) == nullptr) {
                    error = fmt::format("no se pudo crear la marca '{}' en '{}'", mark.label, track.name);
                    return false;
                }
            }
        }
    }
    return true;
}

// AddElement and AddEffect accept names that do not exist in the layout and
// xLights then drops them at load time, so targets are checked here.
bool AddEffects(xLightsShowContext& ctx, const SequenceSpec& spec, std::string& error) {
    SequenceElements& elements = ctx.GetSequenceElements();
    for (const auto& effect : spec.effects) {
        Model* model = ctx.AllModels[effect.target];
        if (model == nullptr) {
            error = fmt::format("el destino '{}' no existe en el layout", effect.target);
            return false;
        }
        if (!effect.subModel.empty() && model->GetSubModel(effect.subModel) == nullptr) {
            error = fmt::format("el submodelo '{}' no existe en el layout", FullTargetName(effect));
            return false;
        }

        Element* el = elements.GetElement(effect.target);
        if (el == nullptr) {
            el = elements.AddElement(effect.target, "model", true, false, false, false, false);
        }
        auto* me = dynamic_cast<ModelElement*>(el);
        if (me == nullptr) {
            error = fmt::format("no se pudo crear el elemento '{}'", effect.target);
            return false;
        }
        if (me->GetEffectLayerCount() == 0) {
            me->AddEffectLayer();
        }

        Element* holder = me;
        if (!effect.subModel.empty()) {
            holder = me->GetSubModel(effect.subModel, true);
        }
        while (holder->GetEffectLayerCount() <= static_cast<size_t>(effect.layer)) {
            holder->AddEffectLayer();
        }

        SettingsMap settings;
        for (const auto& [key, value] : effect.settings) {
            settings[key] = value;
        }
        if (holder->GetEffectLayer(effect.layer)->AddEffect(0, effect.effect, settings.AsString(), effect.palette, effect.startMS, effect.endMS, EFFECT_NOT_SELECTED, false) == nullptr) {
            error = fmt::format("no se pudo crear '{}' en '{}'", effect.effect, FullTargetName(effect));
            return false;
        }
    }
    return true;
}

void VerifyTimingTracks(SequenceElements& elements, const SequenceSpec& spec, std::vector<std::string>& problems) {
    for (const auto& track : spec.timingTracks) {
        TimingElement* el = elements.GetTimingElement(track.name);
        if (el == nullptr) {
            problems.push_back(fmt::format("falta la pista '{}'", track.name));
            continue;
        }
        if (el->GetEffectLayerCount() != track.layers.size()) {
            problems.push_back(fmt::format("la pista '{}' tiene {} capas y se esperaban {}", track.name, el->GetEffectLayerCount(), track.layers.size()));
            continue;
        }
        for (size_t l = 0; l < track.layers.size(); ++l) {
            EffectLayer* layer = el->GetEffectLayer(static_cast<int>(l));
            if (layer->GetEffectCount() != static_cast<int>(track.layers[l].size())) {
                problems.push_back(fmt::format("la capa {} de '{}' tiene {} marcas y se esperaban {}", l, track.name, layer->GetEffectCount(), track.layers[l].size()));
                continue;
            }
            for (size_t m = 0; m < track.layers[l].size(); ++m) {
                const TimingMark& expected = track.layers[l][m];
                Effect* mark = layer->GetEffect(static_cast<int>(m));
                if (mark->GetEffectName() != expected.label || mark->GetStartTimeMS() != expected.startMS || mark->GetEndTimeMS() != expected.endMS) {
                    problems.push_back(fmt::format("la marca {} de la capa {} de '{}' no coincide", m, l, track.name));
                }
            }
        }
    }
}

// The track reference is drawn by the panel's custom row, so it has no property
// of its own in the effect metadata.
bool IsCustomRowSetting(const std::string& id) {
    return id == "Faces_TimingTrack";
}

void VerifySettingAgainstMetadata(const RenderableEffect& renderable, const EffectSpec& effect, const std::string& key, const std::string& value, std::vector<std::string>& problems) {
    if (key.rfind("E_", 0) != 0) {
        return;
    }
    const size_t controlEnd = key.find('_', 2);
    if (controlEnd == std::string::npos) {
        problems.push_back(fmt::format("clave con formato inesperado '{}' en '{}'", key, FullTargetName(effect)));
        return;
    }
    const std::string id = key.substr(controlEnd + 1);
    if (IsCustomRowSetting(id)) {
        return;
    }
    const nlohmann::json* property = renderable.GetPropertyMetadata(id);
    if (property == nullptr) {
        problems.push_back(fmt::format("la clave '{}' de {} no figura en la metadata", key, effect.effect));
        return;
    }
    if (!property->contains("min") || !property->contains("max")) {
        return;
    }
    double low = property->at("min").get<double>();
    double high = property->at("max").get<double>();
    if (key.rfind("E_TEXTCTRL_", 0) == 0 && property->contains("divisor")) {
        const double divisor = property->at("divisor").get<double>();
        low /= divisor;
        high /= divisor;
    }
    const double number = std::strtod(value.c_str(), nullptr);
    if (number < low || number > high) {
        problems.push_back(fmt::format("'{}={}' está fuera de rango [{}, {}]", key, value, low, high));
    }
}

void VerifyFacesReferences(xLightsShowContext& ctx, const EffectSpec& effect, const Effect& loaded, std::vector<std::string>& problems) {
    SequenceElements& elements = ctx.GetSequenceElements();
    const std::string& track = loaded.GetSettings().Get("E_CHOICE_Faces_TimingTrack", "");
    TimingElement* timing = elements.GetTimingElement(track);
    if (timing == nullptr) {
        problems.push_back(fmt::format("Faces apunta a una pista inexistente '{}'", track));
    } else if (timing->GetEffectLayerCount() < 3) {
        problems.push_back(fmt::format("Faces apunta a '{}', que solo tiene {} capas", track, timing->GetEffectLayerCount()));
    }
    Model* model = ctx.AllModels[effect.target];
    const std::string& definition = loaded.GetSettings().Get("E_CHOICE_Faces_FaceDefinition", "");
    if (model == nullptr || !model->GetFaceInfo().contains(definition)) {
        problems.push_back(fmt::format("la cara '{}' no existe en '{}'", definition, effect.target));
    }
}

void VerifyEffects(HeadlessRenderContext& ctx, const SequenceSpec& spec, std::vector<std::string>& problems) {
    SequenceElements& elements = ctx.GetSequenceElements();
    EffectManager& manager = ctx.GetEffectManager();

    for (const auto& effect : spec.effects) {
        const std::string where = FullTargetName(effect);
        Element* holder = FindEffectHolder(elements, effect);
        if (holder == nullptr || holder->GetEffectLayerCount() <= static_cast<size_t>(effect.layer)) {
            problems.push_back(fmt::format("falta el destino o la capa de '{}'", where));
            continue;
        }
        EffectLayer* layer = holder->GetEffectLayer(effect.layer);
        Effect* loaded = nullptr;
        for (Effect* candidate : layer->GetEffects()) {
            if (candidate->GetEffectName() == effect.effect && candidate->GetStartTimeMS() == effect.startMS && candidate->GetEndTimeMS() == effect.endMS) {
                loaded = candidate;
                break;
            }
        }
        if (loaded == nullptr) {
            problems.push_back(fmt::format("falta {} en '{}' entre {} y {} ms", effect.effect, where, effect.startMS, effect.endMS));
            continue;
        }

        RenderableEffect* renderable = manager.GetEffect(effect.effect);
        if (renderable == nullptr) {
            problems.push_back(fmt::format("efecto desconocido '{}'", effect.effect));
            continue;
        }
        for (const auto& [key, value] : effect.settings) {
            const std::string& actual = loaded->GetSettings().Get(key, "");
            if (actual != value) {
                problems.push_back(fmt::format("'{}' en '{}' vale '{}' y se esperaba '{}'", key, where, actual, value));
            }
            VerifySettingAgainstMetadata(*renderable, effect, key, value, problems);
        }

        SettingsMap palette;
        palette.Parse(nullptr, effect.palette, "");
        const std::string actualPalette = loaded->GetPaletteAsString();
        for (const auto& [key, value] : palette) {
            if (actualPalette.find(key + "=" + std::string(value)) == std::string::npos) {
                problems.push_back(fmt::format("la paleta de '{}' perdió '{}'", where, key));
            }
        }

        if (effect.effect == "Faces") {
            VerifyFacesReferences(ctx, effect, *loaded, problems);
        }

        Model* model = ctx.AllModels.GetModel(where);
        if (model == nullptr) {
            problems.push_back(fmt::format("el modelo '{}' no se resuelve", where));
            continue;
        }
        const std::string style = loaded->GetSettings().Get("B_CHOICE_BufferStyle", "");
        if (!style.empty()) {
            const auto styles = model->GetBufferStyles();
            if (std::find(styles.begin(), styles.end(), style) == styles.end()) {
                problems.push_back(fmt::format("'{}' no admite el estilo de buffer '{}'", where, style));
            }
        }

        // Same contract as SequenceChecker: the checks walk the sequence through
        // the effect, and EffectManager never holds a sequence of its own.
        SequenceElements* previous = renderable->GetSequenceElements();
        renderable->SetSequenceElements(&elements);
        const std::list<std::string> reports = renderable->CheckEffectSettings(loaded->GetSettings(), nullptr, model, loaded, false);
        renderable->SetSequenceElements(previous);
        for (const auto& report : reports) {
            if (report.find("ERR:") != std::string::npos) {
                problems.push_back(fmt::format("{} en '{}': {}", effect.effect, where, report));
            }
        }
    }
}

void VerifyNothingExtra(SequenceElements& elements, const SequenceSpec& spec, std::vector<std::string>& problems) {
    size_t timing = 0;
    size_t effects = 0;
    const auto countLayers = [&](const Element& el) {
        for (size_t l = 0; l < el.GetEffectLayerCount(); ++l) {
            for (Effect* effect : el.GetEffectLayer(static_cast<int>(l))->GetEffects()) {
                ++effects;
                if (effect->GetEffectName() == "Video" || effect->GetEffectName() == "Pictures") {
                    problems.push_back(fmt::format("hay un efecto {} no previsto en '{}'", effect->GetEffectName(), el.GetName()));
                }
            }
        }
    };
    for (size_t i = 0; i < elements.GetElementCount(); ++i) {
        Element* el = elements.GetElement(i);
        if (el->GetType() == ElementType::ELEMENT_TYPE_TIMING) {
            ++timing;
            continue;
        }
        countLayers(*el);
        if (auto* me = dynamic_cast<ModelElement*>(el)) {
            for (int s = 0; s < me->GetSubModelCount(); ++s) {
                countLayers(*me->GetSubModel(s));
            }
        }
    }
    if (timing != spec.timingTracks.size()) {
        problems.push_back(fmt::format("hay {} pistas y se esperaban {}", timing, spec.timingTracks.size()));
    }
    if (effects != spec.effects.size()) {
        problems.push_back(fmt::format("hay {} efectos y se esperaban {}", effects, spec.effects.size()));
    }
}

// Owns what WriteSequenceSafely creates, so that every exit path removes exactly
// that and nothing else.
class TemporaryFiles {
public:
    explicit TemporaryFiles(WriteResult& result) :
        _result(result) {}

    ~TemporaryFiles() {
        std::error_code ec;
        if (!_file.empty() && fs::exists(_file, ec)) {
            fs::remove(_file, ec);
            if (ec) {
                _result.leftoverFiles.push_back(_file.string());
            }
        }
        for (const fs::path& dir : _createdDirs) {
            std::error_code dirError;
            if (fs::exists(dir, dirError) && fs::is_empty(dir, dirError)) {
                fs::remove(dir, dirError);
            }
            if (dirError) {
                _result.leftoverFiles.push_back(dir.string());
            }
        }
    }

    bool CreateDirectories(const fs::path& leaf, std::error_code& ec) {
        std::vector<fs::path> missing;
        for (fs::path p = leaf; !p.empty() && !fs::exists(p, ec); p = p.parent_path()) {
            missing.push_back(p);
            if (p == p.parent_path()) {
                break;
            }
        }
        fs::create_directories(leaf, ec);
        if (ec) {
            return false;
        }
        _createdDirs = std::move(missing);
        return true;
    }

    void Own(const fs::path& file) {
        _file = file;
    }

private:
    WriteResult& _result;
    fs::path _file;
    std::vector<fs::path> _createdDirs; // deepest first
};

void WriteThroughTemporary(const std::string& showDir, const SequenceSpec& spec, const std::string& destinationName, TemporaryFiles& temporaries, WriteResult& result) {
    const fs::path name(destinationName);
    if (name.has_parent_path() || name.extension() != ".xsq") {
        result.problems.push_back(fmt::format("el destino debe ser un nombre de archivo .xsq, sin carpetas: '{}'", destinationName));
        return;
    }

    ObtainAccessToURL(showDir, true);
    const fs::path show(showDir);
    const fs::path destination = show / name;
    fs::path backup = destination;
    backup.replace_extension(".xbkp");
    const auto destinationIsFree = [&]() {
        if (FileExists(destination.string())) {
            result.problems.push_back(fmt::format("el destino ya existe: '{}'", destination.string()));
            return false;
        }
        if (FileExists(backup.string())) {
            result.problems.push_back(fmt::format("ya existe un autoguardado con ese nombre: '{}'", backup.string()));
            return false;
        }
        return true;
    };
    if (!destinationIsFree()) {
        return;
    }

    const fs::path tmpDir = show / "ShowComposer" / "tmp";
    std::error_code ec;
    if (!temporaries.CreateDirectories(tmpDir, ec)) {
        result.problems.push_back(fmt::format("no se pudo crear '{}': {}", tmpDir.string(), ec.message()));
        return;
    }

    fs::path temporary;
    for (int attempt = 0;; ++attempt) {
        temporary = tmpDir / fmt::format("{}.{}.composer-tmp.xsq", name.stem().string(), attempt);
        if (!FileExists(temporary.string())) {
            break;
        }
    }
    temporaries.Own(temporary);

    {
        HeadlessRenderContext builder;
        if (!builder.LoadShowFolder(showDir)) {
            result.problems.push_back(fmt::format("no se pudo cargar el show '{}'", showDir));
            return;
        }
        std::string error;
        if (!BuildAndSave(builder, spec, temporary.string(), error)) {
            result.problems.push_back(std::move(error));
            return;
        }
        if (!builder.CloseSequence()) {
            result.problems.push_back("no se pudo cerrar la secuencia construida");
            return;
        }
    }

    {
        HeadlessRenderContext checker;
        if (!checker.LoadShowFolder(showDir)) {
            result.problems.push_back(fmt::format("no se pudo cargar el show '{}' para validar", showDir));
            return;
        }
        if (!checker.OpenSequence(temporary.string())) {
            result.problems.push_back("no se pudo reabrir el archivo temporal");
            return;
        }
        result.problems = VerifySequence(checker, spec);
        if (!checker.CloseSequence()) {
            result.problems.push_back("no se pudo cerrar la secuencia reabierta");
        }
        if (!result.problems.empty()) {
            return;
        }
    }

    if (!destinationIsFree()) {
        return;
    }
    // A hard link fails if the destination exists, unlike std::filesystem::rename,
    // which replaces it silently. That closes the window after the check above.
    fs::create_hard_link(temporary, destination, ec);
    if (ec) {
        result.problems.push_back(fmt::format("no se pudo publicar '{}': {}", destination.string(), ec.message()));
        return;
    }

    result.ok = true;
    result.finalPath = destination.string();
}

} // namespace

bool BuildAndSave(xLightsShowContext& ctx, const SequenceSpec& spec, const std::string& path, std::string& error) {
    if (spec.frameMS <= 0 || spec.durationMS <= 0) {
        error = "la duración y el intervalo de frame deben ser positivos";
        return false;
    }
    if (!ctx.CloseSequence()) {
        error = "no se pudo cerrar la secuencia anterior";
        return false;
    }

    SequenceElements& elements = ctx.GetSequenceElements();
    elements.SetFrequency(1000.0 / spec.frameMS);
    elements.SetViewsManager(&ctx._sequenceViewManager);
    elements.SetSequenceEnd(spec.durationMS);

    if (!AddTimingTracks(elements, spec, error) || !AddEffects(ctx, spec, error)) {
        return false;
    }

    SequenceFile file(path, static_cast<uint32_t>(spec.frameMS));
    file.SetSequenceType("Animation");
    file.SetSequenceDurationMS(spec.durationMS);
    file.SetSequenceTiming(std::to_string(spec.frameMS) + " ms");
    if (!file.Save(elements)) {
        error = fmt::format("SequenceFile::Save falló en '{}'", path);
        return false;
    }
    return true;
}

std::vector<std::string> VerifySequence(HeadlessRenderContext& ctx, const SequenceSpec& spec) {
    std::vector<std::string> problems;
    SequenceElements& elements = ctx.GetSequenceElements();
    VerifyTimingTracks(elements, spec, problems);
    VerifyEffects(ctx, spec, problems);
    VerifyNothingExtra(elements, spec, problems);
    return problems;
}

WriteResult WriteSequenceSafely(const std::string& showDir, const SequenceSpec& spec, const std::string& destinationName) {
    WriteResult result;
    {
        // Scoped so that a failed cleanup is in the result before it is returned.
        TemporaryFiles temporaries(result);
        WriteThroughTemporary(showDir, spec, destinationName, temporaries, result);
    }
    return result;
}

} // namespace composer
