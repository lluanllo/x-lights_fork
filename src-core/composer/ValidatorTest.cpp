#include "composer/ValidatorTest.h"

#include "composer/GenerationValidator.h"
#include "composer/SequenceWriter.h"
#include "composer/WriterTest.h"
#include "models/Model.h"
#include "models/ModelGroup.h"
#include "render/FSEQFile.h"
#include "render/HeadlessRenderContext.h"
#include "utils/ExternalHooks.h"

#include <pugixml.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace composer {

namespace {

constexpr const char* WHITE = "C_BUTTON_Palette1=#FFFFFF,C_CHECKBOX_Palette1=1";
constexpr const char* RED = "C_BUTTON_Palette1=#FF0000,C_CHECKBOX_Palette1=1";
constexpr const char* GREEN = "C_BUTTON_Palette1=#00FF00,C_CHECKBOX_Palette1=1";
constexpr const char* BLACK = "C_BUTTON_Palette1=#000000,C_CHECKBOX_Palette1=1";
constexpr const char* MULTI = "C_BUTTON_Palette1=#FFFFFF,C_BUTTON_Palette2=#FF0000,C_BUTTON_Palette3=#00FF00,C_CHECKBOX_Palette1=1,C_CHECKBOX_Palette2=1,C_CHECKBOX_Palette3=1";

// Spelled as bytes so that the tests do not depend on the compiler's source encoding.
constexpr const char* N_TILDE = "\xC3\xB1";
constexpr const char* A_ACUTE = "\xC3\xA1";
constexpr const char* E_ACUTE = "\xC3\xA9";
constexpr const char* I_ACUTE = "\xC3\xAD";
constexpr const char* O_ACUTE = "\xC3\xB3";
constexpr const char* U_ACUTE = "\xC3\xBA";
constexpr const char* U_DIAERESIS = "\xC3\xBC";
constexpr const char* INVERTED_EXCLAMATION = "\xC2\xA1";
constexpr const char* INVERTED_QUESTION = "\xC2\xBF";

constexpr const char* TRACK = "Fixture, voz";
constexpr const char* MATRIX = "Fixture Matrix";
constexpr const char* TREE = "Fixture Tree";
constexpr const char* FACE = "Fixture Face";
constexpr const char* IMAGE_FACE = "Fixture Image Face";
constexpr const char* STAR_A = "Fixture Star A";
constexpr const char* STAR_B = "Fixture Star B";
constexpr const char* GROUP = "Fixture Group";
constexpr const char* FACE_GROUP = "Fixture Face Group";
constexpr const char* BROKEN_GROUP = "Fixture Broken Group";

void AddCommonAttributes(pugi::xml_node model, const std::string& name, const char* displayAs, int& nextChannel, int nodes) {
    model.append_attribute("name") = name.c_str();
    model.append_attribute("DisplayAs") = displayAs;
    model.append_attribute("StringType") = "RGB Nodes";
    model.append_attribute("LayoutGroup") = "Default";
    model.append_attribute("Dir") = "L";
    model.append_attribute("StartSide") = "T";
    model.append_attribute("Antialias") = "1";
    model.append_attribute("PixelSize") = "2";
    model.append_attribute("Transparency") = "0";
    model.append_attribute("versionNumber") = "8";
    model.append_attribute("StartChannel") = std::to_string(nextChannel).c_str();
    nextChannel += nodes * 3;
}

pugi::xml_node AddMatrix(pugi::xml_node models, const std::string& name, int width, int height, int& nextChannel) {
    pugi::xml_node model = models.append_child("model");
    AddCommonAttributes(model, name, "Matrix", nextChannel, width * height);
    model.append_attribute("NumStrings") = "1";
    model.append_attribute("NodesPerString") = std::to_string(width * height).c_str();
    model.append_attribute("StrandsPerString") = std::to_string(height).c_str();
    model.append_attribute("Vertical") = "false";
    model.append_attribute("AlternateNodes") = "false";
    model.append_attribute("NoZig") = "false";
    return model;
}

pugi::xml_node AddCustom(pugi::xml_node models, const std::string& name, int width, int height, int& nextChannel) {
    pugi::xml_node model = models.append_child("model");
    AddCommonAttributes(model, name, "Custom", nextChannel, width * height);
    std::string compressed;
    int node = 1;
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            compressed += fmt::format("{},{},{};", node++, row, col);
        }
    }
    model.append_attribute("CustomModelCompressed") = compressed.c_str();
    model.append_attribute("CustomWidth") = std::to_string(width).c_str();
    model.append_attribute("CustomHeight") = std::to_string(height).c_str();
    model.append_attribute("Depth") = "1";
    return model;
}

void AddGroup(pugi::xml_node groups, const std::string& name, const std::string& members) {
    pugi::xml_node group = groups.append_child("modelGroup");
    group.append_attribute("name") = name.c_str();
    group.append_attribute("DisplayAs") = "ModelGroup";
    group.append_attribute("DefaultCamera") = "2D";
    group.append_attribute("GridSize") = "400";
    group.append_attribute("layout") = "minimalGrid";
    group.append_attribute("LayoutGroup") = "Default";
    group.append_attribute("models") = members.c_str();
}

