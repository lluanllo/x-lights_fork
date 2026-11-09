#pragma once

#include "composer/ModelCapabilities.h"
#include "composer/TemplateLibrary.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

// Recipes: how a finished show combines effects across the house at one moment. A
// template says how to drive one effect on one figure; a recipe says which effects
// run together, on which families of figures, in which layers and with which native
// parameters, the way a reference show does it in one scene. Recipes are read from
// real sequences with xLights' own loader, or written by a person, and every one
// says where it came from and whether a person has looked at it.
namespace composer {

inline constexpr int RECIPE_SCHEMA_VERSION = 1;
inline constexpr const char* RECIPE_EXTRACTOR_VERSION = "1.0.0";

enum class RecipeOrigin {
    Extracted, // read from a sequence automatically, never looked at by a person
    Reviewed,  // a person looked at it and kept it as it was
    Corrected, // a person changed it
    Composed   // written for the composer, not read from a show
};

struct RecipeLayer {
    int order = 0; // 0 is drawn on top, as in xLights
    std::string effect;
    std::vector<std::pair<std::string, std::string>> settings; // native E_ keys (and B_ for groups), checked against the metadata
    std::string blend = "Normal";                              // T_CHOICE_LayerMethod
    std::string rhythm;                                        // "" sustained; "hits": short restarts
    int hitMS = 0;                                             // hits: how long each one lasts
    int periodMS = 0;                                          // hits: from one start to the next
    int share = 0;                                             // how many figures ran it in the source
};

struct RecipeSurface {
    std::string family; // FamilyIdOf: "mega-tree", "matrix", "arches", "windows", ...
    std::string role;   // "lead", "complement", "accent", "bed"
    std::vector<RecipeLayer> layers;
    std::vector<std::string> targetTypes; // "model", "group", "submodel", as seen
    int minBufferWi = 0;                  // the smallest figure it was seen on
    int minBufferHt = 0;
};

struct RecipeSource {
    std::string package;  // the show it came from, as its maker named it
    std::string sequence; // the file name
    std::string sequenceHash;
    int startMS = 0;
    int endMS = 0;
    std::vector<std::string> effects; // "<row>@<layer>@<start ms>", enough to find them again
};

struct Recipe {
    std::string id;
    int version = 1;
    std::string name;
    RecipeOrigin origin = RecipeOrigin::Extracted;
    std::string authorship; // who made the source: "third-party (...)"
    std::vector<RecipeSource> sources;
    std::string function;              // "bed", "feature", "accent", "build", "calm"
    std::vector<std::string> contexts; // energy bands and parts: "low", "high", "chorus", "intro", ...
    std::vector<RecipeSurface> surfaces;
    int durationMS = 0; // a typical scene's length in the sources
    std::vector<std::string> transitions;
    std::vector<std::string> hues; // what the source used; the show's colours come from the song
    int colors = 0;
    double energy = 0.5;
    double complexity = 0;
    std::vector<std::string> constraints;
    double confidence = 0;
    int support = 1; // scenes of the sources that share it
    std::string extractor;
    // A person's view, kept across extractions.
    bool favorite = false;
    bool excluded = false;
    std::string reviewer;
    std::string reviewedAt;
    std::string note;

    [[nodiscard]] const RecipeSurface* Surface(const std::string& family) const;
    [[nodiscard]] std::string Signature() const;
};

struct RecipeLibrary {
    int schemaVersion = RECIPE_SCHEMA_VERSION;
    std::vector<Recipe> recipes;
    std::vector<std::string> packages; // what was read, with its hash

    [[nodiscard]] Recipe* Find(const std::string& id);
    [[nodiscard]] const Recipe* Find(const std::string& id) const;
};

[[nodiscard]] const char* RecipeOriginId(RecipeOrigin origin);
[[nodiscard]] std::string ToJson(const RecipeLibrary& library);
[[nodiscard]] bool FromJson(const std::string& text, RecipeLibrary& library, std::string& error);

// <showDir>/ShowComposer/recipes.json
[[nodiscard]] std::string RecipeLibraryPath(const std::string& showDir);
[[nodiscard]] bool ReadRecipeLibrary(const std::string& path, RecipeLibrary& library, std::string& error);
[[nodiscard]] bool SaveRecipeLibrary(const std::string& path, const RecipeLibrary& library, std::string& error);

// New extractions replace what was extracted before from the same recipes, and keep
// everything a person decided: favourites, exclusions, reviews and corrections.
void MergeRecipes(RecipeLibrary& into, const RecipeLibrary& extracted);

// A person's actions. Correcting a layer's settings re-checks them.
bool SetRecipeFavorite(RecipeLibrary& library, const std::string& id, bool favorite);
bool SetRecipeExcluded(RecipeLibrary& library, const std::string& id, bool excluded);
bool ReviewRecipe(RecipeLibrary& library, const std::string& id, const std::string& reviewer, const std::string& when, const std::string& note);
bool CorrectRecipeSetting(RecipeLibrary& library, const MetadataLookup& metadata, const std::string& id, const std::string& family, int order, const std::string& key, const std::string& value,
                          const std::string& reviewer, const std::string& when, std::string& error);

// Which xLights role a family of figures stands for.
[[nodiscard]] Role FamilyRole(const std::string& family);

// The native settings of an effect a recipe may keep: the effect's own, checked
// against its metadata, without files, timing tracks or anything tied to one layout.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> PortableSettings(const MetadataLookup& metadata, const std::string& effect,
                                                                                 const std::vector<std::pair<std::string, std::string>>& settings, bool group,
                                                                                 std::vector<std::string>* dropped = nullptr);

// Why a layer cannot go to another show as it is ("" when it can): a VU Meter that
// follows timing marks, say, needs a track the new song does not have.
[[nodiscard]] std::string UnportableReason(const std::string& effect, const std::vector<std::pair<std::string, std::string>>& settings);

// The buffer styles of a group a recipe may carry to another house: none tied to one
// layout's names or sizes.
[[nodiscard]] const std::vector<std::string>& PortableGroupBufferStyles();

// Templates for the choreography, one per recipe layer, with ids
// "recipe/<recipe>/<family>/<order>". Excluded recipes give none.
[[nodiscard]] std::vector<EffectTemplate> RecipeTemplates(const RecipeLibrary& library, const MetadataLookup& metadata);
[[nodiscard]] std::string RecipeTemplateId(const std::string& recipe, const std::string& family, int order);
// Splits a template id back; false when it is not a recipe's.
bool ParseRecipeTemplateId(const std::string& templateId, std::string& recipe, std::string& family, int& order);

// Reading recipes from a finished sequence, in a show folder whose controllers are all
// inactive. 0 written, 1 failed, 2 refused.
struct RecipeExtractOptions {
    std::string showDir;
    std::string sequencePath;
    std::string package;    // the show's name, for provenance
    std::string authorship; // "third-party (...)"
    std::string outPath;    // a recipes.json; merged into when it exists
    int maxRecipes = 40;
};
[[nodiscard]] int RunRecipeExtraction(const RecipeExtractOptions& options);

} // namespace composer
