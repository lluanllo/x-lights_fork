#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

#include "composer/PlanTest.h"

#include "composer/CompositionPlan.h"
#include "composer/GenerationValidator.h"
#include "composer/LyricAligner.h"
#include "composer/LyricDocument.h"
#include "composer/ModelCapabilities.h"
#include "composer/WriterTest.h"
#include "render/HeadlessRenderContext.h"
#include "utils/ExternalHooks.h"

#include <pugixml.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace composer {

namespace {

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

// ---------------------------------------------------------------- fixture

void Common(pugi::xml_node m, const std::string& name, const char* displayAs, int& channel, int nodes) {
    m.append_attribute("name") = name.c_str();
    m.append_attribute("DisplayAs") = displayAs;
    m.append_attribute("StringType") = "RGB Nodes";
    m.append_attribute("LayoutGroup") = "Default";
    m.append_attribute("StartSide") = "B";
    m.append_attribute("Dir") = "L";
    m.append_attribute("versionNumber") = "8";
    m.append_attribute("StartChannel") = std::to_string(channel).c_str();
    channel += nodes * 3;
}

void Attr(pugi::xml_node m, std::initializer_list<std::pair<const char*, std::string>> attributes) {
    for (const auto& [key, value] : attributes) {
        m.append_attribute(key) = value.c_str();
    }
}

pugi::xml_node Custom(pugi::xml_node models, const std::string& name, int width, int height, int& channel) {
    pugi::xml_node m = models.append_child("model");
    Common(m, name, "Custom", channel, width * height);
    std::string compressed;
    int node = 1;
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            compressed += fmt::format("{},{},{};", node++, row, col);
        }
    }
    Attr(m, { { "CustomModelCompressed", compressed }, { "CustomWidth", std::to_string(width) }, { "CustomHeight", std::to_string(height) }, { "Depth", "1" } });
    return m;
}

void Face(pugi::xml_node model) {
    pugi::xml_node face = model.append_child("faceInfo");
    Attr(face, { { "Name", "Boca" }, { "Type", "NodeRange" }, { "CustomColors", "" }, { "Mouth-AI", "1-6" }, { "Mouth-E", "7-10" }, { "Mouth-etc", "11-14" }, { "Mouth-FV", "15-16" }, { "Mouth-L", "17-19" },
                 { "Mouth-MBP", "20" }, { "Mouth-O", "21-23" }, { "Mouth-U", "24" }, { "Mouth-WQ", "25" }, { "Mouth-rest", "26-27" }, { "Eyes-Open", "28-29" }, { "Eyes-Closed", "30" } });
}

void Group(pugi::xml_node groups, const std::string& name, const std::string& members) {
    pugi::xml_node g = groups.append_child("modelGroup");
    Attr(g, { { "name", name }, { "DisplayAs", "ModelGroup" }, { "DefaultCamera", "2D" }, { "GridSize", "400" }, { "layout", "minimalGrid" }, { "LayoutGroup", "Default" }, { "models", members } });
}

// Names are made up for the test; none comes from a real show.
bool WriteFixture(const fs::path& folder) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    pugi::xml_document doc;
    pugi::xml_node root = doc.append_child("xrgb");
    pugi::xml_node models = root.append_child("models");
    pugi::xml_node groups = root.append_child("modelGroups");
    int ch = 1;
    pugi::xml_node m = models.append_child("model");
    Common(m, "Matriz Prueba", "Matrix", ch, 48 * 24);
    Attr(m, { { "NumStrings", "1" }, { "NodesPerString", "1152" }, { "StrandsPerString", "24" }, { "Vertical", "false" }, { "AlternateNodes", "false" }, { "NoZig", "false" } });
    m = models.append_child("model");
    Common(m, "Arbol Grande", "Tree", ch, 800);
    Attr(m, { { "NumStrings", "16" }, { "NodesPerString", "50" }, { "StrandsPerString", "1" }, { "TreeType", "0" }, { "TreeDegrees", "360" }, { "TreeBottomTopRatio", "6" }, { "TreePerspective", "0.2" } });
    m = models.append_child("model");
    Common(m, "Arbol Pequeno", "Tree", ch, 160);
    Attr(m, { { "NumStrings", "8" }, { "NodesPerString", "20" }, { "StrandsPerString", "1" }, { "TreeType", "0" }, { "TreeDegrees", "360" }, { "TreeBottomTopRatio", "6" }, { "TreePerspective", "0.2" } });
    m = models.append_child("model");
    Common(m, "Arbol Cara", "Tree", ch, 600);
    Attr(m, { { "NumStrings", "12" }, { "NodesPerString", "50" }, { "StrandsPerString", "1" }, { "TreeType", "0" }, { "TreeDegrees", "360" }, { "TreeBottomTopRatio", "6" }, { "TreePerspective", "0.2" } });
    Face(m);
    m = models.append_child("model");
    Common(m, "Ventana Matrix 1", "Window Frame", ch, 36);
    Attr(m, { { "TopNodes", "10" }, { "SideNodes", "8" }, { "BottomNodes", "10" }, { "Rotation", "Counter Clockwise" } });
    m = models.append_child("model");
    Common(m, "Arco 1", "Arches", ch, 30);
    Attr(m, { { "NumArches", "1" }, { "NodesPerArch", "30" }, { "Arc", "180" }, { "LightsPerNode", "1" } });
    m = models.append_child("model");
    Common(m, "Excluido", "Arches", ch, 30);
    Attr(m, { { "NumArches", "1" }, { "NodesPerArch", "30" }, { "Arc", "180" }, { "LightsPerNode", "1" } });
    m = models.append_child("model");
    Common(m, "Linea 1", "Single Line", ch, 50);
    Attr(m, { { "NumStrings", "1" }, { "NodesPerString", "50" }, { "LightsPerNode", "1" } });
    m = models.append_child("model");
    Common(m, "Estrella 1", "Star", ch, 20);
    Attr(m, { { "NumStrings", "1" }, { "NodesPerString", "20" }, { "LayerSizes", "20" }, { "StarPoints", "5" } });
    pugi::xml_node face = Custom(models, "Carita", 6, 5, ch);
    Face(face);
    pugi::xml_node free = Custom(models, "Sin Controlador", 30, 10, ch);
    free.append_attribute("Controller") = "No Controller";
    pugi::xml_node arm = free.append_child("subModel");
    Attr(arm, { { "name", "Brazo" }, { "bufferstyle", "Default" }, { "layout", "horizontal" }, { "line0", "1-30" }, { "type", "ranges" } });
    Group(groups, "Grupo Arcos", "Arco 1,Excluido");
    Group(groups, "Grupo Patio", "Linea 1,Estrella 1,Grupo Arcos");
    Group(groups, "Grupo Todo", "Grupo Patio,Arbol Grande,Matriz Prueba");
    Group(groups, "Grupo Roto", "Linea 1,Modelo Que No Existe");
    Group(groups, "Grupo Arboles", "Arbol Grande,Arbol Pequeno");
    return doc.save_file((folder / "xlights_rgbeffects.xml").string().c_str());
}

// ---------------------------------------------------------------- songs

struct SectionDef {
    int start;
    int end;
    SectionKind kind;
    int repeatGroup;
    double energy;
    bool silence = false;
};

