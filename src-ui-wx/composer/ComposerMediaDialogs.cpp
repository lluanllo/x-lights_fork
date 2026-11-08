/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "ComposerMediaDialogs.h"

#include <wx/bitmap.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/choice.h>
#include <wx/dirdlg.h>
#include <wx/image.h>
#include <wx/imaglist.h>
#include <wx/listbox.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <filesystem>

#include "utils/ExternalHooks.h"
#include "composer/MediaAnalysis.h"
#include "composer/MediaValidation.h"
#include "composer/VisualModel.h"
#include "ComposerReviewDialogs.h"
#include "ComposerStoryboard.h"

namespace fs = std::filesystem;
using namespace composer;

namespace {

constexpr int ICON_W = 64;
constexpr int ICON_H = 48;

wxString U(const std::string& s) {
    return wxString::FromUTF8(s);
}

std::string S(const wxString& s) {
    return std::string(s.ToUTF8());
}

wxString Clock(int ms) {
    return wxString::Format("%d:%02d.%d", ms / 60000, ms / 1000 % 60, ms / 100 % 10);
}

wxString KindName(MediaKind kind) {
    switch (kind) {
    case MediaKind::Image: return _("Picture");
    case MediaKind::Animation: return _("Animation");
    case MediaKind::Video: return _("Video");
    default: return _("Not usable");
    }
}

wxString StatusName(const MediaItem& m) {
    if (m.status == MediaStatus::Missing) {
        return _("Missing");
    }
    if (m.status == MediaStatus::Unsupported) {
        return _("Format not supported");
    }
    if (m.status == MediaStatus::Unreadable) {
        return m.problem == "empty" ? _("Empty file") : m.problem == "too-large" ? _("Too large") : _("Damaged or unreadable");
    }
    return m.disabled ? _("Only when chosen") : _("Ready");
}

wxBitmap Thumb(const MediaItem& m, const std::string& showDir, int w, int h) {
    wxImage image;
    const std::string path = m.thumbnail.empty() ? std::string() : MediaAbsolutePath(m.thumbnail, showDir);
    if (!path.empty() && FileExists(path) && image.LoadFile(U(path), wxBITMAP_TYPE_PNG) && image.IsOk()) {
        const double s = std::min(static_cast<double>(w) / image.GetWidth(), static_cast<double>(h) / image.GetHeight());
        const int tw = std::max(1, static_cast<int>(image.GetWidth() * s));
        const int th = std::max(1, static_cast<int>(image.GetHeight() * s));
        image.Rescale(tw, th, wxIMAGE_QUALITY_HIGH);
        image.Resize(wxSize(w, h), wxPoint((w - tw) / 2, (h - th) / 2), 40, 40, 40);
        return wxBitmap(image);
    }
    wxImage blank(w, h);
    blank.SetRGB(wxRect(0, 0, w, h), 40, 40, 40);
    return wxBitmap(blank);
}

wxString Categories(const MediaItem& m) {
    wxString out;
    for (const std::string& c : m.categories) {
        out += (out.empty() ? "" : ", ") + MediaCategoryName(c);
    }
    return out;
}

void FillList(wxListCtrl* list, wxImageList* images, const std::vector<const MediaItem*>& items, const std::string& showDir) {
    list->DeleteAllItems();
    images->RemoveAll();
    long row = 0;
    for (const MediaItem* m : items) {
        const int icon = images->Add(Thumb(*m, showDir, ICON_W, ICON_H));
        list->InsertItem(row, U(m->name), icon);
        list->SetItem(row, 1, KindName(m->kind));
        list->SetItem(row, 2, m->width > 0 ? wxString::Format("%dx%d", m->width, m->height) : wxString());
        list->SetItem(row, 3, m->durationMS > 0 ? Clock(m->durationMS) : wxString());
        list->SetItem(row, 4, Categories(*m));
        wxString shows;
        for (const std::string& id : m->analysis.description) {
            shows += (shows.empty() ? "" : ", ") + VisualConceptName(id);
        }
        const bool weak = !m->analysis.concepts.empty() && m->analysis.concepts.front().level == "weak";
        list->SetItem(row, 5, shows.empty() ? wxString() : (weak ? _("perhaps ") : wxString()) + shows);
        list->SetItem(row, 6, m->analysis.Done() ? wxString::Format("%.0f%%", m->analysis.matrixFit * 100) : wxString());
        list->SetItem(row, 7, m->analysis.Done() ? wxString::Format("%.0f%%", m->analysis.treeFit * 100) : wxString());
        wxString state = StatusName(*m);
        if (m->Usable()) {
            state += m->analysis.mode == "semantic" ? _(", described") : m->analysis.mode == "basic" ? _(", measured") : _(", not analysed");
            for (const std::string& i : m->analysis.issues) {
                state += i == "very-dark" ? _(", very dark") : i == "very-bright" ? _(", very bright") : i == "low-resolution" ? _(", too small") : i == "too-short" ? _(", too short")
                       : i == "static-video" ? _(", does not move") : i.rfind("duplicate-of:", 0) == 0 ? _(", a copy of another") : i.rfind("similar-to:", 0) == 0 ? _(", like another") : wxString();
            }
        }
        list->SetItem(row, 8, state);
        ++row;
    }
}

void Columns(wxListCtrl* list) {
    list->AppendColumn(_("File"), wxLIST_FORMAT_LEFT, 240);
    list->AppendColumn(_("Kind"), wxLIST_FORMAT_LEFT, 90);
    list->AppendColumn(_("Size"), wxLIST_FORMAT_LEFT, 80);
    list->AppendColumn(_("Length"), wxLIST_FORMAT_LEFT, 70);
    list->AppendColumn(_("Categories"), wxLIST_FORMAT_LEFT, 150);
    list->AppendColumn(_("What it shows"), wxLIST_FORMAT_LEFT, 170);
    list->AppendColumn(_("Matrix"), wxLIST_FORMAT_LEFT, 60);
    list->AppendColumn(_("Mega tree"), wxLIST_FORMAT_LEFT, 75);
    list->AppendColumn(_("State"), wxLIST_FORMAT_LEFT, 220);
}

wxString RefusedReason(const std::string& reason) {
    if (reason == "root") {
        return _("a whole disk is not searched; choose the folder that holds the pictures");
    }
    if (reason == "home") {
        return _("the whole home folder is not searched; choose the folder that holds the pictures");
    }
    if (reason == "system") {
        return _("a system folder is not searched");
    }
    if (reason == "not-found" || reason == "not-folder") {
        return _("the folder does not exist");
    }
    return U(reason);
}

const VoiceRole ROLES[] = { VoiceRole::Lead, VoiceRole::Second, VoiceRole::Chorus, VoiceRole::Narrator, VoiceRole::Dialogue, VoiceRole::Extra, VoiceRole::Unknown };

} // namespace

