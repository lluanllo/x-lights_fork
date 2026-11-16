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
    int agreeing = 0, judged = 0;
    for (SceneLight& s : scenes) {
        // What the score asked of each figure here, and what its light did.
        const StageScene* asked = nullptr;
        if (score != nullptr) {
            for (const StageScene& sc : score->scenes) {
                if (sc.id == s.id) {
                    asked = &sc;
                }
            }
        }
        const int f0 = std::clamp(s.startMS / check.frameMS, 0, check.frames);
        const int f1 = std::clamp(s.endMS / check.frameMS, 0, check.frames);
        if (asked != nullptr && f1 > f0) {
            // Figures wired to the same lights: one lit shows on the other.
            std::map<std::string, std::vector<std::string>> partners;
            for (const std::string& l : score->limits) {
                const size_t bar = l.find('|');
                if (l.rfind("shares-lights:", 0) == 0 && bar != std::string::npos) {
                    const std::string a = l.substr(14, bar - 14), b = l.substr(bar + 1);
                    partners[a].push_back(b);
                    partners[b].push_back(a);
                }
            }
            const auto plays = [&](const std::string& f) {
                const FigurePresence* p = asked->PresenceOf(f);
                return p != nullptr ? p->policy != "suspended" : asked->RoleOf(f) != "rest";
            };
            double bp = 0, bg = 0, br = 0;
            for (size_t k = 0; k < figures.size(); ++k) {
                FigureActivity a;
                a.figure = figures[k].name;
                const FigurePresence* p = asked->PresenceOf(a.figure);
                if (p != nullptr) {
                    a.asked = p->policy == "suspended" ? "rest" : p->policy == "reduced" ? "glow" : "play";
                } else {
                    const std::string role = asked->RoleOf(a.figure);
                    a.asked = role == "rest" ? "rest" : role == "background" ? "glow" : "play";
                }
                int clearly = 0; // frames with a good part of its lights lit
                for (int f = f0; f < f1; ++f) {
                    a.lit += share[f][k];
                    a.bright += bright[f][k];
                    clearly += share[f][k] >= 0.3F ? 1 : 0;
                }
                a.lit /= (f1 - f0);
                a.bright /= (f1 - f0);
                // Lit: on average, or clearly for a quarter of a second (a short gesture is seen too).
                a.seen = a.lit >= 0.05 || clearly * check.frameMS >= 250 ? "play" : "dark";
                a.agrees = (a.asked == "rest") == (a.seen == "dark");
                s.seenLit += a.seen == "play" ? 1 : 0;
                if (a.asked == "play") {
                    ++s.askedPlay;
                    bp += a.bright;
                    s.playButDark += a.seen == "dark" ? 1 : 0;
                } else if (a.asked == "glow") {
                    ++s.askedGlow;
                    bg += a.bright;
                } else {
                    ++s.askedRest;
                    br += a.bright;
                    s.restButLit += a.seen == "play" ? 1 : 0;
                    if (a.seen == "play" && partners.contains(a.figure)) {
                        for (const std::string& o : partners.at(a.figure)) {
                            if (plays(o)) {
                                ++s.restLitByShared;
                                break;
                            }
                        }
                    }
                }
                agreeing += a.agrees ? 1 : 0;
                ++judged;
                s.agreement += a.agrees ? 1 : 0;
                s.activity.push_back(std::move(a));
            }
            s.agreement /= std::max<size_t>(1, figures.size());
            s.brightPlay = s.askedPlay > 0 ? bp / s.askedPlay : 0;
            s.brightGlow = s.askedGlow > 0 ? bg / s.askedGlow : 0;
            s.brightRest = s.askedRest > 0 ? br / s.askedRest : 0;
            check.restButLit += s.restButLit;
            check.restLitByShared += s.restLitByShared;
            check.playButDark += s.playButDark;
        }
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
    check.agreement = judged > 0 ? static_cast<double>(agreeing) / judged : 0;
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
        // Each gesture read whole: before, during and after it; in what order its figures lit;
        // whether the one that starts was first and the answers apart.
        for (const GlobalGesture& g : score->gestures) {
            GestureReading r;
            r.gesture = g.id;
            r.kind = GestureKindId(g.kind);
            r.relation = g.relation;
            r.presence = g.presence;
            std::vector<size_t> mine;
            std::vector<std::pair<int, int>> times; // asked, seen
            std::string source;
            for (const GestureParticipant& p : g.participants) {
                const auto it = index.find(p.figure);
                if (it == index.end()) {
                    continue;
                }
                mine.push_back(it->second);
                if (source.empty() && p.role == "source") {
                    source = p.figure;
                }
                for (const GestureArrival& a : check.arrivals) {
                    if (a.gesture == g.id && a.figure == p.figure && a.litMS >= 0) {
                        times.push_back({ a.expectedMS, a.litMS });
                    }
                }
            }
            r.participants = static_cast<int>(mine.size());
            r.seen = static_cast<int>(times.size());
            if (mine.empty()) {
                continue;
            }
            const auto at = [&](int ms) { return std::clamp(ms / check.frameMS, 0, check.frames - 1); };
            const auto mean = [&](const std::vector<size_t>& who, int a, int b) {
                double sum = 0;
                int n = 0;
                for (int f = at(a); f <= at(b); ++f) {
                    for (size_t k : who) {
                        sum += bright[f][k];
                        ++n;
                    }
                }
                return n > 0 ? sum / n : 0.0;
            };
            r.before = mean(mine, g.startMS - 1000, g.startMS - 100);
            double peak = 0;
            for (size_t k : mine) {
                float best = 0;
                for (int f = at(g.startMS); f <= at(g.endMS + 400); ++f) {
                    best = std::max(best, bright[f][k]);
                }
                peak += best;
            }
            r.peak = peak / mine.size();
            double strongest = -1;
            int peakFrame = at(g.startMS);
            for (int f = at(g.startMS); f <= at(g.endMS + 400); ++f) {
                double sum = 0;
                for (size_t k : mine) {
                    sum += bright[f][k];
                }
                if (sum > strongest) {
                    strongest = sum;
                    peakFrame = f;
                }
            }
            r.after = mean(mine, g.endMS + 200, g.endMS + 1200);
            r.rise = r.peak - r.before;
            std::vector<size_t> others;
            for (size_t k = 0; k < figures.size(); ++k) {
                if (std::find(mine.begin(), mine.end(), k) == mine.end()) {
                    others.push_back(k);
                }
            }
            if (!others.empty()) {
                r.houseChange = mean(others, g.startMS, g.endMS) - mean(others, g.startMS - 1000, g.startMS - 100);
            }
            r.contrastVsHouse = r.rise - r.houseChange;
            // Half way back down from the strongest moment.
            const double top = strongest / mine.size();
            const double half = r.before + (top - r.before) / 2;
            for (int f = peakFrame + 1; f < check.frames && f <= at(g.endMS + 3000); ++f) {
                double sum = 0;
                for (size_t k : mine) {
                    sum += bright[f][k];
                }
                if (sum / mine.size() <= half) {
                    r.decayMS = (f - peakFrame) * check.frameMS;
                    break;
                }
            }
            if (times.size() >= 2) {
                std::vector<std::pair<int, int>> byAsked = times;
                std::sort(byAsked.begin(), byAsked.end());
                r.expectedSpreadMS = byAsked.back().first - byAsked.front().first;
                std::vector<int> seenTimes;
                for (const auto& t : times) {
                    seenTimes.push_back(t.second);
                }
                std::sort(seenTimes.begin(), seenTimes.end());
                r.spreadMS = seenTimes.back() - seenTimes.front();
                for (size_t k = 1; k < seenTimes.size(); ++k) {
                    r.largestGapMS = std::max(r.largestGapMS, seenTimes[k] - seenTimes[k - 1]);
                }
                // Ranks (ties share the mean rank) and their correlation.
                const auto ranks = [](const std::vector<double>& v) {
                    std::vector<size_t> idx(v.size());
                    for (size_t i = 0; i < idx.size(); ++i) {
                        idx[i] = i;
                    }
                    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return v[a] < v[b]; });
                    std::vector<double> out(v.size());
                    for (size_t i = 0; i < idx.size();) {
                        size_t j = i;
                        while (j + 1 < idx.size() && v[idx[j + 1]] == v[idx[i]]) {
                            ++j;
                        }
                        for (size_t k = i; k <= j; ++k) {
                            out[idx[k]] = (i + j) / 2.0;
                        }
                        i = j + 1;
                    }
                    return out;
                };
                const auto correlation = [](const std::vector<double>& x, const std::vector<double>& y) {
                    const double n = static_cast<double>(x.size());
                    double mx = 0, my = 0;
                    for (size_t i = 0; i < x.size(); ++i) {
                        mx += x[i] / n;
                        my += y[i] / n;
                    }
                    double sxy = 0, sxx = 0, syy = 0;
                    for (size_t i = 0; i < x.size(); ++i) {
                        sxy += (x[i] - mx) * (y[i] - my);
                        sxx += (x[i] - mx) * (x[i] - mx);
                        syy += (y[i] - my) * (y[i] - my);
                    }
                    return sxx > 0 && syy > 0 ? sxy / std::sqrt(sxx * syy) : -2.0;
                };
                std::vector<double> a, b;
                for (const auto& t : times) {
                    a.push_back(t.first);
                    b.push_back(t.second);
                }
                if (r.expectedSpreadMS > 0) {
                    r.orderAgreement = correlation(ranks(a), ranks(b));
                    const double c = correlation(a, b);
                    r.timingFit = c < -1.5 ? -2 : c * c;
                }
            }
            if (!source.empty() && !times.empty()) {
                int sourceAt = -1, firstOther = 1 << 30;
                for (const GestureArrival& a : check.arrivals) {
                    if (a.gesture != g.id || a.litMS < 0) {
                        continue;
                    }
                    if (a.figure == source) {
                        sourceAt = a.litMS;
                    } else {
                        firstOther = std::min(firstOther, a.litMS);
                    }
                }
                r.firstActsFirst = sourceAt >= 0 && sourceAt <= firstOther;
            }
            if (g.relation == "answer" && times.size() >= 2) {
                std::vector<std::pair<int, int>> byAsked = times;
                std::sort(byAsked.begin(), byAsked.end());
                bool distinct = true;
                for (size_t k = 1; k < byAsked.size(); ++k) {
                    const int step = byAsked[k].first - byAsked[k - 1].first;
                    if (step > 0 && byAsked[k].second - byAsked[k - 1].second < step / 3) {
                        distinct = false;
                    }
                }
                r.answersDistinct = distinct;
            }
            check.readings.push_back(std::move(r));
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
        json sj = { { "id", s.id }, { "mode", s.mode }, { "startMS", s.startMS }, { "endMS", s.endMS }, { "litShare", s.litShare }, { "figuresLit", s.figuresLit }, { "focus", s.focus } };
        if (!s.activity.empty()) {
            sj["askedPlay"] = s.askedPlay;
            sj["askedGlow"] = s.askedGlow;
            sj["askedRest"] = s.askedRest;
            sj["seenLit"] = s.seenLit;
            sj["restButLit"] = s.restButLit;
            sj["restLitByShared"] = s.restLitByShared;
            sj["playButDark"] = s.playButDark;
            sj["agreement"] = s.agreement;
            sj["brightPlay"] = s.brightPlay;
            sj["brightGlow"] = s.brightGlow;
            sj["brightRest"] = s.brightRest;
            json act = json::array();
            for (const FigureActivity& a : s.activity) {
                act.push_back({ { "figure", a.figure }, { "asked", a.asked }, { "lit", a.lit }, { "bright", a.bright }, { "seen", a.seen }, { "agrees", a.agrees } });
            }
            sj["activity"] = std::move(act);
        }
        scenes.push_back(std::move(sj));
    }
    json arrivals = json::array();
    for (const GestureArrival& a : c.arrivals) {
        arrivals.push_back({ { "gesture", a.gesture }, { "figure", a.figure }, { "expectedMS", a.expectedMS }, { "litMS", a.litMS }, { "errorMS", a.errorMS } });
    }
    json readings = json::array();
    for (const GestureReading& r : c.readings) {
        readings.push_back({ { "gesture", r.gesture }, { "kind", r.kind }, { "relation", r.relation }, { "presence", r.presence }, { "participants", r.participants }, { "seen", r.seen },
                             { "before", r.before }, { "peak", r.peak }, { "after", r.after }, { "rise", r.rise }, { "houseChange", r.houseChange }, { "contrastVsHouse", r.contrastVsHouse },
                             { "orderAgreement", r.orderAgreement }, { "timingFit", r.timingFit }, { "spreadMS", r.spreadMS }, { "expectedSpreadMS", r.expectedSpreadMS },
                             { "largestGapMS", r.largestGapMS }, { "firstActsFirst", r.firstActsFirst }, { "answersDistinct", r.answersDistinct }, { "decayMS", r.decayMS } });
    }
    return json({ { "kind", "composer-stage-check" }, { "schemaVersion", c.schemaVersion }, { "ran", c.ran }, { "problem", c.problem }, { "frames", c.frames }, { "frameMS", c.frameMS },
                  { "agreement", c.agreement }, { "restButLit", c.restButLit }, { "restLitByShared", c.restLitByShared }, { "playButDark", c.playButDark }, { "readings", readings },
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
    if (c.agreement > 0) {
        out += fmt::format("score against render: {:.0f}% of figure x scene agree; {} asked to rest were lit ({} by a figure wired to the same lights), {} asked to play were dark\n", c.agreement * 100,
                           c.restButLit, c.restLitByShared, c.playButDark);
    }
    for (const SceneLight& s : c.scenes) {
        out += fmt::format("  {:>7.1f}-{:<7.1f} s {:<14} lit {:5.1f}%  figures {:5.1f}  focus {:3.0f}%", s.startMS / 1000.0, s.endMS / 1000.0, s.mode, s.litShare * 100, s.figuresLit, s.focus * 100);
        if (!s.activity.empty()) {
            out += fmt::format("  asked play {} glow {} rest {}; seen lit {} (rest lit {}, play dark {}); agree {:.0f}%", s.askedPlay, s.askedGlow, s.askedRest, s.seenLit, s.restButLit, s.playButDark,
                               s.agreement * 100);
        }
        out += "\n";
    }
    for (const std::string& n : c.notes) {
        out += n + "\n";
    }
    out += "These are measurements of the render, not a judgement of the show.\n";
    return out;
}

} // namespace composer
