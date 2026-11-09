/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "ComposerRecipeDialogs.h"

#include <wx/button.h>
#include <wx/choicdlg.h>
#include <wx/choice.h>
#include <wx/datetime.h>
#include <wx/filedlg.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/utils.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "utils/ExternalHooks.h"

namespace fs = std::filesystem;
using namespace composer;

namespace {

wxString U(const std::string& s) {
    return wxString::FromUTF8(s.c_str());
}

std::string S(const wxString& s) {
    return s.ToStdString(wxConvUTF8);
}

wxString EnergyName(double e) {
    return e < 0.35 ? _("Calm") : e < 0.7 ? _("Medium") : _("Strong");
}

wxString FunctionName(const std::string& f) {
    if (f == "calm") {
        return _("for quiet parts");
    }
    if (f == "accent") {
        return _("hits on the beat");
    }
    if (f == "feature") {
        return _("a figure leads");
    }
    return _("the whole house moves");
}

wxString SurfacesText(const Recipe& r) {
    wxString out;
    for (const RecipeSurface& s : r.surfaces) {
        if (!out.empty()) {
            out += "; ";
        }
        out += FamilyName(s.family) + ": ";
        for (size_t i = 0; i < s.layers.size(); ++i) {
            out += (i == 0 ? wxString() : wxString(" + ")) + U(s.layers[i].effect) + (s.layers[i].rhythm == "hits" ? wxString(" ") + _("(hits)") : wxString());
        }
    }
    return out;
}

std::string Today() {
    return S(wxDateTime::Now().FormatISODate());
}

} // namespace

wxString FamilyName(const std::string& family) {
    static const std::map<std::string, wxString> NAMES = { { "mega-tree", _("Mega tree") }, { "matrix", _("Matrix") }, { "arches", _("Arches") }, { "windows", _("Windows") },
                                                           { "outline", _("Outline") }, { "trees", _("Trees") }, { "stars", _("Stars") }, { "snowflakes", _("Snowflakes") },
                                                           { "figures", _("Figures") }, { "props", _("Props") }, { "faces", _("Faces") } };
    const auto it = NAMES.find(family);
    return it != NAMES.end() ? it->second : U(family);
}

wxString RecipeOriginName(RecipeOrigin origin) {
    switch (origin) {
    case RecipeOrigin::Extracted:
        return _("Read from a show, not reviewed");
    case RecipeOrigin::Reviewed:
        return _("Reviewed");
    case RecipeOrigin::Corrected:
        return _("Corrected by hand");
    default:
        return _("Made by the composer");
    }
}