wxString MediaCategoryName(const std::string& id) {
    static const std::vector<std::pair<std::string, wxString>> NAMES = {
        { "christmas", _("Christmas") }, { "snow", _("Snow") }, { "fire", _("Fire") }, { "stars", _("Stars") }, { "forest", _("Forest") }, { "animals", _("Animals") }, { "cinema", _("Cinema") },
        { "adventure", _("Adventure") }, { "fantasy", _("Fantasy") }, { "halloween", _("Halloween") }, { "new-year", _("New Year") }, { "lights", _("Lights") }, { "backgrounds", _("Backgrounds") },
        { "other", _("Other") },
    };
    for (const auto& [k, v] : NAMES) {
        if (k == id) {
            return v;
        }
    }
    return U(id);
}

wxString VoiceRoleName(VoiceRole role) {
    switch (role) {
    case VoiceRole::Lead: return _("Lead voice");
    case VoiceRole::Second: return _("Second voice");
    case VoiceRole::Chorus: return _("Choir");
    case VoiceRole::Narrator: return _("Narrator");
    case VoiceRole::Dialogue: return _("Dialogue");
    case VoiceRole::Extra: return _("Other voice");
    default: return _("Unknown voice");
    }
}

// ------------------------------------------------------------------ library

MediaLibraryDialog::MediaLibraryDialog(wxWindow* parent, const std::string& showDir, const std::vector<std::string>& folders) :
    wxDialog(parent, wxID_ANY, _("Pictures and Videos of the Show"), wxDefaultPosition, wxSize(1240, 700), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    _showDir(showDir),
    _folders(folders) {
    std::string error;
    (void)ReadMediaLibrary(MediaLibraryPath(showDir), _library, error);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(new wxStaticText(this, wxID_ANY, _("Show Composer only reads these files: nothing is moved, renamed or deleted. A file outside the show is copied into it only if you agree when generating.")),
               0, wxEXPAND | wxALL, 8);
    auto* folderRow = new wxBoxSizer(wxHORIZONTAL);
    _folderList = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 70));
    folderRow->Add(_folderList, 1, wxEXPAND | wxRIGHT, 6);
    auto* folderButtons = new wxBoxSizer(wxVERTICAL);
    auto* add = new wxButton(this, wxID_ANY, _("Add Folder..."));
    auto* remove = new wxButton(this, wxID_ANY, _("Remove Folder"));
    auto* scan = new wxButton(this, wxID_ANY, _("Search the Folders"));
    folderButtons->Add(add, 0, wxEXPAND | wxBOTTOM, 4);
    folderButtons->Add(remove, 0, wxEXPAND | wxBOTTOM, 4);
    folderButtons->Add(scan, 0, wxEXPAND);
    folderRow->Add(folderButtons, 0);
    sizer->Add(folderRow, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);

    auto* filterRow = new wxBoxSizer(wxHORIZONTAL);
    filterRow->Add(new wxStaticText(this, wxID_ANY, _("Show:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    _filter = new wxChoice(this, wxID_ANY);
    _filter->Append(_("Everything"));
    _filter->Append(_("Pictures and animations"));
    _filter->Append(_("Videos"));
    _filter->Append(_("Not usable"));
    for (const std::string& c : MediaCategories()) {
        _filter->Append(MediaCategoryName(c));
    }
    _filter->SetSelection(0);
    filterRow->Add(_filter, 0, wxRIGHT, 12);
    _summary = new wxStaticText(this, wxID_ANY, "");
    filterRow->Add(_summary, 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(filterRow, 0, wxEXPAND | wxALL, 8);

    auto* body = new wxBoxSizer(wxHORIZONTAL);
    _items = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
    _images = new wxImageList(ICON_W, ICON_H, false);
    _items->SetImageList(_images, wxIMAGE_LIST_SMALL);
    Columns(_items);
    body->Add(_items, 3, wxEXPAND | wxRIGHT, 8);
    auto* side = new wxBoxSizer(wxVERTICAL);
    _preview = new wxStaticBitmap(this, wxID_ANY, wxBitmap(160, 120));
    side->Add(_preview, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, 6);
    _info = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxSize(240, 150));
    side->Add(_info, 0, wxEXPAND | wxBOTTOM, 6);
    side->Add(new wxStaticText(this, wxID_ANY, _("Categories (they decide which songs it suits):")), 0, wxBOTTOM, 2);
    wxArrayString names;
    for (const std::string& c : MediaCategories()) {
        names.Add(MediaCategoryName(c));
    }
    _categories = new wxCheckListBox(this, wxID_ANY, wxDefaultPosition, wxSize(240, 180), names);
    side->Add(_categories, 0, wxEXPAND | wxBOTTOM, 4);
    auto* toAll = new wxButton(this, wxID_ANY, _("Give These Categories to All Selected"));
    side->Add(toAll, 0, wxEXPAND | wxBOTTOM, 6);
    toAll->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { (void)ApplyCategoriesToSelected(); });
    side->Add(new wxStaticText(this, wxID_ANY, _("Your own words, separated by commas:")), 0, wxBOTTOM, 2);
    _tags = new wxTextCtrl(this, wxID_ANY);
    side->Add(_tags, 0, wxEXPAND | wxBOTTOM, 6);
    _disabled = new wxCheckBox(this, wxID_ANY, _("Only when I choose it (never automatically)"));
    side->Add(_disabled, 0, wxBOTTOM, 6);
    auto* labels = new wxButton(this, wxID_ANY, _("What It Shows..."));
    side->Add(labels, 0, wxEXPAND | wxBOTTOM, 4);
    auto* measure = new wxButton(this, wxID_ANY, _("How Well the Program Sees..."));
    side->Add(measure, 0, wxEXPAND | wxBOTTOM, 6);
    labels->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        StoreSelected();
        const MediaItem* m = _library.Find(_selected);
        if (m == nullptr) {
            return;
        }
        VisualConcepts concepts;
        std::string error;
        (void)ReadVisualConcepts(DefaultVisualConceptsPath(), concepts, error);
        MediaLabelsDialog dialog(this, *m, _showDir, concepts);
        if (dialog.ShowModal() == wxID_OK) {
            (void)SetSelectedLabels(dialog.Labels());
        }
    });
    measure->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const MediaValidationSet set = MediaValidationSetFromLibrary(_library);
        const MediaValidationMetrics m = EvaluateMediaValidation(set, "person", 3);
        wxString text;
        if (m.files == 0) {
            text = _("No file has been labelled completely yet: open a file with \"What It Shows...\", say what is really there and tick \"I went through everything it shows\".");
        } else {
            text = wxString::Format(_("Measured on %d files you labelled (duplicates counted once).\nOf what the program said first, %.0f%% was right; of what is really there, it saw %.0f%%."), m.files,
                                    m.precision * 100, m.recall * 100);
            if (!m.reliable) {
                text += "\n\n" + wxString::Format(_("Only %d files: label at least %d before trusting these numbers."), m.files, MIN_VALIDATION_FILES);
            }
            text += "\n\n" + _("Details:") + "\n" + U(ModelReport());
        }
        wxMessageBox(text, _("How Well the Program Sees"), wxOK | wxICON_INFORMATION, this);
    });
    body->Add(side, 0, wxEXPAND);
    sizer->Add(body, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
    sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
    SetSizer(sizer);

    for (const std::string& f : _folders) {
        _folderList->Append(U(f));
    }
    Refill();

    add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxDirDialog dialog(this, _("Choose a Folder of Pictures and Videos"), U(_showDir), wxDD_DIR_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) {
            return;
        }
        std::string refused;
        if (!AddFolder(S(dialog.GetPath()), refused)) {
            wxMessageBox(wxString::Format(_("This folder is not searched: %s."), RefusedReason(refused)), _("Pictures and Videos"), wxOK | wxICON_INFORMATION, this);
        }
    });
    remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const int i = _folderList->GetSelection();
        if (i != wxNOT_FOUND) {
            _folders.erase(_folders.begin() + i);
            _folderList->Delete(i);
        }
    });
    scan->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { (void)Scan(); });
    _filter->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        StoreSelected();
        Refill();
    });
    _items->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& e) {
        StoreSelected();
        const long i = e.GetIndex();
        _selected = i >= 0 && i < static_cast<long>(_shown.size()) ? _shown[i] : std::string();
        ShowSelected();
    });
    Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
        StoreSelected();
        if (!SaveLibrary()) {
            wxMessageBox(_("The library could not be saved in the show's Show Composer folder."), _("Pictures and Videos"), wxOK | wxICON_WARNING, this);
            return;
        }
        e.Skip();
    }, wxID_OK);
}