// `prefix` and `matrixWidth` produce the incompatible layouts of the mismatch cases.
bool WriteFixtureShow(const fs::path& folder, const std::string& prefix, int matrixWidth) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        return false;
    }
    pugi::xml_document doc;
    pugi::xml_node root = doc.append_child("xrgb");
    pugi::xml_node models = root.append_child("models");
    pugi::xml_node groups = root.append_child("modelGroups");
    const auto named = [&](const char* name) { return prefix + name; };

    int channel = 1;
    AddMatrix(models, named(MATRIX), matrixWidth, 20, channel);
    AddMatrix(models, named(TREE), 10, 20, channel);

    pugi::xml_node face = AddCustom(models, named(FACE), 6, 5, channel);
    pugi::xml_node bulb = face.append_child("faceInfo");
    const std::pair<const char*, const char*> shapes[] = {
        { "Name", "Bulb" }, { "Type", "NodeRange" }, { "CustomColors", "" }, { "Mouth-AI", "1-6" }, { "Mouth-E", "7-10" }, { "Mouth-etc", "11-14" }, { "Mouth-FV", "15-16" }, { "Mouth-L", "17-19" }, { "Mouth-MBP", "20" }, { "Mouth-O", "21-23" }, { "Mouth-U", "24" }, { "Mouth-WQ", "25" }, { "Mouth-rest", "26-27" }, { "Eyes-Open", "28-29" }, { "Eyes-Closed", "30" }
    };
    for (const auto& [key, value] : shapes) {
        bulb.append_attribute(key) = value;
    }

    pugi::xml_node imageFace = AddMatrix(models, named(IMAGE_FACE), 10, 10, channel);
    pugi::xml_node portrait = imageFace.append_child("faceInfo");
    const std::pair<const char*, const char*> images[] = {
        { "Name", "Portrait" }, { "Type", "Matrix" }, { "ImagePlacement", "Centered" }, { "CustomColors", "" }, { "Mouth-AI-EyesOpen", "fixture_images/missing_ai.png" }, { "Mouth-rest-EyesOpen", "fixture_images/missing_rest.png" }
    };
    for (const auto& [key, value] : images) {
        portrait.append_attribute(key) = value;
    }

    for (const char* name : { STAR_A, STAR_B }) {
        pugi::xml_node star = AddCustom(models, named(name), 6, 4, channel);
        pugi::xml_node arm = star.append_child("subModel");
        arm.append_attribute("name") = "Arm 1";
        arm.append_attribute("bufferstyle") = "Default";
        arm.append_attribute("layout") = "horizontal";
        arm.append_attribute("line0") = "1-6";
        arm.append_attribute("type") = "ranges";
    }

    AddGroup(groups, named(GROUP), named(STAR_A) + "," + named(STAR_B));
    AddGroup(groups, named(FACE_GROUP), named(FACE) + "," + named(STAR_A));
    AddGroup(groups, named(BROKEN_GROUP), named(STAR_A) + ",Fixture Does Not Exist");
    return doc.save_file((folder / "xlights_rgbeffects.xml").string().c_str());
}

TimingTrackSpec VoiceTrack(size_t layers = 3) {
    TimingTrackSpec track{ TRACK,
                           {
                               { { "uno dos", 4000, 6000 } },
                               { { "uno", 4000, 5000 }, { "dos", 5000, 6000 } },
                               { { "AI", 4000, 4500 }, { "E", 4500, 5000 }, { "O", 5000, 5500 }, { "MBP", 5500, 6000 } },
                           } };
    track.layers.resize(layers);
    return track;
}

EffectSpec FaceEffect(const std::string& target, const std::string& definition, int startMS = 4000, int endMS = 8000) {
    return { target, "", 0, "Faces",
             { { "E_CHOICE_Faces_FaceDefinition", definition }, { "E_CHOICE_Faces_Eyes", "Open" }, { "E_CHOICE_Faces_TimingTrack", TRACK } },
             MULTI, startMS, endMS };
}

EffectSpec Wash(const std::string& target, const std::string& subModel, int startMS, int endMS, const char* palette = RED, TargetKind kind = TargetKind::Any) {
    return { target, subModel, 0, "Color Wash", { { "E_TEXTCTRL_ColorWash_Cycles", "1.0" } }, palette, startMS, endMS, kind };
}

EffectSpec TextEffectOn(const std::string& text, const char* palette, int startMS, int endMS, const char* orientation = "normal") {
    return { MATRIX, "", 0, "Text",
             { { "E_TEXTCTRL_Text", text }, { "E_CHOICE_Text_Dir", "none" }, { "E_CHOICE_Text_Effect", orientation }, { "E_CHOICE_Text_Count", "none" } },
             palette, startMS, endMS };
}

SequenceSpec CorrectSpec() {
    SequenceSpec spec;
    spec.frameMS = 25;
    spec.durationMS = 13000;
    spec.timingTracks.push_back(VoiceTrack());
    spec.timingTracks.push_back({ "Pendientes", { { { "C001", 12000, 12500 } } } });
    spec.effects.push_back({ TREE, "", 0, "Spirals", { { "E_SLIDER_Spirals_Count", "2" }, { "E_TEXTCTRL_Spirals_Movement", "2.0" } }, MULTI, 0, 2000, TargetKind::Model });
    spec.effects.push_back(TextEffectOn(std::string(INVERTED_EXCLAMATION) + "A" + N_TILDE + "o!", WHITE, 2000, 4000));
    spec.effects.push_back(FaceEffect(FACE, "Bulb"));
    spec.effects.push_back(Wash(GROUP, "", 9000, 10000, RED, TargetKind::Group));
    spec.effects.push_back(Wash(STAR_A, "Arm 1", 10000, 11000));
    spec.markers.push_back({ "C001", MATRIX, 12000, 12500 });
    return spec;
}

std::vector<uint32_t> ChannelsOf(const xLightsShowContext& ctx, const std::string& name) {
    std::vector<uint32_t> channels;
    const Model* model = ctx.AllModels.GetModel(name);
    if (model == nullptr) {
        return channels;
    }
    for (uint32_t n = 0; n < model->GetNodeCount(); ++n) {
        const int32_t first = model->NodeStartChannel(n);
        for (int c = 0; c < model->GetChanCountPerNode(); ++c) {
            channels.push_back(static_cast<uint32_t>(first + c));
        }
    }
    return channels;
}

// All frames at once: the fixtures are small and the cases compare far-apart frames.
class RenderedFrames {
public:
    explicit RenderedFrames(const std::string& path) {
        std::unique_ptr<FSEQFile> file(FSEQFile::openFSEQFile(path));
        if (file == nullptr) {
            return;
        }
        _channels = file->getMaxChannel();
        _frameMS = file->getStepTime();
        std::vector<std::pair<uint32_t, uint32_t>> ranges{ { 0, _channels } };
        file->prepareRead(ranges);
        for (uint64_t f = 0; f < file->getNumFrames(); ++f) {
            std::vector<uint8_t> data(_channels, 0);
            std::unique_ptr<FSEQFile::FrameData> frame(file->getFrame(static_cast<uint32_t>(f)));
            if (frame != nullptr) {
                frame->readFrame(data.data(), _channels);
            }
            _frames.push_back(std::move(data));
        }
    }

