#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

#include "composer/ChoreographyTest.h"
#include "composer/MusicAssist.h"
#include "composer/VisualDirection.h"

#include "composer/Choreography.h"
#include "composer/CompositionPlan.h"
#include "composer/GenerationValidator.h"
#include "composer/LyricAligner.h"
#include "composer/LyricDocument.h"
#include "composer/ModelCapabilities.h"
#include "composer/PlanTest.h"
#include "composer/SequenceWriter.h"
#include "composer/TemplateLibrary.h"
#include "composer/WriterTest.h"
#include "effects/EffectManager.h"
#include "render/HeadlessRenderContext.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace composer {

namespace {

constexpr const char* LYRIC_TRACK = "Letra Composer";
constexpr int FRAME = 25;

long long ElapsedMS(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

bool WriteText(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

bool ReadText(const fs::path& path, std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream content;
    content << in.rdbuf();
    text = content.str();
    return true;
}

std::string TemplatesDir(const std::string& given) {
    return given.empty() ? DefaultTemplateDirectory() : given;
}

LyricAlignment Align(const LyricDocument& doc, const std::vector<std::pair<int, int>>& times) {
    AlignerInput in;
    in.document = &doc;
    size_t k = 0;
    for (const LyricLine* line : doc.Lines()) {
        if (k >= times.size()) {
            break;
        }
        LyricCorrection c;
        c.targetId = line->id;
        c.startMS = times[k].first;
        c.endMS = times[k].second;
        in.corrections.push_back(c);
        ++k;
    }
    return AlignLyrics(in);
}

class Suite {
public:
    Suite(const fs::path& root, const ChoreographyTestOptions& options) :
        _root(root),
        _options(options) {}

    int Run() {
        if (!Setup()) {
            return 1;
        }
        TestTemplates();
        TestCompatibility();
        TestRoles();
        TestLyricsAndFaces();
        TestMusic();
        TestVariety();
        TestLevels();
        TestReproducibility();
        TestCorrections();
        TestValidator();
        TestProperties();
        TestSequenceWriter();
        TestEnsemble();
        TestVoiceFaces();
        WriteOutputs();
        TestRealLayout();
        printf("composer-choreography-test: %d checks passed, %d failed (%s)\n", _passed, _failed, _root.string().c_str());
        printf("composer-choreography-test: no template is visually validated; how the animation looks needs a person\n");
        return _failed == 0 ? 0 : 1;
    }

private:
    void Check(bool ok, const std::string& what) {
        printf("  %s %s\n", ok ? "OK   " : "FAILED", what.c_str());
        fflush(stdout);
        ok ? ++_passed : ++_failed;
    }

    void Title(const char* title) {
        printf("=== %s\n", title);
        fflush(stdout);
    }

    bool Setup() {
        _fixture = _root / "fixture";
        if (!WritePlanFixture(_fixture.string())) {
            printf("composer-choreography-test: cannot write the fixture\n");
            return false;
        }
        _ctx = std::make_unique<HeadlessRenderContext>();
        if (!_ctx->LoadShowFolder(_fixture.string())) {
            printf("composer-choreography-test: cannot load the fixture\n");
            return false;
        }
        RoleOverrides roles;
        roles.models.push_back({ "Excluido", "", {}, true, "test" });
        _caps = ExtractCapabilities(*_ctx, &roles);
        _metadata = MetadataFrom(_ctx->GetEffectManager());
        _templatesDir = TemplatesDir(_options.templatesDir);
        auto start = std::chrono::steady_clock::now();
        _library = LoadTemplateLibrary(_templatesDir, _metadata);
        _loadMS = ElapsedMS(start);
        _song = PlanTestSong();
        _doc = ParsePlainLyrics(PlanTestLyrics());
        _alignment = Align(_doc, { { 15000, 19000 }, { 20000, 24000 }, { 31000, 35000 }, { 36000, 40000 } });
        _plans[0] = MakePlan(_song, AutomationLevel::Conservative);
        _plans[1] = MakePlan(_song, AutomationLevel::Balanced);
        _plans[2] = MakePlan(_song, AutomationLevel::Creative);
        return true;
    }

    CompositionPlan MakePlan(const SongAnalysis& song, AutomationLevel level, uint64_t seed = 7, bool lyrics = true) const {
        PlannerInput in;
        in.analysis = &song;
        in.capabilities = &_caps;
        in.options.level = level;
        in.options.seed = seed;
        if (lyrics) {
            in.lyrics = &_doc;
            in.alignment = &_alignment;
        }
        return PlanComposition(in);
    }

    Choreography Make(const CompositionPlan& plan, const SongAnalysis& song, bool lyrics = true, std::vector<ChoreographyCorrection> corrections = {}, const LyricAlignment* alignment = nullptr) const {
        ChoreographyInput in;
        in.plan = &plan;
        in.capabilities = &_caps;
        in.library = &_library;
        in.analysis = &song;
        if (lyrics) {
            in.alignment = alignment != nullptr ? alignment : &_alignment;
            in.lyricTrack = LYRIC_TRACK;
        }
        in.corrections = std::move(corrections);
        return Choreograph(in);
    }

    ValidationReport Validate(const Choreography& c, const CompositionPlan& plan) const {
        return ValidateChoreography(c, plan, _caps, _library, _metadata);
    }

    const ChoreographyEvent* First(const Choreography& c, const std::string& target) const {
        for (const ChoreographyEvent& e : c.events) {
            if (e.target == target) {
                return &e;
            }
        }
        return nullptr;
    }

    // ------------------------------------------------------------ templates

    void TestTemplates() {
        Title("Template library");
        const auto starters = std::count_if(_library.templates.begin(), _library.templates.end(), [](const EffectTemplate& t) { return t.provenance == "neutral-starter"; });
        const auto ensemble = std::count_if(_library.templates.begin(), _library.templates.end(), [](const EffectTemplate& t) { return t.provenance == "reference-derived"; });
        Check(_library.rejected.empty() && starters == 18 && ensemble == 13 && _library.templates.size() == 31,
              fmt::format("1. {} starter and {} ensemble templates load and validate against the effects' metadata ({} rejected) in {} ms", starters, ensemble, _library.rejected.size(), _loadMS));
        for (const RejectedTemplate& r : _library.rejected) {
            printf("    rejected %s: %s\n", r.file.c_str(), r.errors.empty() ? "" : r.errors[0].c_str());
        }
        const EffectTemplate* spirals = _library.Find("tree-spirals");
        bool keys = spirals != nullptr;
        if (spirals != nullptr) {
            for (const TemplateParameter& p : spirals->parameters) {
                keys = keys && ((p.id == "Spirals_Rotation" && p.key == "E_SLIDER_Spirals_Rotation") || (p.id == "Spirals_Movement" && p.key == "E_TEXTCTRL_Spirals_Movement") ||
                                (p.id == "Spirals_Count" && p.key == "E_SLIDER_Spirals_Count") || (p.id == "Spirals_Thickness" && p.key == "E_SLIDER_Spirals_Thickness") ||
                                (p.id == "Spirals_Blend" && p.key == "E_CHECKBOX_Spirals_Blend"));
            }
        }
        Check(keys, "keys follow the effect panel: SLIDER prefix where the metadata says so, TEXTCTRL for divided floats");
        const EffectTemplate* on = _library.Find("steady-on");
        Check(on != nullptr && std::any_of(on->parameters.begin(), on->parameters.end(), [](const TemplateParameter& p) { return p.key == "E_TEXTCTRL_Eff_On_Start"; }), "On's start level keeps its TEXTCTRL prefix from the metadata");

        const fs::path bad = _root / "bad" / "templates";
        std::error_code ec;
        fs::create_directories(bad, ec);
        std::string palettes;
        (void)ReadText(fs::path(_templatesDir).parent_path() / "palettes.json", palettes);
        (void)WriteText(bad.parent_path() / "palettes.json", palettes);
        const auto base = [](const std::string& id, const std::string& effect, const std::string& parameters) {
            return fmt::format(R"({{"schemaVersion":1,"id":"{}","version":1,"effect":"{}","roles":["mega-tree"],"intents":["feature"],"palette":{{"colors":[1,2],"families":["warm"]}},"parameters":[{}]}})", id, effect, parameters);
        };
        (void)WriteText(bad / "a-not-json.json", "{ this is not json");
        (void)WriteText(bad / "b-no-effect.json", base("b", "Rainbow Dragon", ""));
        (void)WriteText(bad / "c-unknown-parameter.json", base("c", "Spirals", R"({"id":"Spirals_Dragons","value":3})"));
        (void)WriteText(bad / "d-out-of-range.json", base("d", "Spirals", R"({"id":"Spirals_Count","value":9})"));
        (void)WriteText(bad / "e-wrong-type.json", base("e", "Spirals", R"({"id":"Spirals_Blend","value":"yes"})"));
        (void)WriteText(bad / "f-not-an-option.json", base("f", "Bars", R"({"id":"Bars_Direction","choices":["sideways"]})"));
        (void)WriteText(bad / "g-runtime.json", base("g", "Faces", R"({"id":"Faces_FaceDefinition","value":"x"})"));
        (void)WriteText(bad / "h-version.json", R"({"schemaVersion":7,"id":"h","version":1,"effect":"Spirals","roles":["mega-tree"],"intents":["feature"]})");
        (void)WriteText(bad / "i-good.json", base("i", "Spirals", R"({"id":"Spirals_Count","energy":[1,3]})"));
        const TemplateLibrary rejected = LoadTemplateLibrary(bad.string(), _metadata);
        const auto why = [&](const char* file) {
            for (const RejectedTemplate& r : rejected.rejected) {
                if (r.file == file) {
                    return r.errors.empty() ? std::string("?") : r.errors.front();
                }
            }
            return std::string();
        };
        Check(rejected.templates.size() == 1 && rejected.templates[0].id == "i", fmt::format("only the valid one of 9 loads ({} rejected)", rejected.rejected.size()));
        Check(why("a-not-json.json") == "not valid JSON", "2. invalid JSON is rejected: " + why("a-not-json.json"));
        Check(why("b-no-effect.json").find("does not exist") != std::string::npos, "3. an effect xLights does not have: " + why("b-no-effect.json"));
        Check(why("c-unknown-parameter.json").find("is not a property") != std::string::npos, "4. an unknown parameter: " + why("c-unknown-parameter.json"));
        Check(why("d-out-of-range.json").find("outside") != std::string::npos, "5. a value out of range: " + why("d-out-of-range.json"));
        Check(why("e-wrong-type.json").find("true or false") != std::string::npos, "a value of the wrong type: " + why("e-wrong-type.json"));
        Check(why("f-not-an-option.json").find("not one of its options") != std::string::npos, "a choice the effect does not offer: " + why("f-not-an-option.json"));
        Check(!why("g-runtime.json").empty(), "a setting only filled at run time cannot be fixed: " + why("g-runtime.json"));
        Check(why("h-version.json").find("schemaVersion") != std::string::npos, "a newer template format is refused: " + why("h-version.json"));
    }

    void TestCompatibility() {
        Title("Compatibility");
        const EffectTemplate* spirals = _library.Find("tree-spirals");
        const EffectTemplate* text = _library.Find("matrix-lyric-text");
        const EffectTemplate* wash = _library.Find("color-wash");
        const EffectTemplate* picture = _library.Find("matrix-picture");
        Check(EvaluateCompatibility(*spirals, *_caps.Find("Arbol Grande")).level == Compatibility::Compatible, "6. spirals on a mega tree: compatible");
        const CompatibilityResult line = EvaluateCompatibility(*spirals, *_caps.Find("Linea 1"));
        Check(line.level == Compatibility::Incompatible && line.reasons[0] == "role", "7. spirals on a line: incompatible, by role");
        EffectTemplate probe = *wash;
        probe.roles = { Role::Figure };
        probe.required = { Capability::Text };
        Check(EvaluateCompatibility(probe, *_caps.Find("Sin Controlador")).level == Compatibility::Unknown, "8. text on a custom model: unknown, not incompatible");
        Check(EvaluateCompatibility(*text, *_caps.Find("Ventana Matrix 1")).level == Compatibility::Incompatible, "text on a window frame: incompatible");
        EffectTemplate small = *picture;
        small.minBufferWi = 64;
        small.minBufferHt = 64;
        const CompatibilityResult tiny = EvaluateCompatibility(small, *_caps.Find("Matriz Prueba"));
        Check(tiny.level == Compatibility::Limited && std::any_of(tiny.reasons.begin(), tiny.reasons.end(), [](const std::string& r) { return r.rfind("buffer-below", 0) == 0; }), "a picture on a matrix below the needed size: limited, with the reason");
        Check(EvaluateCompatibility(*wash, *_caps.Find("Sin Controlador/Brazo")).level == Compatibility::Compatible && EvaluateCompatibility(*spirals, *_caps.Find("Sin Controlador/Brazo")).level == Compatibility::Incompatible,
              "13. submodels take only templates made for them");
        Check(EvaluateCompatibility(*_library.Find("bars-sweep"), *_caps.Find("Grupo Arboles")).level == Compatibility::Compatible, "14. a group takes group templates");
        Check(EvaluateCompatibility(*_library.Find("face-singing"), *_caps.Find("Arbol Pequeno")).level == Compatibility::Incompatible, "faces need a face definition");
        Check(EvaluateCompatibility(*wash, *_caps.Find("Sin Controlador")).level == Compatibility::Compatible, "19. a model with no controller is a valid target");
    }

    // ------------------------------------------------------------ roles

    void TestRoles() {
        Title("Roles");
        const Choreography c = Make(_plans[1], _song);
        const auto fits = [&](const std::string& target) {
            const TargetInfo* t = _caps.Find(target);
            bool any = false;
            bool all = true;
            for (const ChoreographyEvent& e : c.events) {
                if (e.target != target) {
                    continue;
                }
                any = true;
                const EffectTemplate* tmpl = _library.Find(e.templateId);
                all = all && tmpl != nullptr && std::any_of(tmpl->roles.begin(), tmpl->roles.end(), [&](Role r) { return t->HasRole(r); });
            }
            return any && all;
        };
        Check(fits("Arbol Grande"), "9. mega tree: only tree techniques");
        Check(fits("Matriz Prueba"), "10. matrix: only matrix techniques");
        Check(fits("Ventana Matrix 1"), "11. window: only window techniques");
        Check(fits("Arco 1"), "12. arch: only arch techniques");
        Check(First(c, "Excluido") == nullptr && First(c, "Grupo Arcos") == nullptr, "20. the excluded model has no event, nor a group lighting it");
        bool group = false;
        for (uint64_t seed = 1; seed < 8 && !group; ++seed) {
            const CompositionPlan p = MakePlan(_song, AutomationLevel::Creative, seed);
            for (const ChoreographyEvent& e : Make(p, _song).events) {
                group = group || e.type == TargetType::Group;
            }
        }
        Check(group, "groups receive events of their own");
    }

    // ------------------------------------------------------------ lyrics and faces

    void TestLyricsAndFaces() {
        Title("Text and faces");
        const Choreography none = Make(_plans[1], _song, false);
        Check(std::none_of(none.events.begin(), none.events.end(), [](const ChoreographyEvent& e) { return e.effect == "Faces"; }) && std::any_of(none.warnings.begin(), none.warnings.end(), [](const std::string& w) { return w.rfind("face-without-synchronized-track:", 0) == 0; }),
              "15. without a synchronized track no face moves, and it is said");
        const Choreography sung = Make(_plans[1], _song);
        bool faces = false;
        bool facesRight = true;
        for (const ChoreographyEvent& e : sung.events) {
            if (e.effect == "Faces") {
                faces = true;
                const auto track = std::find_if(e.settings.begin(), e.settings.end(), [](const auto& kv) { return kv.first == "E_CHOICE_Faces_TimingTrack"; });
                facesRight = facesRight && track != e.settings.end() && track->second == LYRIC_TRACK && _caps.Find(e.target)->Can(Capability::Face).support == Support::Yes;
            }
        }
        Check(faces && facesRight, "16. synchronized: faces read the Phase 4 track, only on figures with a face");

        const LyricDocument words = ParsePlainLyrics("Hola\nVuelve la Navidad con amor\n");
        const LyricAlignment aligned = Align(words, { { 31000, 34000 }, { 36000, 41000 } });
        PlannerInput in;
        in.analysis = &_song;
        in.capabilities = &_caps;
        in.lyrics = &words;
        in.alignment = &aligned;
        in.options.seed = 7;
        const CompositionPlan plan = PlanComposition(in);
        const Choreography texts = Make(plan, _song, true, {}, &aligned);
        const ChoreographyEvent* shortText = nullptr;
        const ChoreographyEvent* longText = nullptr;
        for (const ChoreographyEvent& e : texts.events) {
            if (e.effect != "Text") {
                continue;
            }
            const auto value = std::find_if(e.settings.begin(), e.settings.end(), [](const auto& kv) { return kv.first == "E_TEXTCTRL_Text"; });
            if (value != e.settings.end() && value->second == "Hola") {
                shortText = &e;
            } else if (value != e.settings.end()) {
                longText = &e;
            }
        }
        const auto direction = [](const ChoreographyEvent* e) {
            const auto d = std::find_if(e->settings.begin(), e->settings.end(), [](const auto& kv) { return kv.first == "E_CHOICE_Text_Dir"; });
            return d == e->settings.end() ? std::string() : d->second;
        };
        Check(shortText != nullptr && direction(shortText) == "none" && shortText->compatibility == Compatibility::Compatible, "17. a short phrase that fits is shown still");
        Check(longText != nullptr && direction(longText) == "left" && std::find(longText->constraints.begin(), longText->constraints.end(), "text-scrolls-to-fit") != longText->constraints.end() && longText->needsReview,
              "18. a phrase too long to read still scrolls, marked as a limitation for review");
        Check(std::all_of(texts.events.begin(), texts.events.end(), [&](const ChoreographyEvent& e) {
                  if (e.effect != "Text") {
                      return true;
                  }
                  const auto value = std::find_if(e.settings.begin(), e.settings.end(), [](const auto& kv) { return kv.first == "E_TEXTCTRL_Text"; });
                  return value != e.settings.end() && (value->second == "Hola" || value->second == "Vuelve la Navidad con amor");
              }),
              "only the person's words are written, none invented");
    }

    // ------------------------------------------------------------ music

    void TestMusic() {
        Title("Music");
        const Choreography c = Make(_plans[1], _song);
        bool restInSilence = true;
        for (const PlanSegment& s : _plans[1].segments) {
            if (std::find(s.flags.begin(), s.flags.end(), "silence") == s.flags.end()) {
                continue;
            }
            for (const ChoreographyEvent& e : c.events) {
                restInSilence = restInSilence && (e.endMS <= s.startMS || e.startMS >= s.endMS);
            }
        }
        Check(restInSilence, "24. nothing moves in a pause");
        const PlanSegment* climax = nullptr;
        const PlanSegment* verse = nullptr;
        for (const PlanSegment& s : _plans[1].segments) {
            climax = std::find(s.flags.begin(), s.flags.end(), "climax") != s.flags.end() ? &s : climax;
            verse = verse == nullptr && s.sectionKind == "verse" ? &s : verse;
        }
        // Models lit, not targets: one group lights many figures.
        const auto inside = [&](const PlanSegment* s) {
            std::set<std::string> models;
            for (const ChoreographyEvent& e : c.events) {
                if (s == nullptr || e.endMS <= s->startMS || e.startMS >= s->endMS) {
                    continue;
                }
                const TargetInfo* t = _caps.Find(e.target);
                if (t->type == TargetType::Group) {
                    models.insert(t->flatModels.begin(), t->flatModels.end());
                } else {
                    models.insert(t->type == TargetType::SubModel ? t->parent : t->name);
                }
            }
            return models.size();
        };
        Check(climax != nullptr && inside(climax) > inside(verse), fmt::format("25. the climax lights more figures ({} against {} in a verse)", inside(climax), inside(verse)));
        const ChoreographyEvent* spiral = nullptr;
        const ChoreographyEvent* calm = nullptr;
        for (const ChoreographyEvent& e : c.events) {
            if (e.templateId == "tree-spirals") {
                const auto count = std::find_if(e.settings.begin(), e.settings.end(), [](const auto& kv) { return kv.first == "E_SLIDER_Spirals_Count"; });
                if (count != e.settings.end() && (spiral == nullptr || e.intensity > spiral->intensity)) {
                    spiral = &e;
                }
                if (count != e.settings.end() && (calm == nullptr || e.intensity < calm->intensity)) {
                    calm = &e;
                }
            }
        }
        if (spiral != nullptr && calm != nullptr && spiral != calm) {
            Check(spiral->settings != calm->settings, "parameters follow the energy: the same technique is set differently in a louder part");
        }
        const SongAnalysis unknown = PlanTestUnknownSong();
        const CompositionPlan up = MakePlan(unknown, AutomationLevel::Balanced, 7, false);
        const Choreography u = Make(up, unknown, false);
        Check(!u.events.empty() && Validate(u, up).ErrorCount() == 0, "26. unknown sections are choreographed from energy and pass validation");
        const SongAnalysis silent = PlanTestSilentSong();
        const CompositionPlan sp = MakePlan(silent, AutomationLevel::Balanced, 7, false);
        const Choreography empty = Make(sp, silent, false);
        Check(empty.events.empty() && Validate(empty, sp).ErrorCount() == 0, "35. a song with nothing to light: an empty choreography, and a valid one");

        SongAnalysis offGrid = _song;
        offGrid.durationMS += 13;
        offGrid.sections.back().endMS = offGrid.durationMS;
        const CompositionPlan offPlan = MakePlan(offGrid, AutomationLevel::Creative);
        const Choreography off = Make(offPlan, offGrid);
        const int lastFrame = offGrid.durationMS - offGrid.durationMS % 25;
        const bool onGrid = std::all_of(off.events.begin(), off.events.end(), [&](const ChoreographyEvent& e) { return e.endMS <= lastFrame && e.endMS % 25 == 0 && e.startMS % 25 == 0; });
        Check(!off.events.empty() && onGrid && Validate(off, offPlan).ErrorCount() == 0, fmt::format("a song {} ms long, off the frames: nothing runs past the last whole frame", offGrid.durationMS));
        int accentsOk = 0;
        int accents = 0;
        for (const ChoreographyEvent& e : c.events) {
            if (e.intent == "beat-accent") {
                ++accents;
                const bool onDownbeat = std::any_of(_song.events.begin(), _song.events.end(), [&](const MusicalEvent& m) { return m.type == MusicalEventType::Downbeat && std::abs(m.timeMS - e.startMS) <= 25; });
                accentsOk += onDownbeat && e.endMS - e.startMS >= 400 ? 1 : 0;
            }
        }
        Check(accents > 0 && accentsOk == accents, fmt::format("accents fall on downbeats and last at least 400 ms ({} of {})", accentsOk, accents));
    }

    // ------------------------------------------------------------ variety

    void TestVariety() {
        Title("Variety and continuity");
        const Choreography c = Make(_plans[1], _song);
        const ChoreographyMetrics& m = c.metrics;
        printf("  balanced: %d events, %d templates, top %s %.0f%%, same-next %.0f%%, %.2f changes/min, %d palettes, %d palette changes, %d review\n", m.events, m.distinctTemplates, m.topTemplate.c_str(),
               m.topTemplateShare * 100, m.repeatedNeighbours * 100, m.changesPerMinute, m.distinctPalettes, m.paletteChanges, m.needsReview);
        Check(m.distinctTemplates >= 6 && m.topTemplateShare <= 0.5, "21. no technique dominates: at least 6 in use, none over half of the effect time");
        std::map<std::string, std::set<std::string>> perSegment;
        bool distinctInSegment = true;
        for (const PlanSegment& s : _plans[1].segments) {
            std::map<std::string, std::set<std::string>> palettes;
            for (const ChoreographyEvent& e : c.events) {
                if (e.segmentId == s.id && e.intent != "beat-accent" && !e.paletteId.empty()) {
                    palettes[e.paletteId].insert(e.target);
                }
            }
            for (const auto& [id, targets] : palettes) {
                distinctInSegment = distinctInSegment && targets.size() <= 2;
            }
        }
        Check(m.distinctPalettes >= 3 && m.paletteChanges > 0 && distinctInSegment, "22. palettes change over the song and are not all the same within a segment");
        const bool transitions = std::any_of(c.events.begin(), c.events.end(), [](const ChoreographyEvent& e) { return !e.transitionIn.empty(); });
        const bool kinds = std::any_of(c.events.begin(), c.events.end(), [](const ChoreographyEvent& e) { return e.continuation == "new"; }) &&
                           std::any_of(c.events.begin(), c.events.end(), [](const ChoreographyEvent& e) { return std::any_of(e.reasons.begin(), e.reasons.end(), [](const Reason& r) { return r.code == "extended-into"; }) || e.continuation == "vary"; });
        Check(transitions && kinds, "23. transitions are native ones, and an effect is extended or varied rather than cut and restarted");
        bool noStrobe = std::none_of(c.events.begin(), c.events.end(), [](const ChoreographyEvent& e) { return e.effect == "Strobe"; });
        Check(noStrobe, "no strobing");
        bool calm = true;
        for (const PlanSegment& s : _plans[1].segments) {
            if (s.energy < 0.3 && std::find(s.flags.begin(), s.flags.end(), "silence") == s.flags.end()) {
                for (const ChoreographyEvent& e : c.events) {
                    if (e.segmentId == s.id) {
                        const EffectTemplate* t = _library.Find(e.templateId);
                        calm = calm && t != nullptr && t->energyMin <= s.energy + 0.05;
                    }
                }
            }
        }
        Check(calm, "quiet parts use techniques meant for low energy");
    }

    void TestLevels() {
        Title("Automation levels");
        ChoreographyMetrics m[3];
        for (int i = 0; i < 3; ++i) {
            m[i] = Make(_plans[i], _song).metrics;
            printf("  %s: %d events, %d templates, %.2f changes/min, %d accents, %.1f active\n", LevelId(_plans[i].level), m[i].events, m[i].distinctTemplates, m[i].changesPerMinute, m[i].accents, m[i].averageActive);
        }
        Check(m[0].accents == 0 && m[1].accents > 0 && m[2].accents > m[1].accents, "27-29. accents: none, some, more");
        Check(m[0].distinctTemplates <= m[1].distinctTemplates && m[1].distinctTemplates <= m[2].distinctTemplates && m[0].distinctTemplates < m[2].distinctTemplates, "variety of techniques grows with the level");
        Check(m[0].averageActive < m[1].averageActive && m[1].averageActive < m[2].averageActive, "and so does how much is moving");
    }

    // ------------------------------------------------------------ reproducibility

    void TestReproducibility() {
        Title("Reproducibility");
        const std::string a = ToJson(Make(_plans[1], _song));
        const std::string b = ToJson(Make(_plans[1], _song));
        Check(a == b, "30. same plan and seed: the same choreography, byte for byte");
        Choreography read;
        std::string error;
        Check(FromJson(a, read, error) && ToJson(read) == a, "it reads back to the same bytes");
        const CompositionPlan other = MakePlan(_song, AutomationLevel::Balanced, 8);
        Check(ToJson(Make(other, _song)) != a, "37. another seed: another choreography");

        const SongAnalysis changed = PlanTestSong(0.7);
        const CompositionPlan changedPlan = MakePlan(changed, AutomationLevel::Balanced);
        const Choreography x = Make(_plans[1], _song);
        const Choreography y = Make(changedPlan, changed);
        // Sections 3 to 5 touch the change (section 4) or join it through continuity.
        const auto outside = [](const Choreography& c) {
            std::vector<std::string> out;
            for (const ChoreographyEvent& e : c.events) {
                if (e.endMS <= 30000 || e.startMS >= 78000) {
                    nlohmann::json j = { e.id, e.startMS, e.endMS, e.templateId, e.paletteId, e.settings };
                    out.push_back(j.dump());
                }
            }
            return out;
        };
        Check(!outside(x).empty() && outside(x) == outside(y) && ToJson(x) != ToJson(y), "31. changing one section leaves the events of distant sections exactly as they were");
    }

    // ------------------------------------------------------------ corrections

    void TestCorrections() {
        Title("Manual decisions");
        const Choreography base = Make(_plans[1], _song);
        const ChoreographyEvent* pick = nullptr;
        for (const ChoreographyEvent& e : base.events) {
            if (e.intent == "feature" && e.target == "Arbol Grande") {
                pick = &e;
                break;
            }
        }
        if (pick == nullptr) {
            Check(false, "an event to lock");
            return;
        }
        ChoreographyCorrection lock;
        lock.action = ChoreographyCorrection::Action::Lock;
        lock.event = *pick;
        lock.event.paletteId = "cool-ice";
        lock.event.colors = { "#0080FF", "#FFFFFF" };
        const CompositionPlan regenerated = MakePlan(_song, AutomationLevel::Creative, 99);
        const Choreography again = Make(regenerated, _song, true, { lock });
        const bool kept = std::any_of(again.events.begin(), again.events.end(), [&](const ChoreographyEvent& e) {
            return e.state == DecisionState::Locked && e.target == lock.event.target && e.startMS == lock.event.startMS && e.endMS == lock.event.endMS && e.paletteId == "cool-ice" && e.settings == lock.event.settings;
        });
        bool noOverlap = true;
        for (const ChoreographyEvent& e : again.events) {
            noOverlap = noOverlap && (e.target != lock.event.target || e.state == DecisionState::Locked || e.endMS <= lock.event.startMS || e.startMS >= lock.event.endMS);
        }
        Check(kept && noOverlap, "32. a locked event, with its palette, survives regenerating with another plan, seed and level");
        Check(!Validate(again, regenerated).Has(issue::ChoreoLockLost), "and the validator agrees it is there");
        ChoreographyCorrection keep;
        keep.action = ChoreographyCorrection::Action::Lock;
        keep.event = *pick;
        const Choreography same = Make(_plans[1], _song, true, { keep });
        const auto atPick = std::count_if(same.events.begin(), same.events.end(), [&](const ChoreographyEvent& e) { return e.target == pick->target && e.layer == pick->layer && e.startMS < pick->endMS && e.endMS > pick->startMS; });
        Check(atPick == 1 && Validate(same, _plans[1]).ErrorCount() == 0, fmt::format("locking an event that is still there replaces it, not doubles it ({} at its time)", atPick));

        ChoreographyCorrection wrong;
        wrong.action = ChoreographyCorrection::Action::SetTemplate;
        wrong.target = "Linea 1";
        wrong.templateId = "tree-spirals";
        const Choreography refused = Make(_plans[1], _song, true, { wrong });
        const bool reported = std::any_of(refused.conflicts.begin(), refused.conflicts.end(), [](const ChoreographyConflict& k) { return k.target == "Linea 1" && k.reason.rfind("template-incompatible", 0) == 0; });
        bool unchanged = true;
        for (const ChoreographyEvent& e : refused.events) {
            unchanged = unchanged && !(e.target == "Linea 1" && e.templateId == "tree-spirals");
        }
        Check(reported && unchanged, "33. an incompatible manual template is reported and the original decision kept");
        ChoreographyCorrection badLock = lock;
        badLock.event.target = "Excluido";
        const Choreography excludedLock = Make(_plans[1], _song, true, { badLock });
        Check(std::any_of(excludedLock.conflicts.begin(), excludedLock.conflicts.end(), [](const ChoreographyConflict& k) { return k.reason == "target-excluded"; }) && First(excludedLock, "Excluido") == nullptr,
              "20. an event locked on an excluded model is refused, not applied");

        ChoreographyCorrection palette;
        palette.action = ChoreographyCorrection::Action::SetPalette;
        palette.target = "Arbol Grande";
        palette.paletteId = "soft-pink";
        ChoreographyCorrection exclude;
        exclude.action = ChoreographyCorrection::Action::Exclude;
        exclude.target = "Arco 1";
        ChoreographyCorrection timing;
        timing.action = ChoreographyCorrection::Action::SetTiming;
        timing.target = pick->target;
        timing.startMS = pick->startMS;
        timing.newStartMS = pick->startMS;
        timing.newEndMS = pick->endMS - 1000;
        const Choreography edited = Make(_plans[1], _song, true, { palette, exclude, timing });
        bool pink = true;
        bool anyPink = false;
        for (const ChoreographyEvent& e : edited.events) {
            if (e.target == "Arbol Grande" && e.effect != "Faces") {
                pink = pink && e.paletteId == "soft-pink";
                anyPink = true;
            }
        }
        Check(anyPink && pink, "a palette chosen by hand is used on that figure");
        Check(First(edited, "Arco 1") == nullptr, "a figure excluded by hand has no event");
        Check(std::any_of(edited.events.begin(), edited.events.end(), [&](const ChoreographyEvent& e) { return e.target == pick->target && e.startMS == pick->startMS && e.endMS == pick->endMS - 1000 && e.state == DecisionState::ManualEdit; }),
              "a duration changed by hand is kept");
        Check(Validate(edited, _plans[1]).ErrorCount() == 0, "and the edited choreography still validates");
    }

    // ------------------------------------------------------------ validator

    void TestValidator() {
        Title("GenerationValidator on a choreography");
        const Choreography c = Make(_plans[1], _song);
        const ValidationReport ok = Validate(c, _plans[1]);
        Check(ok.ErrorCount() == 0, fmt::format("a generated choreography passes ({} warnings)", ok.WarningCount()));
        Check(ok.Has(issue::ChoreoVisualReview) && ok.Find(issue::ChoreoVisualReview)->severity == IssueSeverity::Info, "and says its templates still need a person's eye");
        const auto tamper = [&](const std::function<void(Choreography&)>& change) {
            Choreography t = c;
            change(t);
            return Validate(t, _plans[1]);
        };
        Check(tamper([](Choreography& t) { t.events[0].settings.emplace_back("E_SLIDER_Spirals_Dragons", "1"); }).Has(issue::ChoreoSettingInvalid), "an unknown setting");
        Check(tamper([](Choreography& t) {
                  for (ChoreographyEvent& e : t.events) {
                      if (e.templateId == "tree-spirals") {
                          for (auto& kv : e.settings) {
                              if (kv.first == "E_SLIDER_Spirals_Count") {
                                  kv.second = "40";
                              }
                          }
                      }
                  }
              }).Has(issue::ChoreoSettingInvalid),
              "a setting out of its range");
        Check(tamper([](Choreography& t) { t.events[0].templateId = "dragon-dance"; }).Has(issue::ChoreoTemplateUnknown), "an unknown template");
        Check(tamper([](Choreography& t) { t.events[0].target = "Excluido"; }).Has(issue::ChoreoTargetExcluded), "an excluded target");
        Check(tamper([](Choreography& t) { t.events[0].target = "Nadie"; }).Has(issue::ChoreoTargetUnknown), "a target that does not exist");
        Check(tamper([](Choreography& t) { t.events[0].colors = { "red" }; }).Has(issue::ChoreoPaletteInvalid), "an invalid colour");
        Check(tamper([](Choreography& t) { t.events[0].endMS = t.events[0].startMS + 13; }).Has(issue::ChoreoTimeInvalid), "times off the frames");
        Check(tamper([](Choreography& t) {
                  ChoreographyEvent copy = t.events[0];
                  copy.id += "x";
                  t.events.insert(t.events.begin() + 1, copy);
              }).Has(issue::ChoreoOverlap),
              "two events on the same figure at once");
        Check(tamper([&](Choreography& t) {
                  ChoreographyEvent picture = t.events[0];
                  picture.id = "picture";
                  picture.target = "Matriz Prueba";
                  picture.effect = "Pictures";
                  picture.templateId = "matrix-picture";
                  picture.settings = { { "E_CHOICE_Scaling", "No Scaling" } };
                  picture.startMS = c.durationMS - 4000;
                  picture.endMS = c.durationMS;
                  t.events.push_back(picture);
              }).Has(issue::ChoreoMediaMissing),
              "34. a picture with no file is an error; the engine only leaves a media cue");
        Check(!c.mediaCues.empty() && std::none_of(c.events.begin(), c.events.end(), [](const ChoreographyEvent& e) { return e.effect == "Pictures" || e.effect == "Video"; }), fmt::format("{} media cues, and no picture or video effect", c.mediaCues.size()));
        Check(tamper([](Choreography& t) {
                  for (ChoreographyEvent& e : t.events) {
                      if (e.effect == "Faces") {
                          for (auto& kv : e.settings) {
                              if (kv.first == "E_CHOICE_Faces_TimingTrack") {
                                  kv.second = "Otra";
                              }
                          }
                      }
                  }
              }).Has(issue::ChoreoLyricsTrackMissing),
              "a face pointed at another track");
        CompositionPlan other = _plans[1];
        other.seed = 12345;
        Check(ValidateChoreography(c, other, _caps, _library, _metadata).Has(issue::ChoreoPlanMismatch), "a choreography checked against another plan");
        ValidationIssue sample;
        sample.code = issue::ChoreoDuration;
        sample.args = { { "event", "s1.0/x/0" }, { "length", "100" }, { "min", "400" }, { "max", "1200" } };
        Check(DescribeIssue(sample).find("s1.0/x/0") != std::string::npos && !DescribeSuggestion(sample).empty(), "every new code has a message and a suggestion");
    }

    // ------------------------------------------------------------ properties

    void TestProperties() {
        Title("Properties over many choreographies");
        int made = 0;
        bool valid = true;
        bool withinMax = true;
        bool aboveMin = true;
        bool noGroupConflict = true;
        bool roundTrip = true;
        std::string firstError;
        const SongAnalysis longSong = PlanTestLongSong(20);
        StyleProfile mystery = NeutralStarterProfile();
        mystery.id = "mystery";
        for (const SongAnalysis* song : std::vector<const SongAnalysis*>{ &_song, &longSong }) {
            for (AutomationLevel level : { AutomationLevel::Conservative, AutomationLevel::Balanced, AutomationLevel::Creative }) {
                for (uint64_t seed = 1; seed <= 4; ++seed) {
                    const CompositionPlan plan = MakePlan(*song, level, seed, song == &_song);
                    const Choreography c = Make(plan, *song, song == &_song);
                    ++made;
                    const ValidationReport report = Validate(c, plan);
                    if (report.ErrorCount() > 0 && firstError.empty()) {
                        for (const ValidationIssue& i : report.issues) {
                            if (i.severity == IssueSeverity::Error) {
                                firstError = DescribeIssue(i);
                                break;
                            }
                        }
                    }
                    valid = valid && report.ErrorCount() == 0;
                    noGroupConflict = noGroupConflict && !report.Has(issue::ChoreoGroupConflict);
                    for (const ChoreographyEvent& e : c.events) {
                        const EffectTemplate* t = _library.Find(e.templateId);
                        const PlanSegment* s = nullptr;
                        for (const PlanSegment& seg : plan.segments) {
                            s = seg.id == e.segmentId ? &seg : s;
                        }
                        withinMax = withinMax && t != nullptr && e.endMS - e.startMS <= t->maxMS;
                        const int room = s != nullptr ? s->endMS - s->startMS : 0;
                        aboveMin = aboveMin && t != nullptr && (e.endMS - e.startMS >= t->minMS || e.endMS - e.startMS >= room || e.state != DecisionState::Automatic ||
                                                                std::find(e.constraints.begin(), e.constraints.end(), "shorter-than-template-minimum") != e.constraints.end());
                    }
                    Choreography read;
                    std::string error;
                    roundTrip = roundTrip && FromJson(ToJson(c), read, error) && ToJson(read) == ToJson(c);
                }
            }
        }
        Check(valid, fmt::format("GenerationValidator finds no error in {} choreographies{}", made, firstError.empty() ? "" : ": " + firstError));
        Check(noGroupConflict, "38. no group and member light up together");
        Check(aboveMin, "39. no event shorter than its template allows, unless the segment is, and then it is marked");
        Check(withinMax, "40. no event longer than its template allows, extensions included");
        Check(roundTrip, "every choreography reads back to the same bytes");
        ChoreographyInput in;
        in.plan = &_plans[1];
        in.capabilities = &_caps;
        in.library = &_library;
        in.analysis = &_song;
        in.profile = &mystery;
        const Choreography unknownProfile = Choreograph(in);
        Check(!unknownProfile.events.empty() && std::find(unknownProfile.warnings.begin(), unknownProfile.warnings.end(), "profile-differs-from-plan") != unknownProfile.warnings.end(), "36. an unknown profile is used and reported, nothing breaks");
    }

    // ------------------------------------------------------------ sequence writer

    // ------------------------------------------------------------ ensemble

    StyleProfile ReferenceProfile(std::string& error) const {
        StyleProfile p;
        const fs::path file = fs::path(_templatesDir).parent_path() / "profiles" / "reference-all.json";
        std::ifstream in(file, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        if (!FromJson(text.str(), p, error)) {
            error = file.string() + ": " + error;
        }
        return p;
    }

    void TestEnsemble() {
        Title("The whole display together: themes, scenes and the ensemble");
        std::string error;
        int shipped = 0;
        bool allOn = true;
        for (const auto& entry : fs::directory_iterator(fs::path(_templatesDir).parent_path() / "profiles")) {
            std::ifstream in(entry.path(), std::ios::binary);
            std::ostringstream text;
            text << in.rdbuf();
            StyleProfile p;
            std::string e;
            allOn = allOn && FromJson(text.str(), p, e) && p.ensemble.enabled && !p.ensemble.sources.empty();
            ++shipped;
        }
        Check(shipped == 7 && allOn, fmt::format("{} reference profiles ship, each with the ensemble on and what it was measured from", shipped));
        Check(ToJson(NeutralStarterProfile()).find("ensemble") == std::string::npos, "the neutral profile is written as it always was: no ensemble, the same hash");
        StyleProfile reference = ReferenceProfile(error);
        Check(error.empty() && reference.ensemble.enabled && reference.ensemble.coverageHigh > reference.ensemble.coverageLow, "the reference profile reads back " + error);
        // The fixture is a handful of figures: with six of them in the plan nothing is left
        // for the ensemble, so the plan here keeps to two.
        reference.maxParticipants = 2;

        ThemeCatalog themes;
        const bool loaded = LoadThemeCatalog(DefaultThemeCatalogPath(), themes, error);
        Check(loaded && themes.themes.size() >= 9, fmt::format("the theme catalogue loads: {} themes {}", themes.themes.size(), error));
        const auto measured = std::count_if(themes.themes.begin(), themes.themes.end(), [](const ThemeDef& t) { return t.source == "reference-measured"; });
        Check(measured == 6, fmt::format("{} of them measured in the references", measured));
        ThemeCatalog broken;
        const bool parsed = ParseThemeCatalog(R"({"themes":[{"id":"x","home":[["red"]],"accent":"#FFFFFF"}]})", broken, error);
        Check(!parsed, "a theme with no valid colour is refused: " + error);

        const CompositionPlan plan = MakePlanWith(reference, AutomationLevel::Balanced);
        DirectionInput d;
        d.plan = &plan;
        d.capabilities = &_caps;
        d.profile = &reference;
        d.catalog = &themes;
        d.analysis = &_song;
        d.lyrics = &_doc;
        d.title = "villancico.mp3";
        const ShowVisualDirection direction = DirectShow(d);
        Check(direction.enabled && direction.themeId == "christmas-classic" && direction.themeSource == "lyrics" && direction.themeConfidence > 0.3,
              fmt::format("\"Navidad\" three times in the lyrics: theme {} from {} at {:.2f}", direction.themeId, direction.themeSource, direction.themeConfidence));
        Check(std::none_of(direction.families.begin(), direction.families.end(), [](const VisualFamily& f) {
                  return std::any_of(f.targets.begin(), f.targets.end(), [](const FamilyTarget& t) { return t.target == "Sin Controlador" || t.target == "Excluido"; });
              }),
              "no family holds a figure without a controller or an excluded one");
        bool scenesOk = true;
        std::map<std::string, std::set<std::string>> looks;
        for (const SceneLook& l : direction.scenes) {
            const PlanSegment* seg = nullptr;
            for (const PlanSegment& sg : plan.segments) {
                seg = sg.id == l.segmentId ? &sg : seg;
            }
            const bool silence = seg != nullptr && std::find(seg->flags.begin(), seg->flags.end(), "silence") != seg->flags.end();
            scenesOk = scenesOk && (silence ? l.motif == "rest" : !l.colors.empty());
            if (l.motif != "rest" && l.band != "climax") {
                looks[l.lookKey].insert(l.motif + "/" + std::to_string(l.variant) + "/" + l.colors.front());
            }
        }
        Check(scenesOk, "every scene has colours, and a silence stays a rest");
        Check(std::all_of(looks.begin(), looks.end(), [](const auto& kv) { return kv.second.size() == 1; }), "a section that comes back keeps its colours and its movement");

        ChoreographyInput in;
        in.plan = &plan;
        in.capabilities = &_caps;
        in.library = &_library;
        in.profile = &reference;
        in.analysis = &_song;
        in.alignment = &_alignment;
        in.lyricTrack = LYRIC_TRACK;
        in.direction = &direction;
        const Choreography c = Choreograph(in);
        const ValidationReport report = ValidateChoreography(c, plan, _caps, _library, _metadata);
        Check(report.ErrorCount() == 0, fmt::format("the ensemble choreography passes the checks: {} events, {} errors", c.events.size(), report.ErrorCount()));
        const DirectionMetrics m = MeasureDirection(direction, c, _caps);
        Check(m.paletteCoherence == 1.0, fmt::format("every coloured event uses its scene's colours ({:.2f})", m.paletteCoherence));
        Check(m.coverageMean >= 0.6, fmt::format("most of the display is under an effect during music ({:.2f})", m.coverageMean));
        std::string why;
        for (const std::string& w : c.warnings) {
            why += w.rfind("ensemble", 0) == 0 ? " " + w : "";
        }
        std::set<std::string> claimedGroups;
        for (const PlanSegment& sg : plan.segments) {
            for (const PlanAssignment& a : sg.assignments) {
                if (a.prominence != Prominence::Rest && a.type == TargetType::Group) {
                    claimedGroups.insert(a.target);
                }
            }
        }
        for (const std::string& g : claimedGroups) {
            why += " plan-group:" + g;
        }
        Check(m.hits > 0 && m.startsTogether > 0.5, fmt::format("{} hits; {:.2f} of the starts are shared, as the references cut together{}", m.hits, m.startsTogether, why));
        std::set<int> beats;
        for (const MusicalEvent& e : _song.events) {
            if (e.type == MusicalEventType::Beat || e.type == MusicalEventType::Downbeat) {
                beats.insert(e.timeMS / FRAME * FRAME);
            }
        }
        bool hitsOnBeat = true;
        bool layersRight = true;
        bool silentRest = true;
        bool noFreeModel = true;
        for (const ChoreographyEvent& e : c.events) {
            if (e.intent == "ensemble-hit") {
                hitsOnBeat = hitsOnBeat && (beats.contains(e.startMS) || beats.contains(e.startMS - FRAME) || beats.contains(e.startMS + FRAME));
                layersRight = layersRight && e.layer == 0 && e.fadeInMS == 0 && e.fadeOutMS > 0;
            } else if (e.intent == "ensemble-bed") {
                layersRight = layersRight && e.layer == 1;
            }
            const TargetInfo* t = _caps.Find(e.target);
            noFreeModel = noFreeModel && (t == nullptr || !t->noController || e.intent.rfind("ensemble", 0) != 0);
        }
        for (const PlanSegment& sg : plan.segments) {
            if (std::find(sg.flags.begin(), sg.flags.end(), "silence") == sg.flags.end()) {
                continue;
            }
            for (const ChoreographyEvent& e : c.events) {
                silentRest = silentRest && (e.endMS <= sg.startMS || e.startMS >= sg.endMS);
            }
        }
        Check(hitsOnBeat, "every hit starts on a beat");
        Check(layersRight, "bases under (layer 1), hits over (layer 0) and dying away");
        Check(silentRest, "nothing moves in a pause");
        Check(noFreeModel, "the ensemble never lights a figure without a controller");
        bool apart = true;
        for (const ChoreographyEvent& e : c.events) {
            if (e.intent.rfind("ensemble", 0) != 0) {
                continue;
            }
            for (const ChoreographyEvent& o : c.events) {
                if (o.intent.rfind("ensemble", 0) == 0 || o.segmentId != e.segmentId) {
                    continue;
                }
                apart = apart && !_caps.Overlap(e.target, o.target);
            }
        }
        Check(apart, "the ensemble keeps off the lights of the plan's figures in the same segment");
        Check(ToJson(Choreograph(in)) == ToJson(c), "the same inputs give the same ensemble");
        const Choreography without = Make(_plans[1], _song);
        ChoreographyInput off = in;
        off.plan = &_plans[1];
        off.profile = nullptr;
        off.direction = nullptr;
        Check(ToJson(Choreograph(off)) == ToJson(without), "without a direction the choreography is exactly what it was");

        ChoreographyInput calm = in;
        const CompositionPlan calmPlan = MakePlanWith(reference, AutomationLevel::Conservative);
        DirectionInput dc = d;
        dc.plan = &calmPlan;
        const ShowVisualDirection calmDirection = DirectShow(dc);
        calm.plan = &calmPlan;
        calm.direction = &calmDirection;
        const DirectionMetrics cm = MeasureDirection(calmDirection, Choreograph(calm), _caps);
        Check(cm.hits < m.hits, fmt::format("conservative: fewer hits ({} against {})", cm.hits, m.hits));

        // Where the theme comes from.
        const LyricDocument winter = ParsePlainLyrics("Cae la nieve en invierno\nfrío y nieve\n");
        DirectionInput dw = d;
        dw.lyrics = &winter;
        dw.title = "cancion.mp3";
        Check(DirectShow(dw).themeId == "winter-night", "lyrics about snow and winter: the winter night theme");
        SongCharacter sure;
        sure.provider = "gemini";
        sure.suggestedTheme = "halloween";
        sure.themeConfidence = 0.8;
        dw.character = &sure;
        const ShowVisualDirection fromCharacter = DirectShow(dw);
        Check(fromCharacter.themeId == "halloween" && fromCharacter.themeSource == "song-character", "a song description sure of its theme wins, and says so");
        SongCharacter unsure = sure;
        unsure.suggestedTheme = "uncertain";
        dw.character = &unsure;
        Check(DirectShow(dw).themeSource == "lyrics", "an uncertain description is not taken");
        const LyricDocument nothing = ParsePlainLyrics("La la la\n");
        dw.lyrics = &nothing;
        dw.character = nullptr;
        const ShowVisualDirection fallback = DirectShow(dw);
        Check(fallback.themeSource == "profile-default" && fallback.themeConfidence <= 0.3, "nothing to go on: the profile's theme, at low confidence");
        DirectionInput neutral = d;
        neutral.profile = nullptr;
        Check(!DirectShow(neutral).enabled, "the neutral profile asks for no direction");
    }

    void TestVoiceFaces() {
        Title("Each face sings its voice, and only when that voice does");
        const LyricDocument doc = ParsePlainLyrics("[Narrador]\nHabía una vez una casa encendida\n\n[Voz: Ana]\nUna noche de Navidad\n\n[Voz: Luis]\nLas estrellas brillan\n\n"
                                                   "[Voz: Coro]\nVuelve la Navidad\n");
        // Narrator in the intro, Ana in the verse, Luis in the second verse, the choir in the last chorus.
        const LyricAlignment alignment = Align(doc, { { 3000, 7000 }, { 15000, 19000 }, { 47000, 51000 }, { 63000, 67000 } });
        VoiceCast cast = doc.cast;
        std::vector<std::string> warnings;
        AssignVoiceFaces(cast, _caps, &warnings);
        const VoiceDef* ana = cast.Find("v:ana");
        const VoiceDef* luis = cast.Find("v:luis");
        const VoiceDef* coro = cast.Find("v:coro");
        const VoiceDef* narrator = cast.Find("v:narrador");
        Check(ana != nullptr && luis != nullptr && ana->faces.size() == 1 && luis->faces.size() == 1 && ana->faces != luis->faces,
              fmt::format("the lead and the second voice get a face each: {} and {}", ana != nullptr && !ana->faces.empty() ? ana->faces[0] : "-", luis != nullptr && !luis->faces.empty() ? luis->faces[0] : "-"));
        Check(coro != nullptr && coro->faces.size() >= 2, fmt::format("the choir takes every face ({})", coro != nullptr ? coro->faces.size() : 0));
        Check(narrator != nullptr && narrator->faces.empty(), "the narrator gets no face unless a person gives it one");
        const auto wired = [&](const std::string& face) { const TargetInfo* t = _caps.Find(face); return t != nullptr && !t->noController; };
        Check(ana != nullptr && std::all_of(ana->faces.begin(), ana->faces.end(), wired), "only faces with a controller are given");

        PlannerInput pin;
        pin.analysis = &_song;
        pin.capabilities = &_caps;
        pin.options.level = AutomationLevel::Balanced;
        pin.options.seed = 7;
        pin.lyrics = &doc;
        pin.alignment = &alignment;
        pin.cast = &cast;
        const CompositionPlan plan = PlanComposition(pin);
        bool rightFaces = true;
        int sung = 0;
        for (const PlanSegment& s : plan.segments) {
            for (const PlanAssignment& a : s.assignments) {
                if (a.intent != "face-sing") {
                    continue;
                }
                ++sung;
                const VoiceDef* v = cast.Find(a.voice);
                const bool heard = std::any_of(alignment.phrases.begin(), alignment.phrases.end(), [&](const AlignedPhrase& p) {
                    return p.startMS < s.endMS && p.endMS > s.startMS && std::find(p.voices.begin(), p.voices.end(), a.voice) != p.voices.end();
                });
                rightFaces = rightFaces && v != nullptr && heard && std::find(v->faces.begin(), v->faces.end(), a.target) != v->faces.end();
            }
        }
        Check(sung > 0 && rightFaces, fmt::format("{} singing faces in the plan, each in a segment where its own voice is heard", sung));
        const auto facesIn = [&](int ms) {
            std::set<std::string> out;
            for (const PlanSegment& s : plan.segments) {
                if (ms >= s.startMS && ms < s.endMS) {
                    for (const PlanAssignment& a : s.assignments) {
                        if (a.intent == "face-sing") {
                            out.insert(a.target);
                        }
                    }
                }
            }
            return out;
        };
        Check(facesIn(4000).empty(), "while the narrator speaks, no face sings");
        Check(ana != nullptr && luis != nullptr && facesIn(16000).contains(ana->faces[0]) && !facesIn(16000).contains(luis->faces[0]), "in Ana's verse, her face sings and Luis's does not");
        Check(coro != nullptr && facesIn(64000).size() >= 2, "in the choir, several faces sing together");

        std::map<std::string, std::string> tracks;
        for (const TimingTrackSpec& t : BuildVoiceTracks(alignment, cast, LYRIC_TRACK)) {
            for (const VoiceDef& v : cast.voices) {
                if (t.name == VoiceTrackName(LYRIC_TRACK, v)) {
                    tracks[v.id] = t.name;
                }
            }
        }
        ChoreographyInput cin;
        cin.plan = &plan;
        cin.capabilities = &_caps;
        cin.library = &_library;
        cin.analysis = &_song;
        cin.alignment = &alignment;
        cin.lyricTrack = LYRIC_TRACK;
        cin.voiceTracks = tracks;
        const Choreography c = Choreograph(cin);
        bool ownTrack = true;
        int faces = 0;
        for (const ChoreographyEvent& e : c.events) {
            if (e.effect != "Faces") {
                continue;
            }
            ++faces;
            const auto track = std::find_if(e.settings.begin(), e.settings.end(), [](const auto& kv) { return kv.first == "E_CHOICE_Faces_TimingTrack"; });
            ownTrack = ownTrack && track != e.settings.end() && track->second != LYRIC_TRACK && std::any_of(tracks.begin(), tracks.end(), [&](const auto& kv) { return kv.second == track->second; });
        }
        Check(faces > 0 && ownTrack, fmt::format("{} face effects, each reading its voice's own track", faces));
        Check(ValidateChoreography(c, plan, _caps, _library, _metadata).ErrorCount() == 0, "and the choreography passes the checks");
        VoiceCast off = cast;
        for (VoiceDef& v : off.voices) {
            v.facesOff = v.id == "v:ana";
            if (v.facesOff) {
                v.faces.clear();
            }
        }
        pin.cast = &off;
        const CompositionPlan quiet = PlanComposition(pin);
        bool anaSilent = true;
        for (const PlanSegment& s : quiet.segments) {
            for (const PlanAssignment& a : s.assignments) {
                anaSilent = anaSilent && !(a.intent == "face-sing" && a.voice == "v:ana");
            }
        }
        Check(anaSilent, "a voice set to have no face leaves every face alone");
    }

    CompositionPlan MakePlanWith(const StyleProfile& profile, AutomationLevel level) const {
        PlannerInput in;
        in.analysis = &_song;
        in.capabilities = &_caps;
        in.options.level = level;
        in.options.seed = 7;
        in.lyrics = &_doc;
        in.alignment = &_alignment;
        in.profile = &profile;
        return PlanComposition(in);
    }

    void TestSequenceWriter() {
        Title("A small choreography through SequenceWriter, on the isolated fixture only");
        Choreography c = Make(_plans[1], _song);
        c.events.erase(std::remove_if(c.events.begin(), c.events.end(), [](const ChoreographyEvent& e) { return e.endMS > 46000; }), c.events.end());
        const TimingTrackSpec track = BuildLyricTrack(_alignment, LYRIC_TRACK);
        SequenceSpec spec = ToSequenceSpec(c, { track });
        spec.durationMS = 46000;
        const auto start = std::chrono::steady_clock::now();
        const WriteResult written = WriteSequenceSafely(_fixture.string(), spec, "choreography-check.xsq");
        for (const ValidationIssue& i : written.validation.issues) {
            if (i.severity == IssueSeverity::Error) {
                printf("    %s %s\n", i.code.c_str(), DescribeIssue(i).c_str());
            }
        }
        Check(written.ok, fmt::format("{} effects written, reopened and validated by Phase 2 in {} ms", spec.effects.size(), ElapsedMS(start)));
        if (!written.ok) {
            return;
        }
        HeadlessRenderContext ctx;
        if (!ctx.LoadShowFolder(_fixture.string()) || !ctx.OpenSequence(written.finalPath)) {
            Check(false, "reopening for the render");
            return;
        }
        const std::string fseq = (fs::path(written.finalPath).replace_extension(".fseq")).string();
        const bool rendered = ctx.RenderAndWait() && ctx.WriteFseq(fseq);
        const ValidationReport render = rendered ? ValidateRender(ctx, fseq, spec, Strictness::Generated) : ValidationReport();
        for (const ValidationIssue& i : render.issues) {
            if (i.severity == IssueSeverity::Error) {
                printf("    %s %s\n", i.code.c_str(), DescribeIssue(i).c_str());
            }
        }
        Check(rendered && render.ErrorCount() == 0, fmt::format("rendered, and every effect lights its target ({} errors, {} warnings)", render.ErrorCount(), render.WarningCount()));
        (void)ctx.CloseSequence();
    }

    void WriteOutputs() {
        Title("Outputs");
        const Choreography c = Make(_plans[1], _song);
        Check(WriteText(_root / "choreography.json", ToJson(c)) && WriteText(_root / "choreography-report.txt", ChoreographyReport(c)) && WriteText(_root / "composition-plan.json", ToJson(_plans[1])),
              "choreography.json, its report and the plan it came from written");
    }

    // ------------------------------------------------------------ real layout

    void TestRealLayout() {
        if (_options.showDir.empty()) {
            return;
        }
        Title("Real layout (read only, controllers inactive)");
        std::string refused;
        if (!ControllersAreInactive(_options.showDir, refused)) {
            Check(false, "refused: " + refused);
            return;
        }
        HeadlessRenderContext ctx;
        if (!ctx.LoadShowFolder(_options.showDir)) {
            Check(false, "loading the show");
            return;
        }
        const ModelCapabilities caps = ExtractCapabilities(ctx, nullptr);
        auto start = std::chrono::steady_clock::now();
        const TemplateLibrary library = LoadTemplateLibrary(_templatesDir, MetadataFrom(ctx.GetEffectManager()));
        printf("  templates loaded and validated in %lld ms\n", ElapsedMS(start));
        CompositionPlan plan;
        SongAnalysis song = PlanTestLongSong(20);
        std::string text;
        std::string error;
        if (!_options.planPath.empty()) {
            if (!ReadText(_options.planPath, text) || !FromJson(text, plan, error)) {
                Check(false, "reading the plan: " + error);
                return;
            }
        }
        StyleProfile profile = NeutralStarterProfile();
        if (!_options.profilePath.empty() && (!ReadText(_options.profilePath, text) || !FromJson(text, profile, error))) {
            Check(false, "reading the profile: " + error);
            return;
        }
        for (AutomationLevel level : { AutomationLevel::Conservative, AutomationLevel::Balanced, AutomationLevel::Creative }) {
            if (!_options.level.empty() && _options.level != LevelId(level)) {
                continue;
            }
            if (_options.planPath.empty()) {
                PlannerInput in;
                in.analysis = &song;
                in.capabilities = &caps;
                in.profile = &profile;
                in.options.level = level;
                in.options.seed = _options.seed != 0 ? _options.seed : 2026;
                plan = PlanComposition(in);
            }
            ChoreographyInput in;
            in.plan = &plan;
            in.capabilities = &caps;
            in.library = &library;
            in.profile = &profile;
            in.analysis = _options.planPath.empty() ? &song : nullptr;
            in.level = level;
            if (_options.seed != 0) {
                in.seed = _options.seed;
            }
            start = std::chrono::steady_clock::now();
            const Choreography c = Choreograph(in);
            const long long ms = ElapsedMS(start);
            start = std::chrono::steady_clock::now();
            const std::string json = ToJson(c);
            const long long serialize = ElapsedMS(start);
            const ValidationReport report = ValidateChoreography(c, plan, caps, library, MetadataFrom(ctx.GetEffectManager()));
            const ChoreographyMetrics& m = c.metrics;
            printf("  %s: %d events in %lld ms (json %lld ms, %zu KB), %d templates, top %.0f%%, %.1f active, %d accents, %d review, %d errors, %d warnings\n", LevelId(level), m.events, ms, serialize, json.size() / 1024,
                   m.distinctTemplates, m.topTemplateShare * 100, m.averageActive, m.accents, m.needsReview, report.ErrorCount(), report.WarningCount());
            Check(ms < 5000 && report.ErrorCount() == 0, fmt::format("{}: a 5-minute song choreographed in {} ms and validated", LevelId(level), ms));
            if (level == AutomationLevel::Balanced || !_options.level.empty()) {
                (void)WriteText(_root / "real-layout-choreography.json", json);
                (void)WriteText(_root / "real-layout-choreography-report.txt", ChoreographyReport(c));
            }
        }
        (void)ctx.CloseSequence();
    }

    fs::path _root;
    ChoreographyTestOptions _options;
    fs::path _fixture;
    std::unique_ptr<HeadlessRenderContext> _ctx;
    ModelCapabilities _caps;
    MetadataLookup _metadata;
    std::string _templatesDir;
    TemplateLibrary _library;
    long long _loadMS = 0;
    SongAnalysis _song;
    LyricDocument _doc;
    LyricAlignment _alignment;
    CompositionPlan _plans[3];
    int _passed = 0;
    int _failed = 0;
};

} // namespace

int RunChoreographyTest(const ChoreographyTestOptions& options) {
    fs::path root;
    std::error_code ec;
    for (int i = 0; i < 1000; ++i) {
        root = fs::path(options.outDir) / fmt::format("composer-choreography-test-{}", i);
        if (!fs::exists(root, ec)) {
            break;
        }
    }
    fs::create_directories(root, ec);
    if (ec) {
        printf("composer-choreography-test: cannot create %s\n", root.string().c_str());
        return 1;
    }
    Suite suite(root, options);
    return suite.Run();
}

} // namespace composer