MediaLibraryDialog::~MediaLibraryDialog() {
    delete _images;
}

bool MediaLibraryDialog::AddFolder(const std::string& folder, std::string& refused) {
    if (!IsSafeMediaFolder(folder, refused)) {
        return false;
    }
    ObtainAccessToURL(folder, false);
    if (std::find(_folders.begin(), _folders.end(), folder) == _folders.end()) {
        _folders.push_back(folder);
        _folderList->Append(U(folder));
    }
    return true;
}

bool MediaLibraryDialog::Scan() {
    StoreSelected();
    MediaScanOptions o;
    o.showDir = _showDir;
    o.folders = _folders;
    std::unique_ptr<wxProgressDialog> progress;
    bool stop = false;
    if (!_quiet) {
        progress = std::make_unique<wxProgressDialog>(_("Searching the Folders"), _("Reading the pictures and videos..."), 100, this, wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);
        o.progress = [&](int done, const std::string& name) { stop = !progress->Pulse(wxString::Format(_("%d files read: %s"), done, U(name))); };
        o.cancelled = [&]() { return stop; };
    }
    const MediaLibrary previous = _library;
    _library = ScanMediaFolders(o, &previous);
    progress.reset();
    _selected.clear();
    Refill();
    return !stop;
}

void MediaLibraryDialog::Refill() {
    const int f = _filter->GetSelection();
    std::vector<const MediaItem*> shown;
    for (const MediaItem& m : _library.items) {
        bool keep = true;
        if (f == 1) {
            keep = m.Usable() && m.kind != MediaKind::Video;
        } else if (f == 2) {
            keep = m.Usable() && m.kind == MediaKind::Video;
        } else if (f == 3) {
            keep = !m.Usable();
        } else if (f >= 4) {
            keep = std::find(m.categories.begin(), m.categories.end(), MediaCategories()[f - 4]) != m.categories.end();
        }
        if (keep) {
            shown.push_back(&m);
        }
    }
    _shown.clear();
    for (const MediaItem* m : shown) {
        _shown.push_back(m->id);
    }
    FillList(_items, _images, shown, _showDir);
    int usable = 0, videos = 0, missing = 0;
    for (const MediaItem& m : _library.items) {
        usable += m.Usable() ? 1 : 0;
        videos += m.Usable() && m.kind == MediaKind::Video ? 1 : 0;
        missing += m.status == MediaStatus::Missing ? 1 : 0;
    }
    _summary->SetLabel(wxString::Format(_("%d files: %d can be used (%d videos), %d missing."), static_cast<int>(_library.items.size()), usable, videos, missing));
    ShowSelected();
}