    // One flag per node: comparing masks ignores the colour the glyph was drawn in.
    [[nodiscard]] std::vector<bool> LitMask(int ms, const std::vector<uint32_t>& channels) const {
        std::vector<bool> mask;
        const std::vector<uint8_t>& frame = _frames.at(ms / _frameMS);
        for (size_t i = 0; i + 2 < channels.size(); i += 3) {
            mask.push_back(frame[channels[i]] != 0 || frame[channels[i + 1]] != 0 || frame[channels[i + 2]] != 0);
        }
        return mask;
    }

    [[nodiscard]] std::set<uint32_t> Colors(int ms, const std::vector<uint32_t>& channels) const {
        std::set<uint32_t> colors;
        const std::vector<uint8_t>& frame = _frames.at(ms / _frameMS);
        for (size_t i = 0; i + 2 < channels.size(); i += 3) {
            const uint32_t color = (frame[channels[i]] << 16) | (frame[channels[i + 1]] << 8) | frame[channels[i + 2]];
            if (color == 0xFFFFFF || color == 0xFF0000 || color == 0x00FF00) {
                colors.insert(color);
            }
        }
        return colors;
    }

    [[nodiscard]] uint32_t NodeColor(int ms, const std::vector<uint32_t>& channels) const {
        const std::vector<uint8_t>& frame = _frames.at(ms / _frameMS);
        return (frame[channels[0]] << 16) | (frame[channels[1]] << 8) | frame[channels[2]];
    }

    [[nodiscard]] bool IsValid() const { return !_frames.empty(); }

private:
    uint32_t _channels = 0;
    int _frameMS = 25;
    std::vector<std::vector<uint8_t>> _frames;
};

int CountLit(const std::vector<bool>& mask) {
    return static_cast<int>(std::count(mask.begin(), mask.end(), true));
}

int SimilarityPercent(const std::vector<bool>& a, const std::vector<bool>& b) {
    int both = 0;
    int either = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        both += a[i] && b[i] ? 1 : 0;
        either += a[i] || b[i] ? 1 : 0;
    }
    return either == 0 ? 0 : both * 100 / either;
}

class Suite {
public:
    explicit Suite(const fs::path& root) :
        _root(root),
        _show(root / "fixture"),
        _other(root / "other_names"),
        _reshaped(root / "other_shape") {}

    int Run() {
        if (!WriteFixtureShow(_show, "", 49) || !WriteFixtureShow(_other, "Other ", 49) || !WriteFixtureShow(_reshaped, "", 30)) {
            printf("composer-validator-test: could not write the fixtures under %s\n", _root.string().c_str());
            return 1;
        }
        _ctx = std::make_unique<HeadlessRenderContext>();
        if (!_ctx->LoadShowFolder(_show.string())) {
            printf("composer-validator-test: could not load the fixture show\n");
            return 1;
        }

        Case01CorrectSequence();
        Case02MissingModel();
        Case03MissingGroup();
        Case04MissingSubModel();
        Case05FaceWithoutTrack();
        Case06FaceWithTwoLayers();
        Case07FaceDefinitionIncompatible();
        Case08FaceImagesMissing();
        Case09InvalidSettings();
        Case10MissingMedia();
        Case11MarkerProducesLight();
        Case12And13SpanishText();
        Case14GroupAndMember();
        Case15WrongModel();
        Case16SilentEffect();
        Case17UnexpectedOutput();
        Case18IncompatibleLayout();
        Case19ReopenedSequence();
        Case20Cancellation();
        CaseExtraTiming();
        CaseExtraWriterGate();
        (void)_ctx->CloseSequence();

        printf("composer-validator-test: %d checks passed, %d failed (%s)\n", _passed, _failed, _root.string().c_str());
        return _failed == 0 ? 0 : 1;
    }

private:
    void Check(bool ok, const std::string& what) {
        printf("  %s %s\n", ok ? "OK   " : "FAILED", what.c_str());
        ok ? ++_passed : ++_failed;
    }

    void Title(const char* title) {
        printf("=== %s\n", title);
    }

    void ExpectIssue(const ValidationReport& report, const char* code, const std::string& what) {
        Check(report.Has(code), fmt::format("{}: {}", what, code));
        if (!report.Has(code)) {
            Dump(report);
        }
    }

    void ExpectClean(const ValidationReport& report, const std::string& what) {
        const bool clean = report.Ok() && report.WarningCount() == 0;
        Check(clean, fmt::format("{}: no errors or warnings", what));
        if (!clean) {
            Dump(report);
        }
    }

    void Dump(const ValidationReport& report) {
        for (const ValidationIssue& issue : report.issues) {
            printf("         %s [%s] %d-%d ms: %s\n", issue.code.c_str(), issue.model.c_str(), issue.startMS, issue.endMS, DescribeIssue(issue).c_str());
        }
    }

    std::string PathFor(const std::string& name, const char* extension) const {
        return (_root / (name + extension)).string();
    }

    // Saved outside the show folder: the file is disposable and this is not a publish.
    bool Build(const SequenceSpec& spec, const std::string& name) {
        std::string error;
        const bool ok = BuildAndSave(*_ctx, spec, PathFor(name, ".xsq"), error);
        if (!ok) {
            Check(false, fmt::format("building {}: {}", name, error));
        }
        return ok;
    }

    bool BuildAndOpen(const SequenceSpec& spec, const std::string& name) {
        if (!Build(spec, name)) {
            return false;
        }
        const bool opened = _ctx->OpenSequence(PathFor(name, ".xsq"));
        if (!opened) {
            Check(false, fmt::format("reopening {}", name));
        }
        return opened;
    }

    bool BuildAndRender(const SequenceSpec& spec, const std::string& name) {
        if (!BuildAndOpen(spec, name)) {
            return false;
        }
        const bool rendered = _ctx->RenderAndWait() && _ctx->WriteFseq(PathFor(name, ".fseq"));
        if (!rendered) {
            Check(false, fmt::format("rendering {}", name));
        }
        return rendered;
    }

    ValidationReport Render(const SequenceSpec& expected, const std::string& name) {
        return ValidateRender(*_ctx, PathFor(name, ".fseq"), expected, Strictness::Generated);
    }

