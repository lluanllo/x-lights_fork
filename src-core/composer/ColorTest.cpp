#include "composer/ColorTest.h"

#include "composer/ChromaticDirector.h"
#include "composer/ColorCheck.h"
#include "composer/ColorSpace.h"
#include "composer/FigureGeometry.h"
#include "composer/FigureSemantics.h"
#include "composer/GenerationValidator.h"
#include "composer/ModelCapabilities.h"
#include "composer/PlanTest.h"
#include "composer/ProceduralField.h"
#include "composer/SequenceWriter.h"
#include "composer/SongAnalysis.h"
#include "composer/StageDirector.h"
#include "composer/StageSpace.h"
#include "composer/UnderstandingStore.h"
#include "composer/WriterTest.h"
#include "render/FSEQFile.h"
#include "render/HeadlessRenderContext.h"

#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>

namespace fs = std::filesystem;

namespace composer {

namespace {

constexpr int FRAME = 25;

bool WriteText(const fs::path& path, const std::string& text) {
    FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(path.wstring().c_str(), L"wb");
#else
    f = fopen(path.string().c_str(), "wb");
#endif
    if (f == nullptr) {
        return false;
    }
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    return fclose(f) == 0 && ok;
}

// The scenes a colour score made in one family on purpose, read loosely so any version
// of the file can be measured.
std::set<std::string> IntendedScenes(const std::string& chromaPath) {
    std::set<std::string> out;
    std::string text;
    if (chromaPath.empty() || !ReadTextFile(chromaPath, text)) {
        return out;
    }
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (!root.is_object()) {
        return out;
    }
    for (const nlohmann::json& s : root.value("scenes", nlohmann::json::array())) {
        if (s.is_object() && s.value("monochrome", false)) {
            out.insert(s.value("id", std::string()));
        }
    }
    return out;
}

class Frames {
public:
    explicit Frames(const std::string& path) {
        std::unique_ptr<FSEQFile> file(FSEQFile::openFSEQFile(path));
        if (file == nullptr) {
            return;
        }
        _channels = file->getMaxChannel();
        file->prepareRead({ { 0, _channels } });
        for (uint64_t f = 0; f < file->getNumFrames(); ++f) {
            std::vector<uint8_t> data(_channels, 0);
            std::unique_ptr<FSEQFile::FrameData> frame(file->getFrame(static_cast<uint32_t>(f)));
            if (frame != nullptr) {
                frame->readFrame(data.data(), _channels);
            }
            _frames.push_back(std::move(data));
        }
    }
    [[nodiscard]] bool Valid() const { return !_frames.empty(); }
    [[nodiscard]] int Count() const { return static_cast<int>(_frames.size()); }
    [[nodiscard]] std::array<int, 3> Light(int frame, int channel) const {
        const std::vector<uint8_t>& f = _frames.at(static_cast<size_t>(std::clamp(frame, 0, Count() - 1)));
        if (channel < 0 || static_cast<size_t>(channel + 2) >= f.size()) {
            return { 0, 0, 0 };
        }
        return { f[channel], f[channel + 1], f[channel + 2] };
    }

private:
    uint32_t _channels = 0;
    std::vector<std::vector<uint8_t>> _frames;
};

std::string Palette(const std::vector<std::string>& colors) {
    std::string out;
    for (size_t i = 0; i < colors.size(); ++i) {
        out += fmt::format("{}C_BUTTON_Palette{}={}", out.empty() ? "" : ",", i + 1, colors[i]);
    }
    for (size_t i = 0; i < colors.size(); ++i) {
        out += fmt::format(",C_CHECKBOX_Palette{}=1", i + 1);
    }
    return out;
}

double Hue(const std::string& hex) {
    return OklchOfHex(hex).h;
}

class Suite {
public:
    explicit Suite(fs::path root) :
        _root(std::move(root)) {}

    int Run() {
        std::error_code ec;
        fs::create_directories(_root, ec);
        TestSpace();
        TestPalettes();
        if (Setup()) {
            TestDirector();
            TestEdits();
            TestApply();
            TestRender();
            TestMeasure();
        }
        if (_ctx != nullptr) {
            (void)_ctx->CloseSequence();
        }
        printf("composer-color-test: %d checks passed, %d failed (%s)\n", _passed, _failed, _root.string().c_str());
        return _failed == 0 ? 0 : 1;
    }

private:
    void Check(bool ok, const std::string& what) {
        printf("  %s %s\n", ok ? "OK   " : "FAILED", what.c_str());
        fflush(stdout);
        ok ? ++_passed : ++_failed;
    }

    void Title(const char* t) {
        printf("=== %s\n", t);
        fflush(stdout);
    }

    // ------------------------------------------------------------ colour space