void MediaLibraryDialog::ShowSelected() {
    const MediaItem* m = _library.Find(_selected);
    _categories->Enable(m != nullptr);
    _tags->Enable(m != nullptr);
    _disabled->Enable(m != nullptr);
    for (unsigned i = 0; i < _categories->GetCount(); ++i) {
        _categories->Check(i, m != nullptr && std::find(m->categories.begin(), m->categories.end(), MediaCategories()[i]) != m->categories.end());
    }
    if (m == nullptr) {
        _tags->ChangeValue("");
        _disabled->SetValue(false);
        _info->SetLabel(_("Choose a file to see it and give it categories."));
        _preview->SetBitmap(wxBitmap(160, 120));
        return;
    }
    wxString tags;
    for (const std::string& t : m->tags) {
        tags += (tags.empty() ? "" : ", ") + U(t);
    }
    _tags->ChangeValue(tags);
    _disabled->SetValue(m->disabled);
    _preview->SetBitmap(Thumb(*m, _showDir, 160, 120));
    wxString info = U(m->path) + "\n" + KindName(m->kind) + " - " + StatusName(*m);
    if (m->analysis.Done()) {
        wxString shows;
        for (const MediaConcept& c : m->analysis.concepts) {
            if (shows.length() < 120) {
                shows += (shows.empty() ? "" : ", ") + VisualConceptName(c.id) + " (" + (c.level == "strong" ? _("clear") : c.level == "probable" ? _("probable") : _("doubtful")) + ")";
            }
        }
        wxString colours;
        for (const MediaColor& c : m->analysis.colors) {
            colours += (colours.empty() ? "" : " ") + U(c.hex);
        }
        info += "\n" + (shows.empty() ? _("Nothing recognised.") : wxString::Format(_("Seems to show: %s."), shows));
        info += "\n" + wxString::Format(_("Colours %s; light %.0f%%, contrast %.0f%%, detail %.0f%%. Matrix %.0f%%, mega tree %.0f%%."), colours, m->analysis.brightness * 100, m->analysis.contrast * 100,
                                         m->analysis.complexity * 100, m->analysis.matrixFit * 100, m->analysis.treeFit * 100);
        if (m->analysis.mode != "semantic") {
            info += "\n" + _("Measured without the image model: what it shows is only a guess from its colours.");
        }
    }
    if (m->categorySource == "name") {
        info += "\n" + _("Categories suggested from the file's name: check them.");
    }
    _info->SetLabel(info);
    _info->Wrap(240);
}

