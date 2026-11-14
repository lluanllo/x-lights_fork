#include "composer/StageCheck.h"

#include "render/FSEQFile.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace composer {

namespace {

constexpr int LIT = 24;          // a light counts as lit above this on any channel
constexpr double RISE = 0.12;    // a gesture's arrival: its figure's light rises by this share of full

struct Measured {
    std::string name;
    std::vector<uint32_t> channels; // first channel of each light (3 a light)
};

} // namespace

StageCheck CheckStage(const std::string& fseqPath, const FigureCatalog& geometry, const StageSpace& space, const GlobalStageScore* score) {
    StageCheck check;
    std::unique_ptr<FSEQFile> file(FSEQFile::openFSEQFile(fseqPath));
    if (file == nullptr) {
        check.problem = "fseq-unreadable";
        return check;
    }
    const uint32_t channels = static_cast<uint32_t>(file->getMaxChannel());
    check.frameMS = std::max(1, static_cast<int>(file->getStepTime()));
    check.frames = static_cast<int>(file->getNumFrames());
    std::vector<Measured> figures;
    std::map<std::string, size_t> index;
    for (const StageFigure& f : space.figures) {
        const FigureGeometry* g = geometry.Find(f.name);
        if (!f.placed || f.noController || g == nullptr || g->nodeMap.empty()) {
            continue;
        }
        Measured m;
        m.name = f.name;
        for (const FigureNode& n : g->nodeMap) {
            if (n.channel >= 0 && static_cast<uint32_t>(n.channel) + 2 < channels) {
                m.channels.push_back(static_cast<uint32_t>(n.channel));
            }
        }
        if (!m.channels.empty()) {
            index[m.name] = figures.size();
            check.lights += static_cast<int>(m.channels.size());
            figures.push_back(std::move(m));
        }
    }
    check.figures = static_cast<int>(figures.size());
    if (figures.empty() || check.frames == 0) {
        check.problem = "nothing-to-measure";
        return check;
    }
    // Per frame and figure: the share of its lights lit, and its mean brightness (0-1).
    std::vector<std::vector<float>> share(static_cast<size_t>(check.frames), std::vector<float>(figures.size(), 0));
    std::vector<std::vector<float>> bright(static_cast<size_t>(check.frames), std::vector<float>(figures.size(), 0));
    file->prepareRead({ { 0, channels } });
    std::vector<uint8_t> data(channels, 0);
    for (int f = 0; f < check.frames; ++f) {
        std::fill(data.begin(), data.end(), 0);
        std::unique_ptr<FSEQFile::FrameData> frame(file->getFrame(static_cast<uint32_t>(f)));
        if (frame == nullptr || !frame->readFrame(data.data(), channels)) {
            continue;
        }
        for (size_t k = 0; k < figures.size(); ++k) {
            int lit = 0;
            double sum = 0;
            for (uint32_t c : figures[k].channels) {
                const int m = std::max({ data[c], data[c + 1], data[c + 2] });
                lit += m > LIT ? 1 : 0;
                sum += m;
            }
            share[f][k] = static_cast<float>(lit) / static_cast<float>(figures[k].channels.size());
            bright[f][k] = static_cast<float>(sum / (255.0 * figures[k].channels.size()));
        }
    }
    // The scenes: the score's, or ten-second stretches.
    std::vector<SceneLight> scenes;
    if (score != nullptr && !score->scenes.empty()) {
        for (const StageScene& s : score->scenes) {
            scenes.push_back({ s.id, StageModeId(s.mode), s.startMS, s.endMS });
        }
    } else {
        for (int a = 0; a < check.frames * check.frameMS; a += 10000) {
            scenes.push_back({ fmt::format("{}-{}", a, a + 10000), "", a, std::min(a + 10000, check.frames * check.frameMS) });
        }
    }
    double totalShare = 0, totalFigures = 0, totalFocus = 0;
    int counted = 0;
    for (SceneLight& s : scenes) {
        int n = 0;
        for (int f = s.startMS / check.frameMS; f < s.endMS / check.frameMS && f < check.frames; ++f) {
            int lights = 0, figuresLit = 0;
            double litLights = 0;
            std::vector<float> light;
            for (size_t k = 0; k < figures.size(); ++k) {
                const double litHere = share[f][k] * figures[k].channels.size();
                litLights += litHere;
                lights += static_cast<int>(figures[k].channels.size());
                figuresLit += share[f][k] >= 0.05F ? 1 : 0;
                light.push_back(bright[f][k] * static_cast<float>(figures[k].channels.size()));
            }
            std::sort(light.begin(), light.end(), std::greater<float>());
            double all = 0, top = 0;
            for (size_t k = 0; k < light.size(); ++k) {
                all += light[k];
                top += k < 3 ? light[k] : 0;
            }
            s.litShare += litLights / std::max(1, lights);
            s.figuresLit += figuresLit;
            s.focus += all > 0 ? top / all : 0;
            ++n;
        }
        if (n > 0) {
            s.litShare /= n;
            s.figuresLit /= n;
            s.focus /= n;
            totalShare += s.litShare * n;
            totalFigures += s.figuresLit * n;
            totalFocus += s.focus * n;
            counted += n;
        }
    }
    if (counted > 0) {
        check.litShare = totalShare / counted;
        check.figuresLit = totalFigures / counted;
        check.focus = totalFocus / counted;
    }
    double mean = 0;
    for (const SceneLight& s : scenes) {
        mean += s.litShare;
    }
    mean /= std::max<size_t>(1, scenes.size());
    double spread = 0;
    for (const SceneLight& s : scenes) {
        spread += (s.litShare - mean) * (s.litShare - mean);
    }
    check.sceneContrast = std::sqrt(spread / std::max<size_t>(1, scenes.size()));
    check.scenes = std::move(scenes);
    // Each gesture's figures: when their light rises against just before their part.
    if (score != nullptr) {
        for (const GlobalGesture& g : score->gestures) {
            for (const GestureParticipant& p : g.participants) {
                const auto it = index.find(p.figure);
                if (it == index.end()) {
                    continue;
                }
                GestureArrival a;
                a.gesture = g.id;
                a.figure = p.figure;
                a.expectedMS = g.startMS + p.delayMS;
                const int before = std::clamp((a.expectedMS - 150) / check.frameMS, 0, check.frames - 1);
                const float base = bright[before][it->second];
                for (int f = before + 1; f < check.frames && f * check.frameMS <= a.expectedMS + 400; ++f) {
                    if (bright[f][it->second] >= base + RISE) {
                        a.litMS = f * check.frameMS;
                        break;
                    }
                }
                a.errorMS = a.litMS < 0 ? 0 : a.litMS - a.expectedMS;
                if (a.litMS >= 0) {
                    ++check.arrivalsSeen;
                    check.arrivalsOnTime += std::abs(a.errorMS) <= 100 ? 1 : 0;
                    check.worstErrorMS = std::max(check.worstErrorMS, std::abs(a.errorMS));
                }
                check.arrivals.push_back(a);
            }
        }
        if (!check.arrivals.empty() && check.arrivalsSeen < static_cast<int>(check.arrivals.size())) {
            check.notes.push_back("arrivals-not-seen: a figure already bright under its other effects shows no clear rise; that is not a missed gesture");
        }
    }
    check.ran = true;
    return check;
}