    void TestSpace() {
        Title("Colour as people see it: OKLab and OKLCH, inside what xLights can show");
        bool roundTrip = true;
        for (const char* hex : { "#FF0000", "#00FF00", "#0000FF", "#FFFFFF", "#123456", "#F0A020", "#000000" }) {
            Rgb c;
            roundTrip = roundTrip && ParseHex(hex, c) && ToHex(ToRgbInGamut(ToOklch(c))) == hex;
        }
        Check(roundTrip, "every colour comes back the same through OKLCH");
        const Oklch red = OklchOfHex("#FF0000");
        const Oklch white = OklchOfHex("#FFFFFF");
        Check(std::fabs(red.L - 0.628) < 0.005 && std::fabs(red.C - 0.258) < 0.005 && std::fabs(red.h - 29.2) < 1.0 && std::fabs(white.L - 1.0) < 0.002 && white.C < 0.001,
              fmt::format("known values: red L {:.3f} C {:.3f} h {:.1f}, white L {:.3f}", red.L, red.C, red.h, white.L));
        const Oklch tooMuch { 0.7, 0.4, 150 };
        const Rgb raw = ToRgb(ToOklab(tooMuch));
        const Rgb fitted = ToRgbInGamut(tooMuch);
        const Oklch back = ToOklch(fitted);
        Check(!InGamut(raw) && InGamut(fitted) && std::fabs(back.L - 0.7) < 0.01 && HueDistance(back.h, 150) < 2,
              fmt::format("a colour outside the screen keeps its lightness and hue and loses only chroma (L {:.3f}, h {:.1f}, C {:.3f})", back.L, back.h, back.C));
        Check(DeltaE("#FF0000", "#FF0000") == 0 && DeltaE("#FF0000", "#00FF00") > 40 && DeltaE("#FF0000", "#FE0101") < 1.5, "differences: none, a lot, almost none");
        Check(std::fabs(HueDistance(350, 10) - 20) < 1e-9 && std::fabs(HueDistance(10, 190) - 180) < 1e-9, "hue differences go round the circle");
        Check(ColorFamily(OklchOfHex("#FFFFFF")) == FAMILY_WHITE && ColorFamily(OklchOfHex("#606060")) == FAMILY_NEUTRAL && std::string(ColorFamilyId(ColorFamily(red))) == "red" &&
                  std::string(ColorFamilyId(ColorFamily(OklchOfHex("#0000FF")))) == "blue" && std::string(ColorFamilyId(ColorFamily(OklchOfHex("#00FF00")))) == "green",
              "families a person names: white, grey, red, blue, green");
        const Oklch mixed = Mix(OklchOfHex("#0000FF"), white, 0.5);
        Check(HueDistance(mixed.h, OklchOfHex("#0000FF").h) < 1, "a mix with white keeps the colour's hue (white has none of its own)");
        Check(CuspLightness(Hue("#FFFF00")) > CuspLightness(Hue("#FF0000")) && CuspLightness(Hue("#FF0000")) > CuspLightness(Hue("#0000FF")),
              fmt::format("each hue is fullest at its own lightness: yellow {:.2f} > red {:.2f} > blue {:.2f}", CuspLightness(Hue("#FFFF00")), CuspLightness(Hue("#FF0000")),
                          CuspLightness(Hue("#0000FF"))));
        Check(Warmth(Hue("#FF8000")) > 0.5 && Warmth(Hue("#0080FF")) < -0.5, "orange reads warm, azure cool");
    }

    // ------------------------------------------------------------ palettes

    void TestPalettes() {
        Title("Palettes: a family with a hierarchy, by harmony, inside the gamut");
        static const std::vector<std::string> ROLES = { "anchor", "secondary", "accent", "contrast", "transition", "neutral", "shadow", "highlight" };
        bool complete = true, inGamut = true;
        for (Harmony h : { Harmony::Monochromatic, Harmony::Analogous, Harmony::Complementary, Harmony::SplitComplementary, Harmony::Triadic, Harmony::Tetradic, Harmony::WarmCool }) {
            PaletteRequest r;
            r.anchorHue = 250;
            r.harmony = h;
            const ColorPaletteProfile p = BuildPalette(r);
            for (const std::string& role : ROLES) {
                Rgb c;
                complete = complete && p.Get(role) != nullptr && ParseHex(p.Hex(role), c);
                inGamut = inGamut && InGamut(c);
            }
        }
        Check(complete, "every harmony gives the eight roles: anchor, secondary, accent, contrast, transition, neutral, deep, light");
        Check(inGamut, "every colour of every palette is one xLights can show");
        const auto build = [](Harmony h, double contrast = 0.5, double saturation = 0.8, double lightness = 0.66) {
            PaletteRequest r;
            r.anchorHue = 30;
            r.harmony = h;
            r.contrast = contrast;
            r.saturation = saturation;
            r.lightness = lightness;
            return BuildPalette(r);
        };
        const ColorPaletteProfile comp = build(Harmony::Complementary);
        const ColorPaletteProfile tri = build(Harmony::Triadic);
        const ColorPaletteProfile ana = build(Harmony::Analogous);
        const ColorPaletteProfile mono = build(Harmony::Monochromatic);
        const double anchorHue = comp.Get("anchor")->h;
        Check(HueDistance(comp.Get("accent")->h, anchorHue + 180) < 15, fmt::format("opposite colours: the accent across the circle ({:.0f}° from the anchor)", HueDistance(comp.Get("accent")->h, anchorHue)));
        Check(std::fabs(HueDistance(tri.Get("secondary")->h, anchorHue) - 120) < 15 && std::fabs(HueDistance(tri.Get("accent")->h, anchorHue) - 120) < 15,
              "three colours: a third of the circle apart");
        Check(HueDistance(ana.Get("secondary")->h, anchorHue) <= 40, "neighbouring colours: the secondary next to the anchor");
        bool oneHue = true;
        for (const char* role : { "secondary", "accent", "contrast", "shadow", "highlight" }) {
            const PaletteColor* c = mono.Get(role);
            oneHue = oneHue && (c->C < NEUTRAL_CHROMA || HueDistance(c->h, mono.Get("anchor")->h) < 8);
        }
        Check(oneHue && std::fabs(mono.Get("contrast")->L - mono.Get("anchor")->L) > 0.1, "one colour: every role in the anchor's hue, the contrast in lightness");
        const ColorPaletteProfile soft = build(Harmony::Complementary, 0.0);
        const ColorPaletteProfile strong = build(Harmony::Complementary, 1.0);
        const double softSpread = std::fabs(soft.Get("contrast")->L - soft.Get("anchor")->L);
        const double strongSpread = std::fabs(strong.Get("contrast")->L - strong.Get("anchor")->L);
        Check(strongSpread > softSpread, fmt::format("more contrast spreads the lightness ({:.2f} against {:.2f})", strongSpread, softSpread));
        Check(build(Harmony::Analogous, 0.5, 0.4).Get("anchor")->C < build(Harmony::Analogous, 0.5, 1.0).Get("anchor")->C, "less saturation, less chroma");
        Check(build(Harmony::Analogous, 0.5, 0.8, 0.75).Get("anchor")->L > build(Harmony::Analogous, 0.5, 0.8, 0.55).Get("anchor")->L, "a lighter palette, a lighter anchor");
        Check(DeltaE(ana.Hex("anchor"), ana.Hex("secondary")) >= 10 && DeltaE(tri.Hex("anchor"), tri.Hex("secondary")) >= 10, "the secondary always differs from the anchor");
        PaletteRequest pulled;
        pulled.anchorHue = 30;
        pulled.harmony = Harmony::Complementary;
        pulled.suggested = { { WrapHue(30 + 25 + 20), 1.0 } };
        const ColorPaletteProfile moved = BuildPalette(pulled);
        Check(HueDistance(moved.Get("secondary")->h, 75) < HueDistance(comp.Get("secondary")->h, 75) && moved.reason.find("song's colours") != std::string::npos,
              "a related colour moves towards a colour the song suggests, and says so");
        PaletteRequest again;
        again.anchorHue = 200;
        again.harmony = Harmony::SplitComplementary;
        Check(BuildPalette(again).colors.size() == 8 && BuildPalette(again).Hex("accent") == BuildPalette(again).Hex("accent"), "the same request, the same palette");
        Check(HarmonyFromId("split-complementary", pulled.harmony) && pulled.harmony == Harmony::SplitComplementary && !HarmonyFromId("rainbow", pulled.harmony), "harmony ids read back; unknown ones refused");
    }