void MediaLibraryDialog::StoreSelected() {
    MediaItem* m = _library.Find(_selected);
    if (m == nullptr) {
        return;
    }
    std::vector<std::string> categories;
    for (unsigned i = 0; i < _categories->GetCount(); ++i) {
        if (_categories->IsChecked(i)) {
            categories.push_back(MediaCategories()[i]);
        }
    }
    std::vector<std::string> tags;
    wxString rest = _tags->GetValue();
    while (!rest.empty()) {
        wxString word = rest.BeforeFirst(',').Strip(wxString::both);
        rest = rest.Contains(",") ? rest.AfterFirst(',') : wxString();
        if (!word.empty()) {
            tags.push_back(S(word));
        }
    }
    if (categories != m->categories || tags != m->tags || _disabled->GetValue() != m->disabled) {
        m->categories = categories;
        m->tags = tags;
        m->disabled = _disabled->GetValue();
        m->categorySource = "manual";
        m->reviewed = true;
    }
}

bool MediaLibraryDialog::SetSelectedLabels(const MediaHumanLabels& labels) {
    MediaItem* m = _library.Find(_selected);
    if (m == nullptr) {
        return false;
    }
    m->human = labels;
    ShowSelected();
    return true;
}

std::string MediaLibraryDialog::ModelReport() const {
    const MediaValidationSet set = MediaValidationSetFromLibrary(_library);
    return MediaValidationReport(set, { EvaluateMediaValidation(set, "person", 3) });
}

std::vector<std::string> MediaLibraryDialog::SelectedIds() const {
    std::vector<std::string> ids;
    for (long i = _items->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED); i >= 0; i = _items->GetNextItem(i, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) {
        if (i < static_cast<long>(_shown.size())) {
            ids.push_back(_shown[static_cast<size_t>(i)]);
        }
    }
    return ids;
}

void MediaLibraryDialog::SelectItems(const std::vector<std::string>& ids) {
    StoreSelected();
    for (size_t i = 0; i < _shown.size(); ++i) {
        const bool on = std::find(ids.begin(), ids.end(), _shown[i]) != ids.end();
        _items->SetItemState(static_cast<long>(i), on ? wxLIST_STATE_SELECTED : 0, wxLIST_STATE_SELECTED);
    }
    _selected = ids.empty() ? std::string() : ids.front();
    ShowSelected();
}

int MediaLibraryDialog::ApplyCategoriesToSelected() {
    std::vector<std::string> categories;
    for (unsigned i = 0; i < _categories->GetCount(); ++i) {
        if (_categories->IsChecked(i)) {
            categories.push_back(MediaCategories()[i]);
        }
    }
    int changed = 0;
    for (const std::string& id : SelectedIds()) {
        MediaItem* m = _library.Find(id);
        if (m != nullptr) {
            m->categories = categories;
            m->categorySource = "manual";
            m->reviewed = true;
            ++changed;
        }
    }
    Refill();
    return changed;
}

void MediaLibraryDialog::SelectItem(const std::string& id) {
    StoreSelected();
    _selected = id;
    ShowSelected();
}

void MediaLibraryDialog::SetItemCategories(const std::vector<std::string>& categories) {
    for (unsigned i = 0; i < _categories->GetCount(); ++i) {
        _categories->Check(i, std::find(categories.begin(), categories.end(), MediaCategories()[i]) != categories.end());
    }
    StoreSelected();
}

void MediaLibraryDialog::SetItemDisabled(bool disabled) {
    _disabled->SetValue(disabled);
    StoreSelected();
}

bool MediaLibraryDialog::SaveLibrary() {
    StoreSelected();
    std::string error;
    return SaveMediaLibrary(MediaLibraryPath(_showDir), _library, error);
}

// ------------------------------------------------------------------ chooser

MediaChooserDialog::MediaChooserDialog(wxWindow* parent, const std::string& showDir, const MediaLibrary& library) :
    wxDialog(parent, wxID_ANY, _("Choose a Picture or a Video"), wxDefaultPosition, wxSize(820, 520), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    _items = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
    _images = new wxImageList(ICON_W, ICON_H, false);
    _items->SetImageList(_images, wxIMAGE_LIST_SMALL);
    Columns(_items);
    std::vector<const MediaItem*> usable;
    for (const MediaItem& m : library.items) {
        if (m.Usable()) {
            usable.push_back(&m);
            _ids.push_back(m.id);
        }
    }
    FillList(_items, _images, usable, showDir);
    sizer->Add(_items, 1, wxEXPAND | wxALL, 8);
    sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
    SetSizer(sizer);
    _items->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) { EndModal(wxID_OK); });
}

MediaChooserDialog::~MediaChooserDialog() {
    delete _images;
}

std::string MediaChooserDialog::Chosen() const {
    const long i = _items->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    return i >= 0 && i < static_cast<long>(_ids.size()) ? _ids[i] : std::string();
}