RecipesDialog::RecipesDialog(wxWindow* parent, RecipeLibrary library, std::set<std::string> families, MetadataLookup metadata, std::string showDir) :
    wxDialog(parent, wxID_ANY, _("Recipes"), wxDefaultPosition, wxSize(980, 640), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    _library(std::move(library)),
    _families(std::move(families)),
    _metadata(std::move(metadata)),
    _showDir(std::move(showDir)) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(new wxStaticText(this, wxID_ANY, _("A recipe is how a show combines effects on several figures at once. The composer plays them in its scenes; you choose which.")), 0, wxALL, 8);
    auto* top = new wxBoxSizer(wxHORIZONTAL);
    top->Add(new wxStaticText(this, wxID_ANY, _("Show:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    _filterChoice = new wxChoice(this, wxID_ANY);
    for (const wxString& f : { _("Every recipe"), _("The ones that fit my house"), _("Favourites"), _("Left out") }) {
        _filterChoice->Append(f);
    }
    _filterChoice->SetSelection(_filter);
    top->Add(_filterChoice, 0, wxRIGHT, 12);
    _summary = new wxStaticText(this, wxID_ANY, "");
    top->Add(_summary, 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(top, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    _list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 260), wxLC_REPORT | wxLC_SINGLE_SEL);
    for (const auto& [label, width] : std::vector<std::pair<wxString, int>>{ { _("Recipe"), 430 }, { _("Use"), 150 }, { _("Strength"), 80 }, { _("Seen"), 55 }, { _("State"), 210 } }) {
        _list->AppendColumn(label, wxLIST_FORMAT_LEFT, width);
    }
    sizer->Add(_list, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
    _detail = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 150), wxTE_MULTILINE | wxTE_READONLY);
    sizer->Add(_detail, 0, wxEXPAND | wxALL, 8);
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* favorite = new wxButton(this, wxID_ANY, _("Favourite"));
    auto* exclude = new wxButton(this, wxID_ANY, _("Do Not Use"));
    auto* use = new wxButton(this, wxID_ANY, _("Use Again"));
    auto* reviewed = new wxButton(this, wxID_ANY, _("I Have Reviewed It..."));
    auto* correct = new wxButton(this, wxID_ANY, _("Correct a Setting..."));
    auto* read = new wxButton(this, wxID_ANY, _("Read Recipes from a Sequence..."));
    for (wxButton* b : { favorite, exclude, use, reviewed, correct, read }) {
        buttons->Add(b, 0, wxRIGHT, 4);
    }
    sizer->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    sizer->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
    SetSizer(sizer);

    _filterChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { SetFilter(_filterChoice->GetSelection()); });
    _list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) { ShowDetail(); });
    favorite->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const std::string id = Selected();
        const Recipe* r = _library.Find(id);
        (void)Favorite(id, r != nullptr && !r->favorite);
    });
    exclude->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { (void)Exclude(Selected(), true); });
    use->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { (void)Exclude(Selected(), false); });
    reviewed->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const std::string id = Selected();
        if (id.empty()) {
            return;
        }
        wxTextEntryDialog who(this, _("Who reviewed it? Only a person who looked at it in xLights should say so."), _("Reviewed"));
        if (who.ShowModal() != wxID_OK || who.GetValue().Trim().empty()) {
            return;
        }
        wxTextEntryDialog note(this, _("A note (optional): what you liked or not."), _("Reviewed"));
        note.ShowModal();
        (void)Review(id, S(who.GetValue().Trim()), S(note.GetValue()));
    });
    correct->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const std::string id = Selected();
        const Recipe* r = _library.Find(id);
        if (r == nullptr) {
            return;
        }
        wxArrayString choices;
        std::vector<std::pair<std::string, int>> layers;
        for (const RecipeSurface& s : r->surfaces) {
            for (const RecipeLayer& l : s.layers) {
                choices.Add(FamilyName(s.family) + ": " + U(l.effect));
                layers.push_back({ s.family, l.order });
            }
        }
        wxSingleChoiceDialog which(this, _("Which effect?"), _("Correct a Setting"), choices);
        if (which.ShowModal() != wxID_OK) {
            return;
        }
        const auto [family, order] = layers[static_cast<size_t>(which.GetSelection())];
        wxTextEntryDialog key(this, _("The setting as xLights saves it, for example E_SLIDER_Spirals_Count:"), _("Correct a Setting"));
        if (key.ShowModal() != wxID_OK) {
            return;
        }
        wxTextEntryDialog value(this, _("Its new value:"), _("Correct a Setting"));
        if (value.ShowModal() != wxID_OK) {
            return;
        }
        wxTextEntryDialog who(this, _("Who makes the correction?"), _("Correct a Setting"));
        if (who.ShowModal() != wxID_OK || who.GetValue().Trim().empty()) {
            return;
        }
        std::string error;
        if (!Correct(id, family, order, S(key.GetValue().Trim()), S(value.GetValue().Trim()), S(who.GetValue().Trim()), error)) {
            wxMessageBox(wxString::Format(_("The correction was not accepted: %s"), U(error)), _("Correct a Setting"), wxOK | wxICON_WARNING, this);
        }
    });
    read->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ReadFromSequence(); });
    Fill();
}

bool RecipesDialog::Fits(const Recipe& r) const {
    int present = 0;
    bool lead = false;
    for (const RecipeSurface& s : r.surfaces) {
        if (_families.contains(s.family)) {
            ++present;
            lead = lead || s.role == "lead";
        }
    }
    return present >= 2 || (present >= 1 && lead);
}

void RecipesDialog::SetFilter(int filter) {
    _filter = std::clamp(filter, 0, 3);
    if (_filterChoice != nullptr) {
        _filterChoice->SetSelection(_filter);
    }
    Fill();
}