    // ------------------------------------------------------------ fixture

    bool Setup() {
        _show = _root / "show";
        if (!WritePlanFixture(_show.string())) {
            Check(false, "the fixture show is written");
            return false;
        }
        pugi::xml_document doc;
        const fs::path file = _show / "xlights_rgbeffects.xml";
        if (!doc.load_file(file.string().c_str())) {
            Check(false, "the fixture layout is read");
            return false;
        }
        const std::map<std::string, std::vector<std::pair<const char*, const char*>>> places = {
            { "Matriz Prueba", { { "WorldPosX", "150" }, { "WorldPosY", "300" }, { "ScaleX", "1" }, { "ScaleY", "1" } } },
            { "Arbol Grande", { { "WorldPosX", "450" }, { "WorldPosY", "300" }, { "ScaleX", "1" }, { "ScaleY", "1" } } },
            { "Arbol Cara", { { "WorldPosX", "650" }, { "WorldPosY", "250" }, { "ScaleX", "1" }, { "ScaleY", "1" } } },
            { "Carita", { { "WorldPosX", "820" }, { "WorldPosY", "300" }, { "ScaleX", "1" }, { "ScaleY", "1" } } },
            { "Linea 1", { { "WorldPosX", "1000" }, { "WorldPosY", "150" }, { "X2", "0" }, { "Y2", "300" } } },
        };
        for (pugi::xml_node m : doc.child("xrgb").child("models").children("model")) {
            const auto it = places.find(m.attribute("name").value());
            if (it == places.end()) {
                continue;
            }
            for (const auto& [k, v] : it->second) {
                if (m.attribute(k)) {
                    m.attribute(k).set_value(v);
                } else {
                    m.append_attribute(k) = v;
                }
            }
        }
        if (!doc.save_file(file.string().c_str())) {
            Check(false, "the placed layout is saved");
            return false;
        }
        _ctx = std::make_unique<HeadlessRenderContext>();
        if (!_ctx->LoadShowFolder(_show.string())) {
            Check(false, "the fixture show loads");
            return false;
        }
        _caps = ExtractCapabilities(*_ctx, nullptr);
        _geometry = DescribeFigures(*_ctx, _caps, true);
        _semantics = DescribeSemantics(*_ctx, _geometry, _caps, nullptr);
        _space = BuildStageSpace(_geometry, nullptr);
        // A song of eight parts: silence, intro, verse, chorus, verse, chorus, bridge, the
        // last chorus as the climax, and an ending.
        struct Part {
            const char* kind;
            int start, end, group;
            double energy;
            const char* flag;
        };
        const std::vector<Part> parts = { { "unknown", 0, 2000, -1, 0, "silence" },    { "intro", 2000, 10000, -1, 0.25, "intro" },
                                          { "verse", 10000, 20000, 0, 0.45, "" },      { "chorus", 20000, 30000, 1, 0.8, "" },
                                          { "verse", 30000, 40000, 0, 0.5, "" },       { "chorus", 40000, 50000, 1, 0.82, "" },
                                          { "bridge", 50000, 58000, -1, 0.6, "" },     { "chorus", 58000, 68000, 1, 0.95, "climax" },
                                          { "outro", 68000, 76000, -1, 0.25, "ending" } };
        std::map<int, int> occurrence;
        for (size_t i = 0; i < parts.size(); ++i) {
            PlanSegment s;
            s.id = fmt::format("s{}.0", i);
            s.startMS = parts[i].start;
            s.endMS = parts[i].end;
            s.sectionKind = parts[i].kind;
            s.repeatGroup = parts[i].group;
            s.occurrence = parts[i].group >= 0 ? occurrence[parts[i].group]++ : 0;
            s.energy = parts[i].energy;
            s.targetIntensity = parts[i].energy;
            s.priority = std::string(parts[i].flag) == "climax" ? 3 : 1;
            if (*parts[i].flag != 0) {
                s.flags.push_back(parts[i].flag);
            }
            _plan.segments.push_back(s);
        }
        _plan.durationMS = 76000;
        _song.key = "C major";
        _song.tempo.push_back({ 0, 120, 1 });
        // Every placed model plays every part with a two-colour effect, the groups too.
        for (const PlanSegment& s : _plan.segments) {
            if (s.energy <= 0) {
                continue;
            }
            for (const char* t : { "Matriz Prueba", "Arbol Grande", "Arbol Pequeno", "Arbol Cara", "Linea 1", "Estrella 1", "Arco 1", "Grupo Arboles" }) {
                ChoreographyEvent e;
                e.id = s.id + "/" + t;
                e.target = t;
                e.segmentId = s.id;
                e.startMS = s.startMS;
                e.endMS = s.endMS;
                e.effect = "Color Wash";
                e.colors = { "#FF0000", "#FFFFFF" };
                _choreo.events.push_back(e);
            }
        }
        return true;
    }

