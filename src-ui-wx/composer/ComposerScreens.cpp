/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "ComposerScreens.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>

#include <algorithm>
#include <cmath>

using namespace composer;

namespace {

wxString U(const std::string& s) {
    return wxString::FromUTF8(s);
}

constexpr SurfaceRole ROLES[] = { SurfaceRole::Main, SurfaceRole::Secondary, SurfaceRole::Tree, SurfaceRole::Lyrics, SurfaceRole::Narration, SurfaceRole::Media, SurfaceRole::Support, SurfaceRole::Off };

wxString KindName(const std::string& kind) {
    if (kind == "matrix") {
        return _("Matrix");
    }
    if (kind == "mega-tree") {
        return _("Mega tree");
    }
    if (kind == "tree") {
        return _("Tree");
    }
    if (kind == "grid") {
        return _("Pixel grid");
    }
    if (kind == "submodel") {
        return _("Part of a matrix");
    }
    return U(kind);
}

// A surface's reasons, said simply.
wxString Why(const Surface& s) {
    wxString out;
    const auto add = [&](const wxString& part) {
        if (!part.empty()) {
            out += (out.empty() ? "" : "; ") + part;
        }
    };
    for (const std::string& r : s.reasons) {
        const std::string code = r.substr(0, r.find(':'));
        const std::string detail = r.find(':') == std::string::npos ? std::string() : r.substr(r.find(':') + 1);
        if (r == "chosen-by-person") {
            add(_("your choice"));
        } else if (code == "shows-lyrics") {
            add(wxString::Format(_("shows words %s of the song"), U(detail)));
        } else if (r == "main:largest-free-visible") {
            add(_("the largest screen that is seen and free"));
        } else if (r == "tree:the-mega-tree") {
            add(_("the mega tree"));
        } else if (r == "secondary:second-matrix-worth-it") {
            add(_("big enough for a second screen"));
        } else if (r == "support:lights") {
            add(_("plays with the lights"));
        } else if (r == "support:not-a-picture") {
            add(_("too few lights for a picture"));
        } else if (r == "support:no-controller") {
            add(_("no controller: its pictures would only be seen on the computer"));
        } else if (r == "support:shape-unknown") {
            add(_("not known to draw a picture: choose it to try"));
        } else if (r == "unchecked-shape-chosen-by-person") {
            add(_("check in xLights that the picture looks right"));
        } else if (r == "submodel:only-if-chosen") {
            add(_("only if you choose it"));
        } else if (r == "small-buffer-chosen-by-person") {
            add(_("few lights: the picture will be coarse"));
        }
    }
    if (!s.conflicts.empty()) {
        add(wxString::Format(_("shares its lights with %s"), U(s.conflicts.front())));
    }
    if (s.noController) {
        add(_("no controller"));
    }
    return out;
}

wxString Warning(const std::string& w) {
    const std::string code = w.substr(0, w.find(':'));
    std::string rest = w.find(':') == std::string::npos ? std::string() : w.substr(w.find(':') + 1);
    const std::string first = rest.substr(0, rest.find(':'));
    const std::string second = rest.find(':') == std::string::npos ? std::string() : rest.substr(rest.find(':') + 1);
    if (code == "surface-shares-channels") {
        return wxString::Format(_("%s and %s share lights: they never show pictures at the same time."), U(first), U(second));
    }
    if (code == "surface-no-controller") {
        return wxString::Format(_("%s has no controller: its pictures will only be seen once it is wired."), U(first));
    }
    if (code == "surface-unchecked") {
        return wxString::Format(_("%s is not known to draw a picture well: check it in xLights."), U(first));
    }
    if (code == "surface-coarse") {
        return wxString::Format(_("%s has few lights (%s): its pictures will be coarse."), U(first), U(second));
    }
    return U(w);
}

bool Round(const Surface& s) {
    return (s.kind == "mega-tree" || s.kind == "tree") && s.degrees >= 300;
}

} // namespace

