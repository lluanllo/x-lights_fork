#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

#include "composer/RecipeTest.h"

#include "composer/Choreography.h"
#include "composer/CompositionPlan.h"
#include "composer/GenerationValidator.h"
#include "composer/LyricAligner.h"
#include "composer/LyricDocument.h"
#include "composer/ModelCapabilities.h"
#include "composer/PlanTest.h"
#include "composer/RecipeLibrary.h"
#include "composer/SongVisualPlan.h"
#include "composer/TemplateLibrary.h"
#include "composer/VisualDirection.h"
#include "effects/EffectManager.h"
#include "render/HeadlessRenderContext.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace composer {

namespace {

class Suite {
public:
    explicit Suite(const fs::path& root) :
        _root(root) {}

    int Run() {
        if (!Setup()) {
            return 1;
        }
        TestPortable();
        TestPersistence();
        TestHumanDecisions();
        TestTemplates();
        TestColours();
        TestBrief();
        TestDirector();
        TestChoreography();
        printf("composer-recipe-test: %d checks passed, %d failed (%s)\n", _passed, _failed, _root.string().c_str());
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
        const fs::path fixture = _root / "fixture";
        if (!WritePlanFixture(fixture.string())) {
            printf("composer-recipe-test: cannot write the fixture\n");
            return false;
        }
        _ctx = std::make_unique<HeadlessRenderContext>();
        if (!_ctx->LoadShowFolder(fixture.string())) {
            printf("composer-recipe-test: cannot load the fixture\n");
            return false;
        }
        _caps = ExtractCapabilities(*_ctx, nullptr);
        _metadata = MetadataFrom(_ctx->GetEffectManager());
        _library = LoadTemplateLibrary(DefaultTemplateDirectory(), _metadata);
        _song = PlanTestSong();
        _doc = ParsePlainLyrics(PlanTestLyrics());
        std::string error;
        const fs::path file = fs::path(DefaultTemplateDirectory()).parent_path() / "profiles" / "reference-all.json";
        std::ifstream in(file, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        if (!FromJson(text.str(), _profile, error) || !LoadThemeCatalog(DefaultThemeCatalogPath(), _themes, error)) {
            printf("composer-recipe-test: %s\n", error.c_str());
            return false;
        }
        _profile.maxParticipants = 2;
        PlannerInput pin;
        pin.analysis = &_song;
        pin.capabilities = &_caps;
        pin.options.level = AutomationLevel::Balanced;
        pin.options.seed = 7;
        pin.profile = &_profile;
        _plan = PlanComposition(pin);
        DirectionInput d;
        d.plan = &_plan;
        d.capabilities = &_caps;
        d.profile = &_profile;
        d.catalog = &_themes;
        d.analysis = &_song;
        d.lyrics = &_doc;
        d.title = "villancico.mp3";
        _direction = DirectShow(d);
        for (const VisualFamily& f : _direction.families) {
            if (!f.targets.empty() && f.id != "faces") {
                _families.push_back(f.id);
            }
        }
        if (_families.size() < 2) {
            printf("composer-recipe-test: the fixture has %zu families\n", _families.size());
            return false;
        }
        _recipes = Composed();
        return true;
    }

    // Recipes written for the test on the fixture's own families: one of two layers, a
    // second one, and one a person excluded.
    RecipeLibrary Composed() const {
        RecipeLibrary lib;
        const auto make = [&](const std::string& id, double energy, std::vector<RecipeSurface> surfaces, const std::vector<std::string>& contexts) {
            Recipe r;
            r.id = id;
            r.name = "test " + id;
            r.origin = RecipeOrigin::Composed;
            r.authorship = "composer test";
            r.function = "bed";
            r.contexts = contexts;
            r.surfaces = std::move(surfaces);
            r.durationMS = 8000;
            r.transitions = { "Fade" };
            r.colors = 2;
            r.energy = energy;
            r.confidence = 0.6;
            return r;
        };
        RecipeLayer twinkle{ 0, "Twinkle", PortableSettings(_metadata, "Twinkle", { { "E_SLIDER_Twinkle_Count", "20" } }, false), "Normal" };
        RecipeLayer wash{ 1, "Color Wash", PortableSettings(_metadata, "Color Wash", { { "E_TEXTCTRL_ColorWash_Cycles", "2.0" } }, false), "Normal" };
        RecipeLayer strand{ 0, "SingleStrand", {}, "Normal" };
        RecipeLayer bars{ 0, "Bars", PortableSettings(_metadata, "Bars", { { "E_SLIDER_Bars_BarCount", "2" } }, false), "Normal" };
        RecipeLayer spirals{ 0, "Spirals", {}, "Normal" };
        lib.recipes.push_back(make("aaaa00000001", 0.5, { { _families[0], "lead", { twinkle, wash }, { "model", "group" } }, { _families[1], "complement", { strand }, { "model" } } }, { "mid", "high" }));
        lib.recipes.push_back(make("bbbb00000002", 0.6, { { _families[0], "lead", { spirals }, { "model" } }, { _families[1], "complement", { bars }, { "model" } } }, { "high", "chorus" }));
        Recipe excluded = make("cccc00000003", 0.5, { { _families[0], "lead", { bars }, { "model" } }, { _families[1], "complement", { bars }, { "model" } } }, { "mid", "high", "low" });
        excluded.excluded = true;
        lib.recipes.push_back(excluded);
        return lib;
    }

    void TestPortable() {
        Title("R1. What a recipe may keep of an effect's settings");
        std::vector<std::string> dropped;
        const auto kept = PortableSettings(_metadata, "Pictures", { { "E_TEXTCTRL_Pictures_Filename", "C:/show/foto.png" }, { "E_CHOICE_Pictures_Direction", "left" } }, false, &dropped);
        Check(std::none_of(kept.begin(), kept.end(), [](const auto& kv) { return kv.first.find("Filename") != std::string::npos; }), "a file is never kept");
        const auto twinkle = PortableSettings(_metadata, "Twinkle", { { "E_SLIDER_Twinkle_Count", "20" }, { "E_SLIDER_Twinkle_Count2", "5" }, { "E_SLIDER_Twinkle_Steps", "99999" } }, false, &dropped);
        Check(twinkle.size() == 1 && twinkle.front() == std::pair<std::string, std::string>("E_SLIDER_Twinkle_Count", "20"),
              fmt::format("valid settings stay; unknown ones and values out of range go ({} kept)", twinkle.size()));
        const auto style = PortableSettings(_metadata, "Bars", { { "B_CHOICE_BufferStyle", "Per Preview" } }, false);
        const auto groupStyle = PortableSettings(_metadata, "Bars", { { "B_CHOICE_BufferStyle", "Per Preview" } }, true);
        Check(style.empty() && groupStyle.size() == 1, "a buffer style only stays for a group");
        const auto curve = PortableSettings(_metadata, "Bars", { { "E_VALUECURVE_Bars_BarCount", "Active=TRUE|Id=ID_VALUECURVE_Bars_BarCount|Type=Ramp|Min=1.00|Max=5.00|P1=1.00|P2=5.00|" },
                                                                 { "E_VALUECURVE_Bars_Nothing", "Active=TRUE|Type=Ramp|" } },
                                            false);
        Check(curve.size() == 1, "a value curve stays when its property exists");
        Check(PortableSettings(_metadata, "No Such Effect", { { "E_X", "1" } }, false).empty(), "an effect xLights does not know gives nothing");
    }

    void TestPersistence() {
        Title("R2. Recipes are written and read back, versioned");
        const std::string text = ToJson(_recipes);
        RecipeLibrary back;
        std::string error;
        Check(FromJson(text, back, error) && ToJson(back) == text, "the library reads back exactly " + error);
        nlohmann::json future = nlohmann::json::parse(text);
        future["schemaVersion"] = RECIPE_SCHEMA_VERSION + 1;
        RecipeLibrary refused;
        Check(!FromJson(future.dump(), refused, error), "a later schema is not read: " + error);
        const std::string path = (_root / "show" / "ShowComposer" / "recipes.json").string();
        Check(SaveRecipeLibrary(path, _recipes, error) && ReadRecipeLibrary(path, back, error) && back.recipes.size() == 3 && RecipeLibraryPath((_root / "show").string()) == path,
              "saved where a show keeps it, and read again " + error);
        nlohmann::json old = nlohmann::json::parse(text);
        for (nlohmann::json& r : old["recipes"]) {
            r.erase("favorite");
            r.erase("reviewer");
            r.erase("constraints");
        }
        Check(FromJson(old.dump(), back, error) && !back.recipes.front().favorite, "a file without the newer fields reads with their defaults " + error);
    }

    void TestHumanDecisions() {
        Title("R3. A person's decisions survive a new extraction");
        RecipeLibrary lib = _recipes;
        lib.recipes[0].origin = RecipeOrigin::Extracted;
        lib.recipes[1].origin = RecipeOrigin::Extracted;
        Check(SetRecipeFavorite(lib, "aaaa00000001", true) && lib.Find("aaaa00000001")->favorite, "a favourite");
        Check(ReviewRecipe(lib, "bbbb00000002", "Padre", "2026-11-09", "bonita") && lib.Find("bbbb00000002")->origin == RecipeOrigin::Reviewed, "a review by name");
        Check(!ReviewRecipe(lib, "bbbb00000002", "", "", ""), "a review needs who made it");
        std::string error;
        Check(!CorrectRecipeSetting(lib, _metadata, "aaaa00000001", _families[0], 0, "E_SLIDER_Twinkle_Count", "99999", "Padre", "2026-11-09", error), "a value out of range is refused: " + error);
        Check(CorrectRecipeSetting(lib, _metadata, "aaaa00000001", _families[0], 0, "E_SLIDER_Twinkle_Count", "35", "Padre", "2026-11-09", error) &&
                  lib.Find("aaaa00000001")->origin == RecipeOrigin::Corrected && lib.Find("aaaa00000001")->version == 2,
              "a correction is checked, marks the recipe corrected and raises its version " + error);

        RecipeLibrary extracted = _recipes;
        for (Recipe& r : extracted.recipes) {
            r.origin = RecipeOrigin::Extracted;
            r.excluded = false;
            r.favorite = false;
            r.sources = { { "otra", "otra.xsq", "abc", 1000, 5000, {} } };
        }
        extracted.recipes[1].durationMS = 1234;
        Recipe fresh = extracted.recipes[1];
        fresh.id = "dddd00000004";
        extracted.recipes.push_back(fresh);
        MergeRecipes(lib, extracted);
        const Recipe* a = lib.Find("aaaa00000001");
        const Recipe* b = lib.Find("bbbb00000002");
        const Recipe* c = lib.Find("cccc00000003");
        Check(a != nullptr && a->origin == RecipeOrigin::Corrected && a->favorite && a->sources.size() == 1 && a->surfaces.front().layers.front().settings.front().second == "35",
              "a corrected recipe keeps the person's settings, and its evidence grows");
        Check(b != nullptr && b->origin == RecipeOrigin::Reviewed && b->durationMS == 1234 && b->reviewer == "Padre", "a reviewed one takes the new measurements and keeps its review");
        Check(c != nullptr && c->excluded, "an excluded one stays excluded");
        Check(lib.Find("dddd00000004") != nullptr && lib.recipes.size() == 4, "a new one is added");
        Check(SetRecipeExcluded(lib, "aaaa00000001", true) && !lib.Find("aaaa00000001")->favorite, "excluding a favourite takes the star away");
    }

    void TestTemplates() {
        Title("R4. Each recipe layer becomes a native template");
        const std::vector<EffectTemplate> t = RecipeTemplates(_recipes, _metadata);
        Check(t.size() == 5, fmt::format("five layers of the two recipes in use: {}", t.size()));
        Check(std::none_of(t.begin(), t.end(), [](const EffectTemplate& x) { return x.id.find("cccc") != std::string::npos; }), "nothing from the excluded recipe");
        std::string recipe;
        std::string family;
        int order = -1;
        const EffectTemplate* base = nullptr;
        const EffectTemplate* layer = nullptr;
        for (const EffectTemplate& x : t) {
            if (ParseRecipeTemplateId(x.id, recipe, family, order) && recipe == "aaaa00000001" && family == _families[0]) {
                (order == 1 ? base : layer) = &x;
            }
        }
        Check(base != nullptr && base->effect == "Color Wash" && std::find(base->intents.begin(), base->intents.end(), "ensemble-bed") != base->intents.end(), "the lowest layer is the base the choreography picks");
        Check(layer != nullptr && layer->effect == "Twinkle" && layer->intents == std::vector<std::string>{ "recipe-layer" }, "the layer over it only comes with it");
        Check(layer != nullptr && layer->parameters.size() == 1 && layer->parameters.front().key == "E_SLIDER_Twinkle_Count" && layer->parameters.front().value == "20", "with the recipe's native settings");
        Check(std::all_of(t.begin(), t.end(), [](const EffectTemplate& x) { return !x.visuallyValidated && x.provenance.rfind("recipe:", 0) == 0; }), "none is visually validated, and each says where it came from");
        Check(!ParseRecipeTemplateId("ens-bars", recipe, family, order), "a template of the library is not a recipe's");
    }

    void TestColours() {
        Title("R5. Colours from the song");
        std::string reason;
        SongAnalysis a;
        a.key = "D minor";
        Check(std::fabs(AnchorHue(a, nullptr, reason) - 60) < 0.01 && reason.find("convention") != std::string::npos, "D on the circle of fifths: 60 degrees, said to be a convention");
        a.key = "Bb major";
        Check(std::fabs(AnchorHue(a, nullptr, reason) - 300) < 0.01, "B flat: 300 degrees");
        a.key = "";
        Check(std::fabs(AnchorHue(a, nullptr, reason) - 30) < 0.01, "no key: a warm neutral");
        SongVisualBrief brief;
        brief.source = "local";
        brief.colors = { "#FFFFFF", "#1E6BFF" };
        Check(std::fabs(AnchorHue(a, &brief, reason) - HueOfHex("#1E6BFF")) < 1, "the first coloured colour of the lyrics wins over the key (white is not a hue)");
        const ColorChoice verse = ColorsForPart(60, 210, "verse", 0.5, true, 0);
        const ColorChoice verseMajor = ColorsForPart(60, 210, "verse", 0.5, false, 0);
        const ColorChoice chorus = ColorsForPart(60, 210, "chorus", 0.9, true, 0);
        const ColorChoice christmas = ColorsForPart(60, 210, "chorus", 0.9, true, 1);
        Check(verse.colors != verseMajor.colors, "a minor key changes them");
        Check(std::find(chorus.colors.begin(), chorus.colors.end(), "#FFFFFF") != chorus.colors.end() && verse.colors != chorus.colors, "the chorus is fuller than the verse, with white");
        Check(ColorsForPart(60, 210, "verse", 0.5, true, 0).colors == verse.colors, "the same inputs give the same colours");
        static const std::set<std::string> POLE = { "#D0001A", "#00A33D", "#FFB627", "#FFFFFF" };
        Check(std::all_of(christmas.colors.begin(), christmas.colors.end(), [](const std::string& c) { return POLE.contains(c); }), "at 100% every colour is a Christmas one");
        Check(christmas.colors != chorus.colors && christmas.reason.find("100%") != std::string::npos, "and the slider says so");
        std::string why;
        Check(std::fabs(CompanionHue(60, nullptr, why) - 210) < 0.01, "without colours from the lyrics the companion is the anchor's near opposite");
        SongVisualBrief two;
        two.source = "local";
        two.colors = { "#FF0000", "#FF3300", "#0000FF" };
        Check(std::fabs(CompanionHue(0, &two, why) - 240) < 1, "with them, the next one far enough from the anchor");
        std::set<int> steps;
        for (const char* part : { "intro", "verse", "pre-chorus", "chorus", "bridge", "interlude", "outro" }) {
            for (const std::string& c : ColorsForPart(60, 210, part, 0.6, false, 0).colors) {
                if (c != "#FFFFFF") {
                    steps.insert(static_cast<int>(std::lround(HueOfHex(c) / 30.0)) % 12);
                }
            }
        }
        Check(steps.size() == 2, fmt::format("a whole song in two hues and white ({} hue steps)", steps.size()));
    }

    static double HueOfHex(const std::string& hex) {
        const double r = std::strtol(hex.substr(1, 2).c_str(), nullptr, 16) / 255.0;
        const double g = std::strtol(hex.substr(3, 2).c_str(), nullptr, 16) / 255.0;
        const double b = std::strtol(hex.substr(5, 2).c_str(), nullptr, 16) / 255.0;
        const double mx = std::max({ r, g, b });
        const double mn = std::min({ r, g, b });
        double h = mx == r ? std::fmod((g - b) / (mx - mn), 6.0) : mx == g ? (b - r) / (mx - mn) + 2 : (r - g) / (mx - mn) + 4;
        h *= 60;
        return h < 0 ? h + 360 : h;
    }

    void TestBrief() {
        Title("R6. What a language model may say, checked; and the lyrics alone");
        SongVisualBrief b;
        std::string error;
        Check(ParseVisualBrief(R"({"mood":"playful","colors":["#ff0000","red","#12345"],"imagery":["a","b"],"christmas":3,"energy":-1,"extra":true})", "groq:test", b, error) &&
                  b.colors == std::vector<std::string>{ "#FF0000" } && b.christmas == 1 && b.energy == 0 && b.mood == "playful" && b.source == "groq:test",
              "bad colours dropped, numbers clamped, unknown fields ignored " + error);
        Check(ParseVisualBrief(R"({"mood":"furious"})", "x", b, error) == false, "nothing usable is refused: " + error);
        Check(!ParseVisualBrief("not json", "x", b, error), "text that is not JSON is refused");
        const SongVisualBrief local = LocalVisualBrief("Una estrella en la ventana\nfarolillos rojos en Navidad\ny la nieve sobre el pesebre", "cancion.mp3");
        Check(local.source == "local" && local.christmas >= 0.75 && std::find(local.colors.begin(), local.colors.end(), "#D0001A") != local.colors.end(),
              fmt::format("invented lines: Christmas {:.2f}, red named", local.christmas));
        Check(LocalVisualBrief("el mar azul y la arena", "playa.mp3").christmas == 0, "no Christmas word, no Christmas");
    }

    SongVisualPlan Direct(const VisualPlanOptions& options, const RecipeLibrary* recipes = nullptr) const {
        VisualPlanInput in;
        in.plan = &_plan;
        in.analysis = &_song;
        in.direction = &_direction;
        in.recipes = recipes != nullptr ? recipes : &_recipes;
        in.options = options;
        return PlanSongVisuals(in);
    }

    void TestDirector() {
        Title("R7. The artistic director");
        VisualPlanOptions options;
        const SongVisualPlan p = Direct(options);
        Check(p.enabled && !p.scenes.empty(), fmt::format("a plan for {} scenes", p.scenes.size()));
        Check(ToJson(Direct(options)) == ToJson(p), "the same inputs give the same plan");
        Check(std::all_of(p.scenes.begin(), p.scenes.end(), [](const VisualPlanScene& s) { return !s.recipeId.empty() && s.recipeId != "cccc00000003"; }), "every scene has a recipe, never the excluded one");
        Check(std::all_of(p.scenes.begin(), p.scenes.end(), [](const VisualPlanScene& s) {
                  return std::any_of(s.reasons.begin(), s.reasons.end(), [](const Reason& r) { return r.code == "recipe" && !r.detail.empty(); });
              }),
              "and says why");
        std::map<int, std::set<std::string>> byGroup;
        for (const VisualPlanScene& s : p.scenes) {
            if (s.repeatGroup >= 0) {
                byGroup[s.repeatGroup].insert(s.recipeId);
            }
        }
        Check(!byGroup.empty() && std::all_of(byGroup.begin(), byGroup.end(), [](const auto& kv) { return kv.second.size() == 1; }), "music that comes back plays the recipe it played");
        Check(std::all_of(p.scenes.begin(), p.scenes.end(), [](const VisualPlanScene& s) {
                  const auto rest = std::count_if(s.roles.begin(), s.roles.end(), [](const SurfaceRoleAt& r) { return r.role == "rest"; });
                  return rest * 2 <= static_cast<long>(s.roles.size());
              }),
              "never more than half the families rest");
        Check(std::all_of(p.scenes.begin(), p.scenes.end(), [](const VisualPlanScene& s) { return !s.colors.empty() && !s.colorReason.empty(); }), "every scene has colours, with their reason");
        Check(p.scenes.front().arcStep == "establish" && p.scenes.back().arcStep == "close", "the arc opens and closes");
        VisualPlanOptions varied = options;
        varied.variety = 1;
        const SongVisualPlan v = Direct(varied);
        Check(v.distinctRecipes == 2, fmt::format("with variety both recipes play: {}", v.distinctRecipes));
        VisualPlanOptions none = options;
        none.recipes = false;
        const SongVisualPlan n = Direct(none);
        Check(std::all_of(n.scenes.begin(), n.scenes.end(), [](const VisualPlanScene& s) { return s.recipeId.empty(); }), "recipes off: no scene has one");
        VisualPlanOptions c0 = options;
        c0.christmasInfluence = 0;
        VisualPlanOptions c1 = options;
        c1.christmasInfluence = 1;
        Check(Direct(c0).scenes.front().colors != Direct(c1).scenes.front().colors, "the Christmas slider changes the colours");
        ShowVisualDirection d = _direction;
        ApplyPlanColors(p, d);
        bool applied = true;
        for (const SceneLook& l : d.scenes) {
            if (const VisualPlanScene* s = p.Scene(l.segmentId)) {
                applied = applied && l.colors == s->colors;
            }
        }
        Check(applied, "the direction's scenes take the plan's colours");
        RecipeLibrary elsewhere;
        Recipe distant = _recipes.recipes.front();
        distant.id = "eeee00000005";
        for (RecipeSurface& s : distant.surfaces) {
            s.family = "no-such-family";
        }
        elsewhere.recipes.push_back(distant);
        const SongVisualPlan nofit = Direct(options, &elsewhere);
        Check(std::all_of(nofit.scenes.begin(), nofit.scenes.end(), [](const VisualPlanScene& s) { return s.recipeId.empty(); }) &&
                  std::find(nofit.warnings.begin(), nofit.warnings.end(), "no-recipe-fits-this-layout") != nofit.warnings.end(),
              "a recipe for figures this house does not have is not used, and that is said");
    }

    void TestChoreography() {
        Title("R8. The choreography plays the recipes");
        TemplateLibrary library = _library;
        for (EffectTemplate& t : RecipeTemplates(_recipes, _metadata)) {
            library.templates.push_back(std::move(t));
        }
        VisualPlanOptions options;
        options.variety = 1;
        const SongVisualPlan p = Direct(options);
        ShowVisualDirection direction = _direction;
        ApplyPlanColors(p, direction);
        ChoreographyInput in;
        in.plan = &_plan;
        in.capabilities = &_caps;
        in.library = &library;
        in.profile = &_profile;
        in.analysis = &_song;
        in.direction = &direction;
        in.visualPlan = &p;
        in.recipes = &_recipes;
        const Choreography c = Choreograph(in);
        int fromRecipe = 0;
        int layers = 0;
        bool layerUnder = true;
        bool notExcluded = true;
        bool inScene = true;
        for (const ChoreographyEvent& e : c.events) {
            std::string recipe;
            std::string family;
            int order = 0;
            if (!ParseRecipeTemplateId(e.templateId, recipe, family, order)) {
                continue;
            }
            ++fromRecipe;
            notExcluded = notExcluded && recipe != "cccc00000003";
            const VisualPlanScene* s = p.Scene(e.segmentId);
            inScene = inScene && s != nullptr && s->recipeId == recipe;
            if (e.id.find("/recipe") != std::string::npos) {
                ++layers;
                const auto base = std::find_if(c.events.begin(), c.events.end(), [&](const ChoreographyEvent& o) { return e.id.rfind(o.id + "/recipe", 0) == 0; });
                const bool ok = base != c.events.end() && base->layer < e.layer &&
                                std::any_of(base->settings.begin(), base->settings.end(), [](const auto& kv) { return kv.first == "T_CHOICE_LayerMethod" && kv.second != "Normal"; });
                if (!ok) {
                    const std::string what = base == c.events.end() ? std::string("missing") : fmt::format("{} on {} with {} settings", base->id, base->layer, base->settings.size());
                    printf("    layer %s on %d: base %s\n", e.id.c_str(), e.layer, what.c_str());
                }
                layerUnder = layerUnder && ok;
            }
        }
        Check(fromRecipe > 0, fmt::format("{} events come from the recipes", fromRecipe));
        Check(inScene && notExcluded, "each from its scene's recipe, none from the excluded one");
        Check(layers > 0 && layerUnder, fmt::format("{} layers stacked under their base, which blends with them", layers));
        const ValidationReport report = ValidateChoreography(c, _plan, _caps, library, _metadata);
        Check(report.ErrorCount() == 0, fmt::format("the choreography passes the checks: {} events, {} errors", c.events.size(), report.ErrorCount()));
        for (const ValidationIssue& i : report.issues) {
            if (i.severity == IssueSeverity::Error) {
                printf("    %s %s %s %d-%d\n", i.code.c_str(), i.model.c_str(), i.effect.c_str(), i.startMS, i.endMS);
            }
        }
        bool rested = true;
        for (const VisualPlanScene& s : p.scenes) {
            for (const SurfaceRoleAt& r : s.roles) {
                if (r.role != "rest") {
                    continue;
                }
                for (const ChoreographyEvent& e : c.events) {
                    if (e.segmentId == s.segmentId && e.intent == "ensemble-bed" && std::any_of(e.reasons.begin(), e.reasons.end(), [&](const Reason& x) { return x.code == "ensemble" && x.detail.rfind(r.family, 0) == 0; })) {
                        rested = false;
                    }
                }
            }
        }
        Check(rested, "a family the director rests gets no base in that scene");
        in.visualPlan = nullptr;
        in.recipes = nullptr;
        const Choreography plain = Choreograph(in);
        Check(std::none_of(plain.events.begin(), plain.events.end(), [](const ChoreographyEvent& e) { return e.id.find("/recipe") != std::string::npos; }), "without the director no layer is stacked");
    }

    fs::path _root;
    std::unique_ptr<HeadlessRenderContext> _ctx;
    ModelCapabilities _caps;
    MetadataLookup _metadata;
    TemplateLibrary _library;
    SongAnalysis _song;
    LyricDocument _doc;
    StyleProfile _profile;
    ThemeCatalog _themes;
    CompositionPlan _plan;
    ShowVisualDirection _direction;
    std::vector<std::string> _families;
    RecipeLibrary _recipes;
    int _passed = 0;
    int _failed = 0;
};

} // namespace

int RunRecipeTest(const std::string& workDir, const std::string& showDir) {
    (void)showDir;
    fs::path root = fs::path(workDir) / "composer-recipe-test-0";
    for (int n = 1; fs::exists(root); ++n) {
        root = fs::path(workDir) / fmt::format("composer-recipe-test-{}", n);
    }
    std::error_code ec;
    fs::create_directories(root, ec);
    return Suite(root).Run();
}

} // namespace composer