    ChromaticInput Input(const ChromaticEdits* edits = nullptr, ChromaticOptions options = {}) const {
        ChromaticInput in;
        in.plan = &_plan;
        in.analysis = &_song;
        in.geometry = &_geometry;
        in.semantics = &_semantics;
        in.space = &_space;
        in.choreography = &_choreo;
        in.capabilities = &_caps;
        in.stage = _stage.scenes.empty() ? nullptr : &_stage;
        in.edits = edits;
        in.options = options;
        return in;
    }

    // ------------------------------------------------------------ director

    void TestDirector() {
        Title("The colour director: the song's family, a palette per scene, a role per figure");
        // A stage score over the same scenes: the protagonist in the chorus, a wave in it.
        for (const PlanSegment& s : _plan.segments) {
            StageScene sc;
            sc.id = s.id;
            sc.startMS = s.startMS;
            sc.endMS = s.endMS;
            sc.sectionKind = s.sectionKind;
            sc.mode = s.energy <= 0 ? StageMode::Rest : s.sectionKind == "chorus" ? StageMode::Propagation : StageMode::Group;
            if (s.sectionKind == "chorus") {
                sc.roles.push_back({ "Matriz Prueba", "protagonist", "" });
            }
            _stage.scenes.push_back(sc);
        }
        GlobalGesture wave;
        wave.id = "propagated-wave@21000";
        wave.kind = GestureKind::PropagatedWave;
        wave.startMS = 21000;
        wave.endMS = 24000;
        for (const char* f : { "Matriz Prueba", "Arbol Grande", "Linea 1" }) {
            GestureParticipant p;
            p.figure = f;
            p.function = "wave";
            wave.participants.push_back(p);
        }
        _stage.gestures.push_back(wave);

        const ChromaticScore score = DirectColors(Input());
        _score = score;
        Check(score.enabled && score.scenes.size() == _plan.segments.size(), fmt::format("enabled, one colour plan per scene ({})", score.scenes.size()));
        Check(score.song.colors.size() == 8 && !score.identityReason.empty(), "the song's family has its hierarchy and says where its anchor comes from: " + score.identityReason);
        const auto scene = [&](size_t i) -> const ChromaticScenePlan& { return score.scenes[i]; };
        Check(scene(0).evolution == "rest" && scene(0).monochrome, "the silence is quiet and in one family on purpose");
        Check(scene(1).evolution == "establish" && scene(3).evolution == "expand" && scene(6).evolution == "shift" && scene(7).evolution == "climax" && scene(8).evolution == "resolve",
              fmt::format("the colour story follows the song: {} {} {} {} {}", scene(1).evolution, scene(3).evolution, scene(6).evolution, scene(7).evolution, scene(8).evolution));
        Check(scene(5).evolution == "return" && scene(5).motif == scene(3).motif && !scene(3).motif.empty(), "the second chorus comes back to the first one's motif");
        Check(std::fabs(scene(5).palette.anchorHue - scene(3).palette.anchorHue) < 1 || HueDistance(Hue(scene(5).dominant), Hue(scene(3).dominant)) < 15,
              "related: the returning chorus keeps its anchor");
        std::set<std::string> intro, chorus;
        for (const FigureColorRole& r : scene(1).figures) {
            intro.insert(r.role);
        }
        for (const FigureColorRole& r : scene(3).figures) {
            chorus.insert(r.role);
        }
        Check(chorus.size() >= 3, fmt::format("a lively scene gives the figures different colour roles ({} roles)", chorus.size()));
        // Colours come from the scene's palette, never from nowhere.
        bool fromPalette = true;
        for (const ChromaticScenePlan& sp : score.scenes) {
            std::set<std::string> hexes;
            for (const PaletteColor& c : sp.palette.colors) {
                hexes.insert(c.hex);
            }
            for (const FigureColorRole& r : sp.figures) {
                for (const std::string& c : r.colors) {
                    fromPalette = fromPalette && (r.role == "identity" || r.role == "manual" || hexes.contains(c));
                }
            }
        }
        Check(fromPalette, "every figure's colour is one of its scene's palette: no colour from nowhere");
        std::set<int> chorusFamilies;
        for (const FigureColorRole& r : scene(3).figures) {
            chorusFamilies.insert(ColorFamily(OklchOfHex(r.colors.front())));
        }
        Check(chorusFamilies.size() >= 2 && chorusFamilies.size() <= 5, fmt::format("the chorus is not one colour, nor a rainbow: {} families across its figures", chorusFamilies.size()));
        const FigureColorRole* protagonist = scene(3).RoleOf("Matriz Prueba");
        Check(protagonist != nullptr && protagonist->role == "main", "the protagonist of a propagation carries the scene's colour");
        // The hierarchy by light: the largest family takes the main colour.
        const FigureColorRole* tree = scene(2).RoleOf("Arbol Grande");
        const FigureColorRole* matrix = scene(2).RoleOf("Matriz Prueba");
        Check(matrix != nullptr && tree != nullptr && (matrix->role == "main" || tree->role == "main"), "the largest figures carry the main colour (about 60-30-10 by light)");
        Check(HarmonyContrastOf(scene(1)) <= HarmonyContrastOf(scene(7)), fmt::format("the intro holds less contrast than the climax ({} {:.0f}° against {} {:.0f}°)", HarmonyId(scene(1).palette.harmony),
                                                                                       HarmonyContrastOf(scene(1)), HarmonyId(scene(7).palette.harmony), HarmonyContrastOf(scene(7))) + " " + Describe(scene(7).palette));
        // The gesture: a front that stands out from what is lit under it, and not white.
        const GestureChromaticProfile* g = scene(3).gestures.empty() ? nullptr : &scene(3).gestures.front();
        double closest = 1e9;
        if (g != nullptr) {
            for (const std::string& b : g->background) {
                closest = std::min(closest, DeltaE(g->front, b));
            }
        }
        Check(g != nullptr && closest >= 18, fmt::format("the wave's front stands {:.0f} ΔE from every colour lit on its figures", closest));
        Check(g != nullptr && OklchOfHex(g->front).C >= NEUTRAL_CHROMA, "the wave stands out with a colour, not with white");
        Check(g != nullptr && g->colorBy == "distance" && !g->secondary.empty() && g->secondary != g->front, "a propagated wave changes colour as it travels: front and a second colour");
        Check(scene(6).toNext.kind == "cut", fmt::format("into the climax the colours change at once ({})", scene(6).toNext.kind));
        bool staggered = false;
        for (const ChromaticScenePlan& sp : score.scenes) {
            staggered |= sp.toNext.kind != "cut" && sp.toNext.kind != "hold" && sp.toNext.order.size() >= 3 && sp.toNext.startMS < sp.endMS;
        }
        Check(staggered, "a gradual transition runs figure by figure, before the scene ends");
        // Determinism.
        Check(ToJson(DirectColors(Input())) == ToJson(score), "the same song, layout and choices give the same colour score");
        ChromaticScore back;
        std::string error;
        Check(FromJson(ToJson(score), back, error) && ToJson(back) == ToJson(score), "the colour score is written and read again unchanged");
        // Variety: more roles share a scene when more is asked.
        ChromaticOptions little, lots;
        little.variety = 0;
        lots.variety = 1;
        const auto roles = [&](const ChromaticOptions& o) {
            std::set<std::string> rs;
            for (const FigureColorRole& r : DirectColors(Input(nullptr, o)).scenes[3].figures) {
                rs.insert(r.role);
            }
            return rs.size();
        };
        Check(roles(lots) >= roles(little), fmt::format("more variety, more colour roles in a scene ({} against {})", roles(lots), roles(little)));
        ChromaticOptions mine;
        mine.songColors = { "#1060FF", "#FFC000" };
        mine.christmasInfluence = 1;
        const ChromaticScore chosen = DirectColors(Input(nullptr, mine));
        Check(HueDistance(chosen.song.anchorHue, Hue("#1060FF")) < 5, "the colours a person gives the song win over the Christmas ones");
        ChromaticOptions xmas;
        xmas.christmasInfluence = 1;
        const ChromaticScore christmas = DirectColors(Input(nullptr, xmas));
        Check(HueDistance(christmas.song.anchorHue, 27) < 20 || HueDistance(christmas.song.anchorHue, 145) < 20 || HueDistance(christmas.song.anchorHue, 82) < 20,
              fmt::format("with nothing else, a full Christmas influence starts from a Christmas colour ({:.0f}°)", christmas.song.anchorHue));
        ChromaticOptions off;
        off.enabled = false;
        Choreography untouched = _choreo;
        const ChromaticScore none = DirectColors(Input(nullptr, off));
        Check(!none.enabled && ApplyChromaticScore(none, untouched).empty() && untouched.events.front().colors == _choreo.events.front().colors, "switched off: nothing is changed, to compare");
    }

