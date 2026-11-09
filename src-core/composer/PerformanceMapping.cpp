#include "composer/PerformanceMapping.h"

#include "composer/LyricAligner.h"
#include "composer/LyricDocument.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <tuple>

#define COMPOSER_TEXT(text) text

using json = nlohmann::json;

namespace composer {

namespace {

bool Speaks(const std::string& function) {
    return function == "speak" || function == "sing" || function == "choral-response";
}

json AssignmentJson(const PerformerAssignment& a) {
    return { { "source", a.source }, { "sourceLabel", a.sourceLabel }, { "figures", a.figures }, { "function", a.function }, { "region", a.region }, { "state", a.state },
             { "confidence", a.confidence }, { "reasons", a.reasons }, { "conflicts", a.conflicts } };
}

PerformerAssignment AssignmentFrom(const json& j) {
    PerformerAssignment a;
    a.source = j.value("source", std::string());
    a.sourceLabel = j.value("sourceLabel", std::string());
    a.figures = j.value("figures", std::vector<std::string>());
    a.function = j.value("function", std::string());
    a.region = j.value("region", std::string());
    a.state = j.value("state", std::string("manual"));
    a.confidence = j.value("confidence", 1.0);
    a.reasons = j.value("reasons", std::vector<std::string>());
    return a;
}

int Overlap(int a0, int a1, int b0, int b1) {
    return std::max(0, std::min(a1, b1) - std::max(a0, b0));
}

} // namespace

const PerformerAssignment* PerformanceMap::For(const std::string& source) const {
    for (const PerformerAssignment& a : assignments) {
        if (a.source == source) {
            return &a;
        }
    }
    return nullptr;
}

std::vector<const PerformerAssignment*> PerformanceMap::OfFigure(const std::string& figure) const {
    std::vector<const PerformerAssignment*> out;
    for (const PerformerAssignment& a : assignments) {
        if (std::find(a.figures.begin(), a.figures.end(), figure) != a.figures.end()) {
            out.push_back(&a);
        }
    }
    return out;
}

std::vector<std::pair<int, int>> ActiveWindows(const PerformerAssignment& a, const AudioUnderstanding& audio, const VocalUnderstanding& voices) {
    std::vector<std::pair<int, int>> out;
    if (a.source.rfind("track:", 0) == 0) {
        for (const Intervention* i : voices.Of(a.source.substr(6))) {
            out.push_back({ i->startMS, i->endMS });
        }
    } else if (a.source.rfind("activity:", 0) == 0) {
        for (const ActivitySegment* s : audio.Of(a.source.substr(9))) {
            out.push_back({ s->startMS, s->endMS });
        }
    } else if (a.source.rfind("event:", 0) == 0) {
        for (const SoundEvent& e : audio.events) {
            if (e.kind == a.source.substr(6)) {
                out.push_back({ e.startMS, std::max(e.endMS, e.startMS + 100) });
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

PerformanceMap MapPerformance(const MappingInput& in) {
    PerformanceMap map;
    if (in.voices == nullptr || in.semantics == nullptr || in.geometry == nullptr) {
        map.conflicts.push_back("missing-input");
        return map;
    }
    const FigureSemantics& sem = *in.semantics;
    const FigureCatalog& geo = *in.geometry;
    const auto installed = [&](const std::string& figure) {
        const FigureGeometry* g = geo.Find(figure);
        return g != nullptr && !g->noController;
    };
    // Figures that can speak, best first: real mouth lights, then faces made of pictures;
    // wired before preview-only; faces, then trees, then the rest; then by name.
    std::vector<const FigureProfile*> speakers;
    for (const FigureProfile& p : sem.profiles) {
        if (p.CanSpeak()) {
            speakers.push_back(&p);
        }
    }
    const auto familyRank = [](const std::string& f) { return f == "faces" ? 0 : f == "trees" ? 1 : f == "figures" ? 2 : 3; };
    std::stable_sort(speakers.begin(), speakers.end(), [&](const FigureProfile* a, const FigureProfile* b) {
        const auto key = [&](const FigureProfile* p) { return std::make_tuple(installed(p->figure) ? 0 : 1, p->mouth == "nodes" ? 0 : 1, familyRank(p->family), p->figure); };
        return key(a) < key(b);
    });
    std::set<std::string> used;
    const auto take = [&](const std::function<bool(const FigureProfile&)>& prefer) -> const FigureProfile* {
        for (const FigureProfile* p : speakers) {
            if (!used.contains(p->figure) && prefer(*p)) {
                used.insert(p->figure);
                return p;
            }
        }
        for (const FigureProfile* p : speakers) {
            if (!used.contains(p->figure)) {
                used.insert(p->figure);
                return p;
            }
        }
        return nullptr;
    };
    const auto manualFor = [&](const std::string& source) -> const PerformerAssignment* {
        if (in.manual == nullptr) {
            return nullptr;
        }
        for (const PerformerAssignment& m : *in.manual) {
            if (m.source == source) {
                return &m;
            }
        }
        return nullptr;
    };
    // A person's figures are taken first, so the automatic choice works around them.
    if (in.manual != nullptr) {
        for (const PerformerAssignment& m : *in.manual) {
            used.insert(m.figures.begin(), m.figures.end());
        }
    }

    // Voices: narrators, then the lead, the second voices, and the choir with what is left.
    std::vector<const VoiceTrack*> order;
    for (const VoiceTrack& t : in.voices->tracks) {
        order.push_back(&t);
    }
    const auto kindRank = [](const std::string& k) { return k == "narrator" ? 0 : k == "lead" ? 1 : k == "secondary" ? 2 : k == "chorus" ? 4 : 3; };
    std::stable_sort(order.begin(), order.end(), [&](const VoiceTrack* a, const VoiceTrack* b) { return kindRank(a->kind) < kindRank(b->kind); });
    for (const VoiceTrack* t : order) {
        PerformerAssignment a;
        a.source = "track:" + t->id;
        a.sourceLabel = t->label;
        if (const PerformerAssignment* m = manualFor(a.source)) {
            a = *m;
            a.state = "manual";
            a.confidence = 1;
            map.assignments.push_back(a);
            continue;
        }
        a.function = t->kind == "narrator" || t->emission == "spoken" ? "speak" : t->kind == "chorus" ? "choral-response" : "sing";
        if (t->kind == "chorus") {
            for (const FigureProfile* p : speakers) {
                if (!used.contains(p->figure) && a.figures.size() < 6) {
                    a.figures.push_back(p->figure);
                    used.insert(p->figure);
                }
            }
            a.reasons.push_back("a choir is one sound: it is shown by several figures at once, not split into singers");
            if (a.figures.size() < 2) {
                a.conflicts.push_back("too-few-figures-for-chorus");
            }
        } else {
            const FigureProfile* p = take([&](const FigureProfile& f) {
                return t->kind == "narrator" ? f.family == "trees" || f.family == "figures" : f.family == "faces";
            });
            if (p != nullptr) {
                a.figures.push_back(p->figure);
                a.reasons.push_back(fmt::format("{} can {} ({})", p->figure, a.function == "speak" ? "speak" : "sing", p->mouth == "nodes" ? "a mouth of its own lights" : "a face of pictures"));
            } else {
                a.conflicts.push_back("no-figure-with-mouth");
            }
        }
        a.confidence = std::min(0.8, t->confidence + 0.2);
        map.assignments.push_back(std::move(a));
    }

    // The music's moments: the instruments' turn to the tree or the matrix, the hits to
    // the arches or the outline.
    const auto byFamily = [&](const std::vector<std::string>& families) {
        std::vector<std::string> out;
        for (const std::string& f : families) {
            for (const FigureGeometry& g : geo.figures) {
                if (g.kind == "model" && g.family == f && !g.noController) {
                    out.push_back(g.name);
                }
            }
            if (!out.empty()) {
                return out;
            }
        }
        return out;
    };
    struct Moment {
        const char* source;
        const char* label;
        const char* function;
        std::vector<std::string> families;
        const char* region;
    };
    const std::vector<Moment> moments = { { "activity:instrumental", "Instrumental", "instrumental-lead", { "mega-tree", "matrix" }, "" },
                                          { "activity:percussion", "Percussion", "accent", { "arches", "outline", "windows" }, "outline" } };
    for (const Moment& m : moments) {
        const std::string kind = std::string(m.source).substr(9);
        if (in.audio == nullptr || in.audio->Of(kind).empty()) {
            continue;
        }
        PerformerAssignment a;
        a.source = m.source;
        a.sourceLabel = m.label;
        if (const PerformerAssignment* manual = manualFor(a.source)) {
            a = *manual;
            a.state = "manual";
            a.confidence = 1;
        } else {
            a.function = m.function;
            a.region = m.region;
            a.figures = byFamily(m.families);
            a.confidence = 0.6;
            a.reasons.push_back(std::string("the ") + m.families.front() + " family plays it when the music does");
            if (a.figures.empty()) {
                a.conflicts.push_back("no-figure-for-it");
            }
        }
        map.assignments.push_back(std::move(a));
    }

    // Problems, named.
    for (PerformerAssignment& a : map.assignments) {
        for (const std::string& f : a.figures) {
            const FigureProfile* p = sem.Find(f);
            const FigureGeometry* g = geo.Find(f);
            if (g == nullptr) {
                a.conflicts.push_back("figure-not-in-layout:" + f);
                continue;
            }
            if (g->noController) {
                a.conflicts.push_back("no-controller:" + f);
            }
            if (Speaks(a.function) && (p == nullptr || !p->CanSpeak())) {
                a.conflicts.push_back("figure-has-no-mouth:" + f);
            }
            if (!a.region.empty() && p != nullptr) {
                const FigureRegion* r = p->Region(a.region);
                if (r == nullptr || !r->real) {
                    a.conflicts.push_back("region-missing:" + f + ":" + a.region);
                }
            }
        }
        if (a.region.empty() && Speaks(a.function)) {
            a.region = "mouth";
            for (const std::string& f : a.figures) {
                const FigureProfile* p = sem.Find(f);
                if (p != nullptr && p->mouth == "images") {
                    a.region = "face";
                }
            }
        }
    }
    // Two voices on one figure at the same time cannot both be shown by one mouth.
    if (in.voices != nullptr) {
        for (size_t i = 0; i < map.assignments.size(); ++i) {
            for (size_t j = i + 1; j < map.assignments.size(); ++j) {
                PerformerAssignment& a = map.assignments[i];
                const PerformerAssignment& b = map.assignments[j];
                if (!Speaks(a.function) || !Speaks(b.function) || a.source.rfind("track:", 0) != 0 || b.source.rfind("track:", 0) != 0) {
                    continue;
                }
                for (const std::string& f : a.figures) {
                    if (std::find(b.figures.begin(), b.figures.end(), f) == b.figures.end()) {
                        continue;
                    }
                    bool together = false;
                    for (const Intervention* x : in.voices->Of(a.source.substr(6))) {
                        for (const Intervention* y : in.voices->Of(b.source.substr(6))) {
                            together = together || Overlap(x->startMS, x->endMS, y->startMS, y->endMS) > 200;
                        }
                    }
                    if (together) {
                        a.conflicts.push_back("simultaneous-voices:" + f + ":" + b.source.substr(6));
                    }
                }
            }
        }
    }
    for (const PerformerAssignment& a : map.assignments) {
        for (const std::string& c : a.conflicts) {
            map.conflicts.push_back(a.source + "|" + c);
        }
    }
    return map;
}

std::string ToJson(const std::vector<PerformerAssignment>& manual) {
    json list = json::array();
    for (const PerformerAssignment& a : manual) {
        list.push_back(AssignmentJson(a));
    }
    return json({ { "kind", "composer-performers" }, { "schemaVersion", PERFORMANCE_MAP_SCHEMA_VERSION }, { "assignments", list } }).dump(1);
}

bool FromJson(const std::string& text, std::vector<PerformerAssignment>& manual, std::string& error) {
    try {
        const json root = json::parse(text);
        if (root.value("schemaVersion", 0) < 1 || root.value("schemaVersion", 0) > PERFORMANCE_MAP_SCHEMA_VERSION) {
            error = "performers: schema not read";
            return false;
        }
        std::vector<PerformerAssignment> out;
        for (const json& a : root.value("assignments", json::array())) {
            out.push_back(AssignmentFrom(a));
            out.back().state = "manual";
        }
        manual = std::move(out);
    } catch (const json::exception& e) {
        error = std::string("performers: ") + e.what();
        return false;
    }
    return true;
}

std::string ToJson(const PerformanceMap& map) {
    json list = json::array();
    for (const PerformerAssignment& a : map.assignments) {
        list.push_back(AssignmentJson(a));
    }
    return json({ { "kind", "composer-performance-map" }, { "schemaVersion", map.schemaVersion }, { "assignments", list }, { "conflicts", map.conflicts } }).dump(1);
}

CastFromUnderstanding ApplyPerformersToCast(const VocalUnderstanding& voices, const PerformanceMap& map, const FigureSemantics& semantics, LyricAlignment& alignment, VoiceCast& cast) {
    CastFromUnderstanding out;
    const auto faces = [&](const PerformerAssignment& a) {
        std::vector<std::string> f;
        for (const std::string& figure : a.figures) {
            const FigureProfile* p = semantics.Find(figure);
            if (p != nullptr && p->CanSpeak()) {
                f.push_back(figure);
            } else {
                out.notes.push_back("not-a-face:" + figure);
            }
        }
        return f;
    };
    // The voice of the lyrics a heard track is: one named only by the lyrics, or a heard
    // one the lyrics named.
    const auto lyricVoiceOf = [&](const VoiceTrack& t) -> const VoiceDef* {
        if (t.id.rfind("lyric:", 0) == 0) {
            return cast.Find(t.id.substr(6));
        }
        for (const VoiceDef& d : cast.voices) {
            if (std::find(t.evidence.begin(), t.evidence.end(), "named in the lyrics: " + d.name) != t.evidence.end()) {
                return &d;
            }
        }
        return nullptr;
    };
    // A person's figures for a voice the lyrics name.
    for (VoiceDef& d : cast.voices) {
        const PerformerAssignment* a = map.For("track:lyric:" + d.id);
        for (const VoiceTrack& t : voices.tracks) {
            const PerformerAssignment* m = map.For("track:" + t.id);
            if (a == nullptr && m != nullptr && m->state == "manual" && lyricVoiceOf(t) == &d) {
                a = m;
            }
        }
        if (a == nullptr || a->state != "manual") {
            continue;
        }
        d.faces = faces(*a);
        d.facesOff = a->figures.empty();
        ++out.facesChanged;
    }
    // Unnamed phrases: the voice heard in them.
    for (AlignedPhrase& p : alignment.phrases) {
        if (p.startMS < 0 || p.endMS <= p.startMS || p.instrumental || !p.voices.empty()) {
            continue;
        }
        std::map<std::string, int> heard;
        for (const Intervention& i : voices.interventions) {
            if (i.track.rfind("lyric:", 0) != 0 && i.provenance != "lyrics") {
                heard[i.track] += Overlap(p.startMS, p.endMS, i.startMS, i.endMS);
            }
        }
        std::string best;
        int most = 0;
        for (const auto& [track, ms] : heard) {
            if (ms > most) {
                most = ms;
                best = track;
            }
        }
        if (best.empty() || most * 10 < (p.endMS - p.startMS) * 3) {
            continue;
        }
        const PerformerAssignment* a = map.For("track:" + best);
        const VoiceTrack* t = voices.Track(best);
        if (a == nullptr || t == nullptr) {
            continue;
        }
        const VoiceDef* named = lyricVoiceOf(*t);
        const std::string id = named != nullptr ? named->id : "u:" + best;
        if (cast.Find(id) == nullptr) {
            VoiceDef d;
            d.id = id;
            d.name = t->label;
            d.role = t->kind == "chorus" ? VoiceRole::Chorus : VoiceRole::Unknown; // heard, not named: no role is guessed
            d.delivery = t->emission == "spoken" ? Delivery::Spoken : Delivery::Sung;
            d.faces = faces(*a);
            d.facesOff = d.faces.empty();
            d.source = "understanding";
            cast.voices.push_back(std::move(d));
            ++out.voicesAdded;
        }
        p.voices = { id };
        p.voice = id;
        p.voiceSource = "understanding";
        if (t->emission == "spoken") {
            p.delivery = "spoken";
        }
        ++out.phrasesNamed;
    }
    return out;
}

std::string FunctionPhrase(const std::string& f) {
    static const std::map<std::string, const char*> P = {
        { "speak", COMPOSER_TEXT("Speaks") }, { "sing", COMPOSER_TEXT("Sings") }, { "choral-response", COMPOSER_TEXT("Answers as the choir") },
        { "instrumental-lead", COMPOSER_TEXT("Leads when the instruments play") }, { "accent", COMPOSER_TEXT("Marks the beats") }, { "background", COMPOSER_TEXT("Background") }
    };
    const auto it = P.find(f);
    return it != P.end() ? it->second : f;
}

std::string ConflictPhrase(const std::string& code) {
    const std::string head = code.substr(0, code.find(':'));
    static const std::map<std::string, const char*> P = {
        { "no-figure-with-mouth", COMPOSER_TEXT("No figure with a mouth is left for this voice.") },
        { "too-few-figures-for-chorus", COMPOSER_TEXT("There are not enough figures with a mouth to show the choir.") },
        { "figure-has-no-mouth", COMPOSER_TEXT("This figure has no mouth defined: it cannot speak or sing yet.") },
        { "region-missing", COMPOSER_TEXT("The part of the figure it needs is not tied to real lights.") },
        { "no-controller", COMPOSER_TEXT("This figure has no controller: it is only seen in the preview.") },
        { "simultaneous-voices", COMPOSER_TEXT("Two voices sound at the same time on this figure.") },
        { "figure-not-in-layout", COMPOSER_TEXT("This figure is not in the layout.") },
        { "no-figure-for-it", COMPOSER_TEXT("No figure of a suitable kind was found.") },
    };
    const auto it = P.find(head);
    return it != P.end() ? it->second : code;
}

} // namespace composer