wxString ScreenRoleName(SurfaceRole role) {
    switch (role) {
    case SurfaceRole::Main: return _("Main screen");
    case SurfaceRole::Secondary: return _("Second screen");
    case SurfaceRole::Tree: return _("Tree screen");
    case SurfaceRole::Lyrics: return _("Words");
    case SurfaceRole::Narration: return _("Narration");
    case SurfaceRole::Media: return _("Extra pictures");
    case SurfaceRole::Support: return _("Lights only");
    case SurfaceRole::Off: return _("Not used");
    }
    return {};
}

std::vector<wxString> ScreenWarnings(const SurfacePlan& plan) {
    std::vector<wxString> out;
    for (const std::string& x : plan.warnings) {
        std::string w = x;
        // The same pair of figures is warned about from each side: once is enough.
        const std::string shared = "surface-shares-channels:";
        if (w.rfind(shared, 0) == 0) {
            const std::string rest = w.substr(shared.size());
            const std::string a = rest.substr(0, rest.find(':'));
            const std::string b = rest.find(':') == std::string::npos ? std::string() : rest.substr(rest.find(':') + 1);
            w = shared + std::min(a, b) + ":" + std::max(a, b);
        }
        const wxString line = Warning(w);
        if (std::find(out.begin(), out.end(), line) == out.end()) {
            out.push_back(line);
        }
    }
    return out;
}

wxString ScreenRoleNameOf(const std::string& id) {
    SurfaceRole r;
    return SurfaceRoleFromId(id, r) ? ScreenRoleName(r) : U(id);
}

wxString LyricsModeName(LyricsMode mode) {
    switch (mode) {
    case LyricsMode::LyricsFirst: return _("Words first");
    case LyricsMode::Smart: return _("Smart");
    case LyricsMode::Free: return _("Free");
    }
    return {};
}