    static std::string Describe(const ColorPaletteProfile& p) {
        std::string out;
        for (const PaletteColor& c : p.colors) {
            out += fmt::format("{}={} L{:.2f} C{:.2f} h{:.0f}; ", c.role, c.hex, c.L, c.C, c.h);
        }
        return out;
    }

    static double HarmonyContrastOf(const ChromaticScenePlan& sp) {
        double spread = 0;
        const PaletteColor* a = sp.palette.Get("anchor");
        for (const char* role : { "secondary", "accent", "contrast" }) {
            if (const PaletteColor* c = sp.palette.Get(role); c != nullptr && a != nullptr) {
                spread = std::max(spread, c->C < NEUTRAL_CHROMA ? 0.0 : HueDistance(c->h, a->h));
            }
        }
        return spread;
    }

    void TestEdits() {
        Title("A person's changes win and can be undone");
        ChromaticEdits edits;
        edits.edits.push_back({ "color", 20000, "", "anchor", "#00A0FF", "test" });
        edits.edits.push_back({ "figure-color", -1, "Linea 1", "", "#FF00FF,#FFFFFF", "test" });
        edits.edits.push_back({ "monochrome", 30000, "", "", "", "test" });
        edits.edits.push_back({ "more-contrast", 50000, "", "", "", "test" });
        edits.edits.push_back({ "lock-palette", 10000, "", "", "anchor=#112233;secondary=#445566", "test" });
        const ChromaticScore edited = DirectColors(Input(&edits));
        const ChromaticScenePlan* chorus = edited.Scene("s3.0");
        Check(chorus != nullptr && chorus->dominant == "#00A0FF" && chorus->manual, "a colour a person chose replaces the anchor of that scene");
        const FigureColorRole* line = chorus == nullptr ? nullptr : chorus->RoleOf("Linea 1");
        Check(line != nullptr && line->role == "manual" && line->colors.size() == 2 && line->colors[0] == "#FF00FF", "a figure's colours chosen by hand stand in every scene");
        const ChromaticScenePlan* verse = edited.Scene("s4.0");
        Check(verse != nullptr && verse->monochrome, "a scene asked in one family is in one family");
        const ChromaticScenePlan* locked = edited.Scene("s2.0");
        Check(locked != nullptr && locked->locked && locked->palette.Hex("anchor") == "#112233" && locked->palette.Hex("secondary") == "#445566", "a palette a person fixed is kept");
        const ChromaticScenePlan* bridge = edited.Scene("s6.0");
        const ChromaticScenePlan* bridge0 = _score.Scene("s6.0");
        Check(bridge != nullptr && bridge0 != nullptr && bridge->manual && HarmonyContrastOf(*bridge) >= HarmonyContrastOf(*bridge0), "more contrast asked: at least as much contrast");
        ChromaticEdits back;
        std::string error;
        Check(FromJson(ToJson(edits), back, error) && back.edits.size() == edits.edits.size(), "the edits are written and read again");
        Check(!FromJson(R"({"kind":"composer-color-edits","edits":[{"action":"paint-it-black"}]})", back, error), "an unknown change is refused, not ignored");
        Check(ToJson(DirectColors(Input())) == ToJson(_score), "without the edits, the original plan comes back");
        // A figure's own colours: a part a person coloured is kept.
        FigureSemantics withIdentity = _semantics;
        for (FigureProfile& p : withIdentity.profiles) {
            if (p.figure == "Arbol Grande") {
                FigureRegion r;
                r.id = "foliage";
                r.source = "manual";
                r.real = true;
                r.confirmed = true;
                r.confidence = 1;
                r.color = "#1E8C3A";
                r.colorSource = "manual";
                for (int n = 0; n < 20; ++n) {
                    r.nodes.push_back(n);
                }
                p.regions.push_back(r);
            }
        }
        ChromaticInput in = Input();
        in.semantics = &withIdentity;
        const ChromaticScore kept = DirectColors(in);
        const FigureColorRole* tree = kept.Scene("s3.0") == nullptr ? nullptr : kept.Scene("s3.0")->RoleOf("Arbol Grande");
        Check(tree != nullptr && tree->role == "identity" && tree->colors.front() == "#1E8C3A", "a figure whose colours a person chose keeps them through every palette");
        ChromaticEdits release;
        release.edits.push_back({ "no-identity", -1, "Arbol Grande", "", "", "test" });
        in.edits = &release;
        const ChromaticScore freed = DirectColors(in);
        const FigureColorRole* free = freed.Scene("s3.0") == nullptr ? nullptr : freed.Scene("s3.0")->RoleOf("Arbol Grande");
        Check(free != nullptr && free->role != "identity", "and an explicit exception lets it take the palette");
    }

