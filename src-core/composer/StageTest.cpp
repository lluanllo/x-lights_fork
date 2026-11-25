#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

#include "composer/StageTest.h"

#include "composer/FigureGeometry.h"
#include "composer/FigureSemantics.h"
#include "composer/GenerationValidator.h"
#include "composer/ModelCapabilities.h"
#include "composer/PlanTest.h"
#include "composer/ProceduralField.h"
#include "composer/SequenceWriter.h"
#include "composer/SequenceComparison.h"
#include "composer/ArtisticPreferences.h"
#include "composer/CompositionSelector.h"
#include "composer/StageCheck.h"
#include "composer/StagePresence.h"
#include "composer/StageDirector.h"
#include "composer/UnderstandingStore.h"
#include "composer/WriterTest.h"
#include "composer/StageSpace.h"
#include "composer/SongAnalysis.h"
#include "composer/VocalUnderstanding.h"
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
#include <map>
#include <memory>
#include <set>
#include <tuple>

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
const char* WHITE = "C_BUTTON_Palette1=#FFFFFF,C_CHECKBOX_Palette1=1";
const char* GREEN = "C_BUTTON_Palette1=#1E8C3A,C_CHECKBOX_Palette1=1";
const char* RED = "C_BUTTON_Palette1=#D0201A,C_CHECKBOX_Palette1=1";

class Frames {
public:
    explicit Frames(const std::string& path) {
        std::unique_ptr<FSEQFile> file(FSEQFile::openFSEQFile(path));
        if (file == nullptr) {
            return;
        }
        _channels = file->getMaxChannel();
        _frameMS = file->getStepTime();
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
    [[nodiscard]] int FrameMS() const { return _frameMS; }
    // r, g, b of a light at a frame.
    [[nodiscard]] std::array<int, 3> Light(int frame, int channel) const {
        const std::vector<uint8_t>& f = _frames.at(static_cast<size_t>(std::clamp(frame, 0, Count() - 1)));
        if (channel < 0 || static_cast<size_t>(channel + 2) >= f.size()) {
            return { 0, 0, 0 };
        }
        return { f[channel], f[channel + 1], f[channel + 2] };
    }

private:
    uint32_t _channels = 0;
    int _frameMS = FRAME;
    std::vector<std::vector<uint8_t>> _frames;
};

class Suite {
public:
    explicit Suite(fs::path root) : _root(std::move(root)) {}