ScreensDialog::ScreensDialog(wxWindow* parent, const ModelCapabilities& caps, const CompositionPlan* plan, const ScreenSettings& settings) :
    wxDialog(parent, wxID_ANY, _("Screens"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    _caps(caps),
    _plan(plan),
    _settings(settings) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* modeRow = new wxBoxSizer(wxHORIZONTAL);
    modeRow->Add(new wxStaticText(this, wxID_ANY, _("Words and pictures:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    _mode = new wxChoice(this, wxID_ANY);
    for (const LyricsMode m : { LyricsMode::LyricsFirst, LyricsMode::Smart, LyricsMode::Free }) {
        _mode->Append(LyricsModeName(m));
    }
    modeRow->Add(_mode, 0, wxRIGHT, 8);
    _modeInfo = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxSize(420, -1));
    modeRow->Add(_modeInfo, 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(modeRow, 0, wxEXPAND | wxALL, 8);
    _forPreview = new wxCheckBox(this, wxID_ANY, _("Only to watch on the computer: screens with no controller may get pictures too"));
    _forPreview->SetValue(_settings.preview);
    _forPreview->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { SetPreview(_forPreview->GetValue()); });
    sizer->Add(_forPreview, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

    _list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(760, 220), wxLC_REPORT | wxLC_SINGLE_SEL);
    for (const auto& [label, width] : std::vector<std::pair<wxString, int>>{ { _("Screen"), 170 }, { _("Kind"), 95 }, { _("Size"), 70 }, { _("Lights"), 60 }, { _("Words"), 55 }, { _("Part it plays"), 120 }, { _("Why"), 330 } }) {
        _list->AppendColumn(label, wxLIST_FORMAT_LEFT, width);
    }
    sizer->Add(_list, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);

    auto* edit = new wxFlexGridSizer(2, 4, 6);
    edit->Add(new wxStaticText(this, wxID_ANY, _("Part it plays:")), 0, wxALIGN_CENTER_VERTICAL);
    _role = new wxChoice(this, wxID_ANY);
    _role->Append(_("Automatic"));
    for (const SurfaceRole r : ROLES) {
        _role->Append(ScreenRoleName(r));
    }
    edit->Add(_role, 0);
    edit->Add(new wxStaticText(this, wxID_ANY, _("Front of a round tree:")), 0, wxALIGN_CENTER_VERTICAL);
    auto* front = new wxBoxSizer(wxHORIZONTAL);
    _front = new wxSpinCtrl(this, wxID_ANY, "25", wxDefaultPosition, wxSize(80, -1), wxSP_ARROW_KEYS, 0, 99, 25);
    front->Add(_front, 0, wxRIGHT, 4);
    front->Add(new wxStaticText(this, wxID_ANY, _("% of the way round from its first string")), 0, wxALIGN_CENTER_VERTICAL);
    edit->Add(front, 0);
    edit->Add(new wxStaticText(this, wxID_ANY, _("Turns:")), 0, wxALIGN_CENTER_VERTICAL);
    _turn = new wxChoice(this, wxID_ANY);
    for (const wxString& t : { _("Automatic"), _("Clockwise"), _("Counter-clockwise") }) {
        _turn->Append(t);
    }
    edit->Add(_turn, 0);
    sizer->Add(edit, 0, wxALL, 8);
    _details = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxSize(740, -1));
    sizer->Add(_details, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
    _warnings = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxSize(740, -1));
    _warnings->SetForegroundColour(wxColour(200, 120, 0));
    sizer->Add(_warnings, 0, wxEXPAND | wxALL, 8);
    sizer->Add(new wxStaticText(this, wxID_ANY, _("The program proposes; you correct. Your choices are kept with the show and used every time it is composed.")), 0, wxLEFT | wxRIGHT, 8);
    sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
    SetSizerAndFit(sizer);

    _mode->SetSelection(static_cast<int>(_settings.mode));
    _mode->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { SetMode(static_cast<LyricsMode>(std::max(0, _mode->GetSelection()))); });
    _list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) { ShowSelected(); });
    _role->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        const std::string t = Selected();
        if (t.empty() || _filling) {
            return;
        }
        const int i = _role->GetSelection();
        if (i <= 0) {
            (void)SetAutomatic(t);
        } else {
            (void)SetRole(t, ROLES[i - 1]);
        }
    });
    const auto front_ = [this]() {
        const std::string t = Selected();
        if (t.empty() || _filling) {
            return;
        }
        const int k = _turn->GetSelection();
        (void)SetTreeFront(t, _front->GetValue() / 100.0, k == 1 ? 1 : k == 2 ? -1 : 0);
    };
    _front->Bind(wxEVT_SPINCTRL, [front_](wxSpinEvent&) { front_(); });
    _turn->Bind(wxEVT_CHOICE, [front_](wxCommandEvent&) { front_(); });
    Replan();
}

ScreenChoice& ScreensDialog::ChoiceFor(const std::string& target) {
    _settings.set = true;
    for (ScreenChoice& c : _settings.screens) {
        if (c.target == target) {
            return c;
        }
    }
    ScreenChoice c;
    c.target = target;
    _settings.screens.push_back(c);
    return _settings.screens.back();
}

void ScreensDialog::SetPreview(bool preview) {
    _settings.set = true;
    _settings.preview = preview;
    if (_forPreview != nullptr && _forPreview->GetValue() != preview) {
        _forPreview->SetValue(preview);
    }
    Replan();
}

void ScreensDialog::SetMode(LyricsMode mode) {
    _settings.set = true;
    _settings.mode = mode;
    Replan();
}

bool ScreensDialog::SetRole(const std::string& target, SurfaceRole role) {
    if (_preview.Find(target) == nullptr) {
        return false;
    }
    ScreenChoice& c = ChoiceFor(target);
    c.role = role;
    c.roleSet = true;
    Replan();
    return true;
}

bool ScreensDialog::SetAutomatic(const std::string& target) {
    if (_preview.Find(target) == nullptr) {
        return false;
    }
    ScreenChoice& c = ChoiceFor(target);
    c.roleSet = false;
    std::erase_if(_settings.screens, [](const ScreenChoice& x) { return !x.roleSet && x.frontOffset < 0 && x.turn == 0; });
    Replan();
    return true;
}