void RecipesDialog::Fill() {
    const std::string keep = Selected();
    _rows.clear();
    std::vector<const Recipe*> shown;
    int fitting = 0;
    int favorites = 0;
    int excluded = 0;
    for (const Recipe& r : _library.recipes) {
        fitting += Fits(r) ? 1 : 0;
        favorites += r.favorite ? 1 : 0;
        excluded += r.excluded ? 1 : 0;
        const bool show = _filter == 0 || (_filter == 1 && Fits(r) && !r.excluded) || (_filter == 2 && r.favorite) || (_filter == 3 && r.excluded);
        if (show) {
            shown.push_back(&r);
        }
    }
    std::stable_sort(shown.begin(), shown.end(), [](const Recipe* a, const Recipe* b) {
        return a->favorite != b->favorite ? a->favorite : a->support != b->support ? a->support > b->support : a->id < b->id;
    });
    _list->DeleteAllItems();
    for (const Recipe* r : shown) {
        const long row = _list->InsertItem(_list->GetItemCount(), SurfacesText(*r));
        _list->SetItem(row, 1, FunctionName(r->function));
        _list->SetItem(row, 2, EnergyName(r->energy));
        _list->SetItem(row, 3, wxString::Format("%d", r->support));
        wxString state = r->excluded ? _("Not used") : r->favorite ? _("Favourite") : wxString();
        state += (state.empty() ? wxString() : wxString(" · ")) + RecipeOriginName(r->origin);
        _list->SetItem(row, 4, state);
        _rows.push_back(r->id);
        if (r->id == keep) {
            _list->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        }
    }
    _summary->SetLabel(wxString::Format(_("%d recipes; %d fit this house, %d favourites, %d not used."), static_cast<int>(_library.recipes.size()), fitting, favorites, excluded));
    ShowDetail();
}

std::string RecipesDialog::Selected() const {
    if (_list == nullptr) {
        return "";
    }
    const long row = _list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    return row >= 0 && row < static_cast<long>(_rows.size()) ? _rows[static_cast<size_t>(row)] : std::string();
}

wxString RecipesDialog::Describe(const std::string& id) const {
    const Recipe* r = _library.Find(id);
    if (r == nullptr) {
        return "";
    }
    wxString out = SurfacesText(*r) + "\n";
    out += wxString::Format(_("Use: %s. Strength: %s. Seen %d times. Confidence %.0f%%."), FunctionName(r->function), EnergyName(r->energy), r->support, r->confidence * 100) + "\n";
    out += RecipeOriginName(r->origin);
    if (!r->reviewer.empty()) {
        out += wxString::Format(_(" by %s on %s"), U(r->reviewer), U(r->reviewedAt));
    }
    if (!r->note.empty()) {
        out += " — " + U(r->note);
    }
    out += "\n" + wxString::Format(_("Who made the original: %s"), U(r->authorship.empty() ? std::string("?") : r->authorship)) + "\n";
    for (const RecipeSource& s : r->sources) {
        out += wxString::Format(_("  from \"%s\", %.1f s to %.1f s"), U(s.package), s.startMS / 1000.0, s.endMS / 1000.0) + "\n";
    }
    for (const RecipeSurface& s : r->surfaces) {
        for (const RecipeLayer& l : s.layers) {
            out += "  " + FamilyName(s.family) + " · " + U(l.effect) + (l.blend == "Normal" ? wxString() : " (" + U(l.blend) + ")") + ": ";
            for (const auto& [k, v] : l.settings) {
                out += U(k) + "=" + U(v.size() > 30 ? v.substr(0, 30) + "..." : v) + "  ";
            }
            out += "\n";
        }
    }
    if (!Fits(*r)) {
        out += _("This house does not have the figures this recipe needs.") + "\n";
    }
    return out;
}

void RecipesDialog::ShowDetail() {
    _detail->SetValue(Describe(Selected()));
}

bool RecipesDialog::Favorite(const std::string& id, bool on) {
    if (!SetRecipeFavorite(_library, id, on)) {
        return false;
    }
    _changed = true;
    Fill();
    return true;
}