SongAnalysis MakeSong(const std::vector<SectionDef>& defs, const std::vector<std::pair<int, int>>& climaxes = {}, const std::function<int(int)>& beatAt = {}, const std::function<double(int)>& energyAt = {}) {
    SongAnalysis a;
    a.durationMS = defs.back().end;
    a.sampleRate = 44100;
    a.audioHash = "synthetic";
    for (const SectionDef& d : defs) {
        SongSection s;
        s.startMS = d.start;
        s.endMS = d.end;
        s.kind = d.kind;
        s.repeatGroup = d.repeatGroup;
        s.energy = d.energy;
        s.confidence = 0.5;
        if (d.silence) {
            s.evidence = { "silence" };
            a.events.push_back({ d.start, d.end - d.start, MusicalEventType::Silence, 0, 0.9, -1, "test" });
        }
        a.sections.push_back(s);
    }
    const auto sectionAt = [&](int t) -> const SectionDef& {
        for (const SectionDef& d : defs) {
            if (t >= d.start && t < d.end) {
                return d;
            }
        }
        return defs.back();
    };
    for (int t = 0; t < a.durationMS; t += 100) {
        EnergyPoint p;
        p.timeMS = t;
        const SectionDef& d = sectionAt(t);
        p.intensity = d.silence ? 0 : energyAt ? energyAt(t) : d.energy;
        p.total = p.intensity;
        a.energy.push_back(p);
    }
    int index = 0;
    for (int t = 0; t < a.durationMS;) {
        const int step = beatAt ? beatAt(t) : 500;
        if (!sectionAt(t).silence) {
            a.events.push_back({ t, 0, MusicalEventType::Beat, 0.8, 0.9, -1, "test", index });
            if (index % 4 == 0) {
                a.events.push_back({ t, 0, MusicalEventType::Downbeat, 0.9, 0.8, -1, "test", index, 0 });
            }
            ++index;
        }
        t += step;
    }
    for (const auto& [from, to] : climaxes) {
        a.events.push_back({ from, to - from, MusicalEventType::Climax, 1.0, 0.8, -1, "test" });
    }
    std::sort(a.events.begin(), a.events.end(), [](const MusicalEvent& x, const MusicalEvent& y) { return x.timeMS != y.timeMS ? x.timeMS < y.timeMS : x.type < y.type; });
    return a;
}

// Silence, intro, verse, chorus, verse, chorus with a climax, outro, silence.
SongAnalysis StandardSong(double secondVerseEnergy = 0.45) {
    return MakeSong({ { 0, 2000, SectionKind::Unknown, -1, 0, true },
                      { 2000, 14000, SectionKind::Intro, -1, 0.2 },
                      { 14000, 30000, SectionKind::Verse, 0, 0.45 },
                      { 30000, 46000, SectionKind::Chorus, 1, 0.8 },
                      { 46000, 62000, SectionKind::Verse, 0, secondVerseEnergy },
                      { 62000, 78000, SectionKind::Chorus, 1, 0.85 },
                      { 78000, 86000, SectionKind::Outro, -1, 0.25 },
                      { 86000, 90000, SectionKind::Unknown, -1, 0, true } },
                    { { 70000, 76000 } });
}

const char* LYRICS =
    "[Estrofa 1]\n"
    "Una noche de Navidad\n"
    "Las estrellas brillan\n"
    "\n"
    "[Estribillo]\n"
    "Vuelve la Navidad\n"
    "Vuelve la Navidad\n";

int Active(const PlanSegment& s) {
    return static_cast<int>(std::count_if(s.assignments.begin(), s.assignments.end(), [](const PlanAssignment& a) { return a.prominence != Prominence::Rest; }));
}