    void Case01CorrectSequence() {
        Title("1. Correct sequence: no false positives at any stage");
        const SequenceSpec spec = CorrectSpec();
        const ValidationReport plan = ValidatePlan(*_ctx, spec);
        ExpectClean(plan, "plan");
        Check(plan.Status(IssueCategory::Render) == CheckStatus::NotRun, "the plan stage does not claim a render result");
        if (!Build(spec, "correct")) {
            return;
        }
        ExpectClean(ValidateBuilt(*_ctx, spec), "built objects");
        if (!_ctx->OpenSequence(PathFor("correct", ".xsq"))) {
            Check(false, "reopening");
            return;
        }
        ExpectClean(ValidateSequenceFile(*_ctx, PathFor("correct", ".xsq"), &spec, Strictness::Generated), "reopened file");
        ExpectClean(ValidateSequenceFile(*_ctx, PathFor("correct", ".xsq"), nullptr, Strictness::FromFile), "reopened file, sidecar lost");
        if (!_ctx->RenderAndWait() || !_ctx->WriteFseq(PathFor("correct", ".fseq"))) {
            Check(false, "rendering");
            return;
        }
        const ValidationReport render = Render(spec, "correct");
        ExpectClean(render, "render");
        Check(render.Status(IssueCategory::Render) == CheckStatus::Passed, "render status is PASSED");
        Check(render.VisualStatus() == CheckStatus::NotRun, "visual status stays NOT_RUN");
        Check(render.metrics.size() == spec.effects.size(), fmt::format("one metric per effect ({})", render.metrics.size()));

        const SequenceSpec fromFile = SpecFromSequence(*_ctx);
        Check(fromFile.durationMS == spec.durationMS, fmt::format("the duration is read back from the file ({} ms)", fromFile.durationMS));
        ExpectClean(ValidateRender(*_ctx, PathFor("correct", ".fseq"), fromFile, Strictness::FromFile), "render, sidecar lost");
    }

    void Case02MissingModel() {
        Title("2. Missing model");
        SequenceSpec spec = CorrectSpec();
        spec.effects[0].target = "Fixture Nothing";
        const ValidationReport plan = ValidatePlan(*_ctx, spec);
        ExpectIssue(plan, issue::ModelNotFound, "plan");
        const ValidationIssue* found = plan.Find(issue::ModelNotFound);
        Check(found != nullptr && found->model == "Fixture Nothing", "the issue names the model");
    }

    void Case03MissingGroup() {
        Title("3. Missing and broken groups");
        SequenceSpec spec = CorrectSpec();
        spec.effects[3].target = "Fixture No Group";
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::GroupNotFound, "missing group");