    // ------------------------------------------------------------ into the choreography

    void TestApply() {
        Title("Into the choreography: role colours, kept counts, locked effects untouched, native transitions");
        Choreography c = _choreo;
        ChoreographyEvent locked = c.events.front();
        locked.id = "locked";
        locked.state = DecisionState::Locked;
        locked.colors = { "#123456" };
        ChoreographyEvent chosen = c.events.front();
        chosen.id = "chosen";
        chosen.reasons.push_back({ "palette-set-by-user", "p1" });
        chosen.colors = { "#654321" };
        ChoreographyEvent video = c.events.front();
        video.id = "video";
        video.effect = "Video";
        video.colors = { "#ABCDEF" };
        c.events.push_back(locked);
        c.events.push_back(chosen);
        c.events.push_back(video);
        const std::vector<std::string> notes = ApplyChromaticScore(_score, c);
        bool counts = true, roleColours = true;
        int curves = 0;
        for (size_t i = 0; i < _choreo.events.size(); ++i) {
            const ChoreographyEvent& e = c.events[i];
            counts = counts && e.colors.size() == _choreo.events[i].colors.size();
            const ChromaticScenePlan* sp = _score.Scene(e.segmentId);
            const FigureColorRole* r = sp == nullptr ? nullptr : sp->RoleOf(e.target);
            roleColours = roleColours && r != nullptr && e.colors.front() == r->colors.front();
            for (const std::string& k : e.colorCurves) {
                curves += k.empty() ? 0 : 1;
            }
        }
        Check(!notes.empty() && counts && roleColours, "every effect takes its figure's role colours, as many as it had");
        Check(c.events[c.events.size() - 3].colors.front() == "#123456" && c.events[c.events.size() - 2].colors.front() == "#654321" && c.events.back().colors.front() == "#ABCDEF",
              "a locked effect, a palette a person chose and a video keep their colours");
        Check(curves > 0, fmt::format("{} palette slots change colour into the next scene with a native colour curve", curves));
        // The figures of a transition change one after another.
        std::map<std::string, double> holdOf;
        for (const ChoreographyEvent& e : c.events) {
            if (e.segmentId == "s2.0" && !e.colorCurves.empty() && !e.colorCurves.front().empty()) {
                const std::string& k = e.colorCurves.front();
                const size_t a = k.find(";x=");
                holdOf[e.target] = a == std::string::npos ? 0 : std::strtod(k.c_str() + a + 3, nullptr);
            }
        }
        std::set<double> holds;
        for (const auto& [t, h] : holdOf) {
            holds.insert(std::round(h * 1000));
        }
        Check(holdOf.size() < 2 || holds.size() >= 2, fmt::format("not the whole house at once: {} figures start changing at {} different moments", holdOf.size(), holds.size()));
        const std::string curve = ColorCurveText(2, "#FF8000", "#0060FF", 0.5);
        Check(curve.rfind("Active=TRUE|Id=ID_BUTTON_Palette2|Values=x=0.000^c=#FF8000;x=0.500^c=#FF8000;", 0) == 0 && curve.find("x=1.000^c=#0060FF|") != std::string::npos,
              "the curve is xLights' own: the colour held, then to the next");
        const size_t mid = curve.find("x=0.750^c=");
        const Oklch middle = mid == std::string::npos ? Oklch() : OklchOfHex(curve.substr(mid + 10, 7));
        Check(mid != std::string::npos && middle.C < 0.12, fmt::format("far apart colours meet through a soft middle (chroma {:.2f}), not through a third colour", middle.C));
        GlobalStageScore stage = _stage;
        Check(ColorGestures(_score, stage) == 1 && stage.gestures.front().colorBy == "distance" && stage.gestures.front().colors.size() >= 2 && !stage.gestures.front().colorReason.empty(),
              "the gesture takes its own colours and how they spread, with its reason");
        const FieldSpec spec = ParticipantField(stage.gestures.front(), stage.gestures.front().participants.back(), _space, &_semantics, 21000);
        FieldSpec read;
        std::string error;
        Check(spec.colorBy == "distance" && spec.reach > 0.05 && FromText(ToText(spec), read, error) && read.colorBy == "distance" && std::fabs(read.reach - spec.reach) < 1e-3,
              fmt::format("the field spreads its palette over the gesture's reach ({:.2f} stage units) and is read back", spec.reach));
        FieldSpec bad;
        bad.colorBy = "rainbow";
        Check(!ValidateField(bad, error), "an unknown colour mode is refused");
    }