std::string ToJson(const StageCheck& c) {
    using json = nlohmann::json;
    json scenes = json::array();
    for (const SceneLight& s : c.scenes) {
        scenes.push_back({ { "id", s.id }, { "mode", s.mode }, { "startMS", s.startMS }, { "endMS", s.endMS }, { "litShare", s.litShare }, { "figuresLit", s.figuresLit }, { "focus", s.focus } });
    }
    json arrivals = json::array();
    for (const GestureArrival& a : c.arrivals) {
        arrivals.push_back({ { "gesture", a.gesture }, { "figure", a.figure }, { "expectedMS", a.expectedMS }, { "litMS", a.litMS }, { "errorMS", a.errorMS } });
    }
    return json({ { "kind", "composer-stage-check" }, { "schemaVersion", c.schemaVersion }, { "ran", c.ran }, { "problem", c.problem }, { "frames", c.frames }, { "frameMS", c.frameMS },
                  { "figures", c.figures }, { "lights", c.lights }, { "litShare", c.litShare }, { "figuresLit", c.figuresLit }, { "focus", c.focus }, { "sceneContrast", c.sceneContrast },
                  { "scenes", scenes }, { "arrivals", arrivals }, { "arrivalsSeen", c.arrivalsSeen }, { "arrivalsOnTime", c.arrivalsOnTime }, { "worstErrorMS", c.worstErrorMS },
                  { "notes", c.notes }, { "review", "not-reviewed" } })
        .dump(1);
}

std::string StageCheckReport(const StageCheck& c) {
    if (!c.ran) {
        return "stage check not run: " + c.problem + "\n";
    }
    std::string out = fmt::format("{} figures, {} lights, {} frames of {} ms\n", c.figures, c.lights, c.frames, c.frameMS);
    out += fmt::format("lit: {:.1f}% of the lights on average, {:.1f} figures at once; the three brightest hold {:.0f}% of the light; scenes differ by {:.1f} points\n", c.litShare * 100,
                       c.figuresLit, c.focus * 100, c.sceneContrast * 100);
    if (!c.arrivals.empty()) {
        out += fmt::format("gestures: {} figure parts, {} seen rising, {} within 100 ms of the score (worst {} ms)\n", c.arrivals.size(), c.arrivalsSeen, c.arrivalsOnTime, c.worstErrorMS);
    }
    for (const SceneLight& s : c.scenes) {
        out += fmt::format("  {:>7.1f}-{:<7.1f} s {:<14} lit {:5.1f}%  figures {:5.1f}  focus {:3.0f}%\n", s.startMS / 1000.0, s.endMS / 1000.0, s.mode, s.litShare * 100, s.figuresLit, s.focus * 100);
    }
    for (const std::string& n : c.notes) {
        out += n + "\n";
    }
    out += "These are measurements of the render, not a judgement of the show.\n";
    return out;
}

} // namespace composer