bool ScreensDialog::SetTreeFront(const std::string& target, double frontOffset, int turn) {
    const Surface* s = _preview.Find(target);
    if (s == nullptr || !Round(*s)) {
        return false;
    }
    ScreenChoice& c = ChoiceFor(target);
    c.frontOffset = std::clamp(frontOffset, 0.0, 0.99);
    c.turn = turn;
    Replan();
    return true;
}

std::string ScreensDialog::Selected() const {
    const long i = _list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    return i >= 0 && i < static_cast<long>(_preview.surfaces.size()) ? _preview.surfaces[static_cast<size_t>(i)].target : std::string();
}

void ScreensDialog::Replan() {
    _preview = PlanSurfaces(_caps, _plan, _settings);
    Fill();
}

void ScreensDialog::Fill() {
    const std::string keep = Selected();
    _filling = true;
    _list->DeleteAllItems();
    long row = 0;
    for (const Surface& s : _preview.surfaces) {
        _list->InsertItem(row, U(s.target));
        _list->SetItem(row, 1, KindName(s.kind) + (s.degrees > 0 && s.degrees < 300 && s.kind != "matrix" ? wxString::Format(" %.0f°", s.degrees) : wxString()));
        _list->SetItem(row, 2, wxString::Format("%dx%d", s.bufferWi, s.bufferHt));
        _list->SetItem(row, 3, wxString::Format("%d", s.pixels));
        _list->SetItem(row, 4, _plan != nullptr ? wxString::Format("%.0f%%", s.lyricShare * 100) : wxString("-"));
        _list->SetItem(row, 5, ScreenRoleName(s.role) + (s.roleSource == "person" ? wxString(" *") : wxString()));
        _list->SetItem(row, 6, Why(s));
        if (s.target == keep) {
            _list->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        }
        ++row;
    }
    _filling = false;
    switch (_settings.mode) {
    case LyricsMode::LyricsFirst:
        _modeInfo->SetLabel(_("A screen showing words keeps them all the song; pictures go on the other screens."));
        break;
    case LyricsMode::Smart:
        _modeInfo->SetLabel(_("Words while they are sung; between phrases, in the opening and the instrumentals, pictures there too."));
        break;
    case LyricsMode::Free:
        _modeInfo->SetLabel(_("Every part is yours to choose; a picture still never covers a phrase."));
        break;
    }
    wxString w;
    for (const wxString& line : ScreenWarnings(_preview)) {
        w += (w.empty() ? "" : "\n") + line;
    }
    if (_preview.surfaces.empty()) {
        w = _("No figure in this show can show a picture: matrices, mega trees and dense pixel grids can.");
    }
    _warnings->SetLabel(w);
    ShowSelected();
    Layout();
}

void ScreensDialog::ShowSelected() {
    const std::string t = Selected();
    const Surface* s = t.empty() ? nullptr : _preview.Find(t);
    const ScreenChoice* c = t.empty() ? nullptr : _settings.Find(t);
    _filling = true;
    _role->Enable(s != nullptr);
    _role->SetSelection(c != nullptr && c->roleSet ? 1 + static_cast<int>(std::find(std::begin(ROLES), std::end(ROLES), c->role) - std::begin(ROLES)) : 0);
    const bool round = s != nullptr && Round(*s);
    _front->Enable(round);
    _turn->Enable(round);
    _front->SetValue(c != nullptr && c->frontOffset >= 0 ? static_cast<int>(std::lround(c->frontOffset * 100)) : 25);
    _turn->SetSelection(c == nullptr || c->turn == 0 ? 0 : c->turn > 0 ? 1 : 2);
    _filling = false;
    if (s == nullptr) {
        _details->SetLabel(_("Choose a screen to change the part it plays."));
        return;
    }
    wxString text = wxString::Format(_("%s: %s, %dx%d, %d lights, seen %.0f%% as much as the largest; free of words %.0f%% of the song."), U(s->target), KindName(s->kind), s->bufferWi, s->bufferHt,
                                     s->pixels, s->visibility * 100, s->freeShare * 100);
    if (round) {
        text += " " + _("Videos play on both faces of a round tree, in step; the front sets where the first face is.");
    }
    _details->SetLabel(text);
}
