#include "composer/VocalUnderstanding.h"

#include "composer/LyricAligner.h"
#include "composer/LyricDocument.h"
#include "composer/MusicAssist.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using json = nlohmann::json;

namespace composer {

namespace {

double Round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

double Median(std::vector<double> v) {
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

int Overlap(int a0, int a1, int b0, int b1) {
    return std::max(0, std::min(a1, b1) - std::max(a0, b0));
}

bool IsVoice(const std::string& kind) {
    return kind == "speech" || kind == "singing" || kind == "chorus";
}

// What a voice sounds like over a stretch: its pitch, how far it moves, its brightness.
struct Print {
    double semis = 0;
    double spread = 0;
    double brightness = 0;
    double air = 0;
    int frames = 0;
};

Print PrintOf(const AudioUnderstanding& u, int from, int to) {
    std::vector<double> semis, bright, air;
    for (const SoundFrame& f : u.curve) {
        if (f.timeMS >= from && f.timeMS < to && f.f0 > 0 && f.voicing >= 0.5) {
            semis.push_back(12.0 * std::log2(f.f0 / 440.0));
            bright.push_back(std::log2(std::max(100.0f, f.centroid)));
            air.push_back(f.high);
        }
    }
    Print p;
    p.frames = static_cast<int>(semis.size());
    if (semis.empty()) {
        return p;
    }
    p.semis = Median(semis);
    std::sort(semis.begin(), semis.end());
    p.spread = semis[semis.size() * 3 / 4] - semis[semis.size() / 4];
    p.brightness = Median(bright);
    p.air = Median(air);
    return p;
}

double Distance(const Print& a, const Print& b) {
    // Pitch weighs most: a phrase's brightness and breathiness move with what is said.
    return std::fabs(a.semis - b.semis) / 4.0 + std::fabs(a.spread - b.spread) / 4.0 + std::fabs(a.brightness - b.brightness) / 4.0 + std::fabs(a.air - b.air) / 2.0;
}

std::string Letter(int n) {
    std::string s;
    do {
        s.insert(s.begin(), static_cast<char>('A' + n % 26));
        n = n / 26 - 1;
    } while (n >= 0);
    return s;
}

} // namespace

const VoiceTrack* VocalUnderstanding::Track(const std::string& id) const {
    for (const VoiceTrack& t : tracks) {
        if (t.id == id) {
            return &t;
        }
    }
    return nullptr;
}

std::vector<const Intervention*> VocalUnderstanding::Of(const std::string& track) const {
    std::vector<const Intervention*> out;
    for (const Intervention& i : interventions) {
        if (i.track == track) {
            out.push_back(&i);
        }
    }
    return out;
}

std::vector<std::string> VocalUnderstanding::SoundingAt(int timeMS) const {
    std::set<std::string> out;
    for (const Intervention& i : interventions) {
        if (timeMS >= i.startMS && timeMS < i.endMS) {
            out.insert(i.track);
        }
    }
    return { out.begin(), out.end() };
}

VocalUnderstanding UnderstandVoices(const VocalInput& in) {
    VocalUnderstanding v;
    v.limits = { "diarization-experimental: voices grouped by pitch and timbre, never identified as people",
                 "no-source-separation: no voice is extracted from a mix; a choir stays one track",
                 "quality-not-measured-on-real-music" };
    if (in.audio == nullptr) {
        v.warnings.push_back("missing-audio");
        return v;
    }
    const AudioUnderstanding& u = *in.audio;

    // ---------------------------------------------------------------- tracks
    struct Draft {
        const ActivitySegment* segment;
        std::string track;
        Print print;
    };
    std::vector<Draft> drafts;
    for (const ActivitySegment& s : u.activities) {
        if (IsVoice(s.kind)) {
            drafts.push_back({ &s, s.track, PrintOf(u, s.startMS, s.endMS) });
        }
    }
    // A choir is one track; a voice on its own stem is its own track; a person's track
    // stays; the rest is grouped by how the voice sounds, in the order it first sings.
    std::vector<std::pair<std::string, Print>> clusters;
    for (Draft& d : drafts) {
        if (!d.track.empty()) {
            continue;
        }
        if (d.segment->kind == "chorus") {
            d.track = "chorus";
            continue;
        }
        if (d.print.frames < 5) {
            d.track = "voice:?";
            continue;
        }
        double best = 1e9;
        size_t at = 0;
        for (size_t c = 0; c < clusters.size(); ++c) {
            const double dist = Distance(d.print, clusters[c].second);
            if (dist < best) {
                best = dist;
                at = c;
            }
        }
        if (best <= 1.0) {
            // The voice is what all its phrases sound like, not only its first one.
            Print& c = clusters[at].second;
            const double w = static_cast<double>(c.frames) / std::max(1, c.frames + d.print.frames);
            c.semis = c.semis * w + d.print.semis * (1 - w);
            c.brightness = c.brightness * w + d.print.brightness * (1 - w);
            c.air = c.air * w + d.print.air * (1 - w);
            c.spread = c.spread * w + d.print.spread * (1 - w);
            c.frames += d.print.frames;
            d.track = clusters[at].first;
        } else {
            clusters.push_back({ "voice:" + Letter(static_cast<int>(clusters.size())), d.print });
            d.track = clusters.back().first;
        }
    }
    // Groups that ended up sounding alike are one voice; the letters follow the order in
    // which the voices are first heard.
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t a = 0; a < clusters.size() && !merged; ++a) {
            for (size_t b = a + 1; b < clusters.size() && !merged; ++b) {
                if (Distance(clusters[a].second, clusters[b].second) > 1.0) {
                    continue;
                }
                Print& into = clusters[a].second;
                const Print& from = clusters[b].second;
                const double w = static_cast<double>(into.frames) / std::max(1, into.frames + from.frames);
                into.semis = into.semis * w + from.semis * (1 - w);
                into.brightness = into.brightness * w + from.brightness * (1 - w);
                into.air = into.air * w + from.air * (1 - w);
                into.spread = into.spread * w + from.spread * (1 - w);
                into.frames += from.frames;
                for (Draft& d : drafts) {
                    d.track = d.track == clusters[b].first ? clusters[a].first : d.track;
                }
                clusters.erase(clusters.begin() + static_cast<long>(b));
                merged = true;
            }
        }
    }
    {
        std::map<std::string, std::string> letters;
        std::set<std::string> taken; // a person's own voices keep their names
        for (const Draft& d : drafts) {
            if (!d.segment->track.empty()) {
                taken.insert(d.segment->track);
            }
        }
        int next = 0;
        for (Draft& d : drafts) {
            if (d.track.rfind("voice:", 0) != 0 || d.track == "voice:?" || d.segment->track == d.track || letters.contains(d.track)) {
                continue;
            }
            std::string name;
            do {
                name = "voice:" + Letter(next++);
            } while (taken.contains(name));
            letters[d.track] = name;
        }
        for (Draft& d : drafts) {
            if (letters.contains(d.track) && d.segment->track != d.track) {
                d.track = letters[d.track];
            }
        }
    }

