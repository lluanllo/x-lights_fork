#include "composer/LearningTest.h"

#include "composer/ArtisticPreferences.h"
#include "composer/GenerationSidecar.h"
#include "composer/MediaAnalysis.h"
#include "composer/MediaStoryboard.h"
#include "composer/MediaValidation.h"
#include "composer/VisualModel.h"
#include "composer/VisualReview.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace composer {

namespace {

std::string S(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

class Suite {
public:
    explicit Suite(fs::path root) : _root(std::move(root)) {}

    int Run() {
        TestReview();
        TestSidecar();
        TestLabels();
        TestEvaluation();
        TestLearning();
        printf("composer-learning-test: %d passed, %d failed (%s)\n", _passed, _failed, S(_root).c_str());
        return _failed == 0 ? 0 : 1;
    }

private:
    fs::path _root;
    int _passed = 0;
    int _failed = 0;

    void Check(bool ok, const std::string& what) {
        printf("  %s %s\n", ok ? "OK   " : "FAILED", what.c_str());
        fflush(stdout);
        ok ? ++_passed : ++_failed;
    }

    static void Title(const char* t) {
        printf("=== %s\n", t);
    }

    // A small montage: two screens, three scenes, five shots.
    static VisualMediaStoryboard Board() {
        VisualMediaStoryboard b;
        b.hash = "b1";
        const auto scene = [&](const char* id, int a, int z, const char* kind, const char* role, const char* key) {
            StoryboardScene s;
            s.id = id;
            s.startMS = a;
            s.endMS = z;
            s.sectionKind = kind;
            s.role = role;
            s.key = key;
            b.scenes.push_back(s);
        };
        scene("s0.0", 0, 10000, "intro", "establish", "intro#1.1");
        scene("s1.0", 10000, 20000, "chorus", "chorus", "chorus#1.1");
        scene("s2.0", 20000, 30000, "verse", "verse", "verse#1.1");
        const auto shot = [&](const char* scene, const char* lane, int a, int z, const char* media, MediaKind kind, const char* framing, const char* transition, const char* seen) {
            StoryboardShot s;
            s.sceneId = scene;
            s.lane = lane;
            s.startMS = a;
            s.endMS = z;
            s.mediaId = media;
            s.name = std::string(media) + (kind == MediaKind::Video ? ".mp4" : ".png");
            s.kind = kind;
            s.effect = kind == MediaKind::Video ? "Video" : "Pictures";
            s.source = s.name;
            s.framing = framing;
            s.transition = transition;
            s.reasons = { { "content", std::string(seen) + " (semantic)" } };
            s.id = fmt::format("{}/{}/0", scene, lane);
            b.shots.push_back(s);
        };
        shot("s0.0", "Matriz", 0, 10000, "m:a", MediaKind::Image, "zoom-in", "fade", "snowflakes");
        shot("s1.0", "Matriz", 10000, 15000, "m:b", MediaKind::Video, "video-crop", "cut", "fireworks");
        shot("s1.0", "Arbol", 10000, 20000, "m:c", MediaKind::Image, "tree-faces", "cut", "christmas-tree");
        shot("s2.0", "Matriz", 20000, 30000, "m:d", MediaKind::Video, "video-crop", "crossfade", "stars");
        shot("s2.0", "Arbol", 20000, 30000, "m:e", MediaKind::Image, "pan-x", "fade", "santa");
        b.shots[1].id = "s1.0/Matriz/0";
        for (StoryboardShot& s : b.shots) {
            const StoryboardScene* sc = b.Scene(s.sceneId);
            s.decisionId = fmt::format("{}/{}/1", s.lane, sc->key);
        }
        return b;
    }

    void TestReview() {
        Title("a review of the pictures and videos");
        const VisualMediaStoryboard b = Board();
        VisualReview r = BuildReview(b, "show.xsq", "spec1", nullptr);
        const ReviewCounts n0 = CountReview(r);
        Check(r.clips.size() == 5 && n0.pending == 5 && r.state == "pending", "a new review: every clip pending, nothing approved");
        Check(r.Find("Matriz/chorus#1.1/1") != nullptr && r.Find("Matriz/chorus#1.1/1")->seen.front() == "fireworks" && r.Find("Matriz/chorus#1.1/1")->transition == "cut",
              "each clip says where, what it shows, its movement and how it comes in");
        Check(DecideClip(r, "Matriz/intro#1.1/1", ClipState::Approved, {}, "", "t1") && DecideClip(r, "Matriz/chorus#1.1/1", ClipState::Rejected, { "not-music", "made-up" }, "too loud", "t1"),
              "a clip approved, another rejected with a reason and a note");
        Check(r.Find("Matriz/chorus#1.1/1")->flags == std::vector<std::string>{ "not-music" } && r.state == "in-progress", "only known reasons are kept; the review is in progress");
        std::string why;
        Check(!ApproveShow(r, "Juan", "t2", why) && why == "checklist-incomplete" && r.state != "approved", "the show cannot be approved before the person says what they checked");
        r.checklist = { true, true, true, true, false };
        Check(!ApproveShow(r, "Juan", "t2", why) && r.state != "approved", "four steps of five is a partial review: never an approval");
        r.checklist.finished = true;
        Check(!ApproveShow(r, "Juan", "t2", why) && why == "corrections-not-applied", "not while a clip is rejected and not yet replaced");
        Check(!ApproveShow(r, "", "t2", why) && why != "", "not without the reviewer's name");

        const std::vector<MediaCorrection> cs = ReviewCorrections(r, b);
        const auto find = [&](const std::string& d) {
            const auto it = std::find_if(cs.begin(), cs.end(), [&](const MediaCorrection& c) { return c.decisionId == d; });
            return it != cs.end() ? &*it : nullptr;
        };
        const MediaCorrection* ok = find("Matriz/intro#1.1/1");
        const MediaCorrection* no = find("Matriz/chorus#1.1/1");
        const MediaCorrection* hold = find("Arbol/verse#1.1/1");
        Check(ok != nullptr && ok->action == MediaCorrection::Action::Lock && ok->origin == "person" && ok->mediaId == "m:a", "an approved clip is kept exactly, as the person's");
        Check(no != nullptr && no->action == MediaCorrection::Action::Avoid && no->mediaId == "m:b", "a rejected clip only avoids its file there: the rest is not made again");
        Check(hold != nullptr && hold->action == MediaCorrection::Action::Lock && hold->origin == "review", "a clip nobody judged stays as it was, held by the review");
        std::vector<MediaCorrection> released = cs;
        ReleaseReviewHolds(released);
        Check(std::none_of(released.begin(), released.end(), [](const MediaCorrection& c) { return c.origin == "review"; }) && released.size() == 2, "\"Rebuild\" lets the review's holds go and keeps the person's");

        Check(ReplaceClipMedia(r, "Matriz/chorus#1.1/1", "m:x", "x.png", "t3"), "a file chosen to replace the rejected one");
        const std::vector<MediaCorrection> cs2 = ReviewCorrections(r, b);
        const auto rep = std::find_if(cs2.begin(), cs2.end(), [](const MediaCorrection& c) { return c.decisionId == "Matriz/chorus#1.1/1"; });
        Check(rep != cs2.end() && rep->action == MediaCorrection::Action::Lock && rep->mediaId == "m:x" && rep->shotStartMS == 10000, "replaced in its place, the same shot");

        // The next version of the show: the same decisions keep what the person said.
        VisualMediaStoryboard b2 = b;
        b2.shots[1].mediaId = "m:x";
        b2.shots[4].mediaId = "m:z"; // the planner changed this one
        VisualReview r2 = BuildReview(b2, "show-2.xsq", "spec2", &r);
        Check(r2.Find("Matriz/intro#1.1/1")->state == ClipState::Approved && r2.Find("Arbol/verse#1.1/1")->state == ClipState::Pending &&
                  r2.Find("Matriz/chorus#1.1/1")->state == ClipState::Pending,
              "a new version keeps the approvals of the same decisions; a changed or replaced shot is to be seen again");
        Check(r2.state == "in-progress" && !r2.checklist.opened, "another show is reviewed again: the checklist starts over");
        r2.checklist = { true, true, true, true, true };
        Check(ApproveShow(r2, "Juan", "t4", why) && r2.state == "approved", "with everything checked and nothing rejected, the person approves");
        Check(CountReview(r2).pending > 0 && r2.Find("Arbol/verse#1.1/1")->state == ClipState::Pending, "approving the show does not approve clips nobody judged");
        const VisualReview same = BuildReview(b2, "show-2.xsq", "spec2", &r2);
        const VisualReview other = BuildReview(b2, "show-3.xsq", "spec3", &r2);
        Check(same.state == "approved" && other.state != "approved", "an approval holds for the same show only");

        std::string error;
        VisualReview back;
        Check(VisualReviewFrom(VisualReviewJson(r2), back, error) && back.state == "approved" && back.clips.size() == 5 && back.Find("Matriz/intro#1.1/1")->state == ClipState::Approved,
              "the review reads back as it was");
        json forged = VisualReviewJson(r);
        forged["state"] = "approved";
        Check(VisualReviewFrom(forged, back, error) && back.state != "approved", "a file claiming approval without the checklist is not trusted");
        VisualReview legacy;
        Check(LegacyReviewFrom(json{ { "schemaVersion", 1 }, { "reviewedBy", "Juan" }, { "reviewedAt", "2026-11-07 20:00" }, { "state", "approved" } }, legacy) && legacy.state == "approved" && legacy.legacy,
              "a Phase 8 approval record is still read");
        DecideClip(r2, "Arbol/verse#1.1/1", ClipState::Rejected, {}, "", "t5");
        Check(r2.state == "in-progress" && r2.approvedAt.empty(), "changing a decision after approving opens the review again");
    }

    void TestSidecar() {
        Title("the sidecar keeps the review, and old ones still read");
        GenerationSidecar s;
        s.sequenceFile = "show.xsq";
        s.audioFile = "song.mp3";
        s.audioHash = "abc";
        s.review = BuildReview(Board(), "show.xsq", "spec1", nullptr);
        DecideClip(s.review, "Matriz/intro#1.1/1", ClipState::Approved, {}, "", "t");
        const std::string text = ToJson(s);
        const fs::path file = _root / "show.composer.json";
        std::ofstream(file, std::ios::binary) << text;
        GenerationSidecar read;
        std::string error;
        Check(ReadSidecar(S(file), read, error) == SidecarStatus::Ok && read.review.Find("Matriz/intro#1.1/1") != nullptr &&
                  read.review.Find("Matriz/intro#1.1/1")->state == ClipState::Approved && read.schemaVersion == 2,
              "written, closed and read again: the review is there (schema 2)");
        json v1 = json::parse(text);
        v1["schemaVersion"] = 1;
        v1.erase("review");
        GenerationSidecar old;
        Check(FromJson(v1.dump(), old, error) == SidecarStatus::Ok && old.review.Empty() && old.visualReview == "not-reviewed" && old.schemaVersion == 2,
              "a schema 1 sidecar reads, as a show nobody reviewed");
        json v3 = json::parse(text);
        v3["schemaVersion"] = 3;
        Check(FromJson(v3.dump(), old, error) == SidecarStatus::Newer, "a newer one is refused, not rewritten");
        CorrectionSet c;
        c.review = read.review;
        CorrectionSet c2;
        Check(FromJson(ToJson(c), c2, error) && c2.review.clips.size() == 5, "the review travels with the corrections to the next version");
    }

    static MediaItem Item(const std::string& id, std::vector<MediaConcept> concepts) {
        MediaItem m;
        m.id = id;
        m.name = id + ".png";
        m.path = "C:/x/" + m.name;
        m.kind = MediaKind::Image;
        m.analysis.mode = "semantic";
        m.analysis.model = "siglip@1";
        m.analysis.concepts = std::move(concepts);
        for (const MediaConcept& c : m.analysis.concepts) {
            if (m.analysis.description.size() < 3) {
                m.analysis.description.push_back(c.id);
            }
        }
        return m;
    }

    void TestLabels() {
        Title("a person's labels over the model's");
        MediaItem m = Item("m:holly", { { "portrait", 0.4, 0, "strong", "model" }, { "holly", 0.2, 0, "probable", "model" }, { "lights", 0.05, 0, "weak", "model" }, { "forest", 0.01, 0, "weak", "model" } });
        m.human.confirmed = { "holly" };
        m.human.rejected = { "portrait" };
        m.human.added = { "christmas-tree" };
        m.human.excluded = { "forest" };
        VisualConcepts concepts;
        std::string error;
        (void)ReadVisualConcepts(DefaultVisualConceptsPath(), concepts, error);
        const MediaAnalysis a = EffectiveAnalysis(m, &concepts);
        const auto has = [&](const std::string& id) { return std::any_of(a.concepts.begin(), a.concepts.end(), [&](const MediaConcept& c) { return c.id == id; }); };
        Check(!has("portrait") && !has("forest"), "a rejected or excluded concept is gone");
        Check(!a.concepts.empty() && a.concepts[0].id == "holly" && a.concepts[0].source == "person" && a.concepts[0].level == "strong" && has("christmas-tree") && has("lights"),
              "confirmed and added concepts come first and certain; the rest stays");
        Check(a.description.front() == "holly" && std::find(a.categories.begin(), a.categories.end(), "christmas") != a.categories.end(), "the description and categories follow the person");
        Check(m.analysis.concepts.size() == 4 && m.analysis.concepts[0].id == "portrait" && m.analysis.concepts[0].score == 0.4, "the model's own words are kept untouched");
        // Kept in the library, version 3, read back.
        MediaLibrary lib;
        m.human.notFor = { "tree" };
        m.human.complete = true;
        m.human.reviewer = "Juan";
        m.status = MediaStatus::Ok;
        lib.items.push_back(m);
        const fs::path file = _root / "media-library.json";
        Check(SaveMediaLibrary(S(file), lib, error), "the library with labels is saved");
        std::ifstream in(file, std::ios::binary);
        const json saved = json::parse(in, nullptr, false);
        MediaLibrary back;
        Check(saved.value("version", 0) == 3 && ReadMediaLibrary(S(file), back, error) && back.items.size() == 1 && back.items[0].human.confirmed == m.human.confirmed &&
                  back.items[0].human.notFor == std::vector<std::string>{ "tree" } && back.items[0].human.complete && back.items[0].analysis.concepts.size() == 4,
              "version 3 keeps the labels apart from the analysis");
        json v2 = saved;
        v2["version"] = 2;
        v2["items"][0].erase("human");
        std::ofstream(_root / "v2.json", std::ios::binary) << v2.dump();
        Check(ReadMediaLibrary(S(_root / "v2.json"), back, error) && back.items.size() == 1 && back.items[0].human.Empty(), "a version 2 library still reads");
        json v4 = saved;
        v4["version"] = 4;
        std::ofstream(_root / "v4.json", std::ios::binary) << v4.dump();
        Check(!ReadMediaLibrary(S(_root / "v4.json"), back, error), "a newer one is refused rather than its labels lost");
    }

    void TestEvaluation() {
        Title("the model measured against a person's labels");
        MediaLibrary lib;
        // Twelve labelled files, two of them the same picture: too few to be a measure.
        for (int i = 0; i < 12; ++i) {
            MediaItem m = Item(fmt::format("m:{}", i), { { "snowflakes", 0.4, 0, "strong", "model" }, { "stars", 0.1, 0, "probable", "model" }, { "santa", 0.01, 0, "weak", "model" } });
            m.status = MediaStatus::Ok;
            m.human.complete = i != 11; // the last one is half done: it does not count
            m.human.confirmed = { "snowflakes" };
            m.human.rejected = { "stars", "santa" };
            if (i % 3 == 0) {
                m.human.added = { "snowfall" };
            }
            if (i == 5) {
                m.analysis.issues = { "duplicate-of:m:4" };
            }
            lib.items.push_back(m);
        }
        lib.items[4].analysis.issues = {};
        const MediaValidationSet set = MediaValidationSetFromLibrary(lib);
        const MediaValidationMetrics m = EvaluateMediaValidation(set, "person", 3);
        Check(set.entries.size() == 11, "only files a person labelled completely are in the set");
        Check(m.files == 10 && m.duplicatesSkipped == 1, "a duplicate counts once: it would otherwise weigh twice");
        // Per file: snowflakes right; stars and santa wrong; snowfall missed on 0, 3, 6, 9.
        Check(m.truePositive == 10 && m.falsePositive == 20 && m.falseNegative == 4, fmt::format("right {}, wrong {}, missed {}", m.truePositive, m.falsePositive, m.falseNegative));
        const double p = 10.0 / 30, r = 10.0 / 14;
        Check(std::fabs(m.precision - p) < 1e-9 && std::fabs(m.recall - r) < 1e-9 && std::fabs(m.f1 - 2 * p * r / (p + r)) < 1e-9, "precision, recall and F1");
        Check(m.confidentRight == 10 && m.confidentWrong == 10, "the strong and probable concepts on their own");
        Check(m.confusion.contains({ "stars", "snowfall" }), "which concept stood for which");
        Check(!m.reliable && MediaValidationReport(set, { m }).find("TOO FEW FILES") != std::string::npos, "ten files are said to be too few for a measure");
        for (int i = 12; i < 32; ++i) {
            MediaItem x = Item(fmt::format("m:{}", i), { { "snowflakes", 0.4, 0, "strong", "model" } });
            x.status = MediaStatus::Ok;
            x.human.complete = true;
            x.human.confirmed = { "snowflakes" };
            lib.items.push_back(x);
        }
        const MediaValidationMetrics big = EvaluateMediaValidation(MediaValidationSetFromLibrary(lib), "person", 3);
        Check(big.files >= MIN_VALIDATION_FILES && big.reliable, "with enough files it is a measure");
        Check(MediaValidationReport(MediaValidationSetFromLibrary(lib), { big }).find("(few files)") != std::string::npos, "a concept with few files is still marked as such");
        Check(lib.items[0].analysis.concepts[1].id == "stars", "measuring changes nothing in the library");
    }

    void TestLearning() {
        Title("what the maker likes, learned from reviews");
        ArtisticPreferences prefs;
        Check(ComputePreferences(prefs).empty() && EffectivePreferences(prefs).empty(), "nothing learned without decisions");
        const VisualMediaStoryboard b = Board();
        VisualReview r = BuildReview(b, "uno.xsq", "s1", nullptr);
        Check(LearnFromReview(prefs, r, nullptr, "winter") == 0 && prefs.signals.empty(), "a review with nothing decided teaches nothing: no decision is not a rejection");
        DecideClip(r, "Matriz/chorus#1.1/1", ClipState::Rejected, {}, "", "t");
        Check(LearnFromReview(prefs, r, nullptr, "winter") > 0, "a rejection is a signal");
        const auto value = [&](const std::string& key) {
            for (const Preference& p : ComputePreferences(prefs)) {
                if (p.key == key) {
                    return p;
                }
            }
            return Preference();
        };
        Check(value("file.m:b@Matriz").decisions == 1 && !value("file.m:b@Matriz").active && !EffectivePreferences(prefs).contains("file.m:b@Matriz"),
              "rejected once, a file is not banned: one decision is not enough to act");
        Check(LearnFromReview(prefs, r, nullptr, "winter") == 0 && value("file.m:b@Matriz").decisions == 1, "learning the same review again counts nothing twice");
        VisualReview r2 = BuildReview(b, "dos.xsq", "s2", nullptr);
        DecideClip(r2, "Matriz/chorus#1.1/1", ClipState::Rejected, {}, "", "t");
        LearnFromReview(prefs, r2, nullptr, "winter");
        const Preference f = value("file.m:b@Matriz");
        Check(f.active && f.value < 0 && f.decisions == 2 && EffectivePreferences(prefs).contains("file.m:b@Matriz"), fmt::format("rejected in two shows on that screen, it weighs ({:.2f})", f.value));
        Check(!EffectivePreferences(prefs).contains("file.m:b@Arbol"), "on another screen it is not touched");
        // Global and screen-specific stay apart.
        DecideClip(r2, "Arbol/chorus#1.1/1", ClipState::Rejected, { "wrong-screen" }, "", "t");
        LearnFromReview(prefs, r2, nullptr, "winter");
        Check(value("screen.Arbol.concept.christmas-tree").decisions == 1 && value("concept.christmas-tree").decisions == 0, "\"wrong screen\" teaches about that screen, not about the concept everywhere");
        // A changed decision replaces its signal.
        DecideClip(r2, "Arbol/chorus#1.1/1", ClipState::Approved, {}, "", "t");
        LearnFromReview(prefs, r2, nullptr, "winter");
        Check(value("screen.Arbol.concept.christmas-tree").decisions == 0 && value("concept.christmas-tree").decisions == 1, "a changed decision changes what was learned, never doubles it");
        DecideClip(r2, "Arbol/chorus#1.1/1", ClipState::Pending, {}, "", "t");
        LearnFromReview(prefs, r2, nullptr, "winter");
        Check(value("concept.christmas-tree").decisions == 0, "a decision taken back takes its signal back");
        // Bounds: many agreeing decisions approach, never pass, the limit; one weighs at most one.
        ArtisticPreferences many;
        for (int i = 0; i < 60; ++i) {
            VisualReview x = BuildReview(b, fmt::format("show{}.xsq", i), "s", nullptr);
            DecideClip(x, "Arbol/verse#1.1/1", ClipState::Approved, {}, "", "t");
            LearnFromReview(many, x, nullptr, "");
        }
        const Preference moving = [&] {
            for (const Preference& p : ComputePreferences(many)) {
                if (p.key == "motion.moving") {
                    return p;
                }
            }
            return Preference();
        }();
        Check(moving.active && moving.value > 0.5 && moving.value < 1.0, fmt::format("sixty approvals of a slow pan: a strong, bounded taste for movement ({:.3f})", moving.value));
        Check(std::all_of(many.signals.begin(), many.signals.end(), [](const PreferenceSignal& s) { return s.weight <= 1.0 && std::fabs(s.sign) <= 1.0; }), "no single decision weighs more than one");
        // Recent decisions weigh more, old ones never vanish.
        ArtisticPreferences recency;
        VisualReview old = BuildReview(b, "old.xsq", "s", nullptr);
        DecideClip(old, "Arbol/verse#1.1/1", ClipState::Approved, {}, "", "t");
        LearnFromReview(recency, old, nullptr, "");
        for (int i = 0; i < 30; ++i) {
            VisualReview x = BuildReview(b, fmt::format("later{}.xsq", i), "s", nullptr);
            DecideClip(x, "Matriz/intro#1.1/1", ClipState::Approved, {}, "", "t");
            LearnFromReview(recency, x, nullptr, "");
        }
        const auto support = [&](const std::string& origin) {
            for (const PreferenceSignal& s : recency.signals) {
                if (s.origin == origin && s.key == "motion.moving") {
                    return recency.session - s.session;
                }
            }
            return -1;
        };
        const Preference t = [&] {
            for (const Preference& p : ComputePreferences(recency)) {
                if (p.key == "transition.fade") {
                    return p;
                }
            }
            return Preference();
        }();
        Check(support("old.xsq|Arbol/verse#1.1/1") == 30 && t.support < 30 * 0.5 && t.support > 30 * 0.5 * 0.25, fmt::format("an old review weighs less but is never forgotten (fade backed {:.2f})", t.support));
        // Determinism.
        ArtisticPreferences again;
        for (int i = 0; i < 60; ++i) {
            VisualReview x = BuildReview(b, fmt::format("show{}.xsq", i), "s", nullptr);
            DecideClip(x, "Arbol/verse#1.1/1", ClipState::Approved, {}, "", "t");
            LearnFromReview(again, x, nullptr, "");
        }
        Check(PreferencesJson(again).dump() == PreferencesJson(many).dump(), "the same reviews always teach the same");
        // A person's control.
        SetPreference(prefs, "kind.video", 0.6);
        Check(EffectivePreferences(prefs).at("kind.video") == 0.6, "a value set by hand is used as given");
        EnablePreference(prefs, "file.m:b@Matriz", false);
        Check(!EffectivePreferences(prefs).contains("file.m:b@Matriz") && value("file.m:b@Matriz").decisions == 2, "a preference turned off is kept but not used");
        EnablePreference(prefs, "file.m:b@Matriz", true);
        Check(EffectivePreferences(prefs).contains("file.m:b@Matriz"), "and turned on again");
        prefs.learning = false;
        const auto off = EffectivePreferences(prefs);
        Check(off.size() == 1 && off.contains("kind.video"), "learning off: only the values set by hand");
        prefs.learning = true;
        Check(ForgetReview(prefs, "dos.xsq") > 0 && !EffectivePreferences(prefs).contains("file.m:b@Matriz") && value("file.m:b@Matriz").decisions == 1, "one review forgotten, its signals gone");
        ResetPreference(prefs, "kind.video");
        Check(!EffectivePreferences(prefs).contains("kind.video") && value("kind.video").decisions == 0, "one preference reset");
        std::string error;
        const fs::path file = _root / "ShowComposer" / "preferences.json";
        ArtisticPreferences read;
        Check(SavePreferences(S(file), prefs, error) && ReadPreferences(S(file), read, error) && PreferencesJson(read).dump() == PreferencesJson(prefs).dump(), "kept with the show and read back");
        Check(ReadPreferences(S(_root / "none.json"), read, error) && read.signals.empty(), "no file: nothing learned, no error");
        ResetAllPreferences(prefs);
        Check(prefs.signals.empty() && prefs.overrides.empty() && prefs.learning, "everything reset");
        // A dark file turned down twice: dark pictures weigh less.
        MediaLibrary lib;
        MediaItem dark = Item("m:b", { { "fireworks", 0.3, 0, "strong", "model" } });
        dark.analysis.dark = 0.8;
        dark.analysis.brightness = 0.1;
        lib.items.push_back(dark);
        ArtisticPreferences d;
        for (const char* name : { "a.xsq", "b.xsq" }) {
            VisualReview x = BuildReview(b, name, "s", nullptr);
            DecideClip(x, "Matriz/chorus#1.1/1", ClipState::Rejected, {}, "", "t");
            LearnFromReview(d, x, &lib, "");
        }
        Check(EffectivePreferences(d).contains("light.dark") && EffectivePreferences(d).at("light.dark") < 0, "dark videos turned down twice: a taste against dark ones");
        // Approved in two shows, a file is wanted there, whatever the general taste says.
        ArtisticPreferences kept;
        for (const char* name : { "c.xsq", "e.xsq" }) {
            VisualReview x = BuildReview(b, name, "s", nullptr);
            DecideClip(x, "Matriz/chorus#1.1/1", ClipState::Approved, {}, "", "t");
            LearnFromReview(kept, x, &lib, "");
        }
        Check(EffectivePreferences(kept).contains("file.m:b@Matriz") && EffectivePreferences(kept).at("file.m:b@Matriz") > 0, "a file approved in two shows is wanted on its screen");
    }
};

} // namespace

int RunReviewCommand(const std::string& showDir, const std::string& sequencePath, const std::string& reportDir, const std::string& decisionsFile, const std::string& correctionsOut) {
    std::string error;
    GenerationSidecar sidecar;
    const std::string sidecarPath = SidecarPathFor(sequencePath);
    if (ReadSidecar(sidecarPath, sidecar, error) != SidecarStatus::Ok) {
        printf("composer-review: the sidecar could not be read: %s\n", error.c_str());
        return 1;
    }
    VisualMediaStoryboard board;
    {
        std::ifstream in(fs::path(std::u8string(reportDir.begin(), reportDir.end())) / "storyboard.json", std::ios::binary);
        const json j = json::parse(in, nullptr, false);
        if (!j.is_object() || !StoryboardFrom(j, board, error)) {
            printf("composer-review: no storyboard in the report: %s\n", error.c_str());
            return 1;
        }
    }
    std::ifstream din(fs::path(std::u8string(decisionsFile.begin(), decisionsFile.end())), std::ios::binary);
    const json d = json::parse(din, nullptr, false);
    if (!d.is_object()) {
        printf("composer-review: the decisions file is not JSON\n");
        return 2;
    }
    MediaLibrary library;
    const bool haveLibrary = ReadMediaLibrary(MediaLibraryPath(showDir), library, error);
    const std::string author = d.value("author", std::string());
    VisualReview& review = sidecar.review;
    int applied = 0;
    for (const json& c : d.value("clips", json::array())) {
        std::string id = c.value("decision", std::string());
        if (id.empty()) {
            const std::string file = c.value("file", std::string());
            const std::string lane = c.value("lane", std::string());
            for (const ClipReview& k : review.clips) {
                if (id.empty() && k.name == file && (lane.empty() || k.lane == lane)) {
                    id = k.decisionId;
                }
            }
        }
        ClipState state;
        if (id.empty() || review.Find(id) == nullptr || !ClipStateFromId(c.value("state", std::string("pending")), state)) {
            printf("composer-review: skipped %s\n", c.dump().c_str());
            continue;
        }
        const std::string note = c.value("note", std::string());
        DecideClip(review, id, state, c.value("flags", std::vector<std::string>()), author.empty() ? note : "[" + author + "] " + note, author);
        const std::string replace = c.value("replace", std::string());
        if (!replace.empty() && haveLibrary) {
            const MediaItem* m = library.Find(replace);
            for (const MediaItem& x : library.items) {
                m = m == nullptr && x.name == replace ? &x : m;
            }
            if (m != nullptr) {
                ReplaceClipMedia(review, id, m->id, m->relative.empty() ? m->path : m->relative, author);
            }
        }
        ++applied;
    }
    // Never an approval from a file: the state is in progress at most.
    if (review.state == "approved" && !review.checklist.Complete()) {
        review.state = "in-progress";
    }
    {
        std::ofstream out(fs::path(std::u8string(sidecarPath.begin(), sidecarPath.end())), std::ios::binary | std::ios::trunc);
        out << ToJson(sidecar);
    }
    ArtisticPreferences prefs;
    (void)ReadPreferences(PreferencesPath(showDir), prefs, error);
    const int learned = LearnFromReview(prefs, review, haveLibrary ? &library : nullptr, sidecar.profileId);
    (void)SavePreferences(PreferencesPath(showDir), prefs, error);
    CorrectionSet next;
    next.media = sidecar.media.corrections;
    for (const MediaCorrection& m : ReviewCorrections(review, board)) {
        UpsertMediaCorrection(next.media, m);
    }
    next.review = review;
    next.screens = sidecar.media.screens;
    {
        std::ofstream out(fs::path(std::u8string(correctionsOut.begin(), correctionsOut.end())), std::ios::binary | std::ios::trunc);
        out << ToJson(next);
    }
    const ReviewCounts n = CountReview(review);
    printf("composer-review: %d decisions applied; %d shots: %d approved, %d rejected, %d waiting for a fix, %d pending; %d signals learned; show %s\n", applied, n.clips, n.approved, n.rejected,
           n.needsFix, n.pending, learned, review.state == "approved" ? "approved" : "NOT approved (only a person approves, in the window)");
    for (const Preference& p : ComputePreferences(prefs)) {
        printf("  preference %s: learned %.2f, %d decisions, %s\n", p.key.c_str(), p.learned, p.decisions, p.active ? "in use" : "not yet backed enough");
    }
    return 0;
}

int RunLearningTest(const std::string& workDir) {
    fs::path root;
    std::error_code ec;
    for (int i = 0; i < 1000; ++i) {
        root = fs::path(std::u8string(workDir.begin(), workDir.end())) / fmt::format("composer-learning-test-{}", i);
        if (!fs::exists(root, ec)) {
            break;
        }
    }
    fs::create_directories(root, ec);
    if (ec) {
        printf("composer-learning-test: cannot create the folder\n");
        return 1;
    }
    Suite s(root);
    return s.Run();
}

} // namespace composer