// ------------------------------------------------------------------ voices

VoicesDialog::VoicesDialog(wxWindow* parent, std::vector<VoiceDef> cast, std::vector<VoicePhrase> phrases, std::vector<std::string> faces, std::function<void(int, int)> play) :
    wxDialog(parent, wxID_ANY, _("Voices"), wxDefaultPosition, wxSize(940, 640), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    _cast(std::move(cast)),
    _phrases(std::move(phrases)),
    _faces(std::move(faces)),
    _play(std::move(play)) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(new wxStaticText(this, wxID_ANY, _("Voices come from the lyrics' tags ([Voz: Ana], [Coro], [Narrador]) and from names before a colon. Nothing is guessed from the sound: a phrase with no voice moves no face until you give it one.")),
               0, wxEXPAND | wxALL, 8);
    auto* top = new wxBoxSizer(wxHORIZONTAL);
    _voiceList = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 150), wxLC_REPORT | wxLC_SINGLE_SEL);
    _voiceList->AppendColumn(_("Voice"), wxLIST_FORMAT_LEFT, 150);
    _voiceList->AppendColumn(_("Role"), wxLIST_FORMAT_LEFT, 120);
    _voiceList->AppendColumn(_("Faces"), wxLIST_FORMAT_LEFT, 220);
    top->Add(_voiceList, 1, wxEXPAND | wxRIGHT, 8);
    auto* voiceEdit = new wxFlexGridSizer(2, 4, 6);
    voiceEdit->Add(new wxStaticText(this, wxID_ANY, _("Role:")), 0, wxALIGN_CENTER_VERTICAL);
    _role = new wxChoice(this, wxID_ANY);
    for (VoiceRole r : ROLES) {
        _role->Append(VoiceRoleName(r));
    }
    voiceEdit->Add(_role, 0, wxEXPAND);
    voiceEdit->Add(new wxStaticText(this, wxID_ANY, _("Face:")), 0, wxALIGN_CENTER_VERTICAL);
    _face = new wxChoice(this, wxID_ANY);
    _face->Append(_("Chosen automatically"));
    for (const std::string& f : _faces) {
        _face->Append(U(f));
    }
    voiceEdit->Add(_face, 0, wxEXPAND);
    voiceEdit->AddSpacer(1);
    _facesOff = new wxCheckBox(this, wxID_ANY, _("No face moves for this voice"));
    voiceEdit->Add(_facesOff, 0);
    voiceEdit->AddSpacer(1);
    auto* newVoice = new wxButton(this, wxID_ANY, _("New Voice..."));
    voiceEdit->Add(newVoice, 0);
    top->Add(voiceEdit, 0);
    sizer->Add(top, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);

    auto* bottom = new wxBoxSizer(wxHORIZONTAL);
    _phraseList = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
    _phraseList->AppendColumn(_("From"), wxLIST_FORMAT_LEFT, 70);
    _phraseList->AppendColumn(_("To"), wxLIST_FORMAT_LEFT, 70);
    _phraseList->AppendColumn(_("Phrase"), wxLIST_FORMAT_LEFT, 300);
    _phraseList->AppendColumn(_("Voices"), wxLIST_FORMAT_LEFT, 160);
    _phraseList->AppendColumn(_("How"), wxLIST_FORMAT_LEFT, 70);
    bottom->Add(_phraseList, 1, wxEXPAND | wxRIGHT, 8);
    auto* phraseEdit = new wxBoxSizer(wxVERTICAL);
    phraseEdit->Add(new wxStaticText(this, wxID_ANY, _("Who sings the selected phrases:")), 0, wxBOTTOM, 2);
    _phraseVoices = new wxCheckListBox(this, wxID_ANY, wxDefaultPosition, wxSize(220, 120));
    phraseEdit->Add(_phraseVoices, 0, wxEXPAND | wxBOTTOM, 4);
    _spoken = new wxCheckBox(this, wxID_ANY, _("Spoken, not sung"));
    phraseEdit->Add(_spoken, 0, wxBOTTOM, 4);
    auto* apply = new wxButton(this, wxID_ANY, _("Give Them These Voices"));
    phraseEdit->Add(apply, 0, wxEXPAND | wxBOTTOM, 10);
    auto* splitRow = new wxBoxSizer(wxHORIZONTAL);
    splitRow->Add(new wxStaticText(this, wxID_ANY, _("Before word:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    _splitAt = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(70, -1), wxSP_ARROW_KEYS, 1, 99, 1);
    splitRow->Add(_splitAt, 0);
    phraseEdit->Add(splitRow, 0, wxBOTTOM, 4);
    auto* split = new wxButton(this, wxID_ANY, _("Split the Phrase"));
    phraseEdit->Add(split, 0, wxEXPAND | wxBOTTOM, 4);
    auto* merge = new wxButton(this, wxID_ANY, _("Join With the Next"));
    phraseEdit->Add(merge, 0, wxEXPAND | wxBOTTOM, 10);
    auto* listen = new wxButton(this, wxID_ANY, _("Play the Phrase"));
    phraseEdit->Add(listen, 0, wxEXPAND);
    bottom->Add(phraseEdit, 0);
    sizer->Add(bottom, 1, wxEXPAND | wxALL, 8);
    sizer->Add(new wxStaticText(this, wxID_ANY, _("Your changes are kept with the show and used the next time it is composed.")), 0, wxLEFT | wxRIGHT, 8);
    sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
    SetSizer(sizer);
    RefillVoices();
    RefillPhrases();

    _voiceList->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& e) {
        StoreVoice();
        _voice = e.GetIndex() >= 0 && e.GetIndex() < static_cast<long>(_cast.size()) ? _cast[e.GetIndex()].id : std::string();
        ShowVoice();
    });
    for (wxWindow* w : std::initializer_list<wxWindow*>{ _role, _face }) {
        w->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            StoreVoice();
            RefillVoices();
        });
    }
    _facesOff->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        StoreVoice();
        RefillVoices();
    });
    newVoice->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxTextEntryDialog dialog(this, _("The voice's name, as you want to see it:"), _("New Voice"));
        if (dialog.ShowModal() == wxID_OK && !dialog.GetValue().Strip(wxString::both).empty()) {
            AddVoice(S(dialog.GetValue().Strip(wxString::both)));
        }
    });
    _phraseList->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) {
        const int p = SelectedPhrase();
        if (p < 0) {
            return;
        }
        for (size_t v = 0; v < _cast.size(); ++v) {
            _phraseVoices->Check(static_cast<unsigned>(v), std::find(_phrases[p].voices.begin(), _phrases[p].voices.end(), _cast[v].id) != _phrases[p].voices.end());
        }
        _spoken->SetValue(_phrases[p].spoken);
        _splitAt->SetRange(1, std::max(1, _phrases[p].words - 1));
    });
    apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        std::vector<std::string> voices;
        for (size_t v = 0; v < _cast.size(); ++v) {
            if (_phraseVoices->IsChecked(static_cast<unsigned>(v))) {
                voices.push_back(_cast[v].id);
            }
        }
        for (long i = _phraseList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED); i >= 0; i = _phraseList->GetNextItem(i, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) {
            SetPhraseVoices(static_cast<int>(i), voices, _spoken->GetValue());
        }
    });
    split->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (SelectedPhrase() >= 0) {
            Split(SelectedPhrase(), _splitAt->GetValue());
        }
    });
    merge->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (SelectedPhrase() >= 0) {
            Merge(SelectedPhrase());
        }
    });
    listen->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const int p = SelectedPhrase();
        if (p >= 0 && _play && _phrases[p].startMS >= 0) {
            _play(_phrases[p].startMS, std::max(500, _phrases[p].endMS - _phrases[p].startMS));
        }
    });
    Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
        StoreVoice();
        e.Skip();
    }, wxID_OK);
}