bool Has(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

const PlanAssignment* Find(const PlanSegment& s, const std::string& target) {
    for (const PlanAssignment& a : s.assignments) {
        if (a.target == target) {
            return &a;
        }
    }
    return nullptr;
}

class Suite {
public:
    Suite(const fs::path& root, const std::string& realShow) :
        _root(root),
        _realShow(realShow) {}

    int Run() {
        _standard = StandardSong();
        if (!LoadFixture()) {
            printf("composer-plan-test: the synthetic layout could not be loaded\n");
            return 1;
        }
        TestCapabilities();
        TestRolesFile();
        TestProfile();
        TestLyricsCases();
        TestMusicCases();
        TestConstraints();
        TestCorrections();
        TestReproducibility();
        TestLevels();
        TestProperties();
        TestValidator();
        WriteOutputs();
        TestRealLayout();
        printf("composer-plan-test: %d checks passed, %d failed (%s)\n", _passed, _failed, _root.string().c_str());
        printf("composer-plan-test: profile is the Neutral Starter Profile; no learned style is claimed\n");
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

    PlannerInput Input(const SongAnalysis& song, AutomationLevel level = AutomationLevel::Balanced, uint64_t seed = 7) const {
        PlannerInput in;
        in.analysis = &song;
        in.capabilities = &_caps;
        in.options.level = level;
        in.options.seed = seed;
        return in;
    }

    bool LoadFixture() {
        const fs::path show = _root / "fixture";
        if (!WriteFixture(show)) {
            return false;
        }
        RoleOverrides roles;
        roles.models.push_back({ "Excluido", "", {}, true, "test: kept out" });
        HeadlessRenderContext ctx;
        if (!ctx.LoadShowFolder(show.string())) {
            return false;
        }
        const auto start = std::chrono::steady_clock::now();
        _caps = ExtractCapabilities(ctx, &roles);
        printf("composer-plan-test: synthetic layout extracted in %lld ms: %d models, %d groups, %d submodels\n", ElapsedMS(start), _caps.Count(TargetType::Model), _caps.Count(TargetType::Group), _caps.Count(TargetType::SubModel));
        (void)ctx.CloseSequence();
        return _caps.Count(TargetType::Model) == 11 && _caps.Count(TargetType::Group) == 5;
    }

    bool HasRoleFrom(const std::string& name, Role role, const char* source) const {
        const TargetInfo* t = _caps.Find(name);
        return t != nullptr && std::any_of(t->roles.begin(), t->roles.end(), [&](const RoleAssignment& r) { return r.role == role && r.source == source; });
    }

    // ------------------------------------------------------------ capabilities

    void TestCapabilities() {
        Title("ModelCapabilities on a synthetic layout");
        const TargetInfo* matrix = _caps.Find("Matriz Prueba");
        Check(matrix != nullptr && HasRoleFrom("Matriz Prueba", Role::Matrix, "display-type") && matrix->Can(Capability::Text).support == Support::Yes && matrix->Can(Capability::Video).support == Support::Yes,
              fmt::format("1. matrix: role from its type, text and video ({}x{})", matrix ? matrix->bufferWi : 0, matrix ? matrix->bufferHt : 0));
        const TargetInfo* mega = _caps.Find("Arbol Grande");
        Check(mega != nullptr && HasRoleFrom("Arbol Grande", Role::MegaTree, "geometry"), fmt::format("2. mega tree by size: {} nodes", mega ? mega->nodes : 0));
        Check(HasRoleFrom("Arbol Pequeno", Role::SmallTree, "geometry") && !_caps.Find("Arbol Pequeno")->HasRole(Role::MegaTree), "3. small tree: a Tree is not a mega tree for being a tree");
        const TargetInfo* window = _caps.Find("Ventana Matrix 1");
        Check(window != nullptr && HasRoleFrom("Ventana Matrix 1", Role::Window, "display-type") && !window->HasRole(Role::Matrix), "4. window: \"Ventana Matrix 1\" is a window, not a matrix, despite its name");
        Check(HasRoleFrom("Arco 1", Role::Arch, "display-type"), "5. arch");
        Check(HasRoleFrom("Linea 1", Role::Line, "display-type"), "6. line");
        const TargetInfo* face = _caps.Find("Carita");
        Check(face != nullptr && face->HasRole(Role::Face) && face->HasRole(Role::Figure) && face->Can(Capability::Face).support == Support::Yes && face->PrimaryRole()->role == Role::Face,
              "7. figure with a face: Face from its definition, Figure from its shape");
        const TargetInfo* arches = _caps.Find("Grupo Arcos");
        Check(arches != nullptr && arches->type == TargetType::Group && arches->flatModels.size() == 2, "8. group with its members");
        const TargetInfo* all = _caps.Find("Grupo Todo");
        const bool nested = all != nullptr && Has(all->flatModels, "Linea 1") && Has(all->flatModels, "Arco 1") && Has(all->flatModels, "Arbol Grande") && all->flatModels.size() == 6;
        Check(nested, fmt::format("9. nested groups resolved to {} models", all ? all->flatModels.size() : 0));
        const TargetInfo* arm = _caps.Find("Sin Controlador/Brazo");
        Check(arm != nullptr && arm->type == TargetType::SubModel && arm->parent == "Sin Controlador" && _caps.Overlap("Sin Controlador/Brazo", "Sin Controlador"), "10. submodel, overlapping its parent");
        const TargetInfo* free = _caps.Find("Sin Controlador");
        Check(free != nullptr && free->noController && !free->Excluded() && free->Can(Capability::Geometric).support == Support::Yes, "11. a model with no controller stays available");
        const TargetInfo* excluded = _caps.Find("Excluido");
        Check(excluded != nullptr && excluded->Excluded() && excluded->roles[0].status == RoleStatus::ExcludedManually, "12. excluded by hand");
        Check(mega != nullptr && _caps.Find("Arbol Cara")->HasRole(Role::MegaTree) && _caps.Find("Arbol Cara")->HasRole(Role::Face), "13. one model, two roles: mega tree and face");
        TargetInfo odd;
        odd.name = "Raro";
        odd.displayAs = "Something New";
        odd.nodes = 40;
        odd.bufferWi = 40;
        odd.bufferHt = 1;
        ClassifyTarget(odd);
        Check(odd.roles.empty(), "14. an unrecognised model type has an unknown role, not a guessed one");
        Check(_caps.Overlap("Grupo Todo", "Arco 1") && _caps.Overlap("Grupo Patio", "Grupo Todo") && !_caps.Overlap("Matriz Prueba", "Linea 1"), "group overlaps are known, member by member");
        Check(free->Can(Capability::Text).support == Support::Unknown && free->Can(Capability::Text).reason == "legibility-depends-on-node-layout", "26. text on a custom model is unknown, not assumed");
        Check(window->Can(Capability::Text).support != Support::Yes, "a window frame is not given text it cannot show");
        const TargetInfo* broken = _caps.Find("Grupo Roto");
        const bool reported = std::any_of(broken->warnings.begin(), broken->warnings.end(), [](const std::string& w) { return w.rfind("member-missing", 0) == 0; });
        Check(reported || broken->members.size() == 1, fmt::format("a group member that does not exist is reported, or the layout loader already removed it ({} members)", broken->members.size()));
        const std::string json = ToJson(_caps);
        Check(json.find("\"unresolved\"") != std::string::npos && json.find("\"capabilities\"") != std::string::npos && ToJson(_caps) == json, "capabilities serialize, twice the same");
    }

    void TestRolesFile() {
        Title("roles.json: manual roles win, identity is checked");
        ModelCapabilities caps = _caps;
        RoleOverrides roles = RoleTemplate(caps);
        Check(roles.models.size() == 11 && !roles.models[0].signature.empty(), "a template lists every model with its signature");
        std::string error;
        RoleOverrides read;
        Check(FromJson(ToJson(roles), read, error) && ToJson(read) == ToJson(roles), "template written and read back identically");
        RoleOverrides edits;
        edits.layoutFingerprint = caps.layoutFingerprint;
        edits.models.push_back({ "Carita", caps.Find("Carita")->signature, { Role::Figure }, false, "" });
        edits.models.push_back({ "Arco 1", "Arches:999:1x1", { Role::Prop }, false, "" });
        edits.models.push_back({ "Modelo Viejo", caps.Find("Linea 1")->signature, { Role::Line }, false, "" });
        ApplyRoleOverrides(caps, edits);
        const TargetInfo* face = caps.Find("Carita");
        Check(face->roles.size() == 1 && face->roles[0].role == Role::Figure && face->roles[0].status == RoleStatus::Confirmed, "a manual role is confirmed and replaces the suggestions");
        Check(caps.Find("Arco 1")->roles[0].role == Role::Arch && Has(caps.warnings, "role-identity-changed:Arco 1 (Arches:999:1x1 -> " + caps.Find("Arco 1")->signature + ")"), "a model whose shape changed keeps its inferred role, with a warning");
        Check(std::any_of(caps.warnings.begin(), caps.warnings.end(), [](const std::string& w) { return w.rfind("role-model-missing:Modelo Viejo (possibly renamed to Linea 1)", 0) == 0; }),
              "a missing model is reported, and a same-shaped model only as a possible rename");
        Check(caps.Find("Linea 1")->roles[0].status == RoleStatus::Suggested, "the role is not moved to that model on its own");
        Check(!FromJson("{\"models\":[{\"name\":\"x\",\"roles\":[\"dragon\"]}]}", read, error), "an unknown role in roles.json is an error");
    }

    void TestProfile() {
        Title("Neutral Starter Profile");
        const StyleProfile p = NeutralStarterProfile();
        StyleProfile back;
        std::string error;
        Check(FromJson(ToJson(p), back, error) && ToJson(back) == ToJson(p), "written and read back identically");
        Check(!p.learned && p.name == "Neutral Starter Profile", "not presented as a learned style");
        // The checked-in copy is what a person edits; it must say what the code does.
        const fs::path file = fs::path(__FILE__).parent_path().parent_path().parent_path() / "resources" / "composer" / "neutral-starter-profile.json";
        std::string text;
        if (ReadText(file, text)) {
            std::replace(text.begin(), text.end(), '\r', ' ');
            text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
            std::string expected = ToJson(p);
            expected.erase(std::remove(expected.begin(), expected.end(), ' '), expected.end());
            Check(text == expected || text == expected + "\n", "resources/composer/neutral-starter-profile.json matches the built-in profile");
        } else {
            printf("  (profile file not found next to the sources; check skipped)\n");
        }
        StyleProfile edited = p;
        edited.id = "mine";
        edited.maxSegmentMS = 8000;
        Check(FromJson(ToJson(edited), back, error) && back.maxSegmentMS == 8000, "an edited profile is read with its values");
    }

    // ------------------------------------------------------------ lyrics

    void TestLyricsCases() {
        Title("Lyrics");
        const SongAnalysis song = StandardSong();
        const CompositionPlan none = PlanComposition(Input(song));
        const auto anyIntent = [](const CompositionPlan& p, const char* intent) {
            for (const PlanSegment& s : p.segments) {
                for (const PlanAssignment& a : s.assignments) {
                    if (a.intent == intent) {
                        return true;
                    }
                }
            }
            return false;
        };
        Check(!anyIntent(none, "face-sing") && !anyIntent(none, "lyric-text") && Has(none.warnings, "no-lyrics"), "16. no lyrics: a musical plan, no faces singing and no text");
        const LyricDocument doc = ParsePlainLyrics(LYRICS);
        PlannerInput unsynced = Input(song);
        unsynced.lyrics = &doc;
        const CompositionPlan words = PlanComposition(unsynced);
        Check(!anyIntent(words, "face-sing") && !anyIntent(words, "lyric-text") && Has(words.warnings, "lyrics-not-synchronized") && !words.lyricsHash.empty(),
              "17. lyrics without times: known, hashed, but no face or text pretends to be in time");
        Check(Has(words.warnings, "user-chorus-without-times"), "a tagged chorus without times is reported, not placed");

        AlignerInput align;
        align.document = &doc;
        std::vector<std::pair<int, int>> times = { { 15000, 19000 }, { 20000, 24000 }, { 31000, 35000 }, { 36000, 40000 } };
        size_t k = 0;
        for (const LyricLine* line : doc.Lines()) {
            LyricCorrection c;
            c.targetId = line->id;
            c.startMS = times[k].first;
            c.endMS = times[k].second;
            align.corrections.push_back(c);
            ++k;
        }
        _alignment = AlignLyrics(align);
        PlannerInput synced = Input(song);
        synced.lyrics = &doc;
        synced.alignment = &_alignment;
        const CompositionPlan sung = PlanComposition(synced);
        bool facesOnlyOnFaces = true;
        bool facesWhileSinging = false;
        bool textOnMatrix = false;
        for (const PlanSegment& s : sung.segments) {
            for (const PlanAssignment& a : s.assignments) {
                if (a.intent == "face-sing") {
                    facesOnlyOnFaces = facesOnlyOnFaces && _caps.Find(a.target)->Can(Capability::Face).support == Support::Yes;
                    facesWhileSinging = facesWhileSinging || s.vocalCoverage > 0;
                }
                textOnMatrix = textOnMatrix || (a.intent == "lyric-text" && a.target == "Matriz Prueba");
            }
        }
        Check(sung.lyricsSynchronized && facesWhileSinging && facesOnlyOnFaces, "18. synced lyrics: faces lead while phrases are sung, and only faces that can");
        Check(textOnMatrix, "and the words go to the target that can show text");
        bool noFaceOutside = true;
        for (const PlanSegment& s : sung.segments) {
            for (const PlanAssignment& a : s.assignments) {
                noFaceOutside = noFaceOutside && !(a.intent == "face-sing" && s.vocalCoverage < 0.2);
            }
        }
        Check(noFaceOutside, "no face sings where nothing is sung");
        const PlanSegment* chorus = nullptr;
        const PlanSegment* untagged = nullptr;
        for (const PlanSegment& s : sung.segments) {
            if (Has(s.flags, "user-chorus") && chorus == nullptr) {
                chorus = &s;
            }
            if (s.sectionIndex == 5 && untagged == nullptr) {
                untagged = &s;
            }
        }
        Check(chorus != nullptr && chorus->priority == 2 && Has({ chorus->reasons[0].code, chorus->reasons.size() > 1 ? chorus->reasons[1].code : "", chorus->reasons.size() > 2 ? chorus->reasons[2].code : "" }, "user-tagged-chorus"),
              "20. a chorus tagged by the person is marked and raised");
        Check(chorus != nullptr && untagged != nullptr && chorus->targetIntensity >= untagged->targetIntensity - 0.05, fmt::format("its intensity {:.2f} against the untagged chorus {:.2f}", chorus ? chorus->targetIntensity : 0, untagged ? untagged->targetIntensity : 0));
        _sungPlan = sung;
    }

    // ------------------------------------------------------------ music

    void TestMusicCases() {
        Title("Music");
        const SongAnalysis unknown = MakeSong({ { 0, 20000, SectionKind::Unknown, -1, 0.3 }, { 20000, 40000, SectionKind::Unknown, -1, 0.7 }, { 40000, 60000, SectionKind::Unknown, -1, 0.4 } });
        const CompositionPlan u = PlanComposition(Input(unknown));
        const bool used = std::all_of(u.segments.begin(), u.segments.end(), [](const PlanSegment& s) { return std::any_of(s.reasons.begin(), s.reasons.end(), [](const Reason& r) { return r.code == "section-unknown-energy-used"; }); });
        Check(used && ValidateCompositionPlan(u, _caps, nullptr).ErrorCount() == 0, "19. unknown sections: planned from energy, no structure invented");
        double low = 0;
        double high = 0;
        for (const PlanSegment& s : u.segments) {
            (s.sectionIndex == 1 ? high : low) = std::max(s.sectionIndex == 1 ? high : low, s.density);
        }
        Check(high > low, fmt::format("more density where the energy is higher ({:.2f} > {:.2f})", high, low));

        const CompositionPlan p = PlanComposition(Input(_standard));
        const PlanSegment* climax = nullptr;
        const PlanSegment* verse = nullptr;
        for (const PlanSegment& s : p.segments) {
            if (Has(s.flags, "climax")) {
                climax = &s;
            }
            if (s.sectionKind == "verse" && verse == nullptr) {
                verse = &s;
            }
        }
        const auto leads = [](const PlanSegment& s) { return std::count_if(s.assignments.begin(), s.assignments.end(), [](const PlanAssignment& a) { return a.prominence == Prominence::Lead; }); };
        Check(climax != nullptr && climax->priority == 3 && verse != nullptr && climax->density > verse->density && leads(*climax) >= leads(*verse),
              fmt::format("21. climax: its own segment, priority 3, density {:.2f} against verse {:.2f}", climax ? climax->density : 0, verse ? verse->density : 0));
        bool silent = true;
        int silences = 0;
        for (const PlanSegment& s : p.segments) {
            if (Has(s.flags, "silence")) {
                ++silences;
                silent = silent && Active(s) == 0;
            }
        }
        Check(silences == 2 && silent, fmt::format("22. silence: {} rest segments with nothing active", silences));

        const SongAnalysis step = MakeSong({ { 0, 30000, SectionKind::Unknown, -1, 0.3 } }, {}, {}, [](int t) { return t < 17000 ? 0.2 : 0.8; });
        const CompositionPlan stepped = PlanComposition(Input(step));
        const bool split = std::any_of(stepped.segments.begin(), stepped.segments.end(), [](const PlanSegment& s) { return std::abs(s.startMS - 17000) <= 2000; });
        Check(split, "23. an energy change inside a section splits it there");

        const SongAnalysis tempo = MakeSong({ { 0, 40000, SectionKind::Verse, -1, 0.5 } }, {}, [](int t) { return 600 - t / 200; });
        const CompositionPlan t = PlanComposition(Input(tempo));
        std::set<int> downbeats;
        for (const MusicalEvent& e : tempo.events) {
            if (e.type == MusicalEventType::Downbeat) {
                downbeats.insert(static_cast<int>(std::lround(e.timeMS / 25.0)) * 25);
            }
        }
        bool onDownbeats = true;
        for (size_t i = 1; i < t.segments.size(); ++i) {
            onDownbeats = onDownbeats && downbeats.contains(t.segments[i].startMS);
        }
        Check(t.segments.size() >= 3 && onDownbeats && ValidateCompositionPlan(t, _caps, nullptr).ErrorCount() == 0, fmt::format("24. accelerating tempo: {} segments cut on downbeats", t.segments.size()));
        bool sized = true;
        for (const PlanSegment& s : p.segments) {
            sized = sized && (Has(s.flags, "silence") || (s.endMS - s.startMS >= 4000 && s.endMS - s.startMS <= 16000));
        }
        Check(sized, "segment lengths within the profile, not one per beat");
    }

    // ------------------------------------------------------------ constraints

    void TestConstraints() {
        Title("Constraints");
        bool noOverlap = true;
        bool excludedRests = true;
        for (AutomationLevel level : { AutomationLevel::Conservative, AutomationLevel::Balanced, AutomationLevel::Creative }) {
            for (uint64_t seed : { 1ULL, 2ULL, 3ULL, 99ULL }) {
                const CompositionPlan p = PlanComposition(Input(_standard, level, seed));
                for (const PlanSegment& s : p.segments) {
                    for (size_t i = 0; i < s.assignments.size(); ++i) {
                        const PlanAssignment& a = s.assignments[i];
                        if (a.prominence == Prominence::Rest) {
                            continue;
                        }
                        excludedRests = excludedRests && a.target != "Excluido" && a.target != "Grupo Arcos";
                        for (size_t j = i + 1; j < s.assignments.size(); ++j) {
                            noOverlap = noOverlap && (s.assignments[j].prominence == Prominence::Rest || !_caps.Overlap(a.target, s.assignments[j].target));
                        }
                    }
                }
            }
        }
        Check(noOverlap, "15. a group and its members never compete in an automatic plan (12 plans)");
        Check(excludedRests, "an excluded model never takes part, nor a group lighting it");
        bool freeUsed = false;
        for (uint64_t seed = 1; seed < 6 && !freeUsed; ++seed) {
            const CompositionPlan p = PlanComposition(Input(_standard, AutomationLevel::Creative, seed));
            for (const PlanSegment& s : p.segments) {
                const PlanAssignment* a = Find(s, "Sin Controlador");
                freeUsed = freeUsed || (a != nullptr && a->prominence != Prominence::Rest);
            }
        }
        Check(freeUsed, "a model with no controller takes part visually");
        ModelCapabilities unknown = _caps;
        unknown.targets[unknown.IndexOf("Estrella 1")].roles.clear();
        PlannerInput in = Input(_standard, AutomationLevel::Creative);
        in.capabilities = &unknown;
        bool neverLeads = true;
        for (const PlanSegment& s : PlanComposition(in).segments) {
            const PlanAssignment* a = Find(s, "Estrella 1");
            neverLeads = neverLeads && (a == nullptr || a->prominence != Prominence::Lead);
        }
        Check(neverLeads, "14. a target with an unknown role never leads");
        PlannerInput badLock = Input(_standard);
        PlanCorrection lock;
        lock.action = PlanCorrection::Action::Lock;
        lock.startMS = 14000;
        lock.endMS = 30000;
        PlanAssignment wrong;
        wrong.target = "Linea 1";
        wrong.prominence = Prominence::Lead;
        wrong.intent = "face-sing";
        lock.assignments.push_back(wrong);
        badLock.corrections.push_back(lock);
        const CompositionPlan refused = PlanComposition(badLock);
        const bool reported = std::any_of(refused.conflicts.begin(), refused.conflicts.end(), [](const PlanConflict& c) { return c.target == "Linea 1" && c.reason == "capability-no:face"; });
        bool notApplied = true;
        for (const PlanSegment& s : refused.segments) {
            const PlanAssignment* a = Find(s, "Linea 1");
            notApplied = notApplied && (a == nullptr || a->intent != "face-sing");
        }
        Check(reported && notApplied, "25. an incompatible manual decision (a line singing) is reported and not applied");
    }

    // ------------------------------------------------------------ corrections

    void TestCorrections() {
        Title("Manual decisions");
        const SongAnalysis song = StandardSong();
        const CompositionPlan base = PlanComposition(Input(song));
        PlannerInput in = Input(song);
        PlanCorrection exclude;
        exclude.action = PlanCorrection::Action::Exclude;
        exclude.target = "Arbol Grande";
        in.corrections.push_back(exclude);
        const CompositionPlan without = PlanComposition(in);
        bool rested = true;
        for (const PlanSegment& s : without.segments) {
            const PlanAssignment* a = Find(s, "Arbol Grande");
            rested = rested && a != nullptr && a->prominence == Prominence::Rest && a->state == DecisionState::ManualEdit;
        }
        Check(rested, "exclude a figure for the whole song");

        in.corrections = {};
        PlanCorrection include;
        include.action = PlanCorrection::Action::Include;
        include.startMS = 30000;
        include.endMS = 46000;
        include.target = "Grupo Arboles";
        include.prominence = Prominence::Lead;
        in.corrections.push_back(include);
        const CompositionPlan withGroup = PlanComposition(in);
        bool groupLeads = false;
        bool membersYield = true;
        for (const PlanSegment& s : withGroup.segments) {
            if (s.startMS < 30000 || s.startMS >= 46000) {
                continue;
            }
            const PlanAssignment* g = Find(s, "Grupo Arboles");
            groupLeads = groupLeads || (g != nullptr && g->prominence == Prominence::Lead && g->state == DecisionState::ManualEdit);
            for (const PlanAssignment& a : s.assignments) {
                membersYield = membersYield && (a.target == "Grupo Arboles" || a.prominence == Prominence::Rest || !_caps.Overlap(a.target, "Grupo Arboles"));
            }
        }
        Check(groupLeads && membersYield, "add a group as lead: automatic members give way to it");
        PlannerInput through = Input(song);
        PlanCorrection backDoor = include;
        backDoor.target = "Grupo Todo";
        through.corrections.push_back(backDoor);
        const CompositionPlan refusedGroup = PlanComposition(through);
        Check(std::any_of(refusedGroup.conflicts.begin(), refusedGroup.conflicts.end(), [](const PlanConflict& c) { return c.target == "Grupo Todo" && c.reason == "lights-excluded-model"; }),
              "a group that would light an excluded model is refused, and said so");

        PlanCorrection member = include;
        member.target = "Arbol Grande";
        in.corrections.push_back(member);
        const CompositionPlan both = PlanComposition(in);
        Check(std::any_of(both.conflicts.begin(), both.conflicts.end(), [](const PlanConflict& c) { return c.target == "Arbol Grande" && c.reason.rfind("group-conflict-with-manual", 0) == 0; }),
              "two manual decisions on the same lights are a conflict, not a silent choice");

        in.corrections = {};
        PlanCorrection density;
        density.action = PlanCorrection::Action::SetDensity;
        density.startMS = 14000;
        density.endMS = 30000;
        density.density = 0.9;
        in.corrections.push_back(density);
        const CompositionPlan denser = PlanComposition(in);
        double before = 0;
        double after = 0;
        for (size_t i = 0; i < base.segments.size() && i < denser.segments.size(); ++i) {
            if (base.segments[i].sectionIndex == 2) {
                before = std::max(before, base.segments[i].density);
                after = std::max(after, denser.segments[i].density);
            }
        }
        Check(after > before, fmt::format("change the density of a stretch ({:.2f} -> {:.2f})", before, after));

        in.corrections = {};
        PlanCorrection move;
        move.action = PlanCorrection::Action::MoveBoundary;
        move.startMS = 50000;
        move.endMS = 57000;
        in.corrections.push_back(move);
        const CompositionPlan moved = PlanComposition(in);
        Check(std::any_of(moved.segments.begin(), moved.segments.end(), [](const PlanSegment& s) { return s.startMS == 50000 && s.endMS == 57000 && s.state == DecisionState::ManualEdit; }), "move a segment's limits: the segment is exactly where it was put");

        in.corrections = {};
        std::string lead;
        for (const PlanAssignment& a : base.segments[2].assignments) {
            if (a.prominence == Prominence::Lead) {
                lead = a.target;
            }
        }
        PlanCorrection reject;
        reject.action = PlanCorrection::Action::Reject;
        reject.startMS = base.segments[2].startMS;
        reject.endMS = base.segments[2].endMS;
        reject.target = lead;
        PlanCorrection accept;
        accept.action = PlanCorrection::Action::Accept;
        accept.startMS = base.segments[3].startMS;
        accept.endMS = base.segments[3].endMS;
        in.corrections = { reject, accept };
        const CompositionPlan reviewed = PlanComposition(in);
        const PlanAssignment* rejected = Find(reviewed.segments[2], lead);
        Check(!lead.empty() && rejected != nullptr && rejected->prominence == Prominence::Rest && rejected->state == DecisionState::ManualEdit, fmt::format("reject a proposal: {} rests where it was lead", lead));
        Check(reviewed.segments[3].state == DecisionState::ApprovedPreference && std::all_of(reviewed.segments[3].assignments.begin(), reviewed.segments[3].assignments.end(), [](const PlanAssignment& a) { return a.state == DecisionState::ApprovedPreference; }),
              "accept a proposal: its decisions become approved preferences");
    }

    // ------------------------------------------------------------ reproducibility

    void TestReproducibility() {
        Title("Reproducibility");
        const SongAnalysis song = StandardSong();
        const std::string a = ToJson(PlanComposition(Input(song, AutomationLevel::Balanced, 42)));
        const std::string b = ToJson(PlanComposition(Input(song, AutomationLevel::Balanced, 42)));
        const std::string c = ToJson(PlanComposition(Input(song, AutomationLevel::Balanced, 43)));
        Check(a == b, "27. same input and seed: the same plan, byte for byte");
        Check(a != c, "another seed: another plan");
        CompositionPlan read;
        std::string error;
        Check(FromJson(a, read, error) && ToJson(read) == a, "the plan reads back to the same bytes");

        const CompositionPlan original = PlanComposition(Input(song, AutomationLevel::Balanced, 42));
        const SongAnalysis changed = StandardSong(0.7);
        const CompositionPlan edited = PlanComposition(Input(changed, AutomationLevel::Balanced, 42));
        bool othersSame = original.segments.size() == edited.segments.size();
        bool changedDiffers = false;
        for (size_t i = 0; othersSame && i < original.segments.size(); ++i) {
            const PlanSegment& x = original.segments[i];
            const PlanSegment& y = edited.segments[i];
            CompositionPlan one;
            one.segments = { x };
            CompositionPlan two;
            two.segments = { y };
            if (x.sectionIndex == 4) {
                changedDiffers = changedDiffers || ToJson(one) != ToJson(two);
            } else {
                othersSame = ToJson(one) == ToJson(two);
            }
        }
        Check(othersSame && changedDiffers, "28. changing one section changes only that section's decisions");

        PlannerInput in = Input(song, AutomationLevel::Balanced, 42);
        PlanCorrection lock;
        lock.action = PlanCorrection::Action::Lock;
        lock.startMS = original.segments[3].startMS;
        lock.endMS = original.segments[3].endMS;
        for (const PlanAssignment& x : original.segments[3].assignments) {
            if (x.prominence != Prominence::Rest) {
                lock.assignments.push_back(x);
            }
        }
        in.corrections.push_back(lock);
        in.options.seed = 1234;
        in.options.level = AutomationLevel::Creative;
        const CompositionPlan regenerated = PlanComposition(in);
        const PlanSegment* kept = nullptr;
        for (const PlanSegment& s : regenerated.segments) {
            if (s.startMS == lock.startMS && s.endMS == lock.endMS) {
                kept = &s;
            }
        }
        bool same = kept != nullptr && kept->state == DecisionState::Locked;
        for (const PlanAssignment& x : lock.assignments) {
            const PlanAssignment* y = kept != nullptr ? Find(*kept, x.target) : nullptr;
            same = same && y != nullptr && y->prominence == x.prominence && y->intent == x.intent && y->state == DecisionState::Locked;
        }
        Check(same && ValidateCompositionPlan(regenerated, _caps, nullptr).ErrorCount() == 0, "29. a locked segment survives regenerating with another seed and level");
    }

    // ------------------------------------------------------------ levels

    struct Measure {
        double active = 0;
        double density = 0;
        int distinctLeads = 0;
        int accents = 0;
    };

    Measure MeasurePlan(const CompositionPlan& p) {
        Measure m;
        std::set<std::string> leads;
        int counted = 0;
        for (const PlanSegment& s : p.segments) {
            if (Has(s.flags, "silence")) {
                continue;
            }
            ++counted;
            m.active += Active(s);
            m.density += s.density;
            for (const PlanAssignment& a : s.assignments) {
                if (a.prominence == Prominence::Lead) {
                    leads.insert(a.target);
                }
                m.accents += a.prominence == Prominence::Accent ? 1 : 0;
            }
        }
        m.active /= std::max(1, counted);
        m.density /= std::max(1, counted);
        m.distinctLeads = static_cast<int>(leads.size());
        return m;
    }

    void TestLevels() {
        Title("Automation levels");
        const SongAnalysis song = StandardSong();
        const Measure c = MeasurePlan(PlanComposition(Input(song, AutomationLevel::Conservative)));
        const Measure b = MeasurePlan(PlanComposition(Input(song, AutomationLevel::Balanced)));
        const Measure r = MeasurePlan(PlanComposition(Input(song, AutomationLevel::Creative)));
        printf("  conservative: %.2f active, density %.2f, %d leads, %d accents\n", c.active, c.density, c.distinctLeads, c.accents);
        printf("  balanced:     %.2f active, density %.2f, %d leads, %d accents\n", b.active, b.density, b.distinctLeads, b.accents);
        printf("  creative:     %.2f active, density %.2f, %d leads, %d accents\n", r.active, r.density, r.distinctLeads, r.accents);
        Check(c.active < b.active && b.active < r.active, "30. more figures active from conservative to creative");
        Check(c.density <= b.density && b.density <= r.density, "and more of the layout lit");
        Check(c.accents == 0 && b.accents > 0 && r.accents > b.accents, "accents: none, some, more");
        Check(c.distinctLeads <= b.distinctLeads && b.distinctLeads <= r.distinctLeads && c.distinctLeads < r.distinctLeads, "more variety of leads");
    }

    // ------------------------------------------------------------ properties

    void TestProperties() {
        Title("Properties over many plans");
        int plans = 0;
        bool tiled = true;
        bool leadEverywhere = true;
        bool validated = true;
        bool roundTrip = true;
        double maxShare = 0;
        std::vector<SongAnalysis> songs = { StandardSong(), StandardSong(0.7) };
        std::vector<SectionDef> many;
        for (int i = 0; i < 20; ++i) {
            many.push_back({ i * 15000, (i + 1) * 15000, i % 2 ? SectionKind::Chorus : SectionKind::Verse, i % 2, i % 2 ? 0.75 : 0.45 });
        }
        songs.push_back(MakeSong(many));
        for (const SongAnalysis& song : songs) {
            for (AutomationLevel level : { AutomationLevel::Conservative, AutomationLevel::Balanced, AutomationLevel::Creative }) {
                for (uint64_t seed = 1; seed <= 5; ++seed) {
                    const CompositionPlan p = PlanComposition(Input(song, level, seed));
                    ++plans;
                    int expected = 0;
                    std::map<std::string, int> leadTime;
                    int music = 0;
                    for (const PlanSegment& s : p.segments) {
                        tiled = tiled && s.startMS == expected && s.endMS > s.startMS;
                        expected = s.endMS;
                        const bool silence = Has(s.flags, "silence");
                        const bool lead = std::any_of(s.assignments.begin(), s.assignments.end(), [](const PlanAssignment& a) { return a.prominence == Prominence::Lead; });
                        leadEverywhere = leadEverywhere && (silence || lead);
                        if (!silence) {
                            music += s.endMS - s.startMS;
                            for (const PlanAssignment& a : s.assignments) {
                                if (a.prominence == Prominence::Lead && a.intent != "face-sing") {
                                    leadTime[a.target] += s.endMS - s.startMS;
                                }
                            }
                        }
                    }
                    tiled = tiled && expected == song.durationMS;
                    if (&song == &songs.back()) {
                        for (const auto& [target, ms] : leadTime) {
                            maxShare = std::max(maxShare, static_cast<double>(ms) / music);
                        }
                    }
                    validated = validated && ValidateCompositionPlan(p, _caps, nullptr).ErrorCount() == 0;
                    CompositionPlan read;
                    std::string error;
                    roundTrip = roundTrip && FromJson(ToJson(p), read, error) && ToJson(read) == ToJson(p);
                }
            }
        }
        Check(tiled, fmt::format("segments tile the whole song with no gap ({} plans)", plans));
        Check(leadEverywhere, "every segment with music has a lead");
        Check(validated, "GenerationValidator finds no error in any of them");
        Check(roundTrip, "every plan reads back to the same bytes");
        Check(maxShare <= 0.6, fmt::format("no figure monopolises a 5-minute song: the busiest leads {:.0f}% of it", maxShare * 100));
    }

    void TestValidator() {
        Title("GenerationValidator on a plan");
        CompositionPlan p = PlanComposition(Input(_standard));
        Check(ValidateCompositionPlan(p, _caps, nullptr).ErrorCount() == 0, "a plan as generated passes");
        CompositionPlan wrongLayout = p;
        wrongLayout.layoutHash = "0000";
        Check(ValidateCompositionPlan(wrongLayout, _caps, nullptr).Has(issue::PlanHashMismatch), "another layout's plan");
        CompositionPlan gap = p;
        gap.segments[1].startMS += 1000;
        Check(ValidateCompositionPlan(gap, _caps, nullptr).Has(issue::PlanSegmentInvalid), "a gap between segments");
        CompositionPlan ghost = p;
        ghost.segments[2].assignments.push_back({ "Zzz Inexistente", TargetType::Model, Role::Prop, Prominence::Support, "accompany" });
        Check(ValidateCompositionPlan(ghost, _caps, nullptr).Has(issue::PlanTargetUnknown), "a target the layout does not have");
        CompositionPlan swapped = p;
        std::reverse(swapped.segments[2].assignments.begin(), swapped.segments[2].assignments.end());
        Check(ValidateCompositionPlan(swapped, _caps, nullptr).Has(issue::PlanOrderUnstable), "decisions out of their stable order");
        const auto setIntent = [&](CompositionPlan plan, const std::string& target, const char* intent, Prominence prominence) {
            auto& list = plan.segments[2].assignments;
            auto it = std::find_if(list.begin(), list.end(), [&](const PlanAssignment& a) { return a.target == target; });
            if (it == list.end()) {
                PlanAssignment a;
                a.target = target;
                a.type = _caps.Find(target)->type;
                list.push_back(a);
                std::sort(list.begin(), list.end(), [](const PlanAssignment& x, const PlanAssignment& y) { return x.target < y.target; });
                it = std::find_if(list.begin(), list.end(), [&](const PlanAssignment& a) { return a.target == target; });
            }
            it->intent = intent;
            it->prominence = prominence;
            return plan;
        };
        Check(ValidateCompositionPlan(setIntent(p, "Linea 1", "face-sing", Prominence::Lead), _caps, nullptr).Has(issue::PlanRoleIncompatible), "25. a line asked to sing: incompatible");
        const ValidationReport unknownText = ValidateCompositionPlan(setIntent(p, "Sin Controlador", "lyric-text", Prominence::Support), _caps, nullptr);
        Check(unknownText.Has(issue::PlanCapabilityUnknown) && !unknownText.Has(issue::PlanRoleIncompatible), "26. text on a custom model: unknown, a warning, not an incompatibility");
        Check(ValidateCompositionPlan(setIntent(p, "Excluido", "accompany", Prominence::Support), _caps, nullptr).Has(issue::PlanTargetExcluded), "an excluded model in use");
        Check(ValidateCompositionPlan(setIntent(p, "Grupo Patio", "accompany", Prominence::Support), _caps, nullptr).Has(issue::PlanTargetExcluded), "a group lighting an excluded model in use");
        CompositionPlan twoLeads = setIntent(setIntent(p, "Grupo Arboles", "feature", Prominence::Lead), "Arbol Grande", "feature", Prominence::Lead);
        const ValidationReport conflict = ValidateCompositionPlan(twoLeads, _caps, nullptr);
        const ValidationIssue* found = conflict.Find(issue::PlanGroupConflict);
        Check(found != nullptr && found->severity == IssueSeverity::Error, "a group and its member both leading: confirmed conflict");
        CompositionPlan mixed = setIntent(setIntent(p, "Grupo Arboles", "accompany", Prominence::Support), "Arbol Grande", "feature", Prominence::Lead);
        const ValidationReport mixedReport = ValidateCompositionPlan(mixed, _caps, nullptr);
        const ValidationIssue* possible = mixedReport.Find(issue::PlanGroupConflict);
        Check(possible != nullptr && possible->severity == IssueSeverity::Warning, "a group supporting while its member leads: possible conflict, a warning");
        StyleProfile allowing = NeutralStarterProfile();
        allowing.groupsMayOverlapMembers = true;
        const ValidationReport allowedReport = ValidateCompositionPlan(twoLeads, _caps, &allowing);
        const ValidationIssue* intended = allowedReport.Find(issue::PlanGroupConflict);
        Check(intended != nullptr && intended->severity == IssueSeverity::Info, "the same where the profile allows it: intentional, information only");
        PlannerInput lockIn = Input(_standard);
        PlanCorrection lock;
        lock.action = PlanCorrection::Action::Lock;
        lock.startMS = p.segments[3].startMS;
        lock.endMS = p.segments[3].endMS;
        PlanAssignment star;
        star.target = "Estrella 1";
        star.prominence = Prominence::Lead;
        star.intent = "feature";
        lock.assignments.push_back(star);
        lockIn.corrections.push_back(lock);
        CompositionPlan locked = PlanComposition(lockIn);
        Check(ValidateCompositionPlan(locked, _caps, nullptr).ErrorCount() == 0, "a plan with a locked segment passes");
        for (PlanSegment& s : locked.segments) {
            if (s.state == DecisionState::Locked) {
                for (PlanAssignment& a : s.assignments) {
                    if (a.target == "Estrella 1") {
                        a.prominence = Prominence::Support;
                    }
                }
            }
        }
        Check(ValidateCompositionPlan(locked, _caps, nullptr).Has(issue::PlanLockLost), "and a locked decision that was changed is caught");
        ValidationIssue sample;
        sample.code = issue::PlanGroupConflict;
        sample.model = "A";
        sample.args = { { "segment", "s1.0" }, { "over", "B" }, { "classification", "possible" } };
        Check(DescribeIssue(sample).find("s1.0") != std::string::npos && !DescribeSuggestion(sample).empty(), "every new code has a message and a suggestion");
    }

    void WriteOutputs() {
        Title("Outputs");
        const bool json = WriteText(_root / "composition-plan.json", ToJson(_sungPlan));
        const bool report = WriteText(_root / "composition-plan-report.txt", PlanReport(_sungPlan));
        const bool caps = WriteText(_root / "capabilities.json", ToJson(_caps)) && WriteText(_root / "capabilities-report.txt", CapabilitiesReport(_caps));
        const bool profile = WriteText(_root / "neutral-starter-profile.json", ToJson(NeutralStarterProfile()));
        Check(json && report && caps && profile, "composition-plan.json, its report, the capabilities and the profile written");
        Check(!fs::exists(_root / "fixture" / "xlights_networks.xml") && std::none_of(fs::begin(fs::directory_iterator(_root)), fs::end(fs::directory_iterator(_root)), [](const fs::directory_entry& e) { return e.path().extension() == ".xsq"; }),
              "no sequence written, no network file created");
    }

    // ------------------------------------------------------------ real layout

    void TestRealLayout() {
        if (_realShow.empty()) {
            return;
        }
        Title("Real layout (read only, controllers inactive)");
        std::string refused;
        if (!ControllersAreInactive(_realShow, refused)) {
            Check(false, "refused: " + refused);
            return;
        }
        HeadlessRenderContext ctx;
        auto start = std::chrono::steady_clock::now();
        if (!ctx.LoadShowFolder(_realShow)) {
            Check(false, "loading the show");
            return;
        }
        const long long load = ElapsedMS(start);
        start = std::chrono::steady_clock::now();
        const ModelCapabilities caps = ExtractCapabilities(ctx, nullptr);
        const long long extract = ElapsedMS(start);
        int faces = 0;
        int states = 0;
        int noController = 0;
        for (const TargetInfo& t : caps.targets) {
            if (t.type == TargetType::Model) {
                faces += static_cast<int>(t.faces.size());
                states += static_cast<int>(t.states.size());
                noController += t.noController ? 1 : 0;
            }
        }
        printf("  load %lld ms, extract %lld ms: %d models, %d groups, %d submodels, %d drawable faces, %d states, %d without controller\n", load, extract, caps.Count(TargetType::Model), caps.Count(TargetType::Group),
               caps.Count(TargetType::SubModel), faces, states, noController);
        Check(extract < 2000, fmt::format("capabilities extracted in {} ms (target under 2 s)", extract));
        Check(std::none_of(caps.targets.begin(), caps.targets.end(), [](const TargetInfo& t) { return t.noController && t.Excluded(); }), "no model is excluded for having no controller");
        std::vector<SectionDef> sections;
        for (int i = 0; i < 20; ++i) {
            sections.push_back({ i * 15000, (i + 1) * 15000, i % 2 ? SectionKind::Chorus : SectionKind::Verse, i % 2, 0.3 + 0.03 * i });
        }
        const SongAnalysis song = MakeSong(sections, { { 250000, 262000 } });
        for (AutomationLevel level : { AutomationLevel::Conservative, AutomationLevel::Balanced, AutomationLevel::Creative }) {
            PlannerInput in;
            in.analysis = &song;
            in.capabilities = &caps;
            in.options.level = level;
            in.options.seed = 2026;
            start = std::chrono::steady_clock::now();
            const CompositionPlan plan = PlanComposition(in);
            const long long planned = ElapsedMS(start);
            const ValidationReport report = ValidateCompositionPlan(plan, caps, nullptr);
            const Measure m = MeasurePlan(plan);
            printf("  %s: %zu segments in %lld ms, %.1f active, density %.2f, %d distinct leads, %d errors, %d warnings\n", LevelId(level), plan.segments.size(), planned, m.active, m.density, m.distinctLeads,
                   report.ErrorCount(), report.WarningCount());
            Check(planned < 5000 && report.ErrorCount() == 0, fmt::format("{}: a 5-minute song planned in {} ms and validated", LevelId(level), planned));
            if (level == AutomationLevel::Balanced) {
                (void)WriteText(_root / "real-layout-plan.json", ToJson(plan));
                (void)WriteText(_root / "real-layout-plan-report.txt", PlanReport(plan));
                (void)WriteText(_root / "real-layout-capabilities.json", ToJson(caps));
                (void)WriteText(_root / "real-layout-capabilities-report.txt", CapabilitiesReport(caps));
                (void)WriteText(_root / "real-layout-roles.template.json", ToJson(RoleTemplate(caps)));
            }
        }
        (void)ctx.CloseSequence();
    }

    fs::path _root;
    std::string _realShow;
    ModelCapabilities _caps;
    SongAnalysis _standard;
    LyricAlignment _alignment;
    CompositionPlan _sungPlan;
    int _passed = 0;
    int _failed = 0;
};

} // namespace

bool WritePlanFixture(const std::string& folder) {
    return WriteFixture(fs::path(folder));
}

SongAnalysis PlanTestSong(double secondVerseEnergy) {
    return StandardSong(secondVerseEnergy);
}

SongAnalysis PlanTestUnknownSong() {
    return MakeSong({ { 0, 20000, SectionKind::Unknown, -1, 0.3 }, { 20000, 40000, SectionKind::Unknown, -1, 0.7 }, { 40000, 60000, SectionKind::Unknown, -1, 0.4 } });
}

SongAnalysis PlanTestSilentSong() {
    return MakeSong({ { 0, 10000, SectionKind::Unknown, -1, 0, true } });
}

SongAnalysis PlanTestLongSong(int sections) {
    std::vector<SectionDef> defs;
    for (int i = 0; i < sections; ++i) {
        defs.push_back({ i * 15000, (i + 1) * 15000, i % 2 ? SectionKind::Chorus : SectionKind::Verse, i % 2, 0.3 + 0.03 * i });
    }
    return MakeSong(defs, { { (sections - 3) * 15000 + 5000, (sections - 3) * 15000 + 13000 } });
}

const char* PlanTestLyrics() {
    return LYRICS;
}

int RunCapabilitiesCommand(const std::string& showDir, const std::string& outPath) {
    std::string refused;
    if (!ControllersAreInactive(showDir, refused)) {
        printf("composer-model-capabilities: RECHAZADO: %s\n", refused.c_str());
        return 2;
    }
    RoleOverrides roles;
    bool haveRoles = false;
    const fs::path rolesPath = fs::path(showDir) / "ShowComposer" / "roles.json";
    std::string text;
    if (ReadText(rolesPath, text)) {
        std::string error;
        if (!FromJson(text, roles, error)) {
            printf("composer-model-capabilities: %s cannot be read: %s\n", rolesPath.string().c_str(), error.c_str());
            return 1;
        }
        haveRoles = true;
    }
    HeadlessRenderContext ctx;
    const auto start = std::chrono::steady_clock::now();
    if (!ctx.LoadShowFolder(showDir)) {
        printf("composer-model-capabilities: the show could not be loaded\n");
        return 1;
    }
    const ModelCapabilities caps = ExtractCapabilities(ctx, haveRoles ? &roles : nullptr);
    const long long elapsed = ElapsedMS(start);
    const fs::path out(outPath);
    const fs::path base = out.parent_path() / out.stem();
    const bool ok = WriteText(out, ToJson(caps)) && WriteText(base.string() + "-report.txt", CapabilitiesReport(caps)) && WriteText(base.string() + "-roles.template.json", ToJson(RoleTemplate(caps)));
    printf("%s", CapabilitiesReport(caps).c_str());
    printf("composer-model-capabilities: %s in %lld ms (roles.json %s); written %s\n", ok ? "done" : "FAILED", elapsed, haveRoles ? "applied" : "not present", out.string().c_str());
    (void)ctx.CloseSequence();
    return ok ? 0 : 1;
}

int RunPlanTest(const std::string& workDir, const std::string& showDir) {
    fs::path root;
    std::error_code ec;
    for (int i = 0; i < 1000; ++i) {
        root = fs::path(workDir) / fmt::format("composer-plan-test-{}", i);
        if (!fs::exists(root, ec)) {
            break;
        }
    }
    fs::create_directories(root, ec);
    if (ec) {
        printf("composer-plan-test: cannot create %s\n", root.string().c_str());
        return 1;
    }
    Suite suite(root, showDir);
    return suite.Run();
}

} // namespace composer