    // ------------------------------------------------------------ rendered

    bool Render(const SequenceSpec& spec, const std::string& name, std::string& fseq) {
        const WriteResult written = WriteSequenceSafely(_show.string(), spec, name + ".xsq");
        for (const ValidationIssue& i : written.validation.issues) {
            if (i.severity == IssueSeverity::Error) {
                printf("    %s %s\n", i.code.c_str(), DescribeIssue(i).c_str());
            }
        }
        if (!written.ok || !_ctx->OpenSequence(written.finalPath)) {
            return false;
        }
        fseq = (_root / (name + ".fseq")).string();
        const bool ok = _ctx->RenderAndWait() && _ctx->WriteFseq(fseq);
        (void)_ctx->CloseSequence();
        return ok;
    }

    int Channel(const std::string& figure, int node) const {
        const FigureGeometry* g = _geometry.Find(figure);
        return g == nullptr || node >= static_cast<int>(g->nodeMap.size()) ? -1 : g->nodeMap[static_cast<size_t>(node)].channel;
    }

    static double DeltaTo(const std::array<int, 3>& rgb, const std::string& hex) {
        return DeltaE(ToHex(Rgb { rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0 }), hex);
    }

    void TestRender() {
        Title("Rendered by xLights: a wave that changes colour as it travels, and a colour curve");
        // A slow sweep from the left across the whole stage, red at the start, blue at the far end.
        FieldSpec s;
        s.function = FieldFunction::Sweep;
        s.ox = 0;
        s.oy = 0.5;
        s.angle = 0;
        s.shape = "linear";
        s.speed = 0.5;
        s.width = 0.05;
        s.duration = 4;
        s.colorBy = "distance";
        s.reach = 1;
        SequenceSpec spec;
        spec.frameMS = FRAME;
        spec.durationMS = 4000;
        for (const char* f : { "Matriz Prueba", "Linea 1" }) {
            FieldSpec own = s;
            if (const StageFigure* sf = _space.Find(f); sf != nullptr) {
                own.transform = sf->transform;
            }
            EffectSpec e;
            e.target = f;
            e.effect = "Stage Field";
            e.settings = { { "E_TEXTCTRL_StageField_Spec", ToText(own) }, { "E_SLIDER_StageField_Intensity", "100" } };
            e.palette = Palette({ "#FF0000", "#0000FF" });
            e.startMS = 0;
            e.endMS = 4000;
            e.kind = TargetKind::Model;
            spec.effects.push_back(e);
        }
        // A colour curve on a plain colour, from orange to blue over two seconds.
        EffectSpec on;
        on.target = "Estrella 1";
        on.effect = "On";
        on.palette = "C_BUTTON_Palette1=" + ColorCurveText(1, "#FF8000", "#0040FF", 0.0) + ",C_CHECKBOX_Palette1=1";
        on.startMS = 0;
        on.endMS = 2000;
        on.kind = TargetKind::Model;
        spec.effects.push_back(on);
        std::string fseq;
        const bool rendered = Render(spec, "color-wave", fseq);
        Check(rendered, "written as a native sequence, validated and rendered");
        if (!rendered) {
            return;
        }
        const Frames frames(fseq);
        // At the end everything has been swept: the left of the matrix nearer red, the line (far right) nearer blue.
        const int last = frames.Count() - 2;
        const std::array<int, 3> left = frames.Light(last, Channel("Matriz Prueba", 0));
        const std::array<int, 3> right = frames.Light(last, Channel("Linea 1", 0));
        Check(left[0] > left[2] && right[2] > right[0], fmt::format("near the start red ({},{},{}), far away blue ({},{},{}): the colour travelled", left[0], left[1], left[2], right[0], right[1], right[2]));
        const std::array<int, 3> first = frames.Light(1, Channel("Estrella 1", 0));
        const std::array<int, 3> end = frames.Light(2000 / FRAME - 2, Channel("Estrella 1", 0));
        Check(DeltaTo(first, "#FF8000") < 10 && DeltaTo(end, "#0040FF") < 15, fmt::format("the colour curve starts orange ({},{},{}) and ends blue ({},{},{}) in xLights' own render",
                                                                                            first[0], first[1], first[2], end[0], end[1], end[2]));
    }

    // ------------------------------------------------------------ measured