    // The voices the lyrics name, where they are synchronized: a named voice wins over
    // a letter, the narrator and the choir are taken from the tags.
    std::map<std::string, std::map<std::string, int>> namedBy; // acoustic track -> lyric voice -> ms
    std::map<std::string, const AlignedPhrase*> phrases;
    if (in.alignment != nullptr) {
        for (const Draft& d : drafts) {
            for (const AlignedPhrase& p : in.alignment->phrases) {
                if (p.startMS >= 0 && !p.instrumental && !p.voices.empty()) {
                    namedBy[d.track][p.voices.front()] += Overlap(d.segment->startMS, d.segment->endMS, p.startMS, p.endMS);
                }
            }
        }
    }

    std::map<std::string, VoiceTrack> tracks;
    for (const Draft& d : drafts) {
        VoiceTrack& t = tracks[d.track];
        t.id = d.track;
        t.totalMS += d.segment->endMS - d.segment->startMS;
        t.source = d.segment->provenance.rfind("stem:", 0) == 0 ? "stem" : "acoustic";
    }
    for (auto& [id, t] : tracks) {
        int spoken = 0, sung = 0;
        std::vector<double> f0s;
        double confidence = 0;
        int n = 0;
        for (const Draft& d : drafts) {
            if (d.track != id) {
                continue;
            }
            const int len = d.segment->endMS - d.segment->startMS;
            (d.segment->kind == "speech" ? spoken : sung) += len;
            if (d.print.frames > 0) {
                f0s.push_back(440.0 * std::pow(2.0, d.print.semis / 12.0));
            }
            confidence += d.segment->confidence;
            ++n;
        }
        t.medianF0 = std::round(Median(f0s));
        t.confidence = Round3(n > 0 ? confidence / n : 0);
        t.emission = spoken > sung * 3 ? "spoken" : sung > spoken * 3 ? "sung" : "mixed";
        t.label = id == "chorus" ? "Coro" : id == "voice:?" ? "Voz sin identificar" : id.rfind("voice:", 0) == 0 ? "Voz " + id.substr(6) : id.substr(id.find(':') + 1);
        t.kind = id == "chorus" ? "chorus" : t.emission == "spoken" ? "narrator" : "unknown";
        t.evidence.push_back(fmt::format("{:.1f} s heard", t.totalMS / 1000.0));
        const auto named = namedBy.find(id);
        if (named != namedBy.end() && in.cast != nullptr) {
            const auto top = std::max_element(named->second.begin(), named->second.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
            if (top != named->second.end() && top->second * 2 >= t.totalMS) {
                if (const VoiceDef* def = in.cast->Find(top->first)) {
                    t.label = def->name;
                    t.kind = def->role == VoiceRole::Narrator ? "narrator" : def->role == VoiceRole::Chorus ? "chorus" : def->role == VoiceRole::Lead ? "lead" : def->role == VoiceRole::Second ? "secondary" : t.kind;
                    t.evidence.push_back("named in the lyrics: " + def->name);
                }
            }
        }
    }
    // The sung voice heard longest leads; the other sung voices are second.
    std::string lead;
    int longest = -1;
    for (const auto& [id, t] : tracks) {
        if ((t.kind == "unknown" || t.kind == "lead") && t.emission != "spoken" && id != "voice:?" && t.totalMS > longest) {
            longest = t.totalMS;
            lead = id;
        }
    }
    for (auto& [id, t] : tracks) {
        if (t.kind == "unknown") {
            t.kind = id == lead ? "lead" : id == "voice:?" ? "unknown" : "secondary";
        }
        v.tracks.push_back(t);
    }
    std::sort(v.tracks.begin(), v.tracks.end(), [](const VoiceTrack& a, const VoiceTrack& b) { return a.id < b.id; });

    // --------------------------------------------------------- interventions
    std::map<std::string, int> next;
    for (const Draft& d : drafts) {
        Intervention i;
        i.activityId = d.segment->id;
        i.track = d.track;
        i.startMS = d.segment->startMS;
        i.endMS = d.segment->endMS;
        i.confidence = d.segment->confidence;
        i.provenance = d.segment->provenance;
        const bool unsure = std::any_of(d.segment->evidence.begin(), d.segment->evidence.end(), [](const std::string& e) { return e.find("uncertain") != std::string::npos; });
        i.emission = unsure || d.segment->confidence < 0.4 ? "doubtful" : d.segment->kind == "speech" ? "spoken" : d.segment->kind == "chorus" ? "choral" : "sung";
        i.evidence = d.segment->evidence;
        v.interventions.push_back(std::move(i));
    }
    // Lines the lyrics place where the sound found no voice: the words were sung even if
    // the mix hides them.
    if (in.alignment != nullptr) {
        for (const AlignedPhrase& p : in.alignment->phrases) {
            if (p.startMS < 0 || p.instrumental || p.endMS <= p.startMS) {
                continue;
            }
            const bool covered = std::any_of(v.interventions.begin(), v.interventions.end(), [&](const Intervention& i) { return Overlap(i.startMS, i.endMS, p.startMS, p.endMS) * 2 >= p.endMS - p.startMS; });
            if (covered) {
                continue;
            }
            Intervention i;
            i.track = !p.voices.empty() ? "lyric:" + p.voices.front() : "voice:?";
            i.startMS = p.startMS;
            i.endMS = p.endMS;
            i.confidence = Round3(std::min(0.6, p.confidence));
            i.provenance = "lyrics";
            i.emission = p.delivery == "spoken" ? "spoken" : "sung";
            i.evidence = { "a synchronized line where no voice was heard" };
            if (!v.Track(i.track)) {
                VoiceTrack t;
                t.id = i.track;
                t.source = "lyrics";
                const VoiceDef* def = in.cast != nullptr && !p.voices.empty() ? in.cast->Find(p.voices.front()) : nullptr;
                t.label = def != nullptr ? def->name : "Voz sin identificar";
                t.kind = def != nullptr && def->role == VoiceRole::Narrator ? "narrator" : def != nullptr && def->role == VoiceRole::Chorus ? "chorus" : "unknown";
                t.emission = i.emission;
                t.evidence = { "named in the lyrics only" };
                v.tracks.push_back(t);
            }
            v.interventions.push_back(std::move(i));
        }
    }
    std::sort(v.interventions.begin(), v.interventions.end(), [](const Intervention& a, const Intervention& b) { return a.startMS != b.startMS ? a.startMS < b.startMS : a.track < b.track; });
    for (Intervention& i : v.interventions) {
        i.id = fmt::format("{}#{}", i.track, ++next[i.track]);
    }

    // Words, text and mouth shapes from the alignment; the transcription's text when the
    // lyrics have none.
    for (Intervention& i : v.interventions) {
        if (in.alignment != nullptr) {
            std::string text;
            for (const AlignedPhrase& p : in.alignment->phrases) {
                if (p.startMS >= 0 && Overlap(i.startMS, i.endMS, p.startMS, p.endMS) * 2 >= std::max(1, p.endMS - p.startMS)) {
                    text += (text.empty() ? "" : " / ") + p.text;
                    if (p.delivery == "spoken" && i.emission == "sung") {
                        i.evidence.push_back("the lyrics mark it spoken");
                        i.emission = "spoken";
                    }
                }
            }
            i.text = text;
            for (const AlignedWord& w : in.alignment->words) {
                if (w.startMS >= i.startMS && w.startMS < i.endMS) {
                    i.wordIds.push_back(w.id);
                }
            }
            for (const AlignedPhoneme& ph : in.alignment->phonemes) {
                if (ph.startMS >= i.startMS && ph.startMS < i.endMS && ph.shape != "rest") {
                    i.mouth.push_back({ ph.shape, ph.startMS, ph.endMS });
                }
            }
        }
        if (i.text.empty() && in.transcript != nullptr) {
            for (const TranscriptSegment& s : in.transcript->segments) {
                if (Overlap(i.startMS, i.endMS, s.startMS, s.endMS) * 2 >= std::max(1, s.endMS - s.startMS)) {
                    i.text += (i.text.empty() ? "" : " / ") + s.text;
                }
            }
        }
    }

    // How each one sits with the others and with the music.
    for (Intervention& i : v.interventions) {
        i.relations.push_back(u.Has("music", i.startMS + 1, i.endMS - 1) ? "over-music" : "alone");
        for (const Intervention& o : v.interventions) {
            if (&o == &i || o.track == i.track) {
                continue;
            }
            if (Overlap(i.startMS, i.endMS, o.startMS, o.endMS) > 150) {
                i.relations.push_back("overlaps:" + o.id);
            } else if (o.endMS <= i.startMS && i.startMS - o.endMS <= 1200 && (i.track == "chorus" || i.emission == "choral")) {
                i.relations.push_back("answers:" + o.id);
            }
        }
    }
    if (v.Track("chorus") != nullptr) {
        v.warnings.push_back("chorus-is-one-track");
    }
    return v;
}

std::string ToJson(const VocalUnderstanding& v) {
    json tracks = json::array();
    for (const VoiceTrack& t : v.tracks) {
        tracks.push_back({ { "id", t.id }, { "label", t.label }, { "kind", t.kind }, { "emission", t.emission }, { "source", t.source }, { "medianF0", t.medianF0 },
                           { "confidence", t.confidence }, { "totalMS", t.totalMS }, { "evidence", t.evidence } });
    }
    json list = json::array();
    for (const Intervention& i : v.interventions) {
        json mouth = json::array();
        for (const MouthShape& m : i.mouth) {
            mouth.push_back(json::array({ m.shape, m.startMS, m.endMS }));
        }
        // Only the times of the words: the text itself stays in the lyrics session.
        list.push_back({ { "id", i.id }, { "activity", i.activityId }, { "track", i.track }, { "startMS", i.startMS }, { "endMS", i.endMS }, { "emission", i.emission },
                         { "confidence", i.confidence }, { "words", i.wordIds.size() }, { "mouth", mouth }, { "relations", i.relations }, { "provenance", i.provenance },
                         { "evidence", i.evidence } });
    }
    return json({ { "kind", "composer-vocal-understanding" }, { "schemaVersion", v.schemaVersion }, { "tracks", tracks }, { "interventions", list }, { "limits", v.limits },
                  { "warnings", v.warnings } })
        .dump(1);
}

bool FromJson(const std::string& text, VocalUnderstanding& out, std::string& error) {
    try {
        const json root = json::parse(text);
        if (root.value("schemaVersion", 0) < 1 || root.value("schemaVersion", 0) > VOCAL_UNDERSTANDING_SCHEMA_VERSION) {
            error = "voices: schema not read";
            return false;
        }
        VocalUnderstanding v;
        for (const json& t : root.value("tracks", json::array())) {
            VoiceTrack k;
            k.id = t.value("id", std::string());
            k.label = t.value("label", std::string());
            k.kind = t.value("kind", std::string());
            k.emission = t.value("emission", std::string());
            k.source = t.value("source", std::string());
            k.medianF0 = t.value("medianF0", 0.0);
            k.confidence = t.value("confidence", 0.0);
            k.totalMS = t.value("totalMS", 0);
            k.evidence = t.value("evidence", std::vector<std::string>());
            v.tracks.push_back(std::move(k));
        }
        for (const json& i : root.value("interventions", json::array())) {
            Intervention k;
            k.id = i.value("id", std::string());
            k.activityId = i.value("activity", std::string());
            k.track = i.value("track", std::string());
            k.startMS = i.value("startMS", 0);
            k.endMS = i.value("endMS", 0);
            k.emission = i.value("emission", std::string());
            k.confidence = i.value("confidence", 0.0);
            for (const json& m : i.value("mouth", json::array())) {
                if (m.is_array() && m.size() == 3) {
                    k.mouth.push_back({ m[0].get<std::string>(), m[1].get<int>(), m[2].get<int>() });
                }
            }
            k.relations = i.value("relations", std::vector<std::string>());
            k.provenance = i.value("provenance", std::string());
            k.evidence = i.value("evidence", std::vector<std::string>());
            v.interventions.push_back(std::move(k));
        }
        v.limits = root.value("limits", std::vector<std::string>());
        v.warnings = root.value("warnings", std::vector<std::string>());
        out = std::move(v);
    } catch (const json::exception& e) {
        error = std::string("voices: ") + e.what();
        return false;
    }
    return true;
}

} // namespace composer
