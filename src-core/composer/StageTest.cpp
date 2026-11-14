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
#include "composer/StageDirector.h"
#include "composer/StageSpace.h"
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

namespace fs = std::filesystem;

namespace composer {

namespace {

constexpr int FRAME = 25;
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
        Check(answer != nullptr && answer->participants.size() >= 2 && answer->participants[0].figure == "Arbol Cara" && answer->participants[1].delayMS == 500,
              "in the choir, the soloist calls and the others answer a beat (0.5 s) apart");
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

        TestRealize(song, score);
    }

    // The score into effects, rendered by xLights: each figure lights when the score says.
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
        for (const SceneRole& r : narration->roles) {
            if (r.role == "rest" && _space.Find(r.figure) != nullptr) {
                resting = r.figure;
                break;
            }
        }
        event("bed", resting.c_str(), Prominence::Support, "accompany", 10000, 16000);
        event("face", "Arbol Cara", Prominence::Lead, "face-sing", 11000, 15000);
        in.choreography = &c;
        const std::vector<std::string> notes = RealizeStage(score, in, c);
        for (const std::string& n : notes) {
            printf("    %s\n", n.c_str());
        }
        bool cut = !resting.empty(), faceKept = false, specsValid = true, mouthSafe = true;
        int stageEvents = 0;
        const FigureProfile* face = _semantics.Find("Arbol Cara");
        const FigureRegion* mouth = face != nullptr ? face->Region("mouth") : nullptr;
        for (const ChoreographyEvent& e : c.events) {
            if (e.target == resting && e.provenance.empty()) {
                cut = cut && (e.endMS <= 11000 || e.startMS >= 15000);
            }
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

int RunStageTest(const std::string& workDir) {
    Suite suite(fs::path(workDir) / fmt::format("stage-{}", std::chrono::system_clock::now().time_since_epoch().count()));
    return suite.Run();
}

} // namespace composer