bool RecipesDialog::Exclude(const std::string& id, bool on) {
    if (!SetRecipeExcluded(_library, id, on)) {
        return false;
    }
    _changed = true;
    Fill();
    return true;
}

bool RecipesDialog::Review(const std::string& id, const std::string& reviewer, const std::string& note) {
    if (!ReviewRecipe(_library, id, reviewer, Today(), note)) {
        return false;
    }
    _changed = true;
    Fill();
    return true;
}

bool RecipesDialog::Correct(const std::string& id, const std::string& family, int order, const std::string& key, const std::string& value, const std::string& reviewer, std::string& error) {
    if (!CorrectRecipeSetting(_library, _metadata, id, family, order, key, value, reviewer, Today(), error)) {
        return false;
    }
    _changed = true;
    Fill();
    return true;
}

// The sequence is read from a copy of its show with every controller off, so nothing
// of the real installation is touched and no light is ever sent.
void RecipesDialog::ReadFromSequence() {
    wxFileDialog pick(this, _("Choose a finished sequence"), "", "", _("xLights sequences (*.xsq)|*.xsq"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (pick.ShowModal() != wxID_OK) {
        return;
    }
    const fs::path sequence(S(pick.GetPath()));
    ObtainAccessToURL(sequence.string());
    fs::path show = sequence.parent_path();
    for (int up = 0; up < 3 && !FileExists((show / "xlights_rgbeffects.xml").string()) && show.has_parent_path(); ++up) {
        show = show.parent_path();
    }
    if (!FileExists((show / "xlights_rgbeffects.xml").string())) {
        wxMessageBox(_("Its show folder (with xlights_rgbeffects.xml) was not found next to the sequence."), _("Read Recipes"), wxOK | wxICON_WARNING, this);
        return;
    }
    wxTextEntryDialog who(this, _("Who made this sequence? It is kept with every recipe read from it."), _("Read Recipes"), _("someone else"));
    if (who.ShowModal() != wxID_OK) {
        return;
    }
    const fs::path work = fs::path(_showDir) / "ShowComposer" / "recipe-reading";
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    fs::copy_file(show / "xlights_rgbeffects.xml", work / "xlights_rgbeffects.xml", fs::copy_options::overwrite_existing, ec);
    if (FileExists((show / "xlights_networks.xml").string())) {
        std::ifstream in(show / "xlights_networks.xml", std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        std::string networks = text.str();
        for (size_t at = networks.find("ActiveState=\"Active\""); at != std::string::npos; at = networks.find("ActiveState=\"Active\"", at)) {
            networks.replace(at, 20, "ActiveState=\"Inactive\"");
        }
        std::ofstream(work / "xlights_networks.xml", std::ios::binary) << networks;
    }
    fs::copy_file(sequence, work / sequence.filename(), fs::copy_options::overwrite_existing, ec);
    const fs::path out = work / "recipes.json";
    wxBusyCursor busy;
    const wxString exe = wxStandardPaths::Get().GetExecutablePath();
    const std::vector<wxString> args = { exe, "--composer-recipe-extract", "-s", U(work.string()), "--sequence", U((work / sequence.filename()).string()), "--package", U(sequence.stem().string()),
                                         "--authorship", who.GetValue(), "--out", U(out.string()) };
    std::vector<const wxChar*> argv;
    for (const wxString& a : args) {
        argv.push_back(a.wc_str());
    }
    argv.push_back(nullptr);
    const long code = wxExecute(argv.data(), wxEXEC_SYNC | wxEXEC_HIDE_CONSOLE);
    RecipeLibrary read;
    std::string error;
    if (code != 0 || !ReadRecipeLibrary(out.string(), read, error)) {
        wxMessageBox(_("No recipes could be read from that sequence."), _("Read Recipes"), wxOK | wxICON_WARNING, this);
        fs::remove_all(work, ec);
        return;
    }
    const size_t before = _library.recipes.size();
    MergeRecipes(_library, read);
    fs::remove_all(work, ec);
    _changed = true;
    Fill();
    wxMessageBox(wxString::Format(_("%d new recipes read. They are not reviewed: look at them in xLights before marking them."), static_cast<int>(_library.recipes.size() - before)), _("Read Recipes"),
                 wxOK | wxICON_INFORMATION, this);
}