VoiceDef* VoicesDialog::Voice(const std::string& id) {
    const auto it = std::find_if(_cast.begin(), _cast.end(), [&](const VoiceDef& v) { return v.id == id; });
    return it != _cast.end() ? &*it : nullptr;
}

int VoicesDialog::SelectedPhrase() const {
    const long i = _phraseList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    return i >= 0 && i < static_cast<long>(_phrases.size()) ? static_cast<int>(i) : -1;
}

void VoicesDialog::RefillVoices() {
    _voiceList->DeleteAllItems();
    _phraseVoices->Clear();
    long row = 0;
    for (const VoiceDef& v : _cast) {
        _voiceList->InsertItem(row, U(v.name));
        _voiceList->SetItem(row, 1, VoiceRoleName(v.role));
        wxString faces = v.facesOff ? _("none") : wxString();
        if (!v.facesOff) {
            for (const std::string& f : v.faces) {
                faces += (faces.empty() ? "" : ", ") + U(f);
            }
            faces = faces.empty() ? _("chosen automatically") : faces;
        }
        _voiceList->SetItem(row, 2, faces);
        if (v.id == _voice) {
            _voiceList->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        }
        _phraseVoices->Append(U(v.name));
        ++row;
    }
    ShowVoice();
}

void VoicesDialog::RefillPhrases() {
    _phraseList->DeleteAllItems();
    long row = 0;
    for (const VoicePhrase& p : _phrases) {
        _phraseList->InsertItem(row, p.startMS >= 0 ? Clock(p.startMS) : wxString("-"));
        _phraseList->SetItem(row, 1, p.endMS >= 0 ? Clock(p.endMS) : wxString("-"));
        _phraseList->SetItem(row, 2, U(p.text));
        wxString names;
        for (const std::string& id : p.voices) {
            const VoiceDef* v = Voice(id);
            names += (names.empty() ? "" : ", ") + (v != nullptr ? U(v->name) : U(id));
        }
        _phraseList->SetItem(row, 3, names.empty() ? _("no voice") : names);
        _phraseList->SetItem(row, 4, p.spoken ? _("spoken") : _("sung"));
        ++row;
    }
}

