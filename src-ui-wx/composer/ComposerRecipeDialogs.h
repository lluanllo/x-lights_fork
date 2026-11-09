#pragma once

/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include <wx/dialog.h>

#include <set>
#include <string>
#include <vector>

#include "composer/RecipeLibrary.h"

class wxChoice;
class wxListCtrl;
class wxStaticText;
class wxTextCtrl;

// The recipes the composer may play: what each combines on which figures, where it came
// from and whether someone looked at it. The person marks favourites, leaves some out,
// says they reviewed one, fixes a setting, or reads new ones from a finished sequence.
class RecipesDialog : public wxDialog {
public:
    // `families`: the families of figures this house has, to say which recipes fit it.
    RecipesDialog(wxWindow* parent, composer::RecipeLibrary library, std::set<std::string> families, composer::MetadataLookup metadata, std::string showDir);

    [[nodiscard]] const composer::RecipeLibrary& Library() const { return _library; }
    [[nodiscard]] bool Changed() const { return _changed; }

    // The same calls the controls make, for the UI test.
    void SetFilter(int filter); // 0 all, 1 fitting this house, 2 favourites, 3 left out
    [[nodiscard]] std::vector<std::string> Rows() const { return _rows; }
    bool Favorite(const std::string& id, bool on);
    bool Exclude(const std::string& id, bool on);
    bool Review(const std::string& id, const std::string& reviewer, const std::string& note);
    bool Correct(const std::string& id, const std::string& family, int order, const std::string& key, const std::string& value, const std::string& reviewer, std::string& error);
    [[nodiscard]] wxString Describe(const std::string& id) const;

private:
    void Fill();
    void ShowDetail();
    [[nodiscard]] std::string Selected() const;
    void ReadFromSequence();
    [[nodiscard]] bool Fits(const composer::Recipe& r) const;

    composer::RecipeLibrary _library;
    std::set<std::string> _families;
    composer::MetadataLookup _metadata;
    std::string _showDir;
    std::vector<std::string> _rows;
    int _filter = 1;
    bool _changed = false;

    wxChoice* _filterChoice = nullptr;
    wxStaticText* _summary = nullptr;
    wxListCtrl* _list = nullptr;
    wxTextCtrl* _detail = nullptr;
};

// A family of figures said simply: "Mega árbol", "Arcos"...
[[nodiscard]] wxString FamilyName(const std::string& family);
// Where a recipe stands: "Leída de un espectáculo", "Revisada", "Corregida"...
[[nodiscard]] wxString RecipeOriginName(composer::RecipeOrigin origin);
