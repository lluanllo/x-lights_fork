#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

#include "composer/WriterTest.h"

#include "composer/GenerationValidator.h"
#include "composer/SequenceWriter.h"
#include "utils/ExternalHooks.h"

#include <pugixml.hpp>
#include <spdlog/fmt/fmt.h>

#include <cstdio>
#include <string_view>

namespace composer {

namespace {

constexpr const char* VOICE_TRACK = "Voz, prueba española";
constexpr const char* RESOURCES_TRACK = "Recursos pendientes";

const std::string PALETTE =
    "C_BUTTON_Palette1=#FF0000,C_BUTTON_Palette2=#00FF00,C_BUTTON_Palette3=#0000FF,C_BUTTON_Palette4=#FFFF00,"
    "C_BUTTON_Palette5=#FF00FF,C_BUTTON_Palette6=#00FFFF,C_BUTTON_Palette7=#FFFFFF,C_BUTTON_Palette8=#000000,"
    "C_CHECKBOX_Palette1=1,C_CHECKBOX_Palette2=1";

// With more than one active colour TextEffect draws the string one byte at a
// time, which splits every multi-byte character into invalid glyphs.
const std::string SINGLE_COLOR_PALETTE =
    "C_BUTTON_Palette1=#FFFFFF,C_BUTTON_Palette2=#00FF00,C_BUTTON_Palette3=#0000FF,C_BUTTON_Palette4=#FFFF00,"
    "C_BUTTON_Palette5=#FF00FF,C_BUTTON_Palette6=#00FFFF,C_BUTTON_Palette7=#FF0000,C_BUTTON_Palette8=#000000,"
    "C_CHECKBOX_Palette1=1";

// Exercises every mouth shape, including the ones that are identical on some
// props. It is a mouth test pattern, not the phonetics of the words above it.
std::vector<TimingMark> PhonemePattern(int startMS, const std::vector<const char*>& phonemes) {
    constexpr int PHONEME_MS = 500;
    std::vector<TimingMark> marks;
    int at = startMS;
    for (const char* phoneme : phonemes) {
        marks.push_back({ phoneme, at, at + PHONEME_MS });
        at += PHONEME_MS;
    }
    return marks;
}

TimingTrackSpec VoiceTrack() {
    std::vector<TimingMark> phonemes = PhonemePattern(20000, { "MBP", "E", "AI", "etc", "O", "U", "WQ", "FV" });
    const std::vector<TimingMark> second = PhonemePattern(26000, { "L", "AI", "etc", "E", "O", "MBP", "U", "etc" });
    phonemes.insert(phonemes.end(), second.begin(), second.end());
    return {
        VOICE_TRACK,
        {
            { { "prueba uno", 20000, 24000 }, { "prueba dos", 26000, 30000 } },
            { { "prueba", 20000, 22000 }, { "uno", 22000, 24000 }, { "prueba", 26000, 28000 }, { "dos", 28000, 30000 } },
            phonemes,
        }
    };
}

} // namespace

SequenceSpec BuildWriterTestSpec() {
    SequenceSpec spec;
    spec.frameMS = 25;
    spec.durationMS = 60000;

    spec.timingTracks.push_back(VoiceTrack());
    spec.timingTracks.push_back({ RESOURCES_TRACK,
                                  { { { "C001 · Vídeo · Prueba, año", 56000, 58000 }, { "C002 · Imagen · Prueba", 58000, 59000 } } } });

    spec.effects.push_back({ "Mega Tree", "", 0, "Spirals",
                             { { "E_SLIDER_Spirals_Count", "2" }, { "E_SLIDER_Spirals_Rotation", "20" }, { "E_SLIDER_Spirals_Thickness", "50" }, { "E_TEXTCTRL_Spirals_Movement", "2.0" } },
                             PALETTE, 0, 10000 });
    spec.effects.push_back({ "HD Matrix", "", 0, "Text",
                             { { "E_TEXTCTRL_Text", "¡Año!" }, { "E_CHOICE_Text_Dir", "none" }, { "E_CHOICE_Text_Effect", "normal" }, { "E_CHOICE_Text_Count", "none" } },
                             SINGLE_COLOR_PALETTE, 10000, 20000 });
    // Eyes stay open so that frame differences on the face can only come from the mouth.
    spec.effects.push_back({ "Carita rosa chica", "", 0, "Faces",
                             { { "E_CHOICE_Faces_FaceDefinition", "Upright Bulb" }, { "E_CHOICE_Faces_Eyes", "Open" }, { "E_CHECKBOX_Faces_Outline", "1" }, { "E_CHOICE_Faces_TimingTrack", VOICE_TRACK } },
                             PALETTE, 20000, 32000 });
    spec.effects.push_back({ "COPOS DE NIEVE IZQUIERDA", "", 0, "Color Wash",
                             { { "E_TEXTCTRL_ColorWash_Cycles", "1.0" }, { "B_CHOICE_BufferStyle", "Per Model Default" }, { "T_CHOICE_LayerMethod", "Normal" } },
                             PALETTE, 35000, 45000, TargetKind::Group });
    spec.effects.push_back({ "Copo de Nieve 1", "Arm 1", 0, "Color Wash",
                             { { "E_TEXTCTRL_ColorWash_Cycles", "1.0" } },
                             PALETTE, 45000, 55000 });
    spec.markers.push_back({ "C001", "HD Matrix", 56000, 58000 });
    spec.markers.push_back({ "C002", "HD Matrix", 58000, 59000 });
    return spec;
}

bool ControllersAreInactive(const std::string& showDir, std::string& error) {
    const std::string path = showDir + "/xlights_networks.xml";
    if (!FileExists(path)) {
        error = fmt::format("no existe '{}'", path);
        return false;
    }
    pugi::xml_document doc;
    if (!doc.load_file(path.c_str())) {
        error = fmt::format("no se pudo leer '{}'", path);
        return false;
    }
    for (pugi::xml_node controller : doc.child("Networks").children("Controller")) {
        // Same fallback as Controller's constructor for files without ActiveState.
        std::string_view state = controller.attribute("ActiveState").as_string("");
        const bool inactive = state.empty() ? std::string_view(controller.attribute("Active").as_string("1")) == "0" : state == "Inactive";
        if (!inactive) {
            error = fmt::format("el controlador '{}' no está inactivo. Esta prueba solo se ejecuta sobre una copia del show con todos los controladores en Inactive", controller.attribute("Name").as_string(""));
            return false;
        }
    }
    return true;
}

void PrintValidationSummary(const char* tool, const ValidationReport& report) {
    for (const ValidationIssue& issue : report.issues) {
        if (issue.severity != IssueSeverity::Error) {
            printf("%s: %s %s [%s] %d-%d ms: %s\n", tool, issue.severity == IssueSeverity::Warning ? "WARNING" : "INFO", issue.code.c_str(), issue.model.c_str(), issue.startMS, issue.endMS, DescribeIssue(issue).c_str());
        }
    }
    printf("%s: errors=%d warnings=%d structural=%s configuration=%s render=%s visual=%s\n", tool, report.ErrorCount(), report.WarningCount(), StatusName(report.Status(IssueCategory::Structural)),
           StatusName(report.Status(IssueCategory::Configuration)), StatusName(report.Status(IssueCategory::Render)), StatusName(report.VisualStatus()));
}

int RunWriterTest(const std::string& showDir, const std::string& destinationName) {
    std::string refused;
    if (!ControllersAreInactive(showDir, refused)) {
        printf("composer-writer-test: RECHAZADO: %s\n", refused.c_str());
        return 2;
    }

    const SequenceSpec spec = BuildWriterTestSpec();
    const WriteResult result = WriteSequenceSafely(showDir, spec, destinationName);
    for (const std::string& problem : result.problems) {
        printf("composer-writer-test: ERROR: %s\n", problem.c_str());
    }
    for (const std::string& leftover : result.leftoverFiles) {
        printf("composer-writer-test: NO SE PUDO LIMPIAR: %s\n", leftover.c_str());
    }
    PrintValidationSummary("composer-writer-test", result.validation);
    if (!result.ok) {
        printf("composer-writer-test: FALLO\n");
        return 1;
    }
    printf("composer-writer-test: OK: %s (%zu pistas, %zu efectos, verificada al reabrir)\n", result.finalPath.c_str(), spec.timingTracks.size(), spec.effects.size());
    return result.leftoverFiles.empty() ? 0 : 1;
}

} // namespace composer