        spec = CorrectSpec();
        spec.effects[3].target = MATRIX;
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::TargetKindMismatch, "a model where a group was planned");

        spec = CorrectSpec();
        spec.effects[3].target = BROKEN_GROUP;
        const ValidationReport broken = ValidatePlan(*_ctx, spec);
        printf("         (layout load keeps %zu names in the broken group)\n", dynamic_cast<const ModelGroup*>(_ctx->AllModels.GetModel(BROKEN_GROUP))->ModelNames().size());
        Check(broken.Has(issue::GroupMemberInvalid) || dynamic_cast<const ModelGroup*>(_ctx->AllModels.GetModel(BROKEN_GROUP))->ModelNames().size() == 1, "an invalid member is reported, or the layout loader already removed it");
    }

    void Case04MissingSubModel() {
        Title("4. Missing submodel");
        SequenceSpec spec = CorrectSpec();
        spec.effects[4].subModel = "Arm 99";
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::SubModelNotFound, "plan");
    }

    void Case05FaceWithoutTrack() {
        Title("5. Face without a track");
        SequenceSpec spec = CorrectSpec();
        spec.effects[2].settings[2].second = "Track That Does Not Exist";
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::FaceTrackMissing, "plan");
        if (BuildAndRender(spec, "face_no_track")) {
            ExpectIssue(Render(CorrectSpec(), "face_no_track"), issue::FaceMouthStatic, "render against the intended track");
        }
    }

    void Case06FaceWithTwoLayers() {
        Title("6. Face with a two-layer track");
        SequenceSpec spec = CorrectSpec();
        spec.timingTracks[0] = VoiceTrack(2);
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::FaceInsufficientLayers, "plan");
        if (BuildAndRender(spec, "face_two_layers")) {
            ExpectIssue(ValidateSequenceFile(*_ctx, PathFor("face_two_layers", ".xsq"), nullptr, Strictness::FromFile), issue::FaceInsufficientLayers, "file with two layers");
            ExpectIssue(Render(CorrectSpec(), "face_two_layers"), issue::FaceMouthStatic, "render: the mouth really stays still");
        }

        spec = CorrectSpec();
        spec.timingTracks[0].layers[2].clear();
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::FaceNoPhonemes, "three layers but no phonemes");
    }

    void Case07FaceDefinitionIncompatible() {
        Title("7. Face definition the target cannot draw");
        SequenceSpec spec = CorrectSpec();
        spec.effects[2].target = FACE_GROUP;
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::FaceDefinitionIncompatible, "node range face on a group");
        if (BuildAndRender(spec, "face_on_group")) {
            ExpectIssue(Render(spec, "face_on_group"), issue::RenderTargetSilent, "render: the group really stays dark");
        }

        spec = CorrectSpec();
        spec.effects[2].settings[0].second = "Face That Does Not Exist";
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::FaceDefinitionNotFound, "definition that does not exist");
    }

    void Case08FaceImagesMissing() {
        Title("8. Face definition with missing images");
        SequenceSpec spec = CorrectSpec();
        spec.effects[2] = FaceEffect(IMAGE_FACE, "Portrait");
        const ValidationReport plan = ValidatePlan(*_ctx, spec);
        ExpectIssue(plan, issue::FaceImageMissing, "plan");
        const ValidationIssue* found = plan.Find(issue::FaceImageMissing);
        Check(found != nullptr && DescribeIssue(*found).find("missing_ai.png") != std::string::npos, "the issue names the image");
        if (BuildAndRender(spec, "face_images")) {
            ExpectIssue(Render(spec, "face_images"), issue::RenderPlaceholderOutput, "render: the model is filled with one pure colour");
        }
    }

    void Case09InvalidSettings() {
        Title("9. Effect with invalid settings");
        SequenceSpec spec = CorrectSpec();
        spec.effects[0].settings[0].second = "99";
        const ValidationReport range = ValidatePlan(*_ctx, spec);
        ExpectIssue(range, issue::EffectSettingsInvalid, "out of range");
        spec.effects[0].settings[0].second = "lots";
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::EffectSettingsInvalid, "text in a number");
        spec = CorrectSpec();
        spec.effects[0].settings.push_back({ "E_SLIDER_Spirals_Invented", "1" });
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::EffectSettingUnknown, "unknown key");
        spec = CorrectSpec();
        spec.effects[1].settings[1].second = "sideways";
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::EffectSettingsInvalid, "value not among the options");
        spec = CorrectSpec();
        spec.effects[0].palette = "C_BUTTON_Palette1=notacolor,C_CHECKBOX_Palette1=1";
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::PaletteInvalid, "invalid palette colour");
        spec = CorrectSpec();
        spec.effects[0].settings.push_back({ "B_CHOICE_BufferStyle", "Style That Does Not Exist" });
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::BufferStyleInvalid, "invalid buffer style");
        spec = CorrectSpec();
        spec.effects[1].settings[0].second = "";
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::TextEmpty, "incomplete: text with nothing to show");
        spec = CorrectSpec();
        spec.effects[0].effect = "Effect That Does Not Exist";
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::EffectUnknown, "unknown effect");
    }

    void Case10MissingMedia() {
        Title("10. Video and Pictures without a file");
        SequenceSpec spec = CorrectSpec();
        spec.effects.push_back({ STAR_B, "", 0, "Video", { { "E_FILEPICKERCTRL_Video_Filename", "" } }, WHITE, 0, 1000 });
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::MediaFileMissing, "video with an empty path");

        spec = CorrectSpec();
        spec.effects.push_back({ STAR_B, "", 0, "Pictures", { { "E_TEXTCTRL_Pictures_Filename", "fixture_images/nothing.png" } }, WHITE, 0, 1000 });
        const ValidationReport pictures = ValidatePlan(*_ctx, spec);
        ExpectIssue(pictures, issue::MediaFileMissing, "pictures with an absent file");
        if (BuildAndOpen(spec, "pictures_missing")) {
            const ValidationReport file = ValidateSequenceFile(*_ctx, PathFor("pictures_missing", ".xsq"), &spec, Strictness::Generated);
            ExpectIssue(file, issue::MediaFileMissing, "reopened file");
            const auto both = std::count_if(file.issues.begin(), file.issues.end(), [](const ValidationIssue& i) { return i.severity == IssueSeverity::Error && i.code == issue::EffectSettingsInvalid; });
            Check(both == 0, "xLights' own check for the same file does not count as a second error");
        }
    }

    void Case11MarkerProducesLight() {
        Title("11. Pending marker that lights up");
        const SequenceSpec planned = CorrectSpec();
        SequenceSpec spec = planned;
        spec.effects.push_back(Wash(MATRIX, "", 12000, 12500, GREEN));
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::MarkerProducesLight, "plan");
        if (BuildAndRender(spec, "marker_lit")) {
            const ValidationReport render = Render(planned, "marker_lit");
            ExpectIssue(render, issue::MarkerProducesLight, "render");
            ExpectIssue(render, issue::UnexpectedOutput, "render, from the point of view of the plan");
        }

        spec = CorrectSpec();
        spec.markers[0].target = "Fixture Nothing";
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::MarkerInvalid, "marker on a target that does not exist");
        spec = CorrectSpec();
        spec.markers.clear();
        ExpectClean(ValidatePlan(*_ctx, spec), "a sequence with no markers left is still valid");
    }

    void Case12And13SpanishText() {
        Title("12-13. Spanish text with one colour and with several colours");
        struct Glyph {
            const char* accented;
            const char* plain;
        };
        const Glyph glyphs[] = { { N_TILDE, "n" }, { A_ACUTE, "a" }, { E_ACUTE, "e" }, { I_ACUTE, "i" }, { O_ACUTE, "o" }, { U_ACUTE, "u" }, { U_DIAERESIS, "u" }, { INVERTED_EXCLAMATION, "!" }, { INVERTED_QUESTION, "?" } };
        const std::string phrase = std::string(INVERTED_EXCLAMATION) + "A" + N_TILDE + "o!";
        const std::string word = std::string("se") + N_TILDE + "or";

        constexpr int SLOT = 100;
        SequenceSpec spec;
        spec.frameMS = 25;
        int at = 0;
        const auto add = [&](const std::string& text, const char* palette, const char* orientation = "normal") {
            spec.effects.push_back(TextEffectOn(text, palette, at, at + SLOT, orientation));
            at += SLOT;
            return at - SLOT / 2;
        };
        struct Slots {
            int single;
            int multi;
            int plain;
        };
        std::vector<Slots> slots;
        for (const Glyph& glyph : glyphs) {
            const int single = add(glyph.accented, WHITE);
            const int multi = add(glyph.accented, MULTI);
            const int plain = add(glyph.plain, WHITE);
            slots.push_back({ single, multi, plain });
        }
        const int phraseSingle = add(phrase, WHITE);
        const int phraseMulti = add(phrase, MULTI);
        const int wordMulti = add(word, MULTI);
        const int verticalSingle = add(word, WHITE, "vert text down");
        const int verticalMulti = add(word, MULTI, "vert text down");
        const int lyricStart = at;
        spec.timingTracks.push_back({ "Letra", { { { word, lyricStart, lyricStart + SLOT } }, { { word, lyricStart, lyricStart + SLOT } } } });
        spec.effects.push_back({ MATRIX, "", 0, "Text", { { "E_CHOICE_Text_LyricTrack", "Letra - Words" }, { "E_CHOICE_Text_Dir", "none" }, { "E_CHOICE_Text_Effect", "normal" }, { "E_CHOICE_Text_Count", "none" } }, MULTI, at, at + SLOT });
        const int lyricMulti = at + SLOT / 2;
        at += SLOT;
        spec.durationMS = at;

        ExpectClean(ValidatePlan(*_ctx, spec), "plan");
        if (!BuildAndRender(spec, "spanish_text")) {
            return;
        }
        ExpectClean(Render(spec, "spanish_text"), "render");
        RenderedFrames frames(PathFor("spanish_text", ".fseq"));
        if (!frames.IsValid()) {
            Check(false, "reading the render");
            return;
        }
        const std::vector<uint32_t> matrix = ChannelsOf(*_ctx, MATRIX);
        for (size_t i = 0; i < slots.size(); ++i) {
            const std::vector<bool> single = frames.LitMask(slots[i].single, matrix);
            const std::vector<bool> multi = frames.LitMask(slots[i].multi, matrix);
            const std::vector<bool> plain = frames.LitMask(slots[i].plain, matrix);
            Check(CountLit(single) > 0, fmt::format("{}: one colour draws {} nodes", glyphs[i].accented, CountLit(single)));
            Check(single == multi, fmt::format("{}: several colours draw the same glyph ({} and {} nodes)", glyphs[i].accented, CountLit(single), CountLit(multi)));
            Check(single != plain, fmt::format("{}: differs from {}", glyphs[i].accented, glyphs[i].plain));
        }
        const int oneColor = CountLit(frames.LitMask(phraseSingle, matrix));
        const int severalColors = CountLit(frames.LitMask(phraseMulti, matrix));
        Check(severalColors * 10 >= oneColor * 8 && severalColors * 10 <= oneColor * 12, fmt::format("phrase: several colours light {} nodes and one colour {}", severalColors, oneColor));
        Check(frames.Colors(phraseMulti, matrix).size() >= 2, "phrase: the colours really alternate");
        Check(CountLit(frames.LitMask(wordMulti, matrix)) > 0, "word with several colours lights up");
        Check(frames.LitMask(lyricMulti, matrix) == frames.LitMask(wordMulti, matrix), "the same word from a lyrics track draws the same glyphs");
        // Coloured glyphs round their faintest edge pixels differently from white ones.
        const std::vector<bool> verticalOne = frames.LitMask(verticalSingle, matrix);
        const std::vector<bool> verticalSeveral = frames.LitMask(verticalMulti, matrix);
        const int similarity = SimilarityPercent(verticalOne, verticalSeveral);
        Check(similarity >= 80, fmt::format("vertical text draws the same glyphs with several colours ({} and {} nodes, {}% alike)", CountLit(verticalOne), CountLit(verticalSeveral), similarity));

        SequenceSpec bitmap = CorrectSpec();
        bitmap.effects[1].settings.push_back({ "E_CHOICE_Text_Font", "Calibri" });
        ExpectIssue(ValidatePlan(*_ctx, bitmap), issue::TextUtf8Issue, "accented text with an ASCII-only bitmap font");
        bitmap = CorrectSpec();
        bitmap.effects[1].settings[0].second = "\xC3";
        ExpectIssue(ValidatePlan(*_ctx, bitmap), issue::TextUtf8Issue, "text that is not valid UTF-8");
        bitmap = CorrectSpec();
        bitmap.effects[1].settings[0] = { "E_CHOICE_Text_LyricTrack", TRACK };
        ExpectIssue(ValidatePlan(*_ctx, bitmap), issue::TextLyricTrackInvalid, "lyrics reference without its suffix");
    }

    void Case14GroupAndMember() {
        Title("14. Group and member at the same time");
        const std::vector<uint32_t> starA = ChannelsOf(*_ctx, STAR_A);
        const std::vector<uint32_t> starB = ChannelsOf(*_ctx, STAR_B);
        const auto overlapSpec = [](bool memberFirst, bool blending, const char* method) {
            SequenceSpec spec;
            spec.durationMS = 2000;
            spec.modelBlending = blending;
            EffectSpec group = Wash(GROUP, "", 0, 1000, RED, TargetKind::Group);
            EffectSpec member = Wash(STAR_A, "", 0, 1000, GREEN);
            if (method != nullptr) {
                (memberFirst ? group : member).settings.push_back({ "T_CHOICE_LayerMethod", method });
            }
            spec.effects.push_back(memberFirst ? member : group);
            spec.effects.push_back(memberFirst ? group : member);
            return spec;
        };
        const auto classification = [](const ValidationReport& report) {
            const ValidationIssue* found = report.Find(issue::GroupMemberConflict);
            if (found != nullptr) {
                for (const auto& [key, value] : found->args) {
                    if (key == "classification") {
                        return value;
                    }
                }
            }
            return std::string("none");
        };

        // Each classification is checked against what the renderer really does.
        SequenceSpec spec = overlapSpec(false, false, nullptr);
        ValidationReport report = ValidatePlan(*_ctx, spec);
        Check(classification(report) == overlap::Intentional && report.Ok(), fmt::format("no blending, member below group row: {}", classification(report)));
        if (BuildAndRender(spec, "overlap_detail")) {
            RenderedFrames frames(PathFor("overlap_detail", ".fseq"));
            Check(frames.NodeColor(500, starA) == 0x00FF00 && frames.NodeColor(500, starB) == 0xFF0000, fmt::format("  render: member {:06X} over group {:06X}", frames.NodeColor(500, starA), frames.NodeColor(500, starB)));
        }

        spec = overlapSpec(true, false, nullptr);
        report = ValidatePlan(*_ctx, spec);
        Check(classification(report) == overlap::Confirmed && !report.Ok(), fmt::format("no blending, group below member row: {}", classification(report)));
        if (BuildAndRender(spec, "overlap_hidden")) {
            RenderedFrames frames(PathFor("overlap_hidden", ".fseq"));
            Check(frames.NodeColor(500, starA) == 0xFF0000, fmt::format("  render: the member's effect is really hidden ({:06X})", frames.NodeColor(500, starA)));
        }

        spec = overlapSpec(false, true, nullptr);
        report = ValidatePlan(*_ctx, spec);
        Check(classification(report) == overlap::Possible && report.Ok(), fmt::format("blending, normal mix: {}", classification(report)));

        spec = overlapSpec(false, true, "1 is Mask");
        report = ValidatePlan(*_ctx, spec);
        Check(classification(report) == overlap::Intentional && report.Ok() && report.WarningCount() == 0, fmt::format("blending, mask: {}", classification(report)));

        spec = overlapSpec(false, false, nullptr);
        spec.effects[1].effect = "Off";
        spec.effects[1].settings.clear();
        report = ValidatePlan(*_ctx, spec);
        Check(classification(report) == overlap::Allowed && report.Ok(), fmt::format("an Off effect over the group: {}", classification(report)));

        spec = CorrectSpec();
        report = ValidatePlan(*_ctx, spec);
        Check(!report.Has(issue::GroupMemberConflict), "effects at different times are not a conflict");
    }

    void Case15WrongModel() {
        Title("15. Effect applied to the wrong model");
        const SequenceSpec planned = CorrectSpec();
        SequenceSpec wrong = planned;
        wrong.effects[0].target = STAR_B;
        wrong.effects[0].kind = TargetKind::Any;
        if (BuildAndRender(wrong, "wrong_model")) {
            ExpectIssue(ValidateSequenceFile(*_ctx, PathFor("wrong_model", ".xsq"), &planned, Strictness::Generated), issue::SequenceMismatch, "reopened file against the plan");
            const ValidationReport render = Render(planned, "wrong_model");
            ExpectIssue(render, issue::RenderTargetSilent, "render: the planned target is dark");
            ExpectIssue(render, issue::UnexpectedOutput, "render: another model lit up");
            const ValidationIssue* extra = render.Find(issue::UnexpectedOutput);
            Check(extra != nullptr && extra->model == STAR_B, "the unexpected model is named");
        }
    }

    void Case16SilentEffect() {
        Title("16. Effect that produces no output");
        SequenceSpec spec = CorrectSpec();
        spec.effects[3].palette = BLACK;
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::PaletteEmpty, "plan warns about the colours");
        if (BuildAndRender(spec, "silent")) {
            ExpectIssue(Render(spec, "silent"), issue::RenderTargetSilent, "render");
        }
    }

    void Case17UnexpectedOutput() {
        Title("17. Output during an interval that should be at rest");
        const SequenceSpec planned = CorrectSpec();
        SequenceSpec longer = planned;
        longer.effects[3].endMS = 9500;
        longer.effects[4] = Wash(STAR_A, "Arm 1", 10000, 12000);
        if (BuildAndRender(longer, "rest_lit")) {
            ValidationReport render = Render(planned, "rest_lit");
            ExpectIssue(render, issue::UnexpectedOutput, "render");
            const ValidationIssue* found = render.Find(issue::UnexpectedOutput);
            Check(found != nullptr && found->startMS == 11000 && found->endMS == 12000, fmt::format("the interval is reported ({}-{} ms)", found != nullptr ? found->startMS : -1, found != nullptr ? found->endMS : -1));
        }
    }

    void Case18IncompatibleLayout() {
        Title("18. Sequence and layout that do not match");
        const SequenceSpec spec = CorrectSpec();
        if (!Build(spec, "for_fixture")) {
            return;
        }
        const std::string fingerprint = LayoutFingerprint(*_ctx);
        {
            HeadlessRenderContext other;
            if (!other.LoadShowFolder(_other.string()) || !other.OpenSequence(PathFor("for_fixture", ".xsq"))) {
                Check(false, "opening against the other layout");
                return;
            }
            const ValidationReport report = ValidateSequenceFile(other, PathFor("for_fixture", ".xsq"), nullptr, Strictness::FromFile);
            ExpectIssue(report, issue::LayoutIncompatible, "another layout");
            ExpectIssue(report, issue::ModelNotFound, "another layout");
            const ValidationIssue* found = report.Find(issue::LayoutIncompatible);
            Check(found != nullptr && DescribeIssue(*found).find(MATRIX) != std::string::npos && DescribeIssue(*found).find(_other.string()) != std::string::npos, "the issue names the missing models and the selected folder");
            ExpectIssue(ValidatePlan(other, spec), issue::LayoutIncompatible, "the same check before generating");
            (void)other.CloseSequence();
        }
        {
            // The file sits next to its own layout: the situation of opening a test
            // sequence with the wrong show folder selected.
            const fs::path sibling = _root / "own_layout";
            WriteFixtureShow(sibling, "", 49);
            std::error_code ec;
            fs::copy_file(PathFor("for_fixture", ".xsq"), sibling / "for_fixture.xsq", ec);
            HeadlessRenderContext other;
            if (!other.LoadShowFolder(_other.string()) || !other.OpenSequence((sibling / "for_fixture.xsq").string())) {
                Check(false, "opening from the sibling folder");
                return;
            }
            ExpectIssue(ValidateSequenceFile(other, (sibling / "for_fixture.xsq").string(), nullptr, Strictness::FromFile), issue::ShowFolderMismatch, "show folder other than the sequence's");
            (void)other.CloseSequence();
        }
        {
            HeadlessRenderContext reshaped;
            if (!reshaped.LoadShowFolder(_reshaped.string())) {
                Check(false, "loading the reshaped layout");
                return;
            }
            SequenceSpec recorded = spec;
            recorded.layoutFingerprint = fingerprint;
            Check(fingerprint != LayoutFingerprint(reshaped), "same names and another size give another fingerprint");
            ExpectIssue(ValidatePlan(reshaped, recorded), issue::LayoutIncompatible, "same names, other geometry");
            ExpectClean(ValidatePlan(*_ctx, recorded), "the recorded layout still matches itself");
        }
    }

    void Case19ReopenedSequence() {
        Title("19. Correct sequence after saving and reopening with new instances");
        const SequenceSpec spec = CorrectSpec();
        if (!Build(spec, "reopen")) {
            return;
        }
        HeadlessRenderContext fresh;
        if (!fresh.LoadShowFolder(_show.string()) || !fresh.OpenSequence(PathFor("reopen", ".xsq"))) {
            Check(false, "opening with a new context");
            return;
        }
        const ValidationReport report = ValidateSequenceFile(fresh, PathFor("reopen", ".xsq"), &spec, Strictness::Generated);
        ExpectClean(report, "file");
        Check(report.Status(IssueCategory::Structural) == CheckStatus::Passed && report.Status(IssueCategory::Configuration) == CheckStatus::Passed, "structural and configuration status are PASSED");
        Check(report.Status(IssueCategory::Render) == CheckStatus::NotRun, "render status stays NOT_RUN without a render");
        (void)fresh.CloseSequence();
    }

    void Case20Cancellation() {
        Title("20. Cancellation");
        const SequenceSpec spec = CorrectSpec();
        const ValidationReport plan = ValidatePlan(*_ctx, spec, [] { return true; });
        Check(plan.cancelled && !plan.Ok(), "plan: a cancelled validation is not a pass");
        Check(plan.Status(IssueCategory::Structural) == CheckStatus::Cancelled, "plan: status is CANCELLED");

        if (!BuildAndRender(spec, "cancel")) {
            return;
        }
        int polls = 0;
        const ValidationReport render = ValidateRender(*_ctx, PathFor("cancel", ".fseq"), spec, Strictness::Generated, [&] { return ++polls > 3; });
        Check(render.cancelled && !render.Ok() && polls == 4, fmt::format("render: stops at the first poll that asks for it ({} polls)", polls));
        Check(render.Status(IssueCategory::Render) == CheckStatus::Cancelled, "render: status is CANCELLED");
        Check(ValidateSequenceFile(*_ctx, PathFor("cancel", ".xsq"), &spec, Strictness::Generated, [] { return true; }).cancelled, "file: cancelled");
    }

    void CaseExtraTiming() {
        Title("Extra. Times and tracks");
        SequenceSpec spec = CorrectSpec();
        spec.timingTracks[0].layers[2].back().endMS = spec.durationMS + 1000;
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::TimingInvalid, "mark beyond the end of the song");
        spec = CorrectSpec();
        spec.timingTracks[0].layers[2][1].startMS = 4100;
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::TimingInvalid, "overlapping marks");
        spec = CorrectSpec();
        spec.timingTracks[0].layers[2][0].label = "hello";
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::FacePhonemeUnknown, "a word in the phoneme layer");
        spec = CorrectSpec();
        spec.timingTracks[0].layers[2].push_back({ "O", 7000, 7500 });
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::FacePhonemeOutsideWord, "mouth moving during a pause");
        spec = CorrectSpec();
        spec.effects[0].endMS = spec.durationMS + 25;
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::EffectIntervalInvalid, "effect beyond the end of the song");
        spec = CorrectSpec();
        spec.effects.push_back(Wash(TREE, "", 1000, 1500));
        ExpectIssue(ValidatePlan(*_ctx, spec), issue::EffectOverlapInLayer, "two effects on one layer");
    }

    void CaseExtraWriterGate() {
        Title("Extra. The writer refuses to publish what fails validation");
        const fs::path destination = _show / "gate.xsq";
        SequenceSpec bad = CorrectSpec();
        bad.effects[0].target = "Fixture Nothing";
        WriteResult result = WriteSequenceSafely(_show.string(), bad, "gate.xsq");
        const bool namesModel = std::any_of(result.problems.begin(), result.problems.end(), [](const std::string& p) { return p.find(issue::ModelNotFound) != std::string::npos && p.find("Fixture Nothing") != std::string::npos; });
        Check(!result.ok && namesModel, "refused, with the name of the model");
        Check(!FileExists(destination.string()), "nothing published");
        std::error_code ec;
        Check(!fs::exists(_show / "ShowComposer", ec), "no temporary left behind");

        result = WriteSequenceSafely(_show.string(), CorrectSpec(), "gate.xsq");
        Check(result.ok && result.validation.Ok() && FileExists(destination.string()), "a correct plan is published");
        Check(result.validation.Status(IssueCategory::Structural) == CheckStatus::Passed, "the result carries the validation");
    }

    fs::path _root;
    fs::path _show;
    fs::path _other;
    fs::path _reshaped;
    std::unique_ptr<HeadlessRenderContext> _ctx;
    int _passed = 0;
    int _failed = 0;
};

} // namespace