    int Run() {
        std::error_code ec;
        fs::create_directories(_root, ec);
        TestFields();
        if (Setup()) {
            TestSpace();
            TestWave();
            TestLayers();
            TestDirector();
        }
        if (_ctx != nullptr) {
            (void)_ctx->CloseSequence();
        }
        printf("composer-stage-test: %d checks passed, %d failed (%s)\n", _passed, _failed, _root.string().c_str());
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

    // ------------------------------------------------------------- fields

    void TestFields() {
        Title("Light fields: the same place and time always give the same light");
        FieldSpec wave;
        wave.function = FieldFunction::Wave;
        wave.ox = 0.2;
        wave.oy = 0.5;
        wave.speed = 0.5;
        wave.width = 0.1;
        wave.attack = 0.02;
        wave.release = 0.2;
        Check(std::fabs(ArrivalTime(wave, 0.7, 0.5) - 1.0) < 1e-9, "a wave at 0.5 stage units a second reaches a place 0.5 away in 1 s");
        Check(EvaluateField(wave, 0.7, 0.5, 0.9).intensity == 0, "before the front arrives, nothing");
        const float at = EvaluateField(wave, 0.7, 0.5, 1.0 + 0.05).intensity;
        Check(at > 0.9F, fmt::format("when the front is there, light ({:.2f})", at));
        Check(EvaluateField(wave, 0.7, 0.5, 1.0 + 0.2 + 0.2 + 0.05).intensity == 0, "after the front and its release, dark again");
        // Width: the light lasts the front's thickness over the speed, plus attack and release.
        int litFrames = 0;
        for (int ms = 0; ms < 3000; ms += 5) {
            litFrames += EvaluateField(wave, 0.7, 0.5, ms / 1000.0).intensity > 0.5F ? 1 : 0;
        }
        const double litMS = litFrames * 5.0;
        Check(litMS > 200 && litMS < 420, fmt::format("the front is as thick as asked: lit {:.0f} ms (0.1 units at 0.5 a second = 200 ms, plus the rise and the fall)", litMS));
        FieldSpec lossy = wave;
        lossy.loss = 2;
        Check(EvaluateField(lossy, 0.7, 0.5, 1.05).intensity < EvaluateField(lossy, 0.3, 0.5, 0.25).intensity, "with loss, the far light is weaker");
        FieldSpec echo = wave;
        echo.function = FieldFunction::Echo;
        echo.repeats = 3;
        echo.period = 0.8;
        echo.gain = 0.5;
        // Passes: each time the light rises above a glimmer and falls back, with its height.
        int peaks = 0;
        bool lit = false;
        float top = 0;
        std::vector<float> heights;
        for (int ms = 0; ms < 5000; ms += 5) {
            const float v = EvaluateField(echo, 0.7, 0.5, ms / 1000.0).intensity;
            if (v > 0.02F) {
                lit = true;
                top = std::max(top, v);
            } else if (lit) {
                ++peaks;
                heights.push_back(top);
                lit = false;
                top = 0;
            }
        }
        Check(peaks == 3 && heights.size() == 3 && heights[1] < heights[0] && heights[2] < heights[1], fmt::format("an echo: three passes, each weaker ({} peaks)", peaks));
        FieldSpec sweep = wave;
        sweep.function = FieldFunction::Sweep;
        sweep.shape = "linear";
        sweep.angle = 0;
        sweep.duration = 4;
        Check(EvaluateField(sweep, 0.25, 0.9, 0.5).intensity > 0.5F && EvaluateField(sweep, 0.6, 0.1, 0.5).intensity == 0,
              "a sweep to the right: lit behind its front, dark ahead, whatever the height");
        FieldSpec grow = wave;
        grow.function = FieldFunction::Expansion;
        grow.duration = 3;
        FieldSpec shrink = grow;
        shrink.contract = true;
        Check(EvaluateField(grow, 0.25, 0.5, 0.5).intensity > 0.5F && EvaluateField(grow, 0.9, 0.5, 0.5).intensity == 0 &&
                  EvaluateField(shrink, 0.9, 0.5, 0.2).intensity > 0.5F && EvaluateField(shrink, 0.9, 0.5, 2.6).intensity == 0,
              "an expansion grows from the origin; a contraction closes on it");
        // Every function: in bounds, the same twice, nothing at a negative time.
        bool bounded = true, same = true, beforeNothing = true;
        for (FieldFunction f : AllFieldFunctions()) {
            FieldSpec s = wave;
            s.function = f;
            s.duration = 3;
            s.repeats = 2;
            s.seed = 7;
            for (int k = 0; k < 400; ++k) {
                const double x = (k * 37 % 101) / 100.0, y = (k * 53 % 97) / 96.0, t = (k % 80) * 0.05;
                const FieldSample a = EvaluateField(s, x, y, t);
                const FieldSample b = EvaluateField(s, x, y, t);
                bounded = bounded && a.intensity >= 0 && a.intensity <= 1 && a.position >= 0 && a.position <= 1 && a.intensity == a.intensity;
                same = same && a.intensity == b.intensity && a.position == b.position;
            }
            beforeNothing = beforeNothing && EvaluateField(s, 0.4, 0.4, -0.1).intensity == 0;
        }
        Check(bounded, "every function gives light between 0 and 1, never a not-a-number");
        Check(same, "every function is deterministic");
        Check(beforeNothing, "no function lights anything before the gesture starts");
        FieldSpec flowA = wave, flowB = wave;
        flowA.function = flowB.function = FieldFunction::Flow;
        flowA.seed = 1;
        flowB.seed = 2;
        Check(EvaluateField(flowA, 0.3, 0.4, 0.7).intensity != EvaluateField(flowB, 0.3, 0.4, 0.7).intensity, "a flow's seed changes it, and only the seed does");
        // Text: written and read back the same; mistakes refused.
        FieldSpec full = echo;
        full.transform = { 0.001, 0, 0, 0.2, 0, 0.001, 0, 0.3 };
        full.only = "1-20,25";
        full.skip = "3-4";
        FieldSpec back;
        std::string error;
        Check(FromText(ToText(full), back, error) && ToText(back) == ToText(full), "written as text and read back: the same field");
        Check(!FromText("fn=wobble", back, error) && !FromText("fn=wave;v=0", back, error) && !FromText("fn=wave;speeed=1", back, error) &&
                  !FromText("fn=wave;m=1,2,3", back, error) && !FromText("fn=wave;at=-61", back, error) && FromText("fn=wave;at=-0.1", back, error),
              "an unknown function, a zero speed, a misspelt setting, a bad place or a start a minute early are refused; a light waiting a moment is not");
        const std::vector<char> set = LightSet("1-3,5", 6, true);
        Check(set == std::vector<char>({ 1, 1, 1, 0, 1, 0 }) && LightSet("", 3, true) == std::vector<char>({ 1, 1, 1 }) && LightSet("", 3, false) == std::vector<char>({ 0, 0, 0 }),
              "light numbers as xLights writes them");
    }

    // ------------------------------------------------------------- fixture

    bool Setup() {
        _show = _root / "show";
        if (!WritePlanFixture(_show.string())) {
            Check(false, "the fixture show is written");
            return false;
        }
        // Figures placed apart, left to right, as a house would be drawn.
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
        return true;
    }

    void TestSpace() {
        Title("One stage for the whole installation");
        const StageFigure* matrix = _space.Find("Matriz Prueba");
        const StageFigure* tree = _space.Find("Arbol Grande");
        const StageFigure* line = _space.Find("Linea 1");
        Check(matrix != nullptr && tree != nullptr && line != nullptr && matrix->placed && tree->placed && line->placed, "every figure placed on the stage");
        Check(matrix != nullptr && tree != nullptr && line != nullptr && matrix->cx < tree->cx && tree->cx < line->cx, "left to right as in the layout: matrix, tree, line");
        bool exact = true;
        double worst = 0;
        for (const StageFigure& f : _space.figures) {
            if (f.placed && f.lights > 2) {
                exact = exact && f.precision == "exact";
                worst = std::max(worst, f.fitError);
            }
        }
        Check(exact, fmt::format("one transform places every light of every figure (worst {:.6f} stage units)", worst));
        Check(!_space.physical && std::any_of(_space.limits.begin(), _space.limits.end(), [](const std::string& l) { return l.rfind("schematic", 0) == 0; }),
              "without a calibration it says the stage is a drawing, not measured distances");
        bool inside = true;
        for (const StageFigure& f : _space.figures) {
            if (f.placed) {
                inside = inside && f.minX >= -1e-6 && f.maxX <= 1 + 1e-6 && f.minY >= -1e-6 && f.maxY <= 1 + 1e-6;
            }
        }
        Check(inside, "the whole layout fits the 0-1 stage");
        const FigureGeometry* g = _geometry.Find("Arbol Grande");
        const auto lights = g != nullptr && tree != nullptr ? LightsOnStage(*g, *tree) : std::vector<std::pair<double, double>>();
        const StageAnchor bulb = ResolveAnchor("node:Arbol Grande:1", _space, _geometry, &_semantics);
        Check(bulb.ok && !lights.empty() && std::fabs(bulb.x - lights[0].first) < 1e-9 && std::fabs(bulb.y - lights[0].second) < 1e-9, "a single bulb as the origin: the tree's first light");
        const StageAnchor mouth = ResolveAnchor("region:Arbol Cara:mouth", _space, _geometry, &_semantics);
        const StageAnchor centre = ResolveAnchor("center:Matriz Prueba", _space, _geometry, &_semantics);
        const StageAnchor group = ResolveAnchor("group:Grupo Arboles", _space, _geometry, &_semantics);
        const StageAnchor point = ResolveAnchor("point:0.1,0.2", _space, _geometry, &_semantics);
        Check(mouth.ok && centre.ok && group.ok && point.ok && std::fabs(point.x - 0.1) < 1e-12, "a part of a figure, its centre, a group and a point as origins");
        Check(ResolveAnchor("node:Arbol Grande:99999", _space, _geometry, &_semantics).problem == "light-missing" &&
                  ResolveAnchor("figure:No Existe", _space, _geometry, &_semantics).problem == "figure-missing" &&
                  ResolveAnchor("region:Arbol Pequeno:mouth", _space, _geometry, &_semantics).problem == "region-missing" &&
                  ResolveAnchor("nonsense", _space, _geometry, &_semantics).problem == "malformed",
              "an origin that no longer exists is said, never guessed");
        StageCalibrations cal;
        cal.figures.push_back({ "Linea 1", -0.05, 0.02, 1.0, "measured" });
        std::string error;
        StageCalibrations back;
        Check(FromJson(ToJson(cal), back, error) && back.figures.size() == 1, "a person's correction of a place is written and read again");
        const StageSpace moved = BuildStageSpace(_geometry, &back);
        const StageFigure* l2 = moved.Find("Linea 1");
        Check(l2 != nullptr && line != nullptr && l2->calibrated && std::fabs(l2->cx - (line->cx - 0.05)) < 1e-6 && std::fabs(l2->cy - (line->cy + 0.02)) < 1e-6,
              "the corrected figure moves on the stage, and nothing else");
    }

    // A spec for one figure: the shared gesture with the figure's own place.
    std::string SpecFor(FieldSpec s, const std::string& figure) {
        const StageFigure* f = _space.Find(figure);
        if (f != nullptr) {
            s.transform = f->transform;
        }
        return ToText(s);
    }

    EffectSpec Field(const std::string& figure, int layer, const FieldSpec& s, const char* palette, int startMS, int endMS) {
        EffectSpec e;
        e.target = figure;
        e.layer = layer;
        e.effect = "Stage Field";
        e.settings = { { "E_TEXTCTRL_StageField_Spec", SpecFor(s, figure) }, { "E_SLIDER_StageField_Intensity", "100" }, { "T_CHOICE_LayerMethod", "Normal" } };
        e.palette = palette;
        e.startMS = startMS;
        e.endMS = endMS;
        e.kind = TargetKind::Model;
        return e;
    }

    bool Render(const SequenceSpec& spec, const std::string& name, std::string& fseq) {
        const WriteResult written = WriteSequenceSafely(_show.string(), spec, name + ".xsq");
        for (const ValidationIssue& i : written.validation.issues) {
            if (i.severity == IssueSeverity::Error) {
                printf("    %s %s\n", i.code.c_str(), DescribeIssue(i).c_str());
            }
        }
        for (const std::string& problem : written.problems) {
            printf("    %s\n", problem.c_str());
        }
        if (!written.ok) {
            return false;
        }
        if (!_ctx->OpenSequence(written.finalPath)) {
            return false;
        }
        fseq = (_root / (name + ".fseq")).string();
        const bool ok = _ctx->RenderAndWait() && _ctx->WriteFseq(fseq);
        (void)_ctx->CloseSequence();
        return ok;
    }

    void TestWave() {
        Title("A wave from one bulb crosses figures of different shapes, rendered by xLights");
        const StageAnchor bulb = ResolveAnchor("node:Matriz Prueba:1", _space, _geometry, &_semantics);
        FieldSpec wave;
        wave.function = FieldFunction::Wave;
        wave.ox = bulb.x;
        wave.oy = bulb.y;
        wave.speed = 0.4;
        wave.width = 0.06;
        wave.attack = 0.02;
        wave.release = 0.15;
        wave.duration = 5;
        const std::vector<std::string> figures = { "Matriz Prueba", "Arbol Grande", "Arbol Cara", "Carita", "Linea 1" };
        SequenceSpec spec;
        spec.frameMS = FRAME;
        spec.durationMS = 5000;
        // Each figure joins the gesture at its own moment: the effect starts a little before
        // the front reaches its nearest light, and the field is told how late it joined.
        for (const std::string& f : figures) {
            const FigureGeometry* g = _geometry.Find(f);
            const StageFigure* sf = _space.Find(f);
            double first = 1e9;
            for (const auto& [x, y] : LightsOnStage(*g, *sf)) {
                first = std::min(first, ArrivalTime(wave, x, y));
            }
            const int startMS = std::max(0, static_cast<int>(std::floor(first * 1000 / FRAME)) * FRAME - 4 * FRAME);
            FieldSpec own = wave;
            own.at = startMS / 1000.0;
            spec.effects.push_back(Field(f, 0, own, WHITE, startMS, 5000));
        }
        std::string fseq;
        const auto start = std::chrono::steady_clock::now();
        const bool rendered = Render(spec, "onda-global", fseq);
        const auto renderMS = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        Check(rendered, fmt::format("written as a native sequence, validated and rendered in {} ms", renderMS));
        if (!rendered) {
            return;
        }
        Frames frames(fseq);
        std::map<std::string, double> meanArrival;
        double worstError = 0;
        int measured = 0, late = 0, total = 0, outside = 0;
        std::vector<std::pair<double, double>> distanceArrival;
        for (const std::string& f : figures) {
            const FigureGeometry* g = _geometry.Find(f);
            const auto lights = LightsOnStage(*g, *_space.Find(f));
            double sum = 0;
            int n = 0;
            for (size_t k = 0; k < g->nodeMap.size(); ++k) {
                const double expected = ArrivalTime(wave, lights[k].first, lights[k].second);
                if (expected * 1000 > 4500) {
                    continue; // beyond the gesture's life
                }
                ++total;
                int first = -1;
                for (int fr = 0; fr < frames.Count(); ++fr) {
                    const auto c = frames.Light(fr, g->nodeMap[k].channel);
                    if (std::max({ c[0], c[1], c[2] }) > 60) {
                        first = fr;
                        break;
                    }
                }
                if (first < 0) {
                    ++late;
                    continue;
                }
                const double seen = first * frames.FrameMS() / 1000.0;
                const double error = seen - expected;
                outside += error < -0.026 ? 1 : 0; // lit before the front could be there
                worstError = std::max(worstError, std::fabs(error));
                sum += seen;
                ++n;
                ++measured;
                distanceArrival.push_back({ FieldDistance(wave, lights[k].first, lights[k].second), seen });
            }
            meanArrival[f] = n > 0 ? sum / n : -1;
        }
        Check(measured > 0 && late == 0, fmt::format("every light the wave should reach lit up: {} of {}", measured, total));
        Check(outside == 0, "no light lit before the front could reach it");
        Check(worstError <= 0.06, fmt::format("each light lit when the field says, within {:.0f} ms (two frames and the rise)", worstError * 1000));
        Check(meanArrival["Matriz Prueba"] < meanArrival["Arbol Grande"] && meanArrival["Arbol Grande"] < meanArrival["Arbol Cara"] &&
                  meanArrival["Arbol Cara"] < meanArrival["Carita"] && meanArrival["Carita"] < meanArrival["Linea 1"],
              fmt::format("it reaches the figures in the order of their distance: matrix {:.2f} s, tree {:.2f}, tree with a face {:.2f}, small face {:.2f}, line {:.2f}",
                          meanArrival["Matriz Prueba"], meanArrival["Arbol Grande"], meanArrival["Arbol Cara"], meanArrival["Carita"], meanArrival["Linea 1"]));
        // Continuity: the time to arrive grows with the distance at the gesture's speed,
        // across figures, with no jump where one figure ends and the next begins.
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (const auto& [d, t] : distanceArrival) {
            sx += d;
            sy += t;
            sxx += d * d;
            sxy += d * t;
        }
        const double n = static_cast<double>(distanceArrival.size());
        const double slope = (n * sxy - sx * sy) / std::max(1e-12, n * sxx - sx * sx);
        Check(std::fabs(1.0 / slope - wave.speed) / wave.speed < 0.05, fmt::format("one speed across the whole installation: measured {:.3f} stage units a second, asked {:.3f}", 1.0 / slope, wave.speed));
        _waveFseq = fseq;
    }

    void TestLayers() {
        Title("Layers on one figure: its own colours stay, the mouth is protected");
        const FigureProfile* face = _semantics.Find("Arbol Cara");
        const FigureRegion* mouth = face != nullptr ? face->Region("mouth") : nullptr;
        const FigureGeometry* g = _geometry.Find("Arbol Cara");
        if (mouth == nullptr || g == nullptr) {
            Check(false, "the tree with a face has a mouth");
            return;
        }
        const std::string mouthText = NodeRangesText(mouth->nodes);
        FieldSpec identity;
        identity.function = FieldFunction::Pulse;
        identity.duration = 4;
        identity.attack = 0.001;
        identity.release = 0.001;
        identity.skip = mouthText;
        FieldSpec lips = identity;
        lips.skip.clear();
        lips.only = mouthText;
        const StageAnchor centre = ResolveAnchor("center:Arbol Cara", _space, _geometry, &_semantics);
        FieldSpec wave;
        wave.function = FieldFunction::Wave;
        wave.ox = centre.x;
        wave.oy = centre.y;
        wave.speed = 0.3;
        wave.width = 0.05;
        wave.release = 0.1;
        wave.duration = 3;
        wave.skip = mouthText;
        SequenceSpec spec;
        spec.frameMS = FRAME;
        spec.durationMS = 4000;
        spec.effects.push_back(Field("Arbol Cara", 2, identity, GREEN, 0, 4000)); // its own colour, at the bottom
        spec.effects.push_back(Field("Arbol Cara", 1, lips, RED, 0, 4000));       // the mouth, red
        spec.effects.push_back(Field("Arbol Cara", 0, wave, WHITE, 1000, 4000));  // a wave over both, never on the mouth
        std::string fseq;
        Check(Render(spec, "capas-arbol", fseq), "three layers written and rendered");
        Frames frames(fseq);
        if (!frames.Valid()) {
            return;
        }
        const std::vector<char> isMouth = LightSet(mouthText, static_cast<int>(g->nodeMap.size()), false);
        int mouthRed = 0, mouthTotal = 0, bodyGreenBefore = 0, bodyTotal = 0, whitened = 0;
        for (size_t k = 0; k < g->nodeMap.size(); ++k) {
            const int ch = g->nodeMap[k].channel;
            const auto before = frames.Light(20, ch); // 0.5 s: only the colours of the figure
            if (isMouth[k]) {
                ++mouthTotal;
                bool alwaysRed = true;
                for (int fr = 2; fr < frames.Count() - 2; ++fr) {
                    const auto c = frames.Light(fr, ch);
                    alwaysRed = alwaysRed && c[0] > 180 && c[1] < 60 && c[2] < 60;
                }
                mouthRed += alwaysRed ? 1 : 0;
            } else {
                ++bodyTotal;
                bodyGreenBefore += before[1] > before[0] + 40 && before[1] > before[2] + 40 ? 1 : 0;
                bool white = false;
                for (int fr = 40; fr < frames.Count(); ++fr) {
                    const auto c = frames.Light(fr, ch);
                    white = white || (c[0] > 150 && c[1] > 150 && c[2] > 150);
                }
                whitened += white ? 1 : 0;
            }
        }
        Check(mouthTotal > 0 && mouthRed == mouthTotal, fmt::format("the mouth stays red through the wave: {} of {} lights", mouthRed, mouthTotal));
        Check(bodyGreenBefore == bodyTotal, fmt::format("the tree keeps its own green under everything: {} of {} lights", bodyGreenBefore, bodyTotal));
        Check(whitened * 10 >= bodyTotal * 9, fmt::format("the wave passes over the green (it shows, then the green comes back): {} of {} lights", whitened, bodyTotal));
        int greenAfter = 0;
        for (size_t k = 0; k < g->nodeMap.size(); ++k) {
            if (!isMouth[k]) {
                const auto c = frames.Light(frames.Count() - 5, g->nodeMap[k].channel);
                greenAfter += c[1] > c[0] + 40 ? 1 : 0;
            }
        }
        Check(greenAfter == bodyTotal, "after the wave, the green is whole again: nothing below was destroyed");
    }

    // ------------------------------------------------------------- the director

    // A song made by hand: rest, strong instruments, three close hits, a narration, a
    // choir answering, a climax. Each part should get its own way of using the house.
    struct Song {
        CompositionPlan plan;
        SongAnalysis analysis;
        VocalUnderstanding voices;
        Choreography faces; // the faces singing, as the choreography gives them
    };

    static PlanSegment Segment(const char* id, int a, int b, int priority, std::vector<std::string> flags) {
        PlanSegment s;
        s.id = id;
        s.startMS = a;
        s.endMS = b;
        s.priority = priority;
        s.flags = std::move(flags);
        s.sectionKind = "verse";
        return s;
    }

    static PlanAssignment Sing(const char* figure, const char* voice) {
        PlanAssignment a;
        a.target = figure;
        a.intent = "face-sing";
        a.voice = voice;
        a.prominence = Prominence::Lead;
        return a;
    }

    static void Hit(SongAnalysis& a, int ms, double strength) {
        MusicalEvent e;
        e.timeMS = ms;
        e.type = MusicalEventType::Onset;
        e.intensity = strength;
        e.confidence = 0.9;
        a.events.push_back(e);
    }

    static Intervention Voice(const char* id, int a, int b, const char* emission, std::vector<std::string> relations) {
        Intervention i;
        i.id = id;
        i.track = id;
        i.startMS = a;
        i.endMS = b;
        i.emission = emission;
        i.confidence = 0.9;
        i.relations = std::move(relations);
        return i;
    }

    Song MakeSong() {
        Song s;
        s.plan.durationMS = 24000;
        s.plan.segments.push_back(Segment("s0.0", 0, 3000, 0, { "silence" }));
        s.plan.segments.push_back(Segment("s1.0", 3000, 7000, 2, {}));
        s.plan.segments.push_back(Segment("s2.0", 7000, 11000, 1, {}));
        PlanSegment narration = Segment("s3.0", 11000, 15000, 1, { "lyrics" });
        narration.assignments.push_back(Sing("Arbol Cara", "narrador"));
        narration.vocalCoverage = 1;
        s.plan.segments.push_back(narration);
        PlanSegment choir = Segment("s4.0", 15000, 19000, 1, { "lyrics" });
        choir.assignments.push_back(Sing("Arbol Cara", "solista"));
        choir.assignments.push_back(Sing("Carita", "coro"));
        choir.vocalCoverage = 1;
        s.plan.segments.push_back(choir);
        s.plan.segments.push_back(Segment("s5.0", 19000, 24000, 3, { "climax" }));
        s.analysis.durationMS = 24000;
        s.analysis.tempo.push_back({ 0, 120, 0.9 });
        Hit(s.analysis, 4000, 0.9);  // the strong instruments' hit
        Hit(s.analysis, 8000, 0.8);  // three close hits
        Hit(s.analysis, 8400, 0.8);
        Hit(s.analysis, 8800, 0.8);
        Hit(s.analysis, 16000, 0.8); // in the choir
        Hit(s.analysis, 20000, 0.95); // the climax
        s.voices.interventions.push_back(Voice("narrador#1", 11000, 15000, "spoken", {}));
        s.voices.interventions.push_back(Voice("solista#1", 15000, 16500, "sung", {}));
        s.voices.interventions.push_back(Voice("coro#1", 16600, 18000, "sung", { "answers:solista#1" }));
        for (const auto& [figure, a, b] : std::vector<std::tuple<const char*, int, int>>{ { "Arbol Cara", 11000, 15000 }, { "Arbol Cara", 15000, 19000 }, { "Carita", 15000, 19000 } }) {
            const FigureProfile* p = _semantics.Find(figure);
            const FigureRegion* mouth = p != nullptr ? p->Region("mouth") : nullptr;
            const std::string source = mouth != nullptr ? mouth->source : "";
            ChoreographyEvent e;
            e.id = fmt::format("face/{}/{}", figure, a);
            e.target = figure;
            e.effect = "Faces";
            e.intent = "face-sing";
            e.prominence = Prominence::Lead;
            e.startMS = a;
            e.endMS = b;
            e.settings = { { "E_CHOICE_Faces_FaceDefinition", source.rfind("face-definition:", 0) == 0 ? source.substr(16) : "" } };
            s.faces.events.push_back(e);
        }
        return s;
    }

    StageDirectorInput Input(const Song& s) {
        StageDirectorInput in;
        in.plan = &s.plan;
        in.analysis = &s.analysis;
        in.voices = &s.voices;
        in.geometry = &_geometry;
        in.semantics = &_semantics;
        in.space = &_space;
        in.capabilities = &_caps;
        in.choreography = &s.faces;
        return in;
    }

    static const StageScene* SceneOf(const GlobalStageScore& score, const char* id) {
        for (const StageScene& s : score.scenes) {
            if (s.id == id) {
                return &s;
            }
        }
        return nullptr;
    }

    static std::vector<const GlobalGesture*> GesturesOf(const GlobalStageScore& score, const StageScene* sc) {
        std::vector<const GlobalGesture*> out;
        if (sc != nullptr) {
            for (const std::string& id : sc->gestures) {
                if (const GlobalGesture* g = score.Gesture(id)) {
                    out.push_back(g);
                }
            }
        }
        return out;
    }

    void TestDirector() {
        Title("The director: each part of the song uses the house in its own way");
        const Song song = MakeSong();
        const StageDirectorInput in = Input(song);
        // The tree's parts as the fixture describes them are guesses from its shape: no
        // colour of its own is laid under it until a person confirms them.
        bool guessedIdentity = false;
        for (const StageLayer& l : DirectStage(in).layers) {
            guessedIdentity = guessedIdentity || l.figure == "Arbol Cara";
        }
        int confirmed = 0, guessed = 0;
        for (FigureProfile& p : _semantics.profiles) {
            if (p.figure != "Arbol Cara") {
                continue;
            }
            for (FigureRegion& r : p.regions) {
                if (!r.color.empty() && r.real && r.id != "mouth" && r.id != "eyes" && r.id != "face") {
                    guessed += r.source == "geometry" || r.source == "nodes" ? 1 : 0;
                    r.confirmed = true;
                    ++confirmed;
                }
            }
        }
        Check(guessed == confirmed && confirmed > 0 && !guessedIdentity,
              fmt::format("{} parts guessed from a figure's shape give it no colour of its own until a person confirms them", guessed));
        const GlobalStageScore score = DirectStage(in);
        for (const StageScene& sc : score.scenes) {
            std::string roles;
            for (const SceneRole& r : sc.roles) {
                if (r.role != "rest") {
                    roles += fmt::format("{}{}={}", roles.empty() ? "" : ", ", r.figure, r.role);
                }
            }
            printf("    %s %s [%s] gestures: %zu; %s\n", sc.id.c_str(), StageModeId(sc.mode), sc.context.c_str(), sc.gestures.size(), roles.c_str());
        }
        Check(score.scenes.size() == 6, "one scene for each part of the plan");
        const StageScene* rest = SceneOf(score, "s0.0");
        const StageScene* strong = SceneOf(score, "s1.0");
        const StageScene* hits = SceneOf(score, "s2.0");
        const StageScene* narration = SceneOf(score, "s3.0");
        const StageScene* choir = SceneOf(score, "s4.0");
        const StageScene* climax = SceneOf(score, "s5.0");
        if (rest == nullptr || strong == nullptr || hits == nullptr || narration == nullptr || choir == nullptr || climax == nullptr) {
            Check(false, "every scene found");
            return;
        }
        Check(rest->mode == StageMode::Rest && rest->gestures.empty(), "where the music rests, the house rests: no gesture");
        Check(narration->mode == StageMode::Protagonist && narration->RoleOf("Arbol Cara") == "protagonist", "a narration: the speaking figure alone has the attention");
        {
            // A face that does not sing in the narration, put in the plan by a person, takes part there.
            Song asked = MakeSong();
            PlanAssignment a;
            a.target = "Carita";
            a.intent = "accompany";
            a.prominence = Prominence::Support;
            a.state = DecisionState::ManualEdit;
            asked.plan.segments[3].assignments.push_back(a);
            const StageScene* before = narration;
            const GlobalStageScore withAsked = DirectStage(Input(asked));
            const StageScene* after = SceneOf(withAsked, "s3.0");
            Check(before->RoleOf("Carita") == "rest" && after != nullptr && after->RoleOf("Carita") == "complement" && after->RoleOf("Arbol Cara") == "protagonist",
                  "a figure a person put in the plan takes part where the director would have rested it");
        }
        Check(choir->mode == StageMode::CallResponse, "a voice and a choir answering it: call and answer");
        Check(climax->mode == StageMode::FullEnsemble, "the peak of the song: the whole house");
        Check(strong->mode == StageMode::Propagation, "strong instruments with no voice: a gesture that travels");
        int climaxResting = 0, climaxFigures = 0;
        for (const SceneRole& r : climax->roles) {
            ++climaxFigures;
            climaxResting += r.role == "rest" ? 1 : 0;
        }
        Check(climaxFigures > 4 && climaxResting == 0, fmt::format("at the peak every figure takes part ({} figures)", climaxFigures));
        bool withinBudget = true;
        for (const StageScene& sc : score.scenes) {
            if (sc.budget < 1e8) {
                withinBudget = withinBudget && sc.spent <= sc.budget + 1e-9;
            }
        }
        int narrationResting = 0;
        for (const SceneRole& r : narration->roles) {
            narrationResting += r.role == "rest" ? 1 : 0;
        }
        Check(withinBudget && narrationResting > 0, fmt::format("no scene spends more attention than it has; in the narration {} figures rest", narrationResting));

        // The strong hit: a wave from a bulb, reaching each figure when its distance says.
        const auto strongGestures = GesturesOf(score, strong);
        const GlobalGesture* wave = nullptr;
        for (const GlobalGesture* g : strongGestures) {
            if (g->kind == GestureKind::PropagatedWave && g->startMS == 4000) {
                wave = g;
            }
        }
        Check(wave != nullptr && wave->origin.rfind("node:", 0) == 0 && ResolveAnchor(wave->origin, _space, _geometry, &_semantics).ok,
              fmt::format("the strong hit at 4 s starts a wave in one real bulb ({})", wave != nullptr ? wave->origin : "none"));
        if (wave != nullptr) {
            bool delays = true;
            for (const GestureParticipant& p : wave->participants) {
                const FigureGeometry* g = _geometry.Find(p.figure);
                const StageFigure* f = _space.Find(p.figure);
                double first = 1e9;
                for (const auto& [x, y] : LightsOnStage(*g, *f)) {
                    first = std::min(first, std::hypot(x - wave->ox, y - wave->oy) / wave->speed);
                }
                delays = delays && std::abs(p.delayMS - static_cast<int>(first * 1000)) <= 1;
            }
            Check(delays && wave->participants.size() >= 3, fmt::format("each of its {} figures joins at distance over speed", wave->participants.size()));
            Check(wave->speed >= 0.15 && wave->speed <= 4 && !wave->reasons.empty() && !wave->trigger.empty(), fmt::format("its speed comes from the beat ({:.2f} a second) and it says why", wave->speed));
        }
        // Three close hits: one figure for each, at each hit.
        const GlobalGesture* cascade = nullptr;
        for (const GlobalGesture* g : GesturesOf(score, hits)) {
            if (g->kind == GestureKind::Cascade) {
                cascade = g;
            }
        }
        Check(cascade != nullptr && cascade->participants.size() == 3 && cascade->participants[0].delayMS == 0 && cascade->participants[1].delayMS == 400 &&
                  cascade->participants[2].delayMS == 800 && cascade->participants[0].figure != cascade->participants[1].figure,
              "three close hits make a cascade: three different figures at 0, 0.4 and 0.8 s");
        const GlobalGesture* answer = nullptr;
        for (const GlobalGesture* g : GesturesOf(score, choir)) {
            if (g->kind == GestureKind::CallResponse) {
                answer = g;
            }
        }
        bool overSinger = false;
        for (const GestureParticipant& p : answer != nullptr ? answer->participants : std::vector<GestureParticipant>()) {
            overSinger = overSinger || p.figure == "Arbol Cara" || p.figure == "Carita";
        }
        Check(answer != nullptr && !answer->participants.empty() && answer->participants[0].delayMS == 500 && !overSinger,
              "in the choir, the soloist calls by singing and other figures answer with light, a beat (0.5 s) apart, never over a singing face");
        bool spaced = true;
        for (const StageScene& sc : score.scenes) {
            const auto gs = GesturesOf(score, &sc);
            for (size_t i = 1; i < gs.size(); ++i) {
                spaced = spaced && std::abs(gs[i]->startMS - gs[i - 1]->startMS) >= 1500;
            }
            spaced = spaced && gs.size() <= 4;
        }
        Check(spaced, "not every hit gets a gesture: at most a few a scene, 1.5 s apart");
        bool identity = false, identityKeepsMouth = true;
        const FigureProfile* face = _semantics.Find("Arbol Cara");
        const FigureRegion* mouth = face != nullptr ? face->Region("mouth") : nullptr;
        for (const StageLayer& l : score.layers) {
            if (l.figure == "Arbol Cara" && l.startMS == narration->startMS) {
                identity = true;
                const std::vector<char> skip = LightSet(l.skip, 600, false);
                for (int n : mouth != nullptr ? mouth->nodes : std::vector<int>()) {
                    identityKeepsMouth = identityKeepsMouth && skip[static_cast<size_t>(n)];
                }
            }
        }
        Check(identity && identityKeepsMouth, "the narrator keeps its own colours under everything, its mouth left to the face");

        // Same input, same score; written and read back, the same score.
        Check(ToJson(DirectStage(in)) == ToJson(score), "the same song and layout always give the same score");
        GlobalStageScore back;
        std::string error;
        Check(FromJson(ToJson(score), back, error) && ToJson(back) == ToJson(score), "the score is written and read back unchanged");

        // A person's changes win and are kept apart.
        StageEdits edits;
        edits.edits.push_back({ "mode", 3000, -1, "local-waves", "" });
        edits.edits.push_back({ "exclude", 19000, -1, "Matriz Prueba", "" });
        edits.edits.push_back({ "no-gestures", 7000, -1, "", "" });
        edits.edits.push_back({ "speed", 3000, 4000, "1.5", "" });
        edits.edits.push_back({ "mode", 60000, -1, "rest", "" });
        edits.edits.push_back({ "gesture", 11000, 13000, "propagated-wave", "" });
        edits.edits.push_back({ "identity", 7000, -1, "Arbol Cara", "" });
        StageEdits editsBack;
        Check(FromJson(ToJson(edits), editsBack, error) && editsBack.edits.size() == edits.edits.size(), "a person's changes are written and read again");
        StageDirectorInput edited = in;
        edited.edits = &editsBack;
        const GlobalStageScore changed = DirectStage(edited);
        const StageScene* c1 = SceneOf(changed, "s1.0");
        const StageScene* c2 = SceneOf(changed, "s2.0");
        const StageScene* c5 = SceneOf(changed, "s5.0");
        bool matrixOut = c5 != nullptr && c5->RoleOf("Matriz Prueba") == "rest";
        for (const GlobalGesture* g : GesturesOf(changed, c5)) {
            for (const GestureParticipant& p : g->participants) {
                matrixOut = matrixOut && p.figure != "Matriz Prueba";
            }
        }
        const auto changedWave = GesturesOf(changed, c1);
        bool faster = false;
        for (const GlobalGesture* g : changedWave) {
            faster = faster || (g->startMS == 4000 && std::fabs(g->speed - 1.5) < 1e-9 && g->manual);
        }
        Check(c1 != nullptr && c1->mode == StageMode::LocalWaves && c1->manual, "a mode a person chose is kept");
        Check(matrixOut, "a figure a person took out of a scene takes no part in it");
        Check(c2 != nullptr && c2->gestures.empty(), "a scene a person wanted without gestures has none");
        Check(faster, "a speed a person chose replaces the automatic one");
        Check(std::find(changed.warnings.begin(), changed.warnings.end(), "edit-scene-gone:60000") != changed.warnings.end(), "a change for a scene that no longer exists is said, not applied elsewhere");
        const GlobalGesture* placed = changed.Gesture("propagated-wave@13000");
        bool placedOverSinger = false;
        for (const GestureParticipant& p : placed != nullptr ? placed->participants : std::vector<GestureParticipant>()) {
            placedOverSinger = placedOverSinger || p.figure == "Arbol Cara";
        }
        Check(placed != nullptr && placed->manual && !placed->participants.empty() && !placedOverSinger,
              "a wave a person placed in the narration is there, marked as theirs, and still keeps off the singing face");
        bool keptHere = false;
        for (const StageLayer& l : changed.layers) {
            keptHere = keptHere || (l.figure == "Arbol Cara" && l.startMS == 7000);
        }
        StageEdits never;
        never.edits.push_back({ "no-identity", -1, -1, "Arbol Cara", "" });
        StageDirectorInput withoutColours = in;
        withoutColours.edits = &never;
        bool none2 = true;
        for (const StageLayer& l : DirectStage(withoutColours).layers) {
            none2 = none2 && l.figure != "Arbol Cara";
        }
        Check(keptHere && none2, "a person can ask a figure to keep its colours in a scene where it only accents, or never");
        // An excluded gesture is never chosen.
        StageLibrary personal;
        personal.entries.push_back({ "cascade", "gesture", "", "", {}, {}, false, true, false, "person" });
        StageLibraryEntry mine{ "local:mine", "local", "Mi brillo", "", { "pulse" }, {}, true, false, true, "person" };
        personal.entries.push_back(mine);
        StageLibrary libBack;
        Check(FromJson(ToJson(personal), libBack, error) && libBack.entries.size() == 2, "a person's library is written and read again");
        const StageLibrary library = MergeStageLibrary(libBack);
        Check(library.Find("cascade") != nullptr && library.Find("cascade")->excluded && library.Find("local:mine") != nullptr && library.Find("propagated-wave") != nullptr,
              "the person's choices sit over the built-in gestures");
        StageDirectorInput noCascade = in;
        noCascade.library = &library;
        bool none = true;
        for (const GlobalGesture& g : DirectStage(noCascade).gestures) {
            none = none && g.kind != GestureKind::Cascade;
        }
        Check(none, "a gesture a person excluded is never chosen");

        TestArt(song, score);
        TestRealize(song, score);
        TestSelector();
        TestOrchestra(song);
        TestSongPlan();
        TestGroupFidelity();
        TestSharedOwnership();
    }

    // ------------------------------------------------------------- phase 20

    static int On(const StageScene* sc) {
        int n = 0;
        for (const SceneRole& r : sc != nullptr ? sc->roles : std::vector<SceneRole>()) {
            n += r.role != "rest";
        }
        return n;
    }

    void TestArt(const Song& song, const GlobalStageScore& f19) {
        Title("Phase 20: the song's arc, visual ideas that come back, and how many figures carry them");
        StageDirectorInput same = Input(song);
        same.art = ArtLevel::F19;
        Check(ToJson(DirectStage(same)) == ToJson(f19), "at the F19 level the director decides exactly as before");
        // A song whose first strong part comes back (s1.0 and s4.0 are the same music) and
        // that closes after its peak.
        Song repeated = song;
        repeated.plan.segments[1].repeatGroup = 0;
        repeated.plan.segments[4].repeatGroup = 0;
        repeated.plan.segments[5].endMS = 22000;
        repeated.plan.segments.push_back(Segment("s6.0", 22000, 30000, 1, { "ending" }));
        repeated.plan.durationMS = 30000;
        repeated.analysis.durationMS = 30000;
        MusicalEvent hit;
        hit.type = MusicalEventType::Onset;
        hit.timeMS = 22500;
        hit.intensity = 0.9;
        hit.confidence = 0.9;
        repeated.analysis.events.push_back(hit);
        StageDirectorInput in = Input(repeated);

        in.art = ArtLevel::Direction;
        const GlobalStageScore dir = DirectStage(in);
        const auto narrative = [&](const GlobalStageScore& s, const char* id) {
            const StageScene* sc = SceneOf(s, id);
            return sc != nullptr ? sc->narrative : std::string("?");
        };
        Check(narrative(dir, "s0.0") == "rest" && narrative(dir, "s1.0") == "presentation" && narrative(dir, "s4.0") == "recovery" && narrative(dir, "s5.0") == "climax" &&
                  narrative(dir, "s6.0") == "resolution",
              fmt::format("each scene has its role in the song: {} / {} / {} / {} / {} / {} / {}", narrative(dir, "s0.0"), narrative(dir, "s1.0"), narrative(dir, "s2.0"),
                          narrative(dir, "s3.0"), narrative(dir, "s4.0"), narrative(dir, "s5.0"), narrative(dir, "s6.0")));
        for (const GlobalGesture& g : dir.gestures) {
            std::string delays;
            for (const GestureParticipant& p : g.participants) {
                delays += fmt::format("{}{}", delays.empty() ? "" : ",", p.delayMS);
            }
            const StageScene* sc = dir.SceneAt(g.startMS);
            printf("    %s in %s (%s): %s / %s, delays %s\n", g.id.c_str(), sc != nullptr ? sc->id.c_str() : "?", sc != nullptr ? sc->narrative.c_str() : "?", g.relation.c_str(),
                   g.presence.c_str(), delays.c_str());
        }
        bool described = !dir.gestures.empty();
        bool answering = false, together = false, background = true, peak = true;
        for (const GlobalGesture& g : dir.gestures) {
            described = described && !g.relation.empty() && !g.presence.empty();
            const StageScene* sc = dir.SceneAt(g.startMS);
            const bool climax = sc != nullptr && (sc->narrative == "climax" || sc->narrative == "intensification");
            if ((g.kind == GestureKind::LocalWaves || g.kind == GestureKind::JointPulse) && g.participants.size() >= 2) {
                int latest = 0;
                for (const GestureParticipant& p : g.participants) {
                    latest = std::max(latest, p.delayMS);
                }
                answering = answering || (!climax && g.relation == "answer" && latest > 0);
                together = together || (!climax && latest == 0);
            }
            background = background && (g.kind != GestureKind::AmbientField || g.presence == "background");
            peak = peak && (!climax || g.kind == GestureKind::AmbientField || g.presence == "climax");
        }
        Check(described, "every gesture says how its figures follow in time and how much it should be seen");
        Check(answering && !together, "away from the peak, figures that share a hit answer one another instead of lighting all at once");
        Check(background && peak, "a dim field under a voice stays in the background; the peak's gestures are meant to be seen");
        bool connected = SceneOf(dir, "s0.0") != nullptr && SceneOf(dir, "s0.0")->changes == "the show begins";
        for (const StageScene& sc : dir.scenes) {
            connected = connected && !sc.changes.empty() && !sc.leadsTo.empty() && !sc.attention.empty();
        }
        Check(connected, "each scene says what changes, how it leads on and how many figures carry it");

        in.art = ArtLevel::Motifs;
        const GlobalStageScore motifs = DirectStage(in);
        const VisualMotif* m1 = motifs.Motif("M1");
        const StageScene* back = SceneOf(motifs, "s4.0");
        Check(m1 != nullptr && m1->origin == "scene:s1.0" && !m1->gestureKind.empty() && !m1->figures.empty() && !m1->explanation.empty(),
              fmt::format("the first scene presents a visual idea: who, which gesture ({}), from where", m1 != nullptr ? m1->gestureKind : "none"));
        bool carried = false;
        for (const GlobalGesture& g : motifs.gestures) {
            carried = carried || (g.motif == "M1" && g.startMS >= 3000 && g.startMS < 7000);
        }
        Check(carried, "a gesture of the first scene carries the idea");
        bool sameFigure = false;
        if (m1 != nullptr && back != nullptr) {
            for (const std::string& f : m1->figures) {
                const std::string role = back->RoleOf(f);
                sameFigure = sameFigure || (!role.empty() && role != "rest");
            }
        }
        Check(back != nullptr && back->visualMotif == "M1" && back->motifOperation == "recall" && sameFigure, "the part that comes back brings the same idea back, with its own figures");
        bool presented = false, recalled = false;
        for (const MotifVariant& v : m1 != nullptr ? m1->variants : std::vector<MotifVariant>()) {
            presented = presented || v.operation == "present";
            recalled = recalled || v.operation == "recall";
        }
        Check(presented && recalled, "the idea keeps every scene that uses it and what each does with it");
        GlobalStageScore read;
        std::string error;
        Check(FromJson(ToJson(motifs), read, error) && ToJson(read) == ToJson(motifs) && read.art == static_cast<int>(ArtLevel::Motifs) && read.motifs.size() == motifs.motifs.size(),
              "visual ideas, roles and relations are written and read back unchanged");
        Check(ToJson(DirectStage(in)) == ToJson(motifs), "the same song gives the same ideas");
        if (m1 != nullptr) {
            const StageLibraryEntry kept = CompositionFromMotif(*m1, motifs, "prueba");
            Check(kept.kind == "composition" && kept.custom && kept.source == "person" && kept.figures == m1->figures && kept.gesture == m1->gestureKind && !kept.contexts.empty(),
                  "a visual idea a person wants to keep becomes a composition of their own, with its figures, gesture and kind of part");
        }

        in.art = ArtLevel::Full;
        const GlobalStageScore full = DirectStage(in);
        const StageScene* open = SceneOf(full, "s1.0");
        const StageScene* top = SceneOf(full, "s5.0");
        const StageScene* close = SceneOf(full, "s6.0");
        int all = 0;
        for (const SceneRole& r : top != nullptr ? top->roles : std::vector<SceneRole>()) {
            ++all;
        }
        Check(open != nullptr && open->focus && On(open) <= 3, fmt::format("the idea is presented by a few figures ({}), the others rest", On(open)));
        Check(top != nullptr && On(top) == all && all > 4, fmt::format("at the peak the whole house takes part ({} of {})", On(top), all));
        std::set<int> counts;
        for (const StageScene& sc : full.scenes) {
            if (sc.narrative != "rest") {
                counts.insert(On(&sc));
            }
        }
        Check(counts.size() >= 3, fmt::format("how many figures take part changes with what each scene does ({} different counts)", counts.size()));
        bool opens = false, closes = false;
        for (const GlobalGesture* g : GesturesOf(full, top)) {
            opens = opens || (g->kind == GestureKind::Expansion && g->startMS < top->startMS + 2000 && !g->motif.empty() && g->motif == top->visualMotif);
        }
        for (const GlobalGesture* g : GesturesOf(full, close)) {
            closes = closes || (g->kind == GestureKind::Contraction && g->relation == "progressive-end" && g->level <= 0.6 + 1e-9);
        }
        Check(opens, "the peak opens from where the idea was born");
        Check(close != nullptr && close->focus && On(close) <= 3 && closes, "the last scene closes the idea: a few figures, the light gathering back quietly");
        Check(ToJson(DirectStage(in)) == ToJson(full), "the developed song is the same every time");
        // A person's choices still win over the development.
        StageEdits edits;
        edits.edits.push_back({ "mode", 3000, -1, "full-ensemble", "test" });
        edits.edits.push_back({ "exclude", 19000, -1, "Matriz Prueba", "test" });
        StageDirectorInput edited = in;
        edited.edits = &edits;
        const GlobalStageScore mine = DirectStage(edited);
        const StageScene* chosen = SceneOf(mine, "s1.0");
        bool excludedOut = SceneOf(mine, "s5.0") != nullptr && SceneOf(mine, "s5.0")->RoleOf("Matriz Prueba") == "rest";
        for (const GlobalGesture* g : GesturesOf(mine, SceneOf(mine, "s5.0"))) {
            for (const GestureParticipant& p : g->participants) {
                excludedOut = excludedOut && p.figure != "Matriz Prueba";
            }
        }
        Check(chosen != nullptr && chosen->mode == StageMode::FullEnsemble && chosen->manual && !chosen->focus && excludedOut,
              "a mode a person chose and a figure a person left out win over the development");

        // Compositions: whole ideas a scene can be built on, at three levels.
        const StageLibrary built = BuiltInStageLibrary();
        std::set<std::string> levels;
        int compositions = 0;
        for (const StageLibraryEntry& e : built.entries) {
            if (e.kind == "composition") {
                ++compositions;
                levels.insert(e.level);
            }
        }
        Check(compositions >= 12 && levels.size() == 3, fmt::format("{} built-in compositions, local, of a group and of the whole house", compositions));
        bool fits = true;
        const auto peaks = CompositionsFor(built, "climax");
        for (const StageLibraryEntry* e : peaks) {
            fits = fits && std::find(e->contexts.begin(), e->contexts.end(), "climax") != e->contexts.end();
        }
        Check(!peaks.empty() && fits, "the compositions for a peak are the ones that suit a peak");
        StageLibrary personal;
        StageLibraryEntry fav = *built.Find("composition:gathers-to-close");
        fav.favorite = true;
        fav.level0 = 0.3;
        StageLibraryEntry off = *built.Find("composition:echo-tail");
        off.excluded = true;
        StageLibraryEntry own;
        own.id = "composition:my-chorus";
        own.kind = "composition";
        own.title = "My chorus";
        own.level = "group";
        own.contexts = { "chorus", "ending" };
        own.gesture = "local-waves";
        own.figures = { "Arbol Grande", "Matriz Prueba" };
        own.custom = true;
        own.source = "person";
        personal.entries = { fav, off, own };
        StageLibrary readLib;
        Check(FromJson(ToJson(personal), readLib, error) && ToJson(readLib) == ToJson(personal), "a person's compositions are written and read back with their parts");
        const StageLibrary merged = MergeStageLibrary(readLib);
        const auto endings = CompositionsFor(merged, "ending");
        bool echoOut = true, mineIn = false;
        for (const StageLibraryEntry* e : endings) {
            echoOut = echoOut && e->id != "composition:echo-tail";
            mineIn = mineIn || e->id == "composition:my-chorus";
        }
        Check(!endings.empty() && endings.front()->id == "composition:gathers-to-close" && std::abs(endings.front()->level0 - 0.3) < 1e-9 && echoOut && mineIn,
              "a favourite comes first with the person's own values, an excluded one is never offered, a person's own is");
    }

    // The score into effects, rendered by xLights: each figure lights when the score says.
    // ------------------------------------------------------------- phase 21

    static SelectorScene Facts(const char* context, const char* narrative, int available, int durationMS, int strongHits) {
        SelectorScene s;
        s.context = context;
        s.sectionKind = "verse";
        s.narrative = narrative;
        s.available = available;
        s.durationMS = durationMS;
        s.strongHits = strongHits;
        s.beat = 0.5;
        s.priority = 1;
        s.topologies["tree"] = 1;
        s.topologies["matrix"] = available - 1;
        return s;
    }

    static const CompositionCandidate* Candidate(const CompositionChoice& c, const std::string& id) {
        for (const CompositionCandidate& x : c.candidates) {
            if (x.id == id) {
                return &x;
            }
        }
        return nullptr;
    }

    static bool Says(const CompositionCandidate* c, const std::string& reason) {
        if (c == nullptr) {
            return false;
        }
        for (const std::string& r : c->reasons) {
            if (r.find(reason) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    void TestSelector() {
        Title("Phase 21: the composition each scene is built on, chosen for all its circumstances");
        const StageLibrary library = BuiltInStageLibrary();
        // A voice tells the story, one strong hit, plenty of figures: one figure tells, the house listens.
        SelectorScene told = Facts("narration", "presentation", 6, 8000, 1);
        told.faces = 1;
        const CompositionChoice a = SelectComposition(library, told);
        Check(a.entry != nullptr && a.entry->id == "composition:protagonist-glow" && !a.reasons.empty(),
              fmt::format("a narration that presents the idea: '{}', with its reasons", a.entry != nullptr ? a.entry->id : "none"));
        Check(a.candidates.size() == 12, fmt::format("all 12 compositions weighed and kept with their verdict ({})", a.candidates.size()));
        const CompositionChoice again = SelectComposition(library, told);
        bool same = again.entry == a.entry && again.candidates.size() == a.candidates.size();
        for (size_t i = 0; same && i < a.candidates.size(); ++i) {
            same = a.candidates[i].id == again.candidates[i].id && a.candidates[i].score == again.candidates[i].score && a.candidates[i].verdict == again.candidates[i].verdict;
        }
        Check(same, "the same scene always gets the same composition and the same reasons");
        // Not enough figures: a group composition is ruled out, and says why.
        SelectorScene few = Facts("solo", "development", 2, 8000, 2);
        const CompositionChoice b = SelectComposition(library, few);
        const CompositionCandidate* neighbours = Candidate(b, "composition:neighbours-answer");
        Check(neighbours != nullptr && neighbours->verdict == "rejected" && Says(neighbours, "not enough figures"), "with two figures, 'neighbours answer' (3 or more) is ruled out, and it says why");
        // Too short: nothing that needs time fits a second and a half.
        SelectorScene shortScene = Facts("solo", "development", 6, 1500, 2);
        const CompositionChoice c = SelectComposition(library, shortScene);
        const CompositionCandidate* call = Candidate(c, "composition:call-and-answer");
        Check(call != nullptr && call->verdict == "rejected" && Says(call, "too short"), "a scene too short for a call and its answers rules it out");
        // The music rests: no composition at all, and the director's own way, with the reason.
        SelectorScene silent = Facts("silence", "rest", 6, 8000, 0);
        silent.mode = StageMode::Rest;
        const CompositionChoice d = SelectComposition(library, silent);
        Check(d.entry == nullptr && !d.fallback.empty(), fmt::format("where the music rests no composition is used: '{}'", d.fallback));
        // No hit, no peak, nothing closing: every composition needs something the scene lacks.
        SelectorScene nothing = Facts("solo", "development", 6, 8000, 0);
        const CompositionChoice e = SelectComposition(library, nothing);
        Check(e.entry == nullptr && e.fallback == "no composition can be done here", "a scene with no moment for any composition keeps the director's own way, and says why");
        // A person chose the mode: a composition using another amount of the house is ruled out.
        SelectorScene byPerson = Facts("climax", "climax", 6, 8000, 2);
        byPerson.mode = StageMode::Protagonist;
        byPerson.modeByPerson = true;
        byPerson.peakBegins = true;
        const CompositionChoice f = SelectComposition(library, byPerson);
        const CompositionCandidate* wave = Candidate(f, "composition:wave-house");
        Check(wave != nullptr && wave->verdict == "rejected" && Says(wave, "a person chose another way") && (f.entry == nullptr || f.entry->level == "local"),
              "a mode a person chose wins: the whole-house wave is ruled out in a scene they gave to one figure");
        // The idea comes back: its own composition is preferred.
        SelectorScene back = Facts("solo", "recovery", 6, 8000, 2);
        back.motifComposition = "composition:neighbours-answer";
        back.motifOperation = "recall";
        const CompositionChoice g = SelectComposition(library, back);
        Check(g.entry != nullptr && g.entry->id == "composition:neighbours-answer" && Says(Candidate(g, g.entry->id), "born with"),
              "an idea that comes back brings back the composition it was born with");
        // Developed, it grows to more figures.
        SelectorScene grown = back;
        grown.motifComposition = "composition:protagonist-glow";
        grown.motifOperation = "develop";
        grown.narrative = "development";
        const CompositionChoice h = SelectComposition(library, grown);
        Check(h.entry != nullptr && h.entry->level == "group" && Says(Candidate(h, h.entry->id), "grows"), fmt::format("an idea developed grows from one figure to a group ('{}')", h.entry != nullptr ? h.entry->id : "none"));
        // A person's choice wins, and their 'none' too; an impossible choice is not made.
        SelectorScene asked = Facts("solo", "development", 6, 8000, 2);
        asked.personChoice = "composition:wave-house";
        const CompositionChoice i = SelectComposition(library, asked);
        Check(i.entry != nullptr && i.entry->id == "composition:wave-house" && Says(Candidate(i, "composition:wave-house"), "chosen by a person"), "a composition a person chose for the scene is used");
        asked.personChoice = "none";
        Check(SelectComposition(library, asked).entry == nullptr, "a person can ask for the director's own way");
        SelectorScene impossible = Facts("solo", "development", 2, 8000, 2);
        impossible.personChoice = "composition:neighbours-answer";
        const CompositionChoice j = SelectComposition(library, impossible);
        Check(j.entry == nullptr || j.entry->id != "composition:neighbours-answer", "a composition a person chose that cannot be done here is not forced");
        // Excluded by a person: never chosen.
        StageLibrary personal;
        StageLibraryEntry off;
        off.id = "composition:protagonist-glow";
        off.kind = "composition";
        off.excluded = true;
        personal.entries.push_back(off);
        const StageLibrary merged = MergeStageLibrary(personal);
        const CompositionChoice k = SelectComposition(merged, told);
        Check(k.entry == nullptr || k.entry->id != "composition:protagonist-glow", "a composition a person excluded is never chosen");
        // Fitted to the house: a group composition of 3-4 figures with only 3 there.
        SelectorScene three = Facts("solo", "development", 3, 8000, 2);
        three.target = 6;
        const CompositionChoice l = SelectComposition(library, three);
        Check(l.entry != nullptr && l.figures <= 3 && l.figures >= l.entry->minFigures && !l.adapted.empty(),
              fmt::format("fitted to the figures there: '{}' with {} figures ({})", l.entry != nullptr ? l.entry->id : "none", l.figures, l.adapted));
    }

    void TestOrchestra(const Song& base) {
        Title("Phase 21: the director commands the whole house");
        // The song with an ending after its peak, so the peak is a peak and not the close.
        Song song = base;
        song.plan.segments[5].endMS = 22000;
        song.plan.segments.push_back(Segment("s6.0", 22000, 30000, 1, { "ending" }));
        song.plan.durationMS = 30000;
        song.analysis.durationMS = 30000;
        MusicalEvent hit;
        hit.type = MusicalEventType::Onset;
        hit.timeMS = 22500;
        hit.intensity = 0.9;
        hit.confidence = 0.9;
        song.analysis.events.push_back(hit);
        // As in a real show, every figure has effects of its own from the other engines.
        for (const StageFigure& f : _space.figures) {
            if (f.placed && _geometry.Find(f.name) != nullptr) {
                ChoreographyEvent e;
                e.id = "own/" + f.name;
                e.target = f.name;
                e.effect = "On";
                e.intent = "accompany";
                e.startMS = 3000;
                e.endMS = 30000;
                e.colors = { "#20A040" };
                song.faces.events.push_back(e);
            }
        }
        StageDirectorInput in = Input(song);
        const StageLibrary library = BuiltInStageLibrary();
        in.library = &library;
        in.art = ArtLevel::Full;
        const GlobalStageScore f20 = DirectStage(in);
        bool untouched = true;
        for (const StageScene& sc : f20.scenes) {
            untouched = untouched && sc.composition.empty() && sc.candidates.empty() && sc.presence.empty();
        }
        Check(untouched && f20.version == STAGE_DIRECTOR_ART_VERSION, "at the phase 20 level nothing of phase 21 is decided: it generates as before");
        in.art = ArtLevel::Orchestra;
        GlobalStageScore score = DirectStage(in);
        Check(ToJson(DirectStage(in)) == ToJson(score), "the same song, layout and choices always give the same orchestra");
        int composed = 0, realised = 0, explained = 0;
        for (const StageScene& sc : score.scenes) {
            if (sc.composition.empty()) {
                explained += sc.compositionFallback.empty() ? 0 : 1;
                continue;
            }
            ++composed;
            explained += sc.compositionReasons.empty() ? 0 : 1;
            std::string kinds;
            for (const GlobalGesture* g : GesturesOf(score, &sc)) {
                kinds += fmt::format(" {}[{}]", g->id, g->composition);
            }
            for (const CompositionCandidate& c : sc.candidates) {
                if (c.verdict != "rejected") {
                    kinds += fmt::format(" | {} {:.0f}", c.id.substr(12), c.score);
                }
            }
            printf("    %s %s (%s, %s): %s\n", sc.id.c_str(), sc.composition.c_str(), sc.narrative.c_str(), StageModeId(sc.mode), kinds.c_str());
            for (const GlobalGesture* g : GesturesOf(score, &sc)) {
                if (g->composition == sc.composition) {
                    ++realised;
                    break;
                }
            }
        }
        Check(composed >= 3, fmt::format("the director builds scenes on the library's compositions: {} of {} scenes", composed, score.scenes.size()));
        Check(realised * 2 >= composed, fmt::format("the composition chosen is made: its gesture is in the scene ({} of {})", realised, composed));
        Check(explained == static_cast<int>(score.scenes.size()), "every scene says why it uses its composition, or why none");
        GlobalStageScore back;
        std::string error;
        Check(FromJson(ToJson(score), back, error) && ToJson(back) == ToJson(score), "compositions, candidates and presence are written and read back unchanged");
        // A person's composition for a scene.
        StageEdits edits;
        const StageScene* climax = SceneOf(score, "s5.0");
        edits.edits.push_back({ "composition", 3000, -1, "composition:accent-flash", "" });
        StageDirectorInput withEdit = in;
        withEdit.edits = &edits;
        const GlobalStageScore asked = DirectStage(withEdit);
        const StageScene* s1 = SceneOf(asked, "s1.0");
        Check(s1 != nullptr && s1->composition == "composition:accent-flash", fmt::format("a composition a person chose for a scene is used ('{}')", s1 != nullptr ? s1->composition : ""));
        // Presence: every figure on the stage has a state and a policy in every scene.
        bool complete = true, rested = false, glow = false;
        for (const StageScene& sc : score.scenes) {
            complete = complete && sc.presence.size() == static_cast<size_t>(std::count_if(_space.figures.begin(), _space.figures.end(), [&](const StageFigure& f) {
                           return f.placed && f.lights > 0 && !f.noController && _geometry.Find(f.name) != nullptr;
                       }));
            for (const FigurePresence& p : sc.presence) {
                rested = rested || p.policy == "suspended";
                glow = glow || p.policy == "reduced";
            }
        }
        Check(complete && rested && glow, "every figure gets a presence in every scene: some lead, some glow, some rest");
        const StageScene* peak = climax;
        bool allPlay = peak != nullptr;
        int playing = 0;
        for (const FigurePresence& p : peak != nullptr ? peak->presence : std::vector<FigurePresence>()) {
            // A figure with nothing of its own to show there is said to rest: the score says what the render shows.
            allPlay = allPlay && (p.policy == "normal" || p.why == "nothing of its own to show in this scene");
            playing += p.policy == "normal" ? 1 : 0;
        }
        allPlay = allPlay && playing >= 5;
        for (const FigurePresence& p : peak != nullptr ? peak->presence : std::vector<FigurePresence>()) {
            if (p.policy != "normal") {
                printf("    at the peak %s: %s / %s (%s)\n", p.figure.c_str(), p.state.c_str(), p.policy.c_str(), p.why.c_str());
            }
        }
        Check(allPlay, fmt::format("at the peak the whole house plays: every figure with light of its own ({} figures)", playing));
        TestPresence(song, score);
    }

    void TestPresence(const Song& song, GlobalStageScore score) {
        Title("Phase 21: what the score asks is what the effects do");
        StageDirectorInput in = Input(song);
        const StageLibrary library = BuiltInStageLibrary();
        in.library = &library;
        in.art = ArtLevel::Orchestra;
        // A scene with one lead, a figure glowing and a figure resting.
        const StageScene* scene = nullptr;
        std::string lead, glowing, resting;
        for (const StageScene& sc : score.scenes) {
            std::string l, g, r;
            for (const FigurePresence& p : sc.presence) {
                if (p.state == "protagonist" && l.empty()) l = p.figure;
                if (p.policy == "reduced" && g.empty()) g = p.figure;
                if (p.policy == "suspended" && r.empty() && _geometry.Find(p.figure) != nullptr) r = p.figure;
            }
            if (!l.empty() && !g.empty() && !r.empty()) {
                scene = &sc;
                lead = l;
                glowing = g;
                resting = r;
                break;
            }
        }
        if (scene == nullptr) {
            Check(false, "a scene with a lead, a glow and a rest");
            return;
        }
        const int a = scene->startMS, b = scene->endMS;
        Choreography c;
        c.durationMS = 24000;
        c.frameMS = FRAME;
        const auto event = [&](const std::string& id, const std::string& target, const char* effect, const char* intent, TargetType type, int layer = 0) {
            ChoreographyEvent e;
            e.id = id;
            e.target = target;
            e.type = type;
            e.effect = effect;
            e.intent = intent;
            e.layer = layer;
            e.startMS = a;
            e.endMS = b;
            e.colors = { "#2040FF" };
            c.events.push_back(e);
            return &c.events.back();
        };
        event("lead", lead, "On", "accompany", TargetType::Model);
        event("glow", glowing, "On", "accompany", TargetType::Model);
        event("rest", resting, "On", "accompany", TargetType::Model);
        event("rest-text", resting, "Text", "lyric-text", TargetType::Model, 1);
        event("rest-face", resting, "Faces", "face-sing", TargetType::Model, 2);
        event("rest-video", resting, "Video", "media", TargetType::Model, 3);
        event("rest-locked", resting, "On", "accompany", TargetType::Model, 4)->state = DecisionState::Locked;
        // A group holding figures the scene asks different things of.
        const TargetInfo* group = _caps.Find("Grupo Todo");
        event("group", "Grupo Todo", "On", "accompany", TargetType::Group);
        c.events.back().settings = { { "B_CHOICE_BufferStyle", "Per Model Default" }, { "T_CHOICE_In_Transition_Type", "Fade" }, { "T_TEXTCTRL_Fadein", "0.50" } };
        const std::vector<std::string> notes = ApplyStagePresence(score, in, c);
        for (const std::string& n : notes) {
            printf("    %s\n", n.c_str());
        }
        const auto find = [&](const std::string& id) -> const ChoreographyEvent* {
            for (const ChoreographyEvent& e : c.events) {
                if (e.id == id) {
                    return &e;
                }
            }
            return nullptr;
        };
        Check(find("lead") != nullptr && find("lead")->brightness == 100, "the lead keeps its effect whole");
        const int glowLevel = static_cast<int>(std::lround(score.ambientLevel * 100));
        Check(find("glow") != nullptr && find("glow")->brightness == glowLevel && find("glow")->PaletteString().find(fmt::format("C_SLIDER_Brightness={}", glowLevel)) != std::string::npos,
              fmt::format("a figure kept as a glow is dimmed with xLights' own brightness ({}%)", glowLevel));
        Check(find("rest") == nullptr, "a resting figure's own effect stops for the scene");
        Check(find("rest-text") != nullptr && find("rest-face") != nullptr && find("rest-video") != nullptr, "its words, its singing face and its video stay: they are never touched");
        Check(find("rest-locked") != nullptr && find("rest-locked")->layer == 4, "an effect a person locked stays exactly as it was");
        int members = 0, restingLit = 0, dimmed = 0, groupLeft = 0;
        bool noGroupStyle = true, noFade = true;
        for (const ChoreographyEvent& e : c.events) {
            if (e.target == "Grupo Todo") {
                ++groupLeft;
            }
            if (e.id.rfind("group/", 0) == 0) {
                ++members;
                restingLit += e.target == resting ? 1 : 0;
                dimmed += e.brightness < 100 ? 1 : 0;
                for (const auto& [k, v] : e.settings) {
                    noGroupStyle = noGroupStyle && k != "B_CHOICE_BufferStyle";
                    noFade = noFade && k != "T_TEXTCTRL_Fadein";
                }
            }
        }
        const FigurePresence* gp = nullptr;
        int groupFiguresPlaying = 0;
        for (const std::string& m : group != nullptr ? group->flatModels : std::vector<std::string>()) {
            gp = scene->PresenceOf(m);
            groupFiguresPlaying += gp == nullptr || gp->policy != "suspended" ? 1 : 0;
        }
        Check(group != nullptr && groupLeft == 0 && members == groupFiguresPlaying && restingLit == 0,
              fmt::format("a group effect over figures asked different things is split: {} figure effects for the {} that play, none for the resting one", members, groupFiguresPlaying));
        Check(dimmed > 0 && noGroupStyle && noFade, "the split pieces keep the glow's dimming and drop the group's own layout and fades");
        bool overlap = false;
        for (size_t i = 0; i < c.events.size(); ++i) {
            for (size_t j = i + 1; j < c.events.size(); ++j) {
                const ChoreographyEvent& x = c.events[i];
                const ChoreographyEvent& y = c.events[j];
                overlap = overlap || (x.target == y.target && x.layer == y.layer && x.startMS < y.endMS && y.startMS < x.endMS);
            }
        }
        Check(!overlap, "no two effects share a layer of a figure at the same time");
        // The joins between compositions.
        Choreography joined = c;
        // An effect on the resting figure in the scene after: it comes in there.
        const StageScene* next = nullptr;
        for (size_t i = 0; i + 1 < score.scenes.size(); ++i) {
            if (&score.scenes[i] == scene) {
                next = &score.scenes[i + 1];
            }
        }
        const std::vector<std::string> linked = LinkCompositions(score, in, joined);
        for (const std::string& n : linked) {
            printf("    %s\n", n.c_str());
        }
        bool kinds = !score.transitions.empty();
        for (const CompositionTransition& t : score.transitions) {
            kinds = kinds && !t.kind.empty() && !t.why.empty() && !t.checks.empty();
        }
        Check(kinds && score.transitions.size() + 1 == score.scenes.size(), fmt::format("every join between scenes has a kind and a reason ({} joins)", score.transitions.size()));
        (void)next;
        // The presence written and read back, and the same again.
        GlobalStageScore back;
        std::string error;
        Check(FromJson(ToJson(score), back, error) && ToJson(back) == ToJson(score) && back.transitions.size() == score.transitions.size(),
              "the presence, the shared lights and the joins are written and read back unchanged");
        // Rendered: the light follows the score.
        Choreography whole;
        whole.durationMS = 30000;
        whole.frameMS = FRAME;
        for (const StageFigure& f : _space.figures) {
            if (!f.placed || _geometry.Find(f.name) == nullptr) {
                continue;
            }
            ChoreographyEvent e;
            e.id = "own/" + f.name;
            e.target = f.name;
            e.effect = "On";
            e.intent = "accompany";
            e.startMS = 3000;
            e.endMS = 19000;
            e.colors = { "#20A040" };
            whole.events.push_back(e);
        }
        ChoreographyEvent g;
        g.id = "house";
        g.target = "Grupo Todo";
        g.type = TargetType::Group;
        g.effect = "On";
        g.layer = 0;
        g.startMS = 3000;
        g.endMS = 19000;
        g.colors = { "#A02020" };
        whole.events.push_back(g);
        GlobalStageScore rendered = score;
        (void)ApplyStagePresence(rendered, in, whole);
        (void)RealizeStage(rendered, in, whole);
        const SequenceSpec spec = ToSequenceSpec(whole, {});
        std::string fseq;
        Check(Render(spec, "presencia", fseq), "the whole house's effects, after the score, written and rendered");
        const StageCheck check = CheckStage(fseq, _geometry, _space, &rendered);
        int restLit = 0, judged = 0;
        for (const SceneLight& s : check.scenes) {
            if (s.startMS >= 3000 && s.endMS <= 19000) {
                restLit += s.restButLit - s.restLitByShared;
                judged += static_cast<int>(s.activity.size());
            }
        }
        Check(check.ran && judged > 0 && restLit == 0, fmt::format("in the render no figure the score rests is lit by its own or a group's effect ({} figure x scene measured)", judged));
        Check(check.agreement >= 0.9, fmt::format("the render agrees with the score for {:.0f}% of figure x scene", check.agreement * 100));
        int readings = 0;
        for (const GestureReading& r : check.readings) {
            readings += r.participants > 0 && r.peak >= r.before ? 1 : 0;
        }
        Check(!check.readings.empty() && readings == static_cast<int>(check.readings.size()), fmt::format("every gesture read in time and on the stage ({} readings)", check.readings.size()));
        // Figures wired to the same lights: found, and one gives way.
        ModelCapabilities caps = _caps;
        std::vector<std::string> names;
        for (const StageFigure& f : _space.figures) {
            if (_geometry.Find(f.name) != nullptr) {
                names.push_back(f.name);
            }
        }
        std::sort(names.begin(), names.end());
        TargetInfo* x = nullptr;
        TargetInfo* y = nullptr;
        for (TargetInfo& t : caps.targets) {
            if (names.size() >= 2 && t.name == names[0]) x = &t;
            if (names.size() >= 2 && t.name == names[1]) y = &t;
        }
        if (x == nullptr || y == nullptr) {
            Check(false, "two figures to wire together");
            return;
        }
        x->firstChannel = 1;
        x->lastChannel = 300;
        y->firstChannel = 100;
        y->lastChannel = 200;
        x->channelsPerNode = y->channelsPerNode = 3;
        const std::vector<SharedLights> pairs = FindSharedLights(caps, names);
        bool found = false;
        for (const SharedLights& s : pairs) {
            found = found || (s.a == names[0] && s.b == names[1] && s.relation == "b-inside-a" && s.lights == 34);
        }
        Check(found, "two figures wired to the same lights are found, one inside the other, with the lights they share");
        StageDirectorInput shared = in;
        shared.capabilities = &caps;
        GlobalStageScore both = score;
        for (StageScene& sc : both.scenes) {
            for (FigurePresence& p : sc.presence) {
                if (p.figure == names[0] || p.figure == names[1]) {
                    p.state = p.figure == names[0] ? "protagonist" : "response";
                    p.policy = "normal";
                }
            }
        }
        Choreography none;
        none.frameMS = FRAME;
        (void)ApplyStagePresence(both, shared, none);
        bool gaveWay = !both.scenes.empty();
        for (const StageScene& sc : both.scenes) {
            const FigurePresence* inner = sc.PresenceOf(names[1]);
            gaveWay = gaveWay && (inner == nullptr || inner->policy == "suspended");
        }
        Check(gaveWay, "where both would play, the one inside the other gives way to the one that carries more");
        StageEdits priority;
        priority.edits.push_back({ "priority", -1, -1, names[1] + "|" + names[0], "" });
        shared.edits = &priority;
        GlobalStageScore chosen = score;
        for (StageScene& sc : chosen.scenes) {
            for (FigurePresence& p : sc.presence) {
                if (p.figure == names[0] || p.figure == names[1]) {
                    p.state = "complement";
                    p.policy = "normal";
                }
            }
        }
        (void)ApplyStagePresence(chosen, shared, none);
        bool personWins = !chosen.scenes.empty();
        for (const StageScene& sc : chosen.scenes) {
            const FigurePresence* outer = sc.PresenceOf(names[0]);
            personWins = personWins && (outer == nullptr || outer->policy == "suspended");
        }
        Check(personWins && !chosen.sharedLights.empty() && chosen.sharedLights.front().prioritySource == "person", "a person's choice of which one wins on the shared lights is kept");
    }

    // ------------------------------------------------------------- phase 22

    static PlanScene PlanFacts(const char* id, const char* context, const char* narrative, const char* op, const char* motif, int origin) {
        PlanScene p;
        p.scene = id;
        p.facts = Facts(context, narrative, 6, 8000, 2);
        p.facts.motifOperation = op;
        p.motif = motif;
        p.origin = origin;
        return p;
    }

    void TestSongPlan() {
        Title("Phase 22: the compositions planned for the whole song, not scene by scene");
        const StageLibrary library = BuiltInStageLibrary();
        // Six verses alike: on its own each scene would take the same composition every time.
        std::vector<PlanScene> verses;
        for (int i = 0; i < 6; ++i) {
            verses.push_back(PlanFacts(("v" + std::to_string(i)).c_str(), "solo", "development", "", "", -1));
        }
        const CompositionPlanResult plan = PlanCompositions(library, verses);
        std::map<std::string, int> uses;
        int consecutive = 0, explained = 0, song = 0;
        for (size_t i = 0; i < plan.scenes.size(); ++i) {
            uses[plan.scenes[i].id]++;
            consecutive += i > 0 && plan.scenes[i].id == plan.scenes[i - 1].id ? 1 : 0;
            explained += plan.scenes[i].localBest.empty() || plan.scenes[i].alternatives.empty() ? 0 : 1;
            song += plan.scenes[i].kind == "song" ? 1 : 0;
        }
        int most = 0;
        for (const auto& [id, n] : uses) {
            most = std::max(most, n);
        }
        const std::string alone = SelectComposition(library, verses[0].facts).entry != nullptr ? SelectComposition(library, verses[0].facts).entry->id : "";
        Check(!alone.empty() && most <= 3 && consecutive == 0 && uses.size() >= 2,
              fmt::format("six verses alike: alone each would take '{}'; planned together, {} compositions, the most used {} times, none twice in a row", alone, uses.size(), most));
        Check(song > 0 && explained == static_cast<int>(plan.scenes.size()), "where the song chose another than the scene's own best it says so, with the alternatives and their scores");
        const CompositionPlanResult again = PlanCompositions(library, verses);
        bool same = again.scenes.size() == plan.scenes.size();
        for (size_t i = 0; same && i < plan.scenes.size(); ++i) {
            same = again.scenes[i].id == plan.scenes[i].id && again.scenes[i].kind == plan.scenes[i].kind;
        }
        Check(same && plan.explored > 0, fmt::format("the same song always gets the same plan ({} plans weighed)", plan.explored));
        // An idea presented, then brought back: the composition it was born with comes back, as continuity.
        std::vector<PlanScene> idea = { PlanFacts("p", "solo", "presentation", "present", "M1", 0), PlanFacts("d1", "solo", "development", "develop", "M1", 0),
                                        PlanFacts("d2", "solo", "development", "develop", "M1", 0), PlanFacts("r", "solo", "recovery", "recall", "M1", 0) };
        idea[0].facts.faces = 1;
        const CompositionPlanResult ideaPlan = PlanCompositions(library, idea);
        Check(ideaPlan.scenes.size() == 4 && !ideaPlan.scenes[0].id.empty() && ideaPlan.scenes[3].id == ideaPlan.scenes[0].id && ideaPlan.scenes[3].kind == "recall" && ideaPlan.scenes[3].song >= 0,
              fmt::format("an idea brought back returns with the composition it was born with ('{}'), at no cost: it is continuity", ideaPlan.scenes[0].id));
        // Brought back again and again right away: the second time costs, so another shape can win.
        std::vector<PlanScene> again3 = idea;
        again3.push_back(PlanFacts("r2", "solo", "recovery", "recall", "M1", 0));
        again3.push_back(PlanFacts("r3", "solo", "recovery", "recall", "M1", 0));
        const CompositionPlanResult runs = PlanCompositions(library, again3);
        bool said = false;
        for (const PlannedComposition& p : runs.scenes) {
            for (const std::string& r : p.reasons) {
                said = said || r.find("comes back again right away") != std::string::npos;
            }
        }
        const bool varied = runs.scenes[4].id != runs.scenes[3].id || runs.scenes[5].id != runs.scenes[4].id;
        Check(said || varied, "brought back again right away, the same composition costs a little: another shape can keep the idea alive");
        // A person's choice for a scene stays, whatever the plan would rather.
        std::vector<PlanScene> asked = verses;
        asked[2].facts.personChoice = "composition:wave-house";
        const CompositionPlanResult personal = PlanCompositions(library, asked);
        Check(personal.scenes[2].id == "composition:wave-house" && personal.scenes[2].kind == "person", "a composition a person chose for a scene is kept in the plan");
        asked[2].facts.personChoice = "none";
        Check(PlanCompositions(library, asked).scenes[2].id.empty(), "and a person's 'the director's own way' too");
        // The only one that suits: kept, and said.
        std::vector<PlanScene> peak = verses;
        peak[3].facts.peakBegins = true;
        peak[3].facts.context = "climax";
        peak[3].facts.narrative = "climax";
        peak[3].facts.strongHits = 0;
        const CompositionPlanResult peaked = PlanCompositions(library, peak);
        Check(peaked.scenes[3].id == "composition:peak-opens", fmt::format("where the peak opens, the peak opens ('{}', {})", peaked.scenes[3].id, peaked.scenes[3].kind));
        // The director with the plan, on the fixture song.
        TestPlannedDirector();
    }

    void TestPlannedDirector() {
        Song song = MakeSong();
        for (const StageFigure& f : _space.figures) {
            if (f.placed && _geometry.Find(f.name) != nullptr) {
                ChoreographyEvent e;
                e.id = "own/" + f.name;
                e.target = f.name;
                e.effect = "On";
                e.intent = "accompany";
                e.startMS = 3000;
                e.endMS = 24000;
                e.colors = { "#20A040" };
                song.faces.events.push_back(e);
            }
        }
        StageDirectorInput in = Input(song);
        const StageLibrary library = BuiltInStageLibrary();
        in.library = &library;
        in.art = ArtLevel::Calibrated;
        const GlobalStageScore planned = DirectStage(in);
        int withPlan = 0, composed = 0;
        for (const StageScene& sc : planned.scenes) {
            withPlan += sc.planKind.empty() ? 0 : 1;
            composed += sc.composition.empty() ? 0 : 1;
        }
        Check(planned.compositionPlan == "song" && planned.version == STAGE_DIRECTOR_CALIBRATED_VERSION && withPlan == static_cast<int>(planned.scenes.size()) && composed > 0,
              fmt::format("at the calibrated level every scene comes from the plan for the whole song ({} built on a composition)", composed));
        Check(ToJson(DirectStage(in)) == ToJson(planned), "the plan and the score are the same every time");
        GlobalStageScore back;
        std::string error;
        Check(FromJson(ToJson(planned), back, error) && ToJson(back) == ToJson(planned), "the plan's reasons and alternatives are written and read back unchanged");
        StageDirectorInput local = in;
        local.compositionPlan = 0;
        const GlobalStageScore sceneByScene = DirectStage(local);
        bool none = true;
        for (const StageScene& sc : sceneByScene.scenes) {
            none = none && sc.planKind.empty();
        }
        Check(none && sceneByScene.compositionPlan == "local", "asked scene by scene, the director chooses as in phase 21");
        // The whole learning loop, with a simulated correction (no real one yet): a person made the
        // figures of a scene answer one another; kept for this song, the next show of the song does it.
        const StageScene* target = nullptr;
        for (const StageScene& sc : planned.scenes) {
            const bool answers = sc.composition == "composition:call-and-answer" || sc.composition == "composition:neighbours-answer";
            bool possible = false;
            for (const CompositionCandidate& c : sc.candidates) {
                possible = possible || (c.id == "composition:neighbours-answer" && c.verdict != "rejected");
            }
            if (!sc.composition.empty() && !answers && possible && target == nullptr) {
                target = &sc;
            }
        }
        if (target == nullptr) {
            Check(false, "a scene to correct into an answer (simulated correction)");
            return;
        }
        SequenceComparison comparison;
        comparison.ran = true;
        SequenceChange change;
        change.kind = "composition-changed";
        change.before = target->composition;
        change.after = "answer";
        change.category = "aesthetic";
        change.sceneStartMS = target->startMS;
        change.startMS = target->startMS;
        change.endMS = target->endMS;
        for (const FigurePresence& p : target->presence) {
            if (p.policy == "normal" && change.figures.size() < 4) {
                change.figures.push_back(p.figure);
            }
        }
        change.nature = ChangeNature(change);
        comparison.changes.push_back(change);
        ArtisticPreferences prefs;
        StageEdits learned;
        ChromaticEdits colours;
        ComparisonDecisions record;
        const DecisionOutcome outcome = ApplyComparisonDecisions(prefs, learned, colours, record, comparison, { { 0, "song" } }, "simulada.xsq");
        StageDirectorInput next = in;
        next.edits = &learned;
        const GlobalStageScore again = DirectStage(next);
        const StageScene* after = again.SceneAt(target->startMS + 1);
        const bool answers = after != nullptr && (after->composition == "composition:neighbours-answer" || after->composition == "composition:call-and-answer");
        Check(outcome.songEdits >= 1 && answers && after->planKind == "person",
              fmt::format("simulated correction, the whole loop: compared, explained, kept for this song, and the next show of the song makes that scene answer ({} -> {}, chosen by the person)",
                          target->composition, after != nullptr ? after->composition : std::string("-")));
    }

    // A group effect over figures asked different things, rendered three ways: alone (as drawn on
    // the group), split per figure (phase 21), and whole with the members masked (phase 22).
    void TestGroupFidelity() {
        Title("Phase 22: a group effect keeps its drawing while some of its figures rest");
        const TargetInfo* group = _caps.Find("Grupo Todo");
        std::vector<std::string> members;
        for (const std::string& m : group != nullptr ? group->flatModels : std::vector<std::string>()) {
            const StageFigure* f = _space.Find(m);
            if (f != nullptr && f->placed && _geometry.Find(m) != nullptr) {
                members.push_back(m);
            }
        }
        if (members.size() < 3) {
            Check(false, "a group of three figures or more");
            return;
        }
        const std::string resting = members[0], glowing = members[1];
        GlobalStageScore score;
        score.art = static_cast<int>(ArtLevel::Calibrated);
        score.ambientLevel = 0.4;
        score.durationMS = 8000;
        StageScene sc;
        sc.id = "g";
        sc.startMS = 0;
        sc.endMS = 8000;
        for (const StageFigure& f : _space.figures) {
            if (_geometry.Find(f.name) == nullptr) {
                continue;
            }
            const std::string policy = f.name == resting ? "suspended" : f.name == glowing ? "reduced" : "normal";
            sc.presence.push_back({ f.name, policy == "suspended" ? "rest" : policy == "reduced" ? "ambient" : "complement", policy, "" });
        }
        score.scenes.push_back(sc);
        Song song = MakeSong();
        StageDirectorInput in = Input(song);
        const auto render = [&](const char* effect, int variant, std::string& fseq, Choreography& c) {
            c = Choreography();
            c.durationMS = 8000;
            c.frameMS = FRAME;
            ChoreographyEvent e;
            e.id = "group";
            e.target = "Grupo Todo";
            e.type = TargetType::Group;
            e.effect = effect;
            e.intent = "accompany";
            e.startMS = 1000;
            e.endMS = 7000;
            e.colors = { "#FF8020", "#2080FF" };
            c.events.push_back(e);
            if (variant > 0) {
                GlobalStageScore s = score;
                in.groupPresence = variant == 2 ? 1 : 0;
                (void)ApplyStagePresence(s, in, c);
            }
            SequenceSpec spec = ToSequenceSpec(c, {});
            spec.durationMS = 8000;
            (void)PutGroupsUnderStage(spec, c, _caps);
            return Render(spec, fmt::format("grupo-{}-{}", effect, variant), fseq);
        };
        for (const char* effect : { "Shockwave", "Bars", "On" }) {
            std::string f0, f1, f2;
            Choreography c0, c1, c2;
            const bool ok = render(effect, 0, f0, c0) && render(effect, 1, f1, c1) && render(effect, 2, f2, c2);
            Frames a(f0), b(f1), m(f2);
            if (!ok || !a.Valid() || !b.Valid() || !m.Valid()) {
                Check(false, fmt::format("{}: the three versions rendered", effect));
                continue;
            }
            bool groupKept = false;
            int masks = 0;
            for (const ChoreographyEvent& e : c2.events) {
                groupKept = groupKept || (e.target == "Grupo Todo" && e.effect == effect);
                masks += e.intent == "presence-mask" ? 1 : 0;
            }
            // Light by light against the group's own drawing.
            long sameSplit = 0, sameMask = 0, total = 0;
            double restSplit = 0, restMask = 0, glowRatio = 0;
            int glowN = 0;
            for (int f = 1000 / FRAME + 2; f < 7000 / FRAME - 2; ++f) {
                for (const std::string& name : members) {
                    for (const FigureNode& n : _geometry.Find(name)->nodeMap) {
                        const auto o = a.Light(f, n.channel), s = b.Light(f, n.channel), k = m.Light(f, n.channel);
                        if (name == resting) {
                            restSplit = std::max(restSplit, static_cast<double>(std::max({ s[0], s[1], s[2] })));
                            restMask = std::max(restMask, static_cast<double>(std::max({ k[0], k[1], k[2] })));
                        } else if (name == glowing) {
                            const int lit = std::max({ o[0], o[1], o[2] });
                            if (lit > 40) {
                                glowRatio += static_cast<double>(std::max({ k[0], k[1], k[2] })) / lit;
                                ++glowN;
                            }
                        } else {
                            ++total;
                            sameSplit += std::abs(o[0] - s[0]) + std::abs(o[1] - s[1]) + std::abs(o[2] - s[2]) <= 3 ? 1 : 0;
                            sameMask += std::abs(o[0] - k[0]) + std::abs(o[1] - k[1]) + std::abs(o[2] - k[2]) <= 3 ? 1 : 0;
                        }
                    }
                }
            }
            const double keptMask = total > 0 ? 100.0 * sameMask / total : 0;
            const double keptSplit = total > 0 ? 100.0 * sameSplit / total : 0;
            printf("    %s: the playing figures' lights as the group drew them: split %.1f%%, masked %.1f%%; resting figure max %0.f / %.0f; glow %.2f of the group's light\n", effect, keptSplit,
                   keptMask, restSplit, restMask, glowN > 0 ? glowRatio / glowN : 0.0);
            Check(groupKept && masks >= 2, fmt::format("{}: the group effect stays whole on the group; {} masks on the figures that rest or glow", effect, masks));
            Check(keptMask >= 99.0, fmt::format("{}: the figures that play show exactly the group's drawing ({:.1f}% of their lights the same; split per figure: {:.1f}%)", effect, keptMask, keptSplit));
            Check(restMask == 0 && restSplit == 0, fmt::format("{}: the resting figure stays dark, masked as split", effect));
            if (glowN > 0) {
                Check(std::abs(glowRatio / glowN - 0.4) < 0.08, fmt::format("{}: the glowing figure shows the group's light at {:.0f}%", effect, 100 * glowRatio / glowN));
            } else {
                int added = 0;
                for (int f = 1000 / FRAME + 2; f < 7000 / FRAME - 2; ++f) {
                    for (const FigureNode& n : _geometry.Find(glowing)->nodeMap) {
                        const auto k = m.Light(f, n.channel);
                        added = std::max({ added, k[0], k[1], k[2] });
                    }
                }
                Check(added == 0, fmt::format("{}: the group's drawing never reaches the glowing figure here: nothing to dim, and nothing added", effect));
            }
        }
        // A face on the resting figure stays over its mask.
        Choreography c;
        c.durationMS = 8000;
        c.frameMS = FRAME;
        ChoreographyEvent g;
        g.id = "group";
        g.target = "Grupo Todo";
        g.type = TargetType::Group;
        g.effect = "On";
        g.startMS = 1000;
        g.endMS = 7000;
        c.events.push_back(g);
        ChoreographyEvent face = g;
        face.id = "face";
        face.target = resting;
        face.type = TargetType::Model;
        face.effect = "Faces";
        face.intent = "face-sing";
        c.events.push_back(face);
        ChoreographyEvent locked = g;
        locked.id = "locked";
        locked.target = glowing;
        locked.type = TargetType::Model;
        locked.state = DecisionState::Locked;
        locked.layer = 2;
        c.events.push_back(locked);
        GlobalStageScore s = score;
        in.groupPresence = 1;
        (void)ApplyStagePresence(s, in, c);
        int faceLayer = -1, maskLayer = -1;
        bool lockKept = false;
        for (const ChoreographyEvent& e : c.events) {
            faceLayer = e.id == "face" ? e.layer : faceLayer;
            maskLayer = e.intent == "presence-mask" && e.target == resting ? e.layer : maskLayer;
            lockKept = lockKept || (e.id == "locked" && e.layer == 2 && e.startMS == 1000 && e.endMS == 7000);
        }
        Check(faceLayer >= 0 && maskLayer > faceLayer, "a face on a resting figure stays over its mask: it still sings");
        Check(lockKept, "an effect a person locked is left as it was");
        Check(s.groupPresence == "mask" && s.groupsMasked >= 1 && s.groupsSplit == 0, "the score says the group was kept whole and masked, not split");
    }

    // Two figures wired to the same lights: light by light.
    void TestSharedOwnership() {
        Title("Phase 22: figures that share lights, light by light");
        std::vector<std::string> names;
        for (const StageFigure& f : _space.figures) {
            const FigureGeometry* g = _geometry.Find(f.name);
            if (g != nullptr && f.placed && g->nodeMap.size() >= 20) {
                names.push_back(f.name);
            }
        }
        std::sort(names.begin(), names.end());
        if (names.size() < 2) {
            Check(false, "two figures with lights");
            return;
        }
        const std::string A = names[0], B = names[1];
        ModelCapabilities caps = _caps;
        FigureCatalog geometry = _geometry;
        FigureGeometry* ga = nullptr;
        FigureGeometry* gb = nullptr;
        for (FigureGeometry& g : geometry.figures) {
            ga = g.name == A ? &g : ga;
            gb = g.name == B ? &g : gb;
        }
        const int na = static_cast<int>(ga->nodeMap.size()), nb = static_cast<int>(gb->nodeMap.size());
        // B's first ten lights are A's last ten.
        for (int i = 0; i < na; ++i) {
            ga->nodeMap[static_cast<size_t>(i)].channel = 3 * i;
        }
        for (int i = 0; i < nb; ++i) {
            gb->nodeMap[static_cast<size_t>(i)].channel = 3 * (na - 10) + 3 * i;
        }
        for (TargetInfo& t : caps.targets) {
            if (t.name == A) {
                t.firstChannel = 0;
                t.lastChannel = static_cast<uint32_t>(3 * na - 1);
                t.channelsPerNode = 3;
            } else if (t.name == B) {
                t.firstChannel = static_cast<uint32_t>(3 * (na - 10));
                t.lastChannel = static_cast<uint32_t>(3 * (na - 10) + 3 * nb - 1);
                t.channelsPerNode = 3;
            }
        }
        std::vector<SharedLights> pairs = FindSharedLights(caps, { A, B }, &geometry);
        const SharedLights* pair = pairs.empty() ? nullptr : &pairs.front();
        Check(pair != nullptr && pair->relation == "partial" && pair->lights == 10 && pair->exclusiveA == na - 10 && pair->exclusiveB == nb - 10 &&
                  pair->nodesB == NodeRangesText({ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 }),
              fmt::format("found light by light: 10 lights in common, {} only {}'s and {} only {}'s, each numbered as its figure numbers them", na - 10, A, nb - 10, B));
        Check(pair != nullptr && pair->place == "apart" && pair->kind == "misconfigured", "the same channels drawn at two places of the layout: said to be a likely wiring mistake, nothing changed");
        // The same lights in the same place: a partial overlap, not a mistake.
        for (int i = 0; i < 10; ++i) {
            gb->nodeMap[static_cast<size_t>(i)].wx = ga->nodeMap[static_cast<size_t>(na - 10 + i)].wx;
            gb->nodeMap[static_cast<size_t>(i)].wy = ga->nodeMap[static_cast<size_t>(na - 10 + i)].wy;
            gb->nodeMap[static_cast<size_t>(i)].placed = ga->nodeMap[static_cast<size_t>(na - 10 + i)].placed = true;
        }
        pairs = FindSharedLights(caps, { A, B }, &geometry);
        Check(!pairs.empty() && pairs.front().place == "same" && pairs.front().kind == "partial", "shared lights in the same place: a partial overlap");
        // A person says A wins: B keeps its own lights and gives way only on the shared ones.
        GlobalStageScore score;
        score.art = static_cast<int>(ArtLevel::Calibrated);
        StageScene sc;
        sc.id = "x";
        sc.startMS = 0;
        sc.endMS = 6000;
        sc.presence = { { A, "complement", "normal", "" }, { B, "complement", "normal", "" } };
        score.scenes.push_back(sc);
        StageEdits edits;
        edits.edits.push_back({ "priority", -1, -1, A + "|" + B, "" });
        Song song = MakeSong();
        StageDirectorInput in = Input(song);
        in.capabilities = &caps;
        in.geometry = &geometry;
        in.edits = &edits;
        Choreography c;
        c.frameMS = FRAME;
        c.durationMS = 6000;
        for (const std::string& f : { A, B }) {
            ChoreographyEvent e;
            e.id = "own/" + f;
            e.target = f;
            e.effect = "On";
            e.startMS = 0;
            e.endMS = 6000;
            c.events.push_back(e);
        }
        GlobalStageScore owned = score;
        (void)ApplyStagePresence(owned, in, c);
        const ChoreographyEvent* mask = nullptr;
        bool bPlays = false;
        for (const ChoreographyEvent& e : c.events) {
            mask = e.intent == "shared-mask" ? &e : mask;
            bPlays = bPlays || (e.id == "own/" + B);
        }
        std::string only;
        if (mask != nullptr) {
            FieldSpec f;
            std::string error;
            if (FromText(mask->settings[0].second, f, error)) {
                only = f.only;
            }
        }
        Check(mask != nullptr && mask->target == B && mask->layer == 0 && only == pairs.front().nodesB && bPlays && owned.scenes[0].PresenceOf(B)->policy == "normal",
              "the figure that gives way keeps its own lights, masked only on the ten it shares, over everything it does");
        Check(!owned.sharedLights.empty() && owned.sharedLights.front().scenesOwned == 1 && owned.sharedLights.front().prioritySource == "person", "the score says where the shared lights follow the person's choice");
        SequenceSpec spec = ToSequenceSpec(c, {});
        std::stable_sort(spec.effects.begin(), spec.effects.end(), [&](const EffectSpec& x, const EffectSpec& y) { return (x.target == A) > (y.target == A); }); // A drawn first
        const int moved = OrderSharedLights(spec, owned);
        size_t rowA = 0, rowB = 0;
        for (size_t i = spec.effects.size(); i-- > 0;) {
            rowA = spec.effects[i].target == A ? i : rowA;
            rowB = spec.effects[i].target == B ? i : rowB;
        }
        Check(moved == 1 && rowB < rowA, "its row is drawn before the winner's: on the shared lights the winner's light shows");
        // Over a face that sings, the priority gives way instead.
        Choreography withFace = c;
        withFace.events.clear();
        for (const std::string& f : { A, B }) {
            ChoreographyEvent e;
            e.id = "own/" + f;
            e.target = f;
            e.effect = "On";
            e.startMS = 0;
            e.endMS = 6000;
            withFace.events.push_back(e);
        }
        ChoreographyEvent face = withFace.events.back();
        face.id = "face";
        face.effect = "Faces";
        face.intent = "face-sing";
        face.layer = 1;
        withFace.events.push_back(face);
        GlobalStageScore faced = score;
        (void)ApplyStagePresence(faced, in, withFace);
        bool anyMask = false;
        for (const ChoreographyEvent& e : withFace.events) {
            anyMask = anyMask || e.intent == "shared-mask";
        }
        Check(!anyMask && !faced.sharedLights.empty() && faced.sharedLights.front().note.find("face") != std::string::npos, "a priority that would cover a face that sings is not applied there, and it says so");
        // A person confirms the pair is meant so.
        edits.edits.push_back({ "shared-ok", -1, -1, B + "|" + A, "" });
        GlobalStageScore ok = score;
        Choreography none;
        none.frameMS = FRAME;
        (void)ApplyStagePresence(ok, in, none);
        Check(!ok.sharedLights.empty() && ok.sharedLights.front().confirmed, "a person can confirm two figures share lights on purpose; it is kept");
    }

    void TestRealize(const Song& song, const GlobalStageScore& score) {
        Title("The score as native effects: rendered, every figure on time");
        StageDirectorInput in = Input(song);
        Choreography c;
        c.durationMS = 24000;
        const auto event = [&](const char* id, const char* target, Prominence p, const char* intent, int a, int b) {
            ChoreographyEvent e;
            e.id = id;
            e.target = target;
            e.prominence = p;
            e.intent = intent;
            e.startMS = a;
            e.endMS = b;
            e.effect = "On";
            e.colors = { "#2040FF" };
            e.segmentId = "x";
            c.events.push_back(e);
        };
        // The figures that rest in the narration, each with a background effect over it.
        const StageScene* narration = SceneOf(score, "s3.0");
        std::string resting;
        // A resting figure that none of the measured gestures uses, so locking it changes nothing measured.
        const auto measured = [&](const std::string& f) {
            for (const char* id : { "propagated-wave@4000", "cascade@8000" }) {
                if (const GlobalGesture* g = score.Gesture(id)) {
                    for (const GestureParticipant& p : g->participants) {
                        if (p.figure == f) {
                            return true;
                        }
                    }
                }
            }
            return false;
        };
        for (const SceneRole& r : narration->roles) {
            if (r.role == "rest" && _space.Find(r.figure) != nullptr && !measured(r.figure)) {
                resting = r.figure;
                break;
            }
        }
        event("bed", resting.c_str(), Prominence::Support, "accompany", 10000, 16000);
        event("kept", resting.c_str(), Prominence::Support, "accompany", 12000, 14000);
        c.events.back().state = DecisionState::Locked;
        c.events.back().layer = 3;
        event("face", "Arbol Cara", Prominence::Lead, "face-sing", 11000, 15000);
        in.choreography = &c;
        const std::vector<std::string> notes = RealizeStage(score, in, c);
        for (const std::string& n : notes) {
            printf("    %s\n", n.c_str());
        }
        bool cut = !resting.empty(), faceKept = false, specsValid = true, mouthSafe = true, lockKept = false;
        int stageEvents = 0;
        const FigureProfile* face = _semantics.Find("Arbol Cara");
        const FigureRegion* mouth = face != nullptr ? face->Region("mouth") : nullptr;
        for (const ChoreographyEvent& e : c.events) {
            if (e.target == resting && e.provenance.empty() && e.state != DecisionState::Locked) {
                cut = cut && (e.endMS <= 11000 || e.startMS >= 15000);
            }
            lockKept = lockKept || (e.id == "kept" && e.startMS == 12000 && e.endMS == 14000 && e.layer == 3);
            faceKept = faceKept || (e.id == "face" && e.startMS == 11000 && e.endMS == 15000 && e.layer > 0);
            if (e.provenance.rfind("stage:", 0) == 0) {
                ++stageEvents;
                FieldSpec s;
                std::string error;
                const std::string text = e.settings.empty() ? "" : e.settings[0].second;
                const bool valid = FromText(text, s, error) && ValidateField(s, error);
                if (!valid) {
                    printf("    %s: %s\n      %s\n", e.id.c_str(), error.c_str(), text.c_str());
                }
                specsValid = specsValid && valid;
                if (e.target == "Arbol Cara" && e.intent == "stage-gesture" && mouth != nullptr) {
                    const std::vector<char> skip = LightSet(s.skip, 600, false);
                    for (int n : mouth->nodes) {
                        mouthSafe = mouthSafe && skip[static_cast<size_t>(n)];
                    }
                }
            }
        }
        bool overlap = false;
        for (size_t i = 0; i < c.events.size(); ++i) {
            for (size_t j = i + 1; j < c.events.size(); ++j) {
                const ChoreographyEvent& a = c.events[i];
                const ChoreographyEvent& b = c.events[j];
                overlap = overlap || (a.target == b.target && a.layer == b.layer && a.startMS < b.endMS && b.startMS < a.endMS);
            }
        }
        Check(stageEvents > 0 && specsValid, fmt::format("{} Stage Field effects, every one a valid field", stageEvents));
        Check(cut, fmt::format("a resting figure's background stops for the narration ({})", resting));
        Check(faceKept, "the narrator's face stays, one layer down to make room");
        Check(lockKept, "an effect a person locked is left exactly as it was, even on a figure that rests");
        Check(mouthSafe, "no gesture lights the mouth of a figure with a face");
        Check(!overlap, "no two effects share a layer of a figure at the same time");
        SequenceSpec spec;
        spec.frameMS = FRAME;
        spec.durationMS = 24000;
        for (const ChoreographyEvent& e : c.events) {
            if (e.provenance.rfind("stage:", 0) != 0) {
                continue; // only the score's own effects: what is measured is the director's
            }
            EffectSpec x;
            x.target = e.target;
            x.layer = e.layer;
            x.effect = e.effect;
            x.settings = e.settings;
            x.palette = e.PaletteString();
            x.startMS = e.startMS;
            x.endMS = e.endMS;
            x.kind = TargetKind::Model;
            spec.effects.push_back(x);
        }
        // A group's effect under the wave: where a figure's field is dark, the group shows.
        EffectSpec group;
        group.target = "Grupo Todo";
        group.effect = "On";
        group.palette = RED;
        group.startMS = 3000;
        group.endMS = 7000;
        group.kind = TargetKind::Group;
        spec.effects.push_back(group);
        Check(PutGroupsUnderStage(spec, c, _caps) >= 1 && spec.effects.front().target == "Grupo Todo", "the group holding figures with gestures is drawn before them");
        std::string fseq;
        Check(Render(spec, "partitura", fseq), "the score written as a native sequence and rendered");
        Frames frames(fseq);
        if (!frames.Valid()) {
            return;
        }
        // When does each figure first light inside a window?
        const auto firstLit = [&](const std::string& figure, int fromMS, int toMS) {
            const FigureGeometry* g = _geometry.Find(figure);
            for (int f = fromMS / FRAME; f < toMS / FRAME && f < frames.Count(); ++f) {
                for (const FigureNode& n : g->nodeMap) {
                    const auto c3 = frames.Light(f, n.channel);
                    if (std::max(c3[1], c3[2]) > 40) { // the gestures are white; the group under them red
                        return f * FRAME;
                    }
                }
            }
            return -1;
        };
        int worst = 0, on = 0, total = 0;
        std::string detail;
        for (const char* id : { "propagated-wave@4000", "cascade@8000" }) {
            const GlobalGesture* g = score.Gesture(id);
            if (g == nullptr) {
                continue;
            }
            for (const GestureParticipant& p : g->participants) {
                const int expected = g->startMS + p.delayMS;
                const int lit = firstLit(p.figure, g->startMS - 500, g->endMS + 400);
                ++total;
                const int error = lit < 0 ? 100000 : std::abs(lit - expected);
                worst = std::max(worst, error);
                on += error <= 75 ? 1 : 0;
                detail += fmt::format("{}{} {}ms", detail.empty() ? "" : ", ", p.figure, lit < 0 ? -1 : lit - expected);
            }
        }
        printf("    lit minus expected: %s\n", detail.c_str());
        // The tree's own wave effect has started but the front is not there yet: the group's red.
        if (const GlobalGesture* w = score.Gesture("propagated-wave@4000")) {
            for (const GestureParticipant& p : w->participants) {
                if (p.figure != "Arbol Grande" && p.figure != "Matriz Prueba") {
                    continue;
                }
                const int effectStart = std::max(0, ((w->startMS + p.delayMS - 100) / FRAME) * FRAME);
                int red = 0, n = 0;
                for (const FigureNode& node : _geometry.Find(p.figure)->nodeMap) {
                    const auto c3 = frames.Light(effectStart / FRAME + 1, node.channel);
                    ++n;
                    red += c3[0] > 180 && c3[1] < 60 && c3[2] < 60 ? 1 : 0;
                }
                Check(n > 0 && red * 10 >= n * 9, fmt::format("{}: where the wave has not arrived, the group's effect shows through ({} of {} lights red)", p.figure, red, n));
            }
        }
        Check(total > 0 && on == total, fmt::format("each figure of the wave and the cascade lit at its moment, within 3 frames: {} of {} (worst {} ms)", on, total, worst));
        // Before the strong hit and in the rest, the score lights nothing.
        int early = 0;
        for (const StageFigure& f : _space.figures) {
            if (_geometry.Find(f.name) != nullptr && f.placed) {
                early += firstLit(f.name, 0, 3000) >= 0 ? 1 : 0;
            }
        }
        Check(early == 0, "in the rest at the start, nothing of the score lights");
    }

    fs::path _root;
    fs::path _show;
    std::unique_ptr<HeadlessRenderContext> _ctx;
    ModelCapabilities _caps;
    FigureCatalog _geometry;
    FigureSemantics _semantics;
    StageSpace _space;
    std::string _waveFseq;
    int _passed = 0;
    int _failed = 0;
};

} // namespace

int RunGroupFidelity(const std::string& showDir, const std::string& reportDir, const std::string& outDir) {
    std::string refused;
    if (!ControllersAreInactive(showDir, refused)) {
        printf("composer-group-fidelity: REFUSED: %s\n", refused.c_str());
        return 2;
    }
    HeadlessRenderContext ctx;
    if (!ctx.LoadShowFolder(showDir)) {
        printf("composer-group-fidelity: the show could not be loaded\n");
        return 1;
    }
    const ModelCapabilities caps = ExtractCapabilities(ctx, nullptr);
    const FigureCatalog geometry = DescribeFigures(ctx, caps, true);
    const StageSpace space = BuildStageSpace(geometry, nullptr);
    std::string text, error;
    GlobalStageScore score;
    Choreography chor;
    if (!ReadTextFile((fs::path(reportDir) / "stage-score.json").string(), text) || !FromJson(text, score, error) ||
        !ReadTextFile((fs::path(reportDir) / "choreography.json").string(), text) || !FromJson(text, chor, error)) {
        printf("composer-group-fidelity: the report could not be read: %s\n", error.c_str());
        return 1;
    }
    // The group effects the show kept whole with figures masked, one of each family.
    const std::map<std::string, std::string> family = { { "Shockwave", "radial" }, { "Pinwheel", "radial" }, { "Fan", "radial" }, { "Ripple", "radial" },
                                                         { "Bars", "movement" }, { "SingleStrand", "movement" }, { "Marquee", "movement" }, { "Wave", "movement" }, { "Spirals", "movement" },
                                                         { "On", "fill" }, { "Fill", "fill" }, { "Color Wash", "fill" }, { "Twinkle", "fill" }, { "Shimmer", "fill" } };
    // Of each family, the one with most figures that play while others rest or glow: where the
    // drawing has most to lose.
    std::map<std::string, const ChoreographyEvent*> chosen;
    std::map<std::string, int> chosenPlay;
    for (const ChoreographyEvent& e : chor.events) {
        bool masked = false;
        for (const Reason& r : e.reasons) {
            masked = masked || (r.code == "stage-presence" && r.detail == "group-kept-masked");
        }
        const auto f = family.find(e.effect);
        const StageScene* sc = score.SceneAt(e.startMS);
        const TargetInfo* g = caps.Find(e.target);
        if (!masked || f == family.end() || sc == nullptr || g == nullptr || e.endMS - e.startMS < 2000) {
            continue;
        }
        int play = 0, other = 0;
        for (const std::string& m : g->flatModels) {
            if (const FigurePresence* p = sc->PresenceOf(m); p != nullptr) {
                (p->policy == "normal" ? play : other) += 1;
            }
        }
        if (other > 0 && (!chosen.contains(f->second) || play > chosenPlay[f->second])) {
            chosen[f->second] = &e;
            chosenPlay[f->second] = play;
        }
    }
    std::error_code ec;
    fs::create_directories(outDir, ec);
    nlohmann::json report = nlohmann::json::array();
    int failures = 0;
    for (const auto& [kind, event] : chosen) {
        const StageScene* sc = score.SceneAt(event->startMS);
        if (sc == nullptr) {
            continue;
        }
        // One scene: the one the effect is in, its presence as the show had it.
        GlobalStageScore one = score;
        one.scenes = { *sc };
        one.gestures.clear();
        one.layers.clear();
        StageDirectorInput in;
        in.capabilities = &caps;
        in.geometry = &geometry;
        in.space = &space;
        const int from = std::max(event->startMS, sc->startMS), to = std::min(event->endMS, sc->endMS);
        std::vector<std::string> fseqs;
        Choreography versions[3];
        for (int v = 0; v < 3; ++v) {
            Choreography c;
            c.durationMS = to + 1000;
            c.frameMS = chor.frameMS;
            ChoreographyEvent e = *event;
            e.startMS = from;
            e.endMS = to;
            e.layer = 0;
            c.events.push_back(e);
            if (v > 0) {
                GlobalStageScore s = one;
                in.groupPresence = v == 2 ? 1 : 0;
                (void)ApplyStagePresence(s, in, c);
            }
            SequenceSpec spec = ToSequenceSpec(c, {});
            spec.durationMS = c.durationMS;
            spec.mediaFile.clear();
            (void)PutGroupsUnderStage(spec, c, caps);
            const std::string name = fmt::format("fidelidad-{}-{}.xsq", kind, v);
            const WriteResult written = WriteSequenceSafely(showDir, spec, name);
            const std::string fseq = (fs::path(outDir) / fmt::format("fidelidad-{}-{}.fseq", kind, v)).string();
            if (!written.ok || !ctx.OpenSequence(written.finalPath) || !ctx.RenderAndWait() || !ctx.WriteFseq(fseq)) {
                printf("composer-group-fidelity: %s version %d could not be rendered\n", kind.c_str(), v);
                ++failures;
                break;
            }
            (void)ctx.CloseSequence();
            fs::remove(written.finalPath, ec);
            fseqs.push_back(fseq);
            versions[v] = c;
        }
        if (fseqs.size() != 3) {
            continue;
        }
        Frames a(fseqs[0]), b(fseqs[1]), m(fseqs[2]);
        long total = 0, sameSplit = 0, sameMask = 0;
        int restSplit = 0, restMask = 0, play = 0, rest = 0, glow = 0;
        double glowRatio = 0;
        int glowN = 0;
        // The glowing figures' drawing: how alike the group's light and theirs are (Pearson), as a shape.
        std::vector<double> go, gs, gk;
        const TargetInfo* g = caps.Find(event->target);
        for (const std::string& name : g != nullptr ? g->flatModels : std::vector<std::string>()) {
            const FigureGeometry* fg = geometry.Find(name);
            const FigurePresence* p = sc->PresenceOf(name);
            if (fg == nullptr || p == nullptr) {
                continue;
            }
            const std::string pol = p->policy;
            play += pol == "normal" ? 1 : 0;
            rest += pol == "suspended" ? 1 : 0;
            glow += pol == "reduced" ? 1 : 0;
            for (int f = from / a.FrameMS() + 1; f < to / a.FrameMS() - 1 && f < a.Count(); ++f) {
                for (const FigureNode& n : fg->nodeMap) {
                    const auto o = a.Light(f, n.channel), s = b.Light(f, n.channel), k = m.Light(f, n.channel);
                    if (pol == "suspended") {
                        restSplit = std::max({ restSplit, s[0], s[1], s[2] });
                        restMask = std::max({ restMask, k[0], k[1], k[2] });
                    } else if (pol == "reduced") {
                        const int lit = std::max({ o[0], o[1], o[2] });
                        if (lit > 40) {
                            glowRatio += static_cast<double>(std::max({ k[0], k[1], k[2] })) / lit;
                            ++glowN;
                        }
                        go.push_back(o[0] + o[1] + o[2]);
                        gs.push_back(s[0] + s[1] + s[2]);
                        gk.push_back(k[0] + k[1] + k[2]);
                    } else {
                        ++total;
                        sameSplit += std::abs(o[0] - s[0]) + std::abs(o[1] - s[1]) + std::abs(o[2] - s[2]) <= 3 ? 1 : 0;
                        sameMask += std::abs(o[0] - k[0]) + std::abs(o[1] - k[1]) + std::abs(o[2] - k[2]) <= 3 ? 1 : 0;
                    }
                }
            }
        }
        const double keptSplit = total > 0 ? 100.0 * sameSplit / total : 0, keptMask = total > 0 ? 100.0 * sameMask / total : 0;
        const auto pearson = [](const std::vector<double>& x, const std::vector<double>& y) {
            if (x.size() < 2) {
                return -2.0;
            }
            double mx = 0, my = 0;
            for (size_t i = 0; i < x.size(); ++i) {
                mx += x[i] / x.size();
                my += y[i] / y.size();
            }
            double sxy = 0, sxx = 0, syy = 0;
            for (size_t i = 0; i < x.size(); ++i) {
                sxy += (x[i] - mx) * (y[i] - my);
                sxx += (x[i] - mx) * (x[i] - mx);
                syy += (y[i] - my) * (y[i] - my);
            }
            return sxx > 0 && syy > 0 ? sxy / std::sqrt(sxx * syy) : -2.0;
        };
        const double shapeSplit = pearson(go, gs), shapeMask = pearson(go, gk);
        // -2: one of the two never changes, so their drawings cannot be compared.
        const auto alike = [](double r) { return r < -1.5 ? std::string("n/a (no change to compare)") : fmt::format("{:.3f}", r); };
        const std::string inSplit = play > 0 ? fmt::format("{:.1f}%", keptSplit) : std::string("n/a (no figure plays)");
        const std::string inMask = play > 0 ? fmt::format("{:.1f}%", keptMask) : std::string("n/a (no figure plays)");
        printf("composer-group-fidelity: %s %s on %s, %.1f-%.1f s (scene %s): %d figures play, %d glow, %d rest; the playing figures' lights as the group drew them: split %s, masked %s; resting max %d split / %d masked; glow %.2f of the group's light, its drawing alike %s split / %s masked\n",
               kind.c_str(), event->effect.c_str(), event->target.c_str(), from / 1000.0, to / 1000.0, sc->id.c_str(), play, glow, rest, inSplit.c_str(), inMask.c_str(), restSplit, restMask,
               glowN > 0 ? glowRatio / glowN : -1.0, alike(shapeSplit).c_str(), alike(shapeMask).c_str());
        fflush(stdout);
        report.push_back({ { "kind", kind }, { "effect", event->effect }, { "group", event->target }, { "fromMS", from }, { "toMS", to }, { "scene", sc->id },
                           { "play", play }, { "glow", glow }, { "rest", rest }, { "keptSplit", keptSplit }, { "keptMasked", keptMask }, { "restLitSplit", restSplit },
                           { "restLitMasked", restMask }, { "glowRatio", glowN > 0 ? glowRatio / glowN : -1.0 }, { "lightFrames", total },
                           { "glowShapeSplit", shapeSplit }, { "glowShapeMasked", shapeMask } });
    }
    (void)WriteText(fs::path(outDir) / "group-fidelity.json", report.dump(1));
    printf("composer-group-fidelity: %zu group effects compared; nothing here was judged by a person\n", report.size());
    return failures == 0 && !report.empty() ? 0 : 1;
}

// Real groups of the layout under a controlled presence: in one scene, three figures of a group
// play, one glows and one rests, while an effect is drawn on the whole group. Rendered by xLights
// as the group's effect alone, split per figure and kept whole with masks, and compared light by
// light. A second case puts a singing face on a resting figure, a video on a playing one and a
// locked effect on a glowing one, and checks they still show exactly as they would alone.
// Groups are chosen by a rule (members with lights and a place), never by name.
int RunGroupFidelityControlled(const std::string& showDir, const std::string& outDir) {
    std::string refused;
    if (!ControllersAreInactive(showDir, refused)) {
        printf("composer-group-fidelity: REFUSED: %s\n", refused.c_str());
        return 2;
    }
    HeadlessRenderContext ctx;
    if (!ctx.LoadShowFolder(showDir)) {
        printf("composer-group-fidelity: the show could not be loaded\n");
        return 1;
    }
    const ModelCapabilities caps = ExtractCapabilities(ctx, nullptr);
    const FigureCatalog geometry = DescribeFigures(ctx, caps, true);
    const StageSpace space = BuildStageSpace(geometry, nullptr);
    const int frameMS = 25, from = 1000, to = 7000, length = 8000;
    // A group's figures with lights and a place, in the group's order.
    const auto usable = [&](const TargetInfo& g) {
        std::vector<std::string> out;
        for (const std::string& m : g.flatModels) {
            const StageFigure* f = space.Find(m);
            const FigureGeometry* fg = geometry.Find(m);
            if (f != nullptr && f->placed && fg != nullptr && fg->nodeMap.size() >= 8 && std::find(out.begin(), out.end(), m) == out.end()) {
                out.push_back(m);
            }
        }
        return out;
    };
    std::vector<const TargetInfo*> groups;
    for (const TargetInfo& t : caps.targets) {
        if (t.type == TargetType::Group) {
            groups.push_back(&t);
        }
    }
    std::sort(groups.begin(), groups.end(), [](const TargetInfo* a, const TargetInfo* b) { return a->name < b->name; });
    // Groups of five to eight such figures, each set of figures used once; groups with a figure
    // that shares lights with another left out (those are split on purpose, see the shared lights).
    std::set<std::string> sharing;
    std::vector<std::string> names;
    for (const StageFigure& f : space.figures) {
        names.push_back(f.name);
    }
    for (const SharedLights& s : FindSharedLights(caps, names, &geometry)) {
        sharing.insert(s.a);
        sharing.insert(s.b);
    }
    std::vector<std::pair<const TargetInfo*, std::vector<std::string>>> fives;
    std::set<std::set<std::string>> seen;
    for (const TargetInfo* g : groups) {
        const std::vector<std::string> m = usable(*g);
        const std::set<std::string> key(m.begin(), m.end());
        const bool shares = std::any_of(m.begin(), m.end(), [&](const std::string& x) { return sharing.contains(x); });
        if (m.size() >= 5 && m.size() <= 8 && !shares && !seen.contains(key)) {
            seen.insert(key);
            fives.push_back({ g, m });
        }
    }
    struct Case {
        std::string kind, effect;
        const TargetInfo* group = nullptr;
        std::vector<std::string> members;
        bool extras = false;
    };
    std::vector<Case> cases;
    const std::vector<std::pair<std::string, std::string>> wanted = { { "radial", "Shockwave" }, { "radial", "Pinwheel" }, { "movement", "Bars" }, { "movement", "Wave" } };
    for (const auto& [kind, effect] : wanted) {
        for (const auto& [g, m] : fives) {
            cases.push_back({ kind, effect, g, m, false });
        }
    }
    std::map<std::string, bool> proven; // effect -> shown, lit and moving, on at least one real group
    // The protected case: a group with a face, the face resting.
    for (const TargetInfo* g : groups) {
        std::vector<std::string> m = usable(*g);
        const auto face = std::find_if(m.begin(), m.end(), [&](const std::string& x) {
            const TargetInfo* t = caps.Find(x);
            return t != nullptr && !t->faces.empty();
        });
        if (m.size() >= 3 && face != m.end() && !std::any_of(m.begin(), m.end(), [&](const std::string& x) { return sharing.contains(x); })) {
            std::rotate(m.begin(), face, face + 1); // the face last: it rests
            std::rotate(m.begin(), m.begin() + 1, m.end());
            cases.push_back({ "protected", "Bars", g, m, true });
            break;
        }
    }
    std::string video;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fs::path(showDir) / "Videos", ec)) {
        // As xLights keeps it: relative to the show folder.
        const std::string rel = "Videos/" + entry.path().filename().string();
        if (entry.path().extension() == ".mp4" && (video.empty() || rel < video)) {
            video = rel;
        }
    }
    fs::create_directories(outDir, ec);
    nlohmann::json report = nlohmann::json::array();
    int failures = 0, checked = 0, passed = 0;
    const auto verdict = [&](bool ok, const std::string& what) {
        ++checked;
        passed += ok ? 1 : 0;
        printf("  %s %s\n", ok ? "OK    " : "FAILED", what.c_str());
    };
    for (size_t ci = 0; ci < cases.size(); ++ci) {
        const Case& cs = cases[ci];
        // Presence: the last member rests, the one before glows, the others play.
        const std::string resting = cs.members.back(), glowing = cs.members[cs.members.size() - 2];
        GlobalStageScore score;
        score.art = static_cast<int>(ArtLevel::Calibrated);
        score.ambientLevel = 0.4;
        score.durationMS = length;
        StageScene sc;
        sc.id = "controlled";
        sc.startMS = 0;
        sc.endMS = length;
        for (const StageFigure& f : space.figures) {
            const std::string policy = f.name == resting ? "suspended" : f.name == glowing ? "reduced" : "normal";
            sc.presence.push_back({ f.name, policy == "suspended" ? "rest" : policy == "reduced" ? "ambient" : "complement", policy, "controlled test" });
        }
        score.scenes.push_back(sc);
        StageDirectorInput in;
        in.capabilities = &caps;
        in.geometry = &geometry;
        in.space = &space;
        ChoreographyEvent group;
        group.id = "group";
        group.target = cs.group->name;
        group.type = TargetType::Group;
        group.effect = cs.effect;
        group.intent = "accompany";
        group.startMS = from;
        group.endMS = to;
        group.colors = { "#FF8020", "#2080FF" };
        // Clearly moving, so a drawing restarted on each figure would show.
        if (cs.effect == "Bars") {
            group.settings = { { "E_TEXTCTRL_Bars_Cycles", "3.0" }, { "E_SLIDER_Bars_BarCount", "2" } };
        } else if (cs.effect == "Shockwave") {
            group.settings = { { "E_SLIDER_Shockwave_Cycles", "3" }, { "E_SLIDER_Shockwave_Start_Radius", "1" }, { "E_SLIDER_Shockwave_End_Radius", "120" } };
        } else if (cs.effect == "Wave") {
            group.settings = { { "E_TEXTCTRL_Wave_Speed", "40.00" } };
        } else if (cs.effect == "Pinwheel") {
            group.settings = { { "E_SLIDER_Pinwheel_Speed", "25" }, { "E_SLIDER_Pinwheel_Arms", "6" }, { "E_SLIDER_Pinwheel_Thickness", "60" }, { "E_SLIDER_Pinwheel_ArmSize", "300" } };
        }
        std::vector<ChoreographyEvent> extras;
        if (cs.extras) {
            const TargetInfo* t = caps.Find(resting);
            ChoreographyEvent face;
            face.id = "face";
            face.target = resting;
            face.type = TargetType::Model;
            face.effect = "Faces";
            face.intent = "face-sing";
            face.startMS = from;
            face.endMS = to;
            face.layer = 1;
            face.colors = { "#FFFFFF" };
            face.settings = { { "E_CHOICE_Faces_FaceDefinition", t->faces.front() }, { "E_CHOICE_Faces_TimingTrack", "Letra de prueba" } };
            extras.push_back(face);
            if (!video.empty()) {
                ChoreographyEvent v = face;
                v.id = "video";
                v.target = cs.members.front();
                v.effect = "Video";
                v.intent = "media";
                v.settings = { { "E_FILEPICKERCTRL_Video_Filename", video } };
                extras.push_back(v);
            }
            ChoreographyEvent locked = face;
            locked.id = "locked";
            locked.target = glowing;
            locked.effect = "On";
            locked.intent = "stage-identity";
            locked.state = DecisionState::Locked;
            locked.startMS = 2000;
            locked.endMS = 4000;
            locked.layer = 2;
            locked.colors = { "#20FF40" };
            locked.settings = {};
            extras.push_back(locked);
        }
        // The face sings a phrase of mouth shapes, as from a lyrics track.
        TimingTrackSpec lyrics;
        lyrics.name = "Letra de prueba";
        lyrics.layers.resize(3);
        lyrics.layers[0].push_back({ "la la la", from, to });
        const char* shapes[] = { "AI", "O", "E", "MBP", "U", "AI", "etc", "O", "E", "rest", "AI", "MBP" };
        for (int k = 0; k < 12; ++k) {
            const int a = from + k * 500, b = a + 500;
            if (k % 3 == 0) {
                lyrics.layers[1].push_back({ "la", a, std::min(to, a + 1500) });
            }
            lyrics.layers[2].push_back({ shapes[k], a, b });
        }
        // 0 the group alone, 1 split, 2 masked; with extras 3 masked with them, 4 the extras alone.
        const int count = cs.extras ? 5 : 3;
        std::vector<std::string> fseqs;
        std::vector<Choreography> versions;
        GlobalStageScore maskedScore;
        for (int v = 0; v < count; ++v) {
            Choreography c;
            c.durationMS = length;
            c.frameMS = frameMS;
            if (v != 4) {
                c.events.push_back(group);
            }
            if (v >= 3) {
                c.events.insert(c.events.end(), extras.begin(), extras.end());
            }
            if (v >= 1 && v <= 3) {
                GlobalStageScore s = score;
                in.groupPresence = v == 1 ? 0 : 1;
                (void)ApplyStagePresence(s, in, c);
                if (v == 2) {
                    maskedScore = s;
                }
            }
            SequenceSpec spec = ToSequenceSpec(c, cs.extras ? std::vector<TimingTrackSpec>{ lyrics } : std::vector<TimingTrackSpec>{});
            spec.durationMS = length;
            spec.mediaFile.clear();
            (void)PutGroupsUnderStage(spec, c, caps);
            const std::string name = fmt::format("controlada-{}-{}-{}-{}.xsq", ci, cs.kind, cs.effect, v);
            const WriteResult written = WriteSequenceSafely(showDir, spec, name);
            const std::string fseq = (fs::path(outDir) / fmt::format("controlada-{}-{}-{}-{}.fseq", ci, cs.kind, cs.effect, v)).string();
            if (!written.ok || !ctx.OpenSequence(written.finalPath) || !ctx.RenderAndWait() || !ctx.WriteFseq(fseq)) {
                printf("composer-group-fidelity: %s %s version %d could not be rendered\n", cs.kind.c_str(), cs.effect.c_str(), v);
                for (const ValidationIssue& i : written.validation.issues) {
                    if (i.severity == IssueSeverity::Error) {
                        std::string args;
                        for (const auto& [k, x] : i.args) {
                            args += " " + k + "=" + x;
                        }
                        printf("    %s %s %s%s\n", i.code.c_str(), i.model.c_str(), i.effect.c_str(), args.c_str());
                    }
                }
                ++failures;
                break;
            }
            (void)ctx.CloseSequence();
            fs::remove(written.finalPath, ec);
            fseqs.push_back(fseq);
            versions.push_back(c);
        }
        if (static_cast<int>(fseqs.size()) != count) {
            continue;
        }
        Frames a(fseqs[0]), b(fseqs[1]), m(fseqs[2]);
        const int f0 = from / frameMS + 2, f1 = to / frameMS - 2;
        long total = 0, sameSplit = 0, sameMask = 0, moving = 0, lit = 0;
        int restSplit = 0, restMask = 0;
        double glowRatio = 0;
        int glowN = 0;
        std::vector<double> go, gs, gk;
        nlohmann::json perFigure = nlohmann::json::array();
        for (const std::string& name : cs.members) {
            const FigureGeometry* fg = geometry.Find(name);
            const std::string pol = name == resting ? "suspended" : name == glowing ? "reduced" : "normal";
            long figTotal = 0, figSplit = 0, figMask = 0;
            for (int f = f0; f < f1 && f < a.Count(); ++f) {
                for (const FigureNode& n : fg->nodeMap) {
                    const auto o = a.Light(f, n.channel), s = b.Light(f, n.channel), k = m.Light(f, n.channel);
                    if (pol == "suspended") {
                        restSplit = std::max({ restSplit, s[0], s[1], s[2] });
                        restMask = std::max({ restMask, k[0], k[1], k[2] });
                    } else if (pol == "reduced") {
                        const int lit = std::max({ o[0], o[1], o[2] });
                        if (lit > 40) {
                            glowRatio += static_cast<double>(std::max({ k[0], k[1], k[2] })) / lit;
                            ++glowN;
                        }
                        go.push_back(o[0] + o[1] + o[2]);
                        gs.push_back(s[0] + s[1] + s[2]);
                        gk.push_back(k[0] + k[1] + k[2]);
                    } else {
                        ++total;
                        ++figTotal;
                        lit += std::max({ o[0], o[1], o[2] }) > 20 ? 1 : 0;
                        const bool sSame = std::abs(o[0] - s[0]) + std::abs(o[1] - s[1]) + std::abs(o[2] - s[2]) <= 3;
                        const bool kSame = std::abs(o[0] - k[0]) + std::abs(o[1] - k[1]) + std::abs(o[2] - k[2]) <= 3;
                        sameSplit += sSame ? 1 : 0;
                        sameMask += kSame ? 1 : 0;
                        figSplit += sSame ? 1 : 0;
                        figMask += kSame ? 1 : 0;
                        if (f >= f0 + 8) {
                            const auto p = a.Light(f - 8, n.channel); // 200 ms before
                            moving += std::abs(o[0] - p[0]) + std::abs(o[1] - p[1]) + std::abs(o[2] - p[2]) > 3 ? 1 : 0;
                        }
                    }
                }
            }
            perFigure.push_back({ { "policy", pol }, { "lights", fg->nodeMap.size() }, { "keptSplit", figTotal > 0 ? 100.0 * figSplit / figTotal : -1.0 },
                                  { "keptMasked", figTotal > 0 ? 100.0 * figMask / figTotal : -1.0 } });
        }
        const auto pearson = [](const std::vector<double>& x, const std::vector<double>& y) {
            if (x.size() < 2) {
                return -2.0;
            }
            double mx = 0, my = 0;
            for (size_t i = 0; i < x.size(); ++i) {
                mx += x[i] / x.size();
                my += y[i] / y.size();
            }
            double sxy = 0, sxx = 0, syy = 0;
            for (size_t i = 0; i < x.size(); ++i) {
                sxy += (x[i] - mx) * (y[i] - my);
                sxx += (x[i] - mx) * (x[i] - mx);
                syy += (y[i] - my) * (y[i] - my);
            }
            // Flat within rounding: two flat series have the same shape; one flat cannot be compared.
            const double flat = 1e-6 * static_cast<double>(x.size());
            if (sxx <= flat || syy <= flat) {
                return sxx <= flat && syy <= flat ? 1.0 : -2.0;
            }
            return sxy / std::sqrt(sxx * syy);
        };
        const double keptSplit = total > 0 ? 100.0 * sameSplit / total : 0, keptMask = total > 0 ? 100.0 * sameMask / total : 0;
        const double ratio = glowN > 0 ? glowRatio / glowN : -1.0, shapeSplit = pearson(go, gs), shapeMask = pearson(go, gk);
        double meanO = 0, meanK = 0;
        for (size_t i = 0; i < go.size(); ++i) {
            meanO += go[i] / go.size();
            meanK += gk[i] / gk.size();
        }
        printf("    the glowing figure: %zu light x frame samples, %d of them lit in the group's drawing; mean light %.1f in the drawing, %.1f masked\n", go.size(), glowN, meanO, meanK);
        int masks = 0;
        bool whole = false;
        for (const ChoreographyEvent& e : versions[2].events) {
            masks += e.intent == "presence-mask" ? 1 : 0;
            whole = whole || (e.target == cs.group->name && e.effect == cs.effect && e.startMS == from && e.endMS == to);
        }
        printf("composer-group-fidelity: controlled %s %s on the real group \"%s\" of %zu figures (%zu play, 1 glows, 1 rests): the playing figures' lights as the group drew them: split %.1f%%, masked %.1f%% (%.0f%% of them changing within 200 ms); resting max %d split / %d masked; glow %.2f of the group's light, its drawing alike %.3f split / %.3f masked\n",
               cs.kind.c_str(), cs.effect.c_str(), cs.group->name.c_str(), cs.members.size(), cs.members.size() - 2, keptSplit, keptMask, total > 0 ? 100.0 * moving / total : 0.0, restSplit, restMask, ratio, shapeSplit, shapeMask);
        verdict(whole && masks == 2 && maskedScore.groupsMasked >= 1 && maskedScore.groupsSplit == 0, fmt::format("{} {}: the group's effect stays whole, one mask on the resting figure and one on the glowing one", cs.kind, cs.effect));
        // Only a drawing that lights and moves on the playing figures can show it is not restarted.
        const double litShare = total > 0 ? 1.0 * lit / total : 0, movingShare = total > 0 ? 1.0 * moving / total : 0;
        const bool measurable = litShare >= 0.05 && movingShare >= 0.05;
        if (measurable) {
            verdict(keptMask >= 99.0, fmt::format("{} {}: the playing figures show the group's own drawing, not restarted per figure ({:.1f}%; split per figure {:.1f}%; {:.0f}% lit, {:.0f}% moving)", cs.kind, cs.effect,
                                                  keptMask, keptSplit, 100 * litShare, 100 * movingShare));
            proven[cs.effect] = proven[cs.effect] || keptMask >= 99.0;
        } else {
            printf("  (not measurable here: on these figures the drawing is %.0f%% lit and %.0f%% moving)\n", 100 * litShare, 100 * movingShare);
        }
        verdict(restMask == 0, fmt::format("{} {}: the resting figure stays dark (max {})", cs.kind, cs.effect, restMask));
        if (glowN > 0) {
            verdict(std::abs(ratio - 0.4) < 0.08 && shapeMask > 0.95, fmt::format("{} {}: the glowing figure keeps the drawing at {:.0f}% (alike {:.3f})", cs.kind, cs.effect, 100 * ratio, shapeMask));
        } else {
            verdict(true, fmt::format("{} {}: the group's drawing never reaches the glowing figure in this window: nothing to dim", cs.kind, cs.effect));
        }
        nlohmann::json row = { { "kind", cs.kind }, { "effect", cs.effect }, { "lit", litShare }, { "measurable", measurable }, { "group", cs.group->name }, { "members", cs.members }, { "resting", resting }, { "glowing", glowing }, { "figures", cs.members.size() }, { "keptSplit", keptSplit }, { "keptMasked", keptMask },
                               { "moving", total > 0 ? 1.0 * moving / total : 0.0 }, { "restLitSplit", restSplit }, { "restLitMasked", restMask }, { "glowRatio", ratio },
                               { "glowShapeSplit", shapeSplit }, { "glowShapeMasked", shapeMask }, { "masks", masks }, { "perFigure", perFigure } };
        if (cs.extras) {
            // The protected effects, against the same effects rendered alone.
            Frames with(fseqs[3]), alone(fseqs[4]);
            const auto shown = [&](const std::string& figure, int a0, int a1) {
                long lit = 0, same = 0;
                for (int f = a0 / frameMS + 2; f < a1 / frameMS - 2; ++f) {
                    for (const FigureNode& n : geometry.Find(figure)->nodeMap) {
                        const auto x = alone.Light(f, n.channel), y = with.Light(f, n.channel);
                        if (std::max({ x[0], x[1], x[2] }) > 20) {
                            ++lit;
                            same += std::abs(x[0] - y[0]) + std::abs(x[1] - y[1]) + std::abs(x[2] - y[2]) <= 3 ? 1 : 0;
                        }
                    }
                }
                return std::pair<long, long>(lit, same);
            };
            const auto [faceLit, faceSame] = shown(resting, from, to);
            verdict(faceLit > 0 && faceSame == faceLit, fmt::format("protected: the face on the resting figure sings as it would alone ({} of {} lights the same)", faceSame, faceLit));
            row["faceLit"] = faceLit;
            row["faceSame"] = faceSame;
            if (!video.empty()) {
                const auto [videoLit, videoSame] = shown(cs.members.front(), from, to);
                verdict(videoLit > 0 && videoSame == videoLit, fmt::format("protected: the video on a playing figure shows as it would alone ({} of {} lights the same)", videoSame, videoLit));
                row["videoLit"] = videoLit;
                row["videoSame"] = videoSame;
            } else {
                printf("  (no video in the show's Videos folder: the video case was not tried)\n");
            }
            const auto [lockLit, lockSame] = shown(glowing, 2000, 4000);
            bool lockKept = false;
            for (const ChoreographyEvent& e : versions[3].events) {
                lockKept = lockKept || (e.id == "locked" && e.layer == 2 && e.startMS == 2000 && e.endMS == 4000 && e.effect == "On" && e.state == DecisionState::Locked);
            }
            verdict(lockKept && lockLit > 0 && lockSame == lockLit, fmt::format("protected: the effect a person locked on the glowing figure is left as it was and shows as it would alone ({} of {} lights)", lockSame, lockLit));
            row["lockedLit"] = lockLit;
            row["lockedSame"] = lockSame;
        }
        std::vector<std::string> hashes;
        for (const std::string& f : fseqs) {
            hashes.push_back(Sha256OfFile(f).substr(0, 16));
        }
        row["fseqSha256"] = hashes;
        report.push_back(row);
        fflush(stdout);
    }
    for (const auto& [kind, effect] : wanted) {
        verdict(proven[effect], fmt::format("{}: shown lit and moving on at least one real group, never restarted per figure", effect));
    }
    (void)WriteText(fs::path(outDir) / "group-fidelity-controlled.json", report.dump(1));
    printf("composer-group-fidelity: controlled: %zu real groups, %d checks passed, %d failed; nothing here was judged by a person\n", report.size(), passed, checked - passed);
    return failures == 0 && checked > 0 && passed == checked && report.size() == cases.size() && cases.size() >= 5 ? 0 : 1;
}

int RunStageMeasure(const std::string& showDir, const std::string& fseqPath, const std::string& scorePath, const std::string& outDir) {
    std::string refused;
    if (!ControllersAreInactive(showDir, refused)) {
        printf("composer-stage-measure: REFUSED: %s\n", refused.c_str());
        return 2;
    }
    HeadlessRenderContext ctx;
    if (!ctx.LoadShowFolder(showDir)) {
        printf("composer-stage-measure: the show could not be loaded\n");
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
            printf("composer-stage-measure: score not read: %s\n", error.c_str());
            return 1;
        }
    }
    const StageCheck check = CheckStage(fseqPath, figures, space, hasScore ? &score : nullptr);
    std::error_code ec;
    fs::create_directories(fs::path(outDir), ec);
    const bool ok = WriteText(fs::path(outDir) / "stage-check.json", ToJson(check)) && WriteText(fs::path(outDir) / "stage-check.txt", StageCheckReport(check));
    printf("%s", StageCheckReport(check).c_str());
    return check.ran && ok ? 0 : 1;
}

int RunStageTest(const std::string& workDir) {
    Suite suite(fs::path(workDir) / fmt::format("stage-{}", std::chrono::system_clock::now().time_since_epoch().count()));
    return suite.Run();
}

} // namespace composer
