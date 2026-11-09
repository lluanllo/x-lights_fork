#include "composer/RenderUnderstanding.h"

#include "render/FSEQFile.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>

#define COMPOSER_TEXT(text) text

using json = nlohmann::json;

namespace composer {

namespace {

// The lights a trace reads: the whole figure, then each region tied to real lights.
struct Probe {
    std::string figure;
    std::string region;
    std::vector<int> channels; // first channel of each light
    int perLight = 3;
};

std::vector<Probe> ProbesOf(const FigureCatalog& geometry, const FigureSemantics& semantics, const std::vector<std::string>& figures) {
    std::vector<Probe> probes;
    const std::set<std::string> wanted(figures.begin(), figures.end());
    for (const FigureGeometry& g : geometry.figures) {
        if (g.kind != "model" || g.nodeMap.empty() || (!wanted.empty() && !wanted.contains(g.name))) {
            continue;
        }
        std::map<int, int> channelOf;
        Probe whole{ g.name, "", {}, std::max(1, g.channelsPerNode) };
        for (const FigureNode& n : g.nodeMap) {
            if (n.channel >= 0) {
                channelOf[n.index] = n.channel;
                whole.channels.push_back(n.channel);
            }
        }
        if (whole.channels.empty()) {
            continue;
        }
        probes.push_back(whole);
        if (const FigureProfile* p = semantics.Find(g.name)) {
            for (const FigureRegion& r : p->regions) {
                if (!r.real || r.nodes.empty()) {
                    continue;
                }
                Probe region{ g.name, r.id, {}, whole.perLight };
                for (int node : r.nodes) {
                    if (auto it = channelOf.find(node); it != channelOf.end()) {
                        region.channels.push_back(it->second);
                    }
                }
                if (!region.channels.empty()) {
                    probes.push_back(std::move(region));
                }
            }
        }
    }
    return probes;
}

class TraceBuilder {
public:
    TraceBuilder(std::vector<Probe> probes, const RenderCheckOptions& options) : _probes(std::move(probes)), _o(options) {
        _lit.resize(_probes.size());
        _sum.resize(_probes.size());
    }

    // One frame; frames are folded into steps of `stepMS`.
    void Frame(const uint8_t* data, size_t size, int timeMS) {
        const int step = timeMS / std::max(1, _o.stepMS);
        if (step != _step) {
            Flush();
            _step = step;
        }
        for (size_t p = 0; p < _probes.size(); ++p) {
            const Probe& probe = _probes[p];
            Sum& s = _sum[p];
            std::vector<uint8_t>& lit = _lit[p];
            lit.resize(probe.channels.size(), 0);
            int changed = 0;
            for (size_t i = 0; i < probe.channels.size(); ++i) {
                const size_t c = static_cast<size_t>(probe.channels[i]);
                double r = 0, g = 0, b = 0;
                if (c + probe.perLight <= size) {
                    r = data[c];
                    g = probe.perLight >= 3 ? data[c + 1] : r;
                    b = probe.perLight >= 3 ? data[c + 2] : r;
                }
                const double v = std::max({ r, g, b }) / 255.0;
                s.level += v;
                s.r += r;
                s.g += g;
                s.b += b;
                const uint8_t on = v > 0.1 ? 1 : 0;
                changed += on != lit[i] ? 1 : 0;
                lit[i] = on;
            }
            s.motion = std::max(s.motion, static_cast<double>(changed) / std::max<size_t>(1, probe.channels.size()));
            s.lights += static_cast<double>(probe.channels.size());
        }
    }