void VoicesDialog::ShowVoice() {
    const VoiceDef* v = Voice(_voice);
    _role->Enable(v != nullptr);
    _face->Enable(v != nullptr && !v->facesOff);
    _facesOff->Enable(v != nullptr);
    if (v == nullptr) {
        return;
    }
    _role->SetSelection(static_cast<int>(std::find(std::begin(ROLES), std::end(ROLES), v->role) - std::begin(ROLES)));
    _facesOff->SetValue(v->facesOff);
    const auto face = v->faces.empty() ? _faces.end() : std::find(_faces.begin(), _faces.end(), v->faces.front());
    _face->SetSelection(face == _faces.end() ? 0 : static_cast<int>(face - _faces.begin()) + 1);
    _shownFace = _face->GetSelection();
}

void VoicesDialog::StoreVoice() {
    VoiceDef* v = Voice(_voice);
    if (v == nullptr || _role->GetSelection() < 0) {
        return;
    }
    const VoiceRole role = ROLES[_role->GetSelection()];
    // The face changes only when the person picked another one: a choir keeps its faces.
    std::vector<std::string> faces = v->faces;
    if (_face->GetSelection() != _shownFace) {
        faces = _face->GetSelection() > 0 ? std::vector<std::string>{ _faces[_face->GetSelection() - 1] } : std::vector<std::string>();
    }
    if (role == v->role && faces == v->faces && _facesOff->GetValue() == v->facesOff) {
        return;
    }
    v->role = role;
    v->faces = faces;
    v->facesOff = _facesOff->GetValue();
    v->delivery = role == VoiceRole::Narrator || role == VoiceRole::Dialogue ? Delivery::Spoken : v->delivery;
    v->source = "manual";
    v->reviewed = true;
    if (std::find(_edited.begin(), _edited.end(), v->id) == _edited.end()) {
        _edited.push_back(v->id);
    }
}

std::vector<VoiceDef> VoicesDialog::Voices() const {
    std::vector<VoiceDef> out;
    for (const VoiceDef& v : _cast) {
        if (std::find(_edited.begin(), _edited.end(), v.id) != _edited.end()) {
            out.push_back(v);
        }
    }
    return out;
}

void VoicesDialog::SetPhraseVoices(int phrase, const std::vector<std::string>& voices, bool spoken) {
    if (phrase < 0 || phrase >= static_cast<int>(_phrases.size())) {
        return;
    }
    _phrases[phrase].voices = voices;
    _phrases[phrase].spoken = spoken;
    LyricCorrection c;
    c.level = LyricCorrection::Level::Voice;
    c.targetId = _phrases[phrase].id;
    c.voices = voices;
    c.delivery = spoken ? "spoken" : "sung";
    UpsertCorrection(_corrections, c);
    RefillPhrases();
}

void VoicesDialog::SetVoiceFacesOff(const std::string& voice, bool off) {
    _voice = voice;
    ShowVoice();
    _facesOff->SetValue(off);
    StoreVoice();
    RefillVoices();
}

void VoicesDialog::SetVoiceRole(const std::string& voice, VoiceRole role) {
    _voice = voice;
    ShowVoice();
    _role->SetSelection(static_cast<int>(std::find(std::begin(ROLES), std::end(ROLES), role) - std::begin(ROLES)));
    StoreVoice();
    RefillVoices();
}

void VoicesDialog::AddVoice(const std::string& name) {
    const std::string id = VoiceIdOf(name);
    if (Voice(id) == nullptr) {
        VoiceDef v;
        v.id = id;
        v.name = name;
        v.role = VoiceRole::Extra;
        v.source = "manual";
        v.reviewed = true;
        _cast.push_back(v);
        _edited.push_back(id);
    }
    _voice = id;
    RefillVoices();
}

void VoicesDialog::Split(int phrase, int beforeWord) {
    if (phrase < 0 || phrase >= static_cast<int>(_phrases.size()) || beforeWord <= 0 || beforeWord >= _phrases[phrase].words) {
        return;
    }
    LyricCorrection c;
    c.level = LyricCorrection::Level::Split;
    c.targetId = _phrases[phrase].id;
    c.wordIndex = beforeWord;
    UpsertCorrection(_corrections, c);
    VoicePhrase second = _phrases[phrase];
    second.id = fmt::format("{}~{}", second.id, beforeWord);
    second.text = "...";
    second.words = _phrases[phrase].words - beforeWord;
    _phrases[phrase].words = beforeWord;
    _phrases.insert(_phrases.begin() + phrase + 1, second);
    RefillPhrases();
}

void VoicesDialog::Merge(int phrase) {
    if (phrase < 0 || phrase + 1 >= static_cast<int>(_phrases.size())) {
        return;
    }
    LyricCorrection c;
    c.level = LyricCorrection::Level::Merge;
    c.targetId = _phrases[phrase].id;
    UpsertCorrection(_corrections, c);
    _phrases[phrase].text += " " + _phrases[phrase + 1].text;
    _phrases[phrase].endMS = _phrases[phrase + 1].endMS;
    _phrases[phrase].words += _phrases[phrase + 1].words;
    _phrases.erase(_phrases.begin() + phrase + 1);
    RefillPhrases();
}