int RunValidateCommand(const std::string& showDir, const std::string& xsqPath, const std::string& fseqPath, bool useWriterSpec) {
    // Loading a show pings its active controllers; a folder with no networks file has none.
    if (FileExists(showDir + "/xlights_networks.xml")) {
        std::string refused;
        if (!ControllersAreInactive(showDir, refused)) {
            printf("composer-validate: REFUSED: %s\n", refused.c_str());
            return 2;
        }
    }
    ObtainAccessToURL(showDir, false);
    ObtainAccessToURL(xsqPath, false);

    HeadlessRenderContext ctx;
    if (!ctx.LoadShowFolder(showDir)) {
        printf("composer-validate: REFUSED: cannot load the show folder '%s'\n", showDir.c_str());
        return 2;
    }
    if (!ctx.OpenSequence(xsqPath)) {
        printf("composer-validate: REFUSED: cannot open the sequence '%s'\n", xsqPath.c_str());
        return 2;
    }
    const SequenceSpec writerSpec = BuildWriterTestSpec();
    const SequenceSpec* planned = useWriterSpec ? &writerSpec : nullptr;
    const Strictness strictness = useWriterSpec ? Strictness::Generated : Strictness::FromFile;

    printf("composer-validate: show folder: %s\ncomposer-validate: sequence: %s\ncomposer-validate: layout has %zu models and groups\n", showDir.c_str(), xsqPath.c_str(), ctx.AllModels.GetModels().size());
    ValidationReport report = ValidateSequenceFile(ctx, xsqPath, planned, strictness);
    if (!fseqPath.empty()) {
        const SequenceSpec expected = useWriterSpec ? writerSpec : SpecFromSequence(ctx);
        report.Merge(ValidateRender(ctx, fseqPath, expected, strictness));
    }
    for (const ValidationIssue& issue : report.issues) {
        if (issue.severity == IssueSeverity::Error) {
            printf("composer-validate: ERROR %s [%s] %d-%d ms: %s Suggestion: %s\n", issue.code.c_str(), issue.model.c_str(), issue.startMS, issue.endMS, DescribeIssue(issue).c_str(), DescribeSuggestion(issue).c_str());
        }
    }
    for (const RenderMetric& metric : report.metrics) {
        printf("composer-validate: METRIC %s / %s %d-%d ms: frames with light=%d distinct frames=%d max lit=%d of %d channels\n", metric.target.c_str(), metric.effect.c_str(), metric.startMS, metric.endMS, metric.framesWithLight, metric.distinctFrames, metric.maxChannelsLit, metric.channels);
    }
    PrintValidationSummary("composer-validate", report);
    (void)ctx.CloseSequence();
    return report.Ok() ? 0 : 1;
}

int RunValidatorTest(const std::string& workDir) {
    std::error_code ec;
    fs::path root;
    for (int attempt = 0;; ++attempt) {
        root = fs::path(workDir) / fmt::format("composer-validator-test-{}", attempt);
        if (!fs::exists(root, ec)) {
            break;
        }
    }
    fs::create_directories(root, ec);
    if (ec) {
        printf("composer-validator-test: cannot create '%s': %s\n", root.string().c_str(), ec.message().c_str());
        return 1;
    }
    Suite suite(root);
    return suite.Run();
}

} // namespace composer