    RenderTraces Finish(int durationMS) {
        Flush();
        RenderTraces t;
        t.stepMS = _o.stepMS;
        t.steps = (durationMS + _o.stepMS - 1) / std::max(1, _o.stepMS);
        for (size_t p = 0; p < _probes.size(); ++p) {
            const Probe& probe = _probes[p];
            FigureTrace* f = nullptr;
            for (FigureTrace& x : t.figures) {
                f = x.figure == probe.figure ? &x : f;
            }
            if (f == nullptr) {
                t.figures.push_back({ probe.figure, static_cast<int>(probe.channels.size()), {} });
                f = &t.figures.back();
            }
            RegionTrace r = _traces.size() > p ? _traces[p] : RegionTrace{};
            r.region = probe.region;
            r.level.resize(t.steps, 0);
            r.motion.resize(t.steps, 0);
            r.color.resize(t.steps, 0);
            f->regions.push_back(std::move(r));
        }
        return t;
    }

private:
    struct Sum {
        double level = 0, r = 0, g = 0, b = 0, lights = 0, motion = 0;
    };

    void Flush() {
        if (_step < 0) {
            return;
        }
        _traces.resize(_probes.size());
        for (size_t p = 0; p < _probes.size(); ++p) {
            RegionTrace& r = _traces[p];
            const Sum& s = _sum[p];
            const size_t at = static_cast<size_t>(_step);
            r.level.resize(std::max(r.level.size(), at + 1), 0);
            r.motion.resize(std::max(r.motion.size(), at + 1), 0);
            r.color.resize(std::max(r.color.size(), at + 1), 0);
            if (s.lights > 0) {
                r.level[at] = static_cast<float>(s.level / s.lights);
                r.motion[at] = static_cast<float>(s.motion);
                const auto channel = [&](double v) { return static_cast<uint32_t>(std::clamp(std::lround(v / s.lights), 0L, 255L)); };
                r.color[at] = (channel(s.r) << 16) | (channel(s.g) << 8) | channel(s.b);
            }
        }
        _sum.assign(_probes.size(), Sum{});
    }