    void TestMeasure() {
        Title("Measuring what the render showed: one colour, varied figures, a gesture that blends in");
        const std::vector<std::string> figures = { "Matriz Prueba", "Arbol Grande", "Arbol Pequeno", "Linea 1", "Estrella 1" };
        const auto plain = [&](const std::vector<std::string>& colours, const std::string& name) {
            SequenceSpec spec;
            spec.frameMS = FRAME;
            spec.durationMS = 8000;
            for (size_t i = 0; i < figures.size(); ++i) {
                EffectSpec e;
                e.target = figures[i];
                e.layer = 1; // under the flash: in xLights layer 0 is drawn on top
                e.effect = "On";
                e.palette = Palette({ colours[i % colours.size()] });
                e.startMS = 0;
                e.endMS = 8000;
                e.kind = TargetKind::Model;
                spec.effects.push_back(e);
            }
            // A gesture-like flash on the line at 4 s, in a chosen colour.
            EffectSpec flash;
            flash.target = "Linea 1";
            flash.layer = 0;
            flash.effect = "On";
            flash.palette = Palette({ colours.size() > 1 ? "#00FFFF" : "#FF1010" });
            flash.startMS = 4000;
            flash.endMS = 4500;
            flash.kind = TargetKind::Model;
            spec.effects.push_back(flash);
            std::string fseq;
            return Render(spec, name, fseq) ? fseq : std::string();
        };
        GlobalStageScore score;
        StageScene sc;
        sc.id = "all";
        sc.startMS = 0;
        sc.endMS = 8000;
        score.scenes.push_back(sc);
        GlobalGesture g;
        g.id = "flash@4000";
        g.startMS = 4000;
        g.endMS = 4500;
        g.participants.push_back({ "Linea 1", "source", 0, 1, "pulse", "" });
        score.gestures.push_back(g);
        const std::string red = plain({ "#E01010" }, "una-sola");
        const std::string varied = plain({ "#E01010", "#10C040", "#2050FF", "#FFC000" }, "variada");
        if (red.empty() || varied.empty()) {
            Check(false, "the measured sequences are rendered");
            return;
        }
        const ColorCheck one = CheckColors(red, _geometry, _space, &score);
        const ColorCheck many = CheckColors(varied, _geometry, _space, &score);
        Check(one.ran && one.dominant == "red" && one.monotoneShare > 0.9, fmt::format("all in red: {:.0f}% of the time in one colour", one.monotoneShare * 100));
        Check(std::any_of(one.warnings.begin(), one.warnings.end(), [](const std::string& w) { return w.rfind("house-in-one-colour", 0) == 0; }) &&
                  std::any_of(one.warnings.begin(), one.warnings.end(), [](const std::string& w) { return w.rfind("figures-alike", 0) == 0; }),
              "one colour and figures alike are said, with why");
        Check(many.ran && many.monotoneShare < one.monotoneShare && many.figureDifference > one.figureDifference + 10 && many.effectiveColors > one.effectiveColors,
              fmt::format("four colours: {:.0f}% in one colour, figures {:.0f} ΔE apart, about {:.1f} families", many.monotoneShare * 100, many.figureDifference, many.effectiveColors));
        Check(!one.gestures.empty() && one.gestures.front().againstBefore < 15, fmt::format("a red flash over red blends in ({:.1f} ΔE)", one.gestures.empty() ? 0.0 : one.gestures.front().againstBefore));
        Check(!many.gestures.empty() && many.gestures.front().againstBefore >= 18, fmt::format("a cyan flash over amber stands out ({:.1f} ΔE)", many.gestures.empty() ? 0.0 : many.gestures.front().againstBefore));
        std::set<std::string> intended = { "all" };
        const ColorCheck meant = CheckColors(red, _geometry, _space, &score, &intended);
        Check(meant.ran && meant.scenes.front().intended && !std::any_of(meant.warnings.begin(), meant.warnings.end(), [](const std::string& w) { return w.rfind("house-in-one-colour", 0) == 0; }),
              "a scene in one colour on purpose is not counted as monotony");
        Check(ToJson(one).find("\"review\": \"not-reviewed\"") != std::string::npos, "a measurement, never an approval");
        Check(ToJson(CheckColors(red, _geometry, _space, &score)) == ToJson(one), "the same render, the same measurement");
    }

    fs::path _root;
    fs::path _show;
    std::unique_ptr<HeadlessRenderContext> _ctx;
    ModelCapabilities _caps;
    FigureCatalog _geometry;
    FigureSemantics _semantics;
    StageSpace _space;
    CompositionPlan _plan;
    SongAnalysis _song;
    Choreography _choreo;
    GlobalStageScore _stage;
    ChromaticScore _score;
    int _passed = 0;
    int _failed = 0;
};

} // namespace

int RunColorMeasure(const std::string& showDir, const std::string& fseqPath, const std::string& scorePath, const std::string& chromaPath, const std::string& outDir) {
    std::string refused;
    if (!ControllersAreInactive(showDir, refused)) {
        printf("composer-color-measure: REFUSED: %s\n", refused.c_str());
        return 2;
    }
    HeadlessRenderContext ctx;
    if (!ctx.LoadShowFolder(showDir)) {
        printf("composer-color-measure: the show could not be loaded\n");
        return 1;
    }
    const ModelCapabilities caps = ExtractCapabilities(ctx, nullptr);
    const FigureCatalog figures = DescribeFigures(ctx, caps, true);
    const StageSpace space = BuildStageSpace(figures, nullptr);
    GlobalStageScore score;
    bool hasScore = false;
    if (!scorePath.empty()) {
        std::string text, error;
        hasScore = ReadTextFile(scorePath, text) && FromJson(text, score, error);
        if (!hasScore) {
            printf("composer-color-measure: score not read: %s\n", error.c_str());
            return 1;
        }
    }
    const std::set<std::string> intended = IntendedScenes(chromaPath);
    const ColorCheck check = CheckColors(fseqPath, figures, space, hasScore ? &score : nullptr, &intended);
    std::error_code ec;
    fs::create_directories(fs::path(outDir), ec);
    const bool ok = WriteText(fs::path(outDir) / "color-check.json", ToJson(check)) && WriteText(fs::path(outDir) / "color-check.txt", ColorCheckReport(check));
    printf("%s", ColorCheckReport(check).c_str());
    return check.ran && ok ? 0 : 1;
}

int RunColorTest(const std::string& workDir) {
    Suite suite(fs::path(workDir) / fmt::format("color-{}", std::chrono::system_clock::now().time_since_epoch().count()));
    return suite.Run();
}

} // namespace composer