    std::vector<Probe> _probes;
    RenderCheckOptions _o;
    std::vector<Sum> _sum;
    std::vector<std::vector<uint8_t>> _lit;
    std::vector<RegionTrace> _traces;
    int _step = -1;
};

float Mean(const std::vector<float>& v, int from, int to) {
    from = std::max(0, from);
    to = std::min(static_cast<int>(v.size()), to);
    if (to <= from) {
        return 0;
    }
    double sum = 0;
    for (int i = from; i < to; ++i) {
        sum += v[i];
    }
    return static_cast<float>(sum / (to - from));
}

bool Inside(const std::vector<std::pair<int, int>>& windows, int ms, int margin) {
    return std::any_of(windows.begin(), windows.end(), [&](const auto& w) { return ms >= w.first - margin && ms < w.second + margin; });
}

bool SpeaksFunction(const std::string& f) {
    return f == "speak" || f == "sing" || f == "choral-response";
}

} // namespace

const RegionTrace* FigureTrace::Region(const std::string& region) const {
    for (const RegionTrace& r : regions) {
        if (r.region == region) {
            return &r;
        }
    }
    return nullptr;
}

const FigureTrace* RenderTraces::Figure(const std::string& figure) const {
    for (const FigureTrace& f : figures) {
        if (f.figure == figure) {
            return &f;
        }
    }
    return nullptr;
}

bool ReadRenderTraces(const std::string& fseqPath, const FigureCatalog& geometry, const FigureSemantics& semantics, const std::vector<std::string>& figures,
                      const RenderCheckOptions& options, RenderTraces& traces, std::string& error, const RenderCheckCancel& cancel) {
    std::unique_ptr<FSEQFile> file(FSEQFile::openFSEQFile(fseqPath));
    if (file == nullptr) {
        error = "render-file-unreadable";
        return false;
    }
    const uint32_t channels = file->getMaxChannel();
    const int frameMS = std::max(1, file->getStepTime());
    const int frames = static_cast<int>(file->getNumFrames());
    file->prepareRead({ { 0, channels } });
    std::vector<Probe> probes = ProbesOf(geometry, semantics, figures);
    std::set<std::string> outside;
    for (const Probe& p : probes) {
        for (int c : p.channels) {
            if (static_cast<uint32_t>(c + p.perLight) > channels) {
                outside.insert(p.figure);
            }
        }
    }
    std::erase_if(probes, [&](const Probe& p) { return outside.contains(p.figure); });
    TraceBuilder builder(std::move(probes), options);
    std::vector<uint8_t> data(channels, 0);
    for (int f = 0; f < frames; ++f) {
        if (cancel && cancel()) {
            error = "cancelled";
            return false;
        }
        std::fill(data.begin(), data.end(), 0);
        std::unique_ptr<FSEQFile::FrameData> frame(file->getFrame(static_cast<uint32_t>(f)));
        if (frame == nullptr || !frame->readFrame(data.data(), channels)) {
            error = "render-file-unreadable";
            return false;
        }
        builder.Frame(data.data(), data.size(), f * frameMS);
    }
    traces = builder.Finish(frames * frameMS);
    traces.outside.assign(outside.begin(), outside.end());
    return true;
}

RenderTraces TracesFromFrames(const std::vector<std::vector<uint8_t>>& frames, int frameMS, const FigureCatalog& geometry, const FigureSemantics& semantics,
                              const std::vector<std::string>& figures, const RenderCheckOptions& options) {
    TraceBuilder builder(ProbesOf(geometry, semantics, figures), options);
    for (size_t f = 0; f < frames.size(); ++f) {
        builder.Frame(frames[f].data(), frames[f].size(), static_cast<int>(f) * frameMS);
    }
    return builder.Finish(static_cast<int>(frames.size()) * frameMS);
}

RenderUnderstanding CompareRender(const RenderTraces& traces, const RenderIntention& in, const RenderCheckOptions& o) {
    RenderUnderstanding r;
    r.stepMS = traces.stepMS;
    r.durationMS = traces.steps * traces.stepMS;
    r.limits = { "reads-light-not-pictures: it sees how much and which lights shine, not whether a picture or a text is the right one",
                 "mouth-motion-not-phonemes: it sees a mouth move, not whether it says the right sound",
                 "no-approval: a person decides; this only points at moments" };
    for (const std::string& f : traces.outside) {
        r.limits.push_back("outside-file:" + f);
    }
    const int step = std::max(1, traces.stepMS);
    const auto add = [&](RenderFinding f) {
        f.id = fmt::format("{}@{}@{}", f.kind, f.figure + (f.region.empty() ? "" : "." + f.region), f.startMS);
        r.findings.push_back(std::move(f));
    };
    std::map<std::string, std::vector<std::pair<int, int>>> speakingWindows; // figure -> when it should speak
    const bool full = in.performers != nullptr && in.audio != nullptr && in.voices != nullptr;
    if (full) {
        for (const PerformerAssignment& a : in.performers->assignments) {
            const auto windows = ActiveWindows(a, *in.audio, *in.voices);
            for (const std::string& figure : a.figures) {
                const FigureTrace* t = traces.Figure(figure);
                if (t == nullptr) {
                    continue;
                }
                const bool speaks = SpeaksFunction(a.function);
                const RegionTrace* region = t->Region(a.region.empty() || a.region == "face" ? "" : a.region);
                if (region == nullptr) {
                    region = t->Region("");
                }
                if (speaks) {
                    auto& w = speakingWindows[figure];
                    w.insert(w.end(), windows.begin(), windows.end());
                }
                IntentionCheck check{ a.source, figure, region->region, 0, 0 };
                for (const auto& [from, to] : windows) {
                    if (to - from < o.minWindowMS) {
                        continue;
                    }
                    ++check.windows;
                    const int s0 = from / step;
                    const int s1 = std::max(s0 + 1, to / step);
                    const float level = Mean(region->level, s0, s1);
                    const float motion = Mean(region->motion, s0, s1);
                    const bool shows = speaks && !region->region.empty() ? motion >= o.movingShare : level >= o.litLevel;
                    if (shows) {
                        ++check.honoured;
                        continue;
                    }
                    RenderFinding f;
                    f.kind = speaks && !region->region.empty() ? (level >= o.litLevel ? "mouth-still" : "expected-dark") : "expected-dark";
                    f.figure = figure;
                    f.region = region->region;
                    f.source = a.source;
                    f.startMS = from;
                    f.endMS = to;
                    f.severity = "warning";
                    f.measured = speaks && !region->region.empty() ? motion : level;
                    f.detail = fmt::format("{} {} {:.0f}%", a.sourceLabel, FunctionPhrase(a.function), f.measured * 100);
                    add(std::move(f));
                    // Was it shown on another figure instead?
                    for (const FigureTrace& other : traces.figures) {
                        if (other.figure == figure) {
                            continue;
                        }
                        const RegionTrace* mouth = other.Region("mouth");
                        if (speaks && mouth != nullptr && Mean(mouth->motion, s0, s1) >= o.movingShare * 2) {
                            RenderFinding v;
                            v.kind = "voice-on-other-figure";
                            v.figure = other.figure;
                            v.region = "mouth";
                            v.source = a.source;
                            v.startMS = from;
                            v.endMS = to;
                            v.severity = "check";
                            v.measured = Mean(mouth->motion, s0, s1);
                            v.detail = a.sourceLabel;
                            add(std::move(v));
                        }
                    }
                }
                r.checks.push_back(check);
            }
        }
    }

    // Mouths that move when nobody speaks through them, above all in instrumentals.
    for (const FigureTrace& t : traces.figures) {
        const RegionTrace* mouth = t.Region("mouth");
        if (mouth == nullptr) {
            continue;
        }
        const auto& windows = speakingWindows[t.figure];
        int runStart = -1;
        for (int s = 0; s <= traces.steps; ++s) {
            const int ms = s * step;
            const bool moving = s < traces.steps && mouth->motion[s] >= o.movingShare && !Inside(windows, ms, 300);
            if (moving && runStart < 0) {
                runStart = s;
            }
            if (!moving && runStart >= 0) {
                const int from = runStart * step;
                if (ms - from >= o.minWindowMS) {
                    const bool instrumental = in.audio != nullptr && in.audio->Has("instrumental", from, ms);
                    add({ "", "mouth-moves-without-voice", t.figure, "mouth", "", from, ms, instrumental ? "warning" : "check", Mean(mouth->motion, runStart, s),
                          instrumental ? "instrumental" : "" });
                }
                runStart = -1;
            }
        }
    }

    // Parts of a figure that never light, and figures that never light at all.
    for (const FigureTrace& t : traces.figures) {
        for (const RegionTrace& region : t.regions) {
            const float peak = region.level.empty() ? 0 : *std::max_element(region.level.begin(), region.level.end());
            if (peak < o.litLevel) {
                add({ "", region.region.empty() ? "figure-silent" : "region-never-lit", t.figure, region.region, "", 0, r.durationMS, "check", peak, "" });
            }
        }
    }

    // Stretches where the music plays and the whole show is dark.
    if (in.audio != nullptr && !traces.figures.empty()) {
        int runStart = -1;
        for (int s = 0; s <= traces.steps; ++s) {
            const int ms = s * step;
            bool dark = s < traces.steps && in.audio->Has("music", ms, ms + step);
            for (const FigureTrace& t : traces.figures) {
                dark = dark && (t.regions.empty() || t.regions.front().level[s] < o.litLevel);
            }
            if (dark && runStart < 0) {
                runStart = s;
            }
            if (!dark && runStart >= 0) {
                if (ms - runStart * step >= 2000) {
                    add({ "", "dark-while-music", "", "", "", runStart * step, ms, "warning", 0, "" });
                }
                runStart = -1;
            }
        }
    }

    // Moments to look at: where a voice starts, the instruments' turns, the drops and
    // re-entries, and every warning; spaced apart, in time order.
    std::vector<CaptureMoment> wanted;
    if (full) {
        for (const PerformerAssignment& a : in.performers->assignments) {
            for (const auto& [from, to] : ActiveWindows(a, *in.audio, *in.voices)) {
                if (to - from >= o.minWindowMS) {
                    wanted.push_back({ std::min(to - 1, from + 500), a.source.rfind("track:", 0) == 0 ? "voice-starts" : "instrumental", a.figures });
                    break;
                }
            }
        }
    }
    if (in.audio != nullptr) {
        for (const SoundEvent& e : in.audio->events) {
            if (e.kind == "drop" || e.kind == "reentry") {
                wanted.push_back({ e.startMS + 200, e.kind, {} });
            }
        }
    }
    for (const RenderFinding& f : r.findings) {
        if (f.severity == "warning") {
            wanted.push_back({ (f.startMS + f.endMS) / 2, "finding:" + f.id, f.figure.empty() ? std::vector<std::string>() : std::vector<std::string>{ f.figure } });
        }
    }
    std::stable_sort(wanted.begin(), wanted.end(), [](const CaptureMoment& a, const CaptureMoment& b) {
        const bool fa = a.reason.rfind("finding:", 0) == 0;
        const bool fb = b.reason.rfind("finding:", 0) == 0;
        return fa != fb ? fa : a.timeMS < b.timeMS;
    });
    for (const CaptureMoment& c : wanted) {
        if (static_cast<int>(r.captures.size()) >= o.maxCaptures || c.timeMS < 0 || (r.durationMS > 0 && c.timeMS >= r.durationMS)) {
            continue;
        }
        const bool near = std::any_of(r.captures.begin(), r.captures.end(), [&](const CaptureMoment& k) { return std::abs(k.timeMS - c.timeMS) < o.captureSpacingMS; });
        if (!near) {
            r.captures.push_back(c);
        }
    }
    std::sort(r.captures.begin(), r.captures.end(), [](const CaptureMoment& a, const CaptureMoment& b) { return a.timeMS < b.timeMS; });
    return r;
}

std::string ToJson(const RenderUnderstanding& r) {
    json checks = json::array();
    for (const IntentionCheck& c : r.checks) {
        checks.push_back({ { "source", c.source }, { "figure", c.figure }, { "region", c.region }, { "windows", c.windows }, { "honoured", c.honoured } });
    }
    json findings = json::array();
    for (const RenderFinding& f : r.findings) {
        findings.push_back({ { "id", f.id }, { "kind", f.kind }, { "figure", f.figure }, { "region", f.region }, { "source", f.source }, { "startMS", f.startMS },
                             { "endMS", f.endMS }, { "severity", f.severity }, { "measured", std::round(f.measured * 1000) / 1000 }, { "detail", f.detail } });
    }
    json captures = json::array();
    for (const CaptureMoment& c : r.captures) {
        captures.push_back({ { "timeMS", c.timeMS }, { "reason", c.reason }, { "figures", c.figures } });
    }
    return json({ { "kind", "composer-render-understanding" }, { "schemaVersion", r.schemaVersion }, { "sequence", r.sequence }, { "stepMS", r.stepMS }, { "durationMS", r.durationMS },
                  { "review", r.review }, { "checks", checks }, { "findings", findings }, { "captures", captures }, { "limits", r.limits } })
        .dump(1);
}

std::string FindingPhrase(const std::string& kind) {
    static const std::map<std::string, const char*> P = {
        { "expected-dark", COMPOSER_TEXT("It was meant to perform here and stays dark.") },
        { "mouth-still", COMPOSER_TEXT("It was meant to speak or sing here and its mouth does not move.") },
        { "mouth-moves-without-voice", COMPOSER_TEXT("Its mouth moves while nobody speaks through it.") },
        { "voice-on-other-figure", COMPOSER_TEXT("This voice seems to be shown on another figure.") },
        { "region-never-lit", COMPOSER_TEXT("This part of the figure never lights.") },
        { "figure-silent", COMPOSER_TEXT("This figure never lights.") },
        { "dark-while-music", COMPOSER_TEXT("Everything is dark while the music plays.") },
    };
    const auto it = P.find(kind);
    return it != P.end() ? it->second : kind;
}

std::string CaptureTimes(const RenderUnderstanding& r) {
    std::string out;
    for (const CaptureMoment& c : r.captures) {
        out += (out.empty() ? "" : ",") + std::to_string(c.timeMS);
    }
    return out;
}

} // namespace composer
