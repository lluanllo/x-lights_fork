/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "ComposerReviewDialogs.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/choicdlg.h>
#include <wx/choice.h>
#include <wx/datetime.h>
#include <wx/image.h>
#include <wx/listctrl.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/numdlg.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/statbmp.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/utils.h>

#include <algorithm>
#include <map>
#include <cmath>

#include "ComposerMediaDialogs.h"
#include "ComposerScreens.h"
#include "ComposerStoryboard.h"

using namespace composer;

namespace {

wxString U(const std::string& s) {
    return wxString::FromUTF8(s);
}

std::string S(const wxString& s) {
    return std::string(s.ToUTF8());
}

wxString Clock(int ms) {
    return wxString::Format("%d:%02d.%d", ms / 60000, ms / 1000 % 60, ms / 100 % 10);
}

std::string Now() {
    return S(wxDateTime::Now().FormatISOCombined(' '));
}

wxString SceneName(const std::string& role) {
    return role == "establish" ? _("the opening") : role == "verse" ? _("the verses") : role == "chorus" ? _("the choruses") : role == "instrumental" ? _("the instrumentals")
         : role == "bridge" ? _("the bridge") : role == "climax" ? _("the climax") : role == "ending" ? _("the ending") : _("the calm parts");
}

wxString KindText(const std::string& kind) {
    return kind == "video" ? _("Video") : kind == "animation" ? _("Animation") : _("Picture");
}

bool Has(const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

// A clip's reasons ("code: detail; code: detail") said simply.
wxString WhyText(const std::string& why, bool first) {
    wxString out;
    size_t at = 0;
    while (at < why.size()) {
        const size_t end = std::min(why.find("; ", at), why.size());
        const std::string part = why.substr(at, end - at);
        at = end + 2;
        const std::string code = part.substr(0, part.find(':'));
        const std::string detail = part.find(": ") == std::string::npos ? std::string() : part.substr(part.find(": ") + 2);
        wxString text;
        if (code == "chosen-by-person" || code == "locked-by-person") {
            text = _("your choice");
        } else if (code == "theme-category") {
            text = detail == "neutral" || detail == "no-theme" ? _("fits any theme") : wxString::Format(_("theme: %s"), U(detail.substr(0, detail.find(" ("))));
        } else if (code == "scene-role") {
            text = _("suits the part of the song");
        } else if (code == "story") {
            text = _("follows on from the shot before");
        } else if (code == "between-phrases") {
            text = _("between the sung phrases");
        } else if (code == "repeat-keeps-media") {
            text = _("the music repeats, so does the picture");
        } else {
            continue;
        }
        out += (out.empty() ? "" : ", ") + text;
        if (first) {
            break;
        }
    }
    return out;
}

} // namespace

wxString ClipStateName(ClipState s) {
    switch (s) {
    case ClipState::Approved: return _("Approved");
    case ClipState::Rejected: return _("Rejected");
    case ClipState::NeedsFix: return _("Needs a fix");
    default: return _("Pending");
    }
}

wxString ClipFlagName(const std::string& f) {
    if (f == "not-music") {
        return _("does not fit the music");
    }
    if (f == "not-scene") {
        return _("does not fit the scene");
    }
    if (f == "repeats") {
        return _("comes back too often");
    }
    if (f == "wrong-screen") {
        return _("not the right screen for it");
    }
    if (f == "wrong-motion") {
        return _("the movement is wrong");
    }
    if (f == "wrong-transition") {
        return _("the way in does not work");
    }
    return U(f);
}

wxString PreferenceValueText(double v) {
    const int pct = static_cast<int>(std::lround(std::fabs(v) * 100));
    return v >= 0 ? wxString::Format(_("more (+%d%%)"), pct) : wxString::Format(_("less (-%d%%)"), pct);
}

wxString PreferenceName(const std::string& key, const MediaLibrary* library) {
    const auto after = [&](const std::string& prefix) { return key.substr(prefix.size()); };
    if (key == "kind.video") {
        return _("Videos rather than pictures");
    }
    if (key == "director.variety") {
        return _("More variety from scene to scene");
    }
    if (key == "director.christmas") {
        return _("Christmas colours rather than the song's");
    }
    if (key == "light.dark") {
        return _("Dark pictures and videos");
    }
    if (key == "motion.moving") {
        return _("Slow camera movements");
    }
    if (key == "repetition") {
        return _("Files coming back in a song");
    }
    if (key == "amount") {
        return _("How much of the song has pictures");
    }
    if (key.rfind("transition.", 0) == 0) {
        return wxString::Format(_("Coming in with a %s"), TransitionName(after("transition.")));
    }
    if (key.rfind("concept.", 0) == 0) {
        return wxString::Format(_("Pictures with %s"), VisualConceptName(after("concept.")));
    }
    if (key.rfind("screen.", 0) == 0) {
        const std::string rest = after("screen.");
        if (const size_t p = rest.rfind(".presence"); p != std::string::npos && p + 9 == rest.size()) {
            return wxString::Format(_("Pictures on %s"), U(rest.substr(0, p)));
        }
        if (const size_t c = rest.find(".concept."); c != std::string::npos) {
            return wxString::Format(_("%s on %s"), VisualConceptName(rest.substr(c + 9)), U(rest.substr(0, c)));
        }
    }
    if (key.rfind("scene.", 0) == 0) {
        const std::string rest = after("scene.");
        const size_t dot = rest.find('.');
        const std::string role = rest.substr(0, dot);
        const std::string what = dot == std::string::npos ? std::string() : rest.substr(dot + 1);
        if (what == "motion.moving") {
            return wxString::Format(_("Movement in %s"), SceneName(role));
        }
        if (what.rfind("concept.", 0) == 0) {
            return wxString::Format(_("%s in %s"), VisualConceptName(what.substr(8)), SceneName(role));
        }
    }
    if (key.rfind("profile.", 0) == 0) {
        const std::string rest = after("profile.");
        const size_t dot = rest.find('.');
        const std::string what = dot == std::string::npos ? std::string() : rest.substr(dot + 1);
        return wxString::Format(_("%s, with the style %s"), what == "kind.video" ? _("Videos rather than pictures") : _("Slow camera movements"), U(rest.substr(0, dot)));
    }
    if (key.rfind("file.", 0) == 0) {
        const std::string rest = after("file.");
        const size_t at = rest.rfind('@');
        const std::string id = rest.substr(0, at);
        const MediaItem* m = library != nullptr ? library->Find(id) : nullptr;
        return wxString::Format(_("%s on %s"), m != nullptr ? U(m->name) : U(id), U(at == std::string::npos ? std::string() : rest.substr(at + 1)));
    }
    return U(key);
}

// ------------------------------------------------------------------ review

ReviewDialog::ReviewDialog(wxWindow* parent, VisualReview review, const ReviewContext& context, const MediaLibrary& library, const std::string& showDir) :
    wxDialog(parent, wxID_ANY, _("Review the Pictures and Videos"), wxDefaultPosition, wxSize(1220, 760), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    _review(std::move(review)),
    _context(context),
    _library(library),
    _showDir(showDir) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    _summary = new wxStaticText(this, wxID_ANY, "");
    sizer->Add(_summary, 0, wxEXPAND | wxALL, 8);
    auto* orderRow = new wxBoxSizer(wxHORIZONTAL);
    orderRow->Add(new wxStaticText(this, wxID_ANY, _("Order:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    _order = new wxChoice(this, wxID_ANY);
    _order->Append(_("As they come in the song"));
    _order->Append(_("Screen by screen"));
    _order->SetSelection(0);
    orderRow->Add(_order, 0);
    sizer->Add(orderRow, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    _list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 300), wxLC_REPORT | wxLC_SINGLE_SEL);
    for (const auto& [label, width] : std::vector<std::pair<wxString, int>>{ { _("Screen"), 110 }, { _("From"), 60 }, { _("To"), 60 }, { _("File"), 170 }, { _("Kind"), 65 },
                                                                         { _("What it shows"), 160 }, { _("Why"), 200 }, { _("Sure"), 50 }, { _("Movement"), 120 }, { _("In / out"), 120 }, { _("State"), 100 } }) {
        _list->AppendColumn(label, wxLIST_FORMAT_LEFT, width);
    }
    sizer->Add(_list, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
    _detail = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 36));
    sizer->Add(_detail, 0, wxEXPAND | wxALL, 8);
    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    auto* yes = new wxButton(this, wxID_ANY, _("Approve"));
    auto* no = new wxButton(this, wxID_ANY, _("Reject"));
    auto* change = new wxButton(this, wxID_ANY, _("Another File..."));
    auto* why = new wxButton(this, wxID_ANY, _("It Does Not Work Because..."));
    auto* note = new wxButton(this, wxID_ANY, _("Note..."));
    auto* never = new wxButton(this, wxID_ANY, _("Never Use This File"));
    auto* undo = new wxButton(this, wxID_ANY, _("Not Decided"));
    for (wxButton* b : { yes, no, change, why, note, never, undo }) {
        actions->Add(b, 0, wxRIGHT, 4);
    }
    sizer->Add(actions, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

    auto* box = new wxStaticBoxSizer(wxVERTICAL, this, _("What you checked in xLights"));
    const wxString steps[5] = { _("I opened the sequence in xLights"), _("I looked at the visual composition"), _("I looked at the main screens"),
                                _("I checked the transitions and the movements"), _("I finished the review") };
    auto* grid = new wxGridSizer(3, 4, 12);
    for (int i = 0; i < 5; ++i) {
        _steps[i] = new wxCheckBox(box->GetStaticBox(), wxID_ANY, steps[i]);
        grid->Add(_steps[i], 0);
        _steps[i]->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
            ReviewChecklist k;
            k.opened = _steps[0]->GetValue();
            k.composition = _steps[1]->GetValue();
            k.surfaces = _steps[2]->GetValue();
            k.motion = _steps[3]->GetValue();
            k.finished = _steps[4]->GetValue();
            SetChecklist(k);
        });
    }
    box->Add(grid, 0, wxALL, 4);
    auto* approveRow = new wxBoxSizer(wxHORIZONTAL);
    approveRow->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, _("Reviewed by:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    _reviewer = new wxTextCtrl(box->GetStaticBox(), wxID_ANY, U(_review.reviewer.empty() ? S(wxGetUserName()) : _review.reviewer), wxDefaultPosition, wxSize(200, -1));
    approveRow->Add(_reviewer, 0, wxRIGHT, 8);
    _approve = new wxButton(box->GetStaticBox(), wxID_ANY, _("Approve the Show"));
    approveRow->Add(_approve, 0, wxRIGHT, 8);
    _state = new wxStaticText(box->GetStaticBox(), wxID_ANY, "");
    approveRow->Add(_state, 1, wxALIGN_CENTER_VERTICAL);
    box->Add(approveRow, 0, wxEXPAND | wxALL, 4);
    sizer->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* apply = new wxButton(this, wxID_ANY, _("Apply the Corrections and Make the Show Again"));
    buttons->Add(apply, 0, wxRIGHT, 12);
    buttons->AddStretchSpacer();
    buttons->Add(new wxButton(this, wxID_OK, _("Save the Review")), 0, wxRIGHT, 4);
    buttons->Add(new wxButton(this, wxID_CANCEL, _("Cancel")), 0);
    sizer->Add(buttons, 0, wxEXPAND | wxALL, 8);
    SetSizer(sizer);

    _order->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { SetOrderByScreen(_order->GetSelection() == 1); });
    _list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) { ShowState(); });
    const auto decide = [this](ClipState s) {
        const std::string id = Selected();
        if (!id.empty()) {
            (void)Decide(id, s);
        }
    };
    yes->Bind(wxEVT_BUTTON, [decide](wxCommandEvent&) { decide(ClipState::Approved); });
    no->Bind(wxEVT_BUTTON, [decide](wxCommandEvent&) { decide(ClipState::Rejected); });
    undo->Bind(wxEVT_BUTTON, [decide](wxCommandEvent&) { decide(ClipState::Pending); });
    change->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const std::string id = Selected();
        if (id.empty()) {
            return;
        }
        MediaChooserDialog chooser(this, _showDir, _library);
        if (chooser.ShowModal() == wxID_OK && !chooser.Chosen().empty()) {
            (void)Replace(id, chooser.Chosen());
        }
    });
    why->Bind(wxEVT_BUTTON, [this, why](wxCommandEvent&) {
        if (Selected().empty()) {
            return;
        }
        wxMenu menu;
        const std::vector<std::string>& flags = ClipFlags();
        for (size_t i = 0; i < flags.size(); ++i) {
            menu.Append(static_cast<int>(wxID_HIGHEST + 100 + i), ClipFlagName(flags[i]));
        }
        menu.Bind(wxEVT_MENU, [this, flags](wxCommandEvent& e) {
            const size_t i = static_cast<size_t>(e.GetId() - wxID_HIGHEST - 100);
            if (i < flags.size()) {
                Flag(flags[i]);
            }
        });
        PopupMenu(&menu, why->GetPosition() + wxPoint(0, why->GetSize().y));
    });
    note->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const std::string id = Selected();
        const ClipReview* c = id.empty() ? nullptr : _review.Find(id);
        if (c == nullptr) {
            return;
        }
        wxTextEntryDialog d(this, _("Your note about this shot:"), _("Note"), U(c->note));
        if (d.ShowModal() == wxID_OK) {
            (void)Decide(id, c->state, c->flags, S(d.GetValue()));
        }
    });
    never->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const std::string id = Selected();
        ClipReview* c = id.empty() ? nullptr : _review.Find(id);
        if (c == nullptr) {
            return;
        }
        c->neverUse = true;
        if (!Has(_neverUse, c->mediaId)) {
            _neverUse.push_back(c->mediaId);
        }
        (void)Decide(id, ClipState::Rejected, c->flags, c->note);
    });
    _approve->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        std::string why;
        if (!Approve(S(_reviewer->GetValue().Trim()), why)) {
            wxMessageBox(why == "checklist-incomplete"   ? _("Tick every step first: the show is only approved once you have watched it in xLights.")
                         : why == "no-reviewer"          ? _("Write who reviewed it.")
                         : why == "corrections-not-applied" ? _("Some shots are rejected or wait for another file: apply the corrections and review the new version.")
                                                         : U(why),
                         _("Approve the Show"), wxOK | wxICON_INFORMATION, this);
        }
    });
    apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        _apply = true;
        EndModal(wxID_OK);
    });
    const ReviewChecklist& k = _review.checklist;
    const bool done[5] = { k.opened, k.composition, k.surfaces, k.motion, k.finished };
    for (int i = 0; i < 5; ++i) {
        _steps[i]->SetValue(done[i]);
    }
    Fill();
}

bool ReviewDialog::Decide(const std::string& id, ClipState state, const std::vector<std::string>& flags, const std::string& note) {
    if (!DecideClip(_review, id, state, flags, note, Now())) {
        return false;
    }
    Fill();
    return true;
}

bool ReviewDialog::Replace(const std::string& id, const std::string& mediaId) {
    const MediaItem* m = _library.Find(mediaId);
    if (m == nullptr || !ReplaceClipMedia(_review, id, m->id, m->relative.empty() ? m->path : m->relative, Now())) {
        return false;
    }
    Fill();
    return true;
}

void ReviewDialog::Flag(const std::string& flag) {
    const std::string id = Selected();
    const ClipReview* c = id.empty() ? nullptr : _review.Find(id);
    if (c == nullptr) {
        return;
    }
    std::vector<std::string> flags = c->flags;
    if (!Has(flags, flag)) {
        flags.push_back(flag);
    }
    (void)Decide(id, c->state == ClipState::Approved || c->state == ClipState::Pending ? ClipState::Rejected : c->state, flags, c->note);
}

void ReviewDialog::SetChecklist(const ReviewChecklist& k) {
    _review.checklist = k;
    if (_review.state == "approved" && !k.Complete()) {
        _review.state = "in-progress";
        _review.approvedAt.clear();
    } else if (_review.state == "pending" && (k.opened || k.composition || k.surfaces || k.motion || k.finished)) {
        _review.state = "in-progress";
    }
    const bool done[5] = { k.opened, k.composition, k.surfaces, k.motion, k.finished };
    for (int i = 0; i < 5; ++i) {
        if (_steps[i] != nullptr && _steps[i]->GetValue() != done[i]) {
            _steps[i]->SetValue(done[i]);
        }
    }
    Fill();
}

bool ReviewDialog::Approve(const std::string& reviewer, std::string& why) {
    const bool ok = ApproveShow(_review, reviewer, Now(), why);
    Fill();
    return ok;
}

void ReviewDialog::SetOrderByScreen(bool byScreen) {
    _byScreen = byScreen;
    if (_order->GetSelection() != (byScreen ? 1 : 0)) {
        _order->SetSelection(byScreen ? 1 : 0);
    }
    Fill();
}

std::vector<std::string> ReviewDialog::RowOrder() const {
    return _rows;
}

std::string ReviewDialog::Selected() const {
    const long i = _list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    return i >= 0 && i < static_cast<long>(_rows.size()) ? _rows[static_cast<size_t>(i)] : std::string();
}

void ReviewDialog::Fill() {
    const std::string keep = Selected();
    std::vector<const ClipReview*> clips;
    for (const ClipReview& c : _review.clips) {
        clips.push_back(&c);
    }
    std::stable_sort(clips.begin(), clips.end(), [&](const ClipReview* a, const ClipReview* b) {
        if (_byScreen && a->lane != b->lane) {
            return a->lane < b->lane;
        }
        return a->startMS < b->startMS;
    });
    _list->DeleteAllItems();
    _rows.clear();
    long row = 0;
    int files = 0;
    std::vector<std::string> seenFiles;
    for (const ClipReview* c : clips) {
        if (!Has(seenFiles, c->mediaId)) {
            seenFiles.push_back(c->mediaId);
            ++files;
        }
        wxString shows;
        for (size_t i = 0; i < c->seen.size() && i < 2; ++i) {
            shows += (shows.empty() ? "" : ", ") + VisualConceptName(c->seen[i]);
        }
        wxString inOut = TransitionName(c->transition.empty() ? "fade" : c->transition);
        inOut += " / " + (c->transitionOut.empty() ? _("cut") : _("fade"));
        wxString state = ClipStateName(c->state);
        for (const std::string& f : c->flags) {
            state += ", " + ClipFlagName(f);
        }
        if (!c->replacementId.empty()) {
            const MediaItem* m = _library.Find(c->replacementId);
            state += " -> " + (m != nullptr ? U(m->name) : U(c->replacementId));
        }
        _list->InsertItem(row, U(c->lane));
        _list->SetItem(row, 1, Clock(c->startMS));
        _list->SetItem(row, 2, Clock(c->endMS));
        _list->SetItem(row, 3, U(c->name));
        _list->SetItem(row, 4, KindText(c->kind));
        _list->SetItem(row, 5, shows.empty() ? _("not known") : shows);
        _list->SetItem(row, 6, WhyText(c->why, false));
        _list->SetItem(row, 7, c->confidence >= 0 ? wxString::Format("%.0f%%", c->confidence * 100) : wxString("-"));
        _list->SetItem(row, 8, FramingName(c->framing));
        _list->SetItem(row, 9, inOut);
        _list->SetItem(row, 10, state);
        if (c->state == ClipState::Rejected || c->state == ClipState::NeedsFix) {
            _list->SetItemTextColour(row, wxColour(200, 60, 60));
        } else if (c->state == ClipState::Approved) {
            _list->SetItemTextColour(row, wxColour(40, 150, 60));
        }
        if (c->decisionId == keep) {
            _list->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        }
        _rows.push_back(c->decisionId);
        ++row;
    }
    const ReviewCounts n = CountReview(_review);
    wxString screens;
    for (const std::string& s : _context.screens) {
        screens += (screens.empty() ? "" : ", ") + U(s);
    }
    wxString moves;
    for (const auto& [k, v] : _context.transitions) {
        moves += (moves.empty() ? "" : ", ") + wxString::Format("%s %d", TransitionName(k), v);
    }
    wxString text = wxString::Format(_("%s - %s, %s long. Screens: %s. %d shots from %d files; transitions: %s."), U(_context.sequence), U(_context.song), Clock(_context.durationMS),
                                     screens.empty() ? _("none") : screens, n.clips, files, moves.empty() ? _("none") : moves);
    text += "\n" + wxString::Format(_("To see: %d pending, %d approved, %d rejected, %d waiting for a fix."), n.pending, n.approved, n.rejected, n.needsFix);
    for (const wxString& w : _context.warnings) {
        text += "\n" + _("Needs your attention: ") + w;
    }
    _summary->SetLabel(text);
    _summary->Wrap(1180);
    ShowState();
    Layout();
}

void ReviewDialog::ShowState() {
    const std::string id = Selected();
    const ClipReview* c = id.empty() ? nullptr : _review.Find(id);
    _detail->SetLabel(c == nullptr ? _("Choose a shot to decide about it. Watch it in xLights first: what is written here is only what the program decided.")
                                   : wxString::Format(_("%s on %s, %s to %s. Why: %s%s"), U(c->name), U(c->lane), Clock(c->startMS), Clock(c->endMS), WhyText(c->why, false), c->note.empty() ? wxString() : "\n" + _("Your note: ") + U(c->note)));
    _detail->Wrap(1180);
    const bool approved = _review.state == "approved";
    _approve->Enable(!approved);
    _state->SetLabel(approved ? wxString::Format(_("Approved by %s, %s."), U(_review.reviewer), U(_review.approvedAt))
                     : _review.checklist.Complete() ? _("Every step checked: you can approve the show.")
                                                    : _("Pending visual review: the show is only approved by you, after watching it in xLights."));
}

// ------------------------------------------------------------------ labels

MediaLabelsDialog::MediaLabelsDialog(wxWindow* parent, const MediaItem& item, const std::string& showDir, const VisualConcepts& concepts) :
    wxDialog(parent, wxID_ANY, _("What This File Shows"), wxDefaultPosition, wxSize(760, 640), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    _item(item),
    _concepts(concepts),
    _labels(item.human) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* top = new wxBoxSizer(wxHORIZONTAL);
    wxImage image;
    wxBitmap bitmap(160, 120);
    if (!item.thumbnail.empty() && image.LoadFile(U(MediaAbsolutePath(item.thumbnail, showDir)), wxBITMAP_TYPE_PNG) && image.IsOk()) {
        const double k = std::min(160.0 / std::max(1, image.GetWidth()), 120.0 / std::max(1, image.GetHeight()));
        image.Rescale(std::max(1, static_cast<int>(image.GetWidth() * k)), std::max(1, static_cast<int>(image.GetHeight() * k)), wxIMAGE_QUALITY_HIGH);
        bitmap = wxBitmap(image);
    }
    top->Add(new wxStaticBitmap(this, wxID_ANY, bitmap), 0, wxRIGHT, 8);
    wxString info = U(item.name) + "\n" + U(item.path) + "\n" + wxString::Format("%dx%d", item.width, item.height);
    info += "\n" + (item.analysis.mode == "semantic" ? _("Looked at with the image model.") : item.analysis.Done() ? _("Measured without the image model: only colours and light.") : _("Not analysed yet."));
    top->Add(new wxStaticText(this, wxID_ANY, info), 1);
    sizer->Add(top, 0, wxEXPAND | wxALL, 8);
    sizer->Add(new wxStaticText(this, wxID_ANY, _("The program proposes; you correct. Your word counts over the program's, which is kept to compare.")), 0, wxLEFT | wxRIGHT, 8);
    _list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 220), wxLC_REPORT | wxLC_SINGLE_SEL);
    for (const auto& [label, width] : std::vector<std::pair<wxString, int>>{ { _("What it shows"), 220 }, { _("The program said"), 170 }, { _("You say"), 200 } }) {
        _list->AppendColumn(label, wxLIST_FORMAT_LEFT, width);
    }
    sizer->Add(_list, 1, wxEXPAND | wxALL, 8);
    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    auto* yes = new wxButton(this, wxID_ANY, _("It Is There"));
    auto* no = new wxButton(this, wxID_ANY, _("It Is Not"));
    auto* never = new wxButton(this, wxID_ANY, _("Never for This File"));
    auto* clear = new wxButton(this, wxID_ANY, _("Not Said"));
    auto* add = new wxButton(this, wxID_ANY, _("Add What Is Missing..."));
    for (wxButton* b : { yes, no, never, clear, add }) {
        actions->Add(b, 0, wxRIGHT, 4);
    }
    sizer->Add(actions, 0, wxLEFT | wxRIGHT, 8);
    _scores = new wxCheckBox(this, wxID_ANY, _("Show the program's scores (advanced)"));
    sizer->Add(_scores, 0, wxALL, 8);
    sizer->Add(new wxStaticText(this, wxID_ANY, _("Never use it for:")), 0, wxLEFT | wxRIGHT, 8);
    wxArrayString uses;
    for (const wxString& u : { _("the matrices"), _("the trees"), _("calm parts"), _("intense parts"), _("the opening"), _("the ending") }) {
        uses.Add(u);
    }
    _uses = new wxCheckListBox(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 110), uses);
    for (size_t i = 0; i < MediaUses().size(); ++i) {
        _uses->Check(static_cast<unsigned>(i), Has(_labels.notFor, MediaUses()[i]));
    }
    sizer->Add(_uses, 0, wxEXPAND | wxALL, 8);
    auto* doneRow = new wxBoxSizer(wxHORIZONTAL);
    _complete = new wxCheckBox(this, wxID_ANY, _("I went through everything it shows"));
    _complete->SetValue(_labels.complete);
    doneRow->Add(_complete, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    doneRow->Add(new wxStaticText(this, wxID_ANY, _("By:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    _reviewer = new wxTextCtrl(this, wxID_ANY, U(_labels.reviewer.empty() ? S(wxGetUserName()) : _labels.reviewer), wxDefaultPosition, wxSize(180, -1));
    doneRow->Add(_reviewer, 0);
    sizer->Add(doneRow, 0, wxALL, 8);
    sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
    SetSizer(sizer);

    const auto set = [this](const std::string& d) {
        const std::string c = Selected();
        if (!c.empty()) {
            SetDecision(c, d);
        }
    };
    yes->Bind(wxEVT_BUTTON, [set](wxCommandEvent&) { set("yes"); });
    no->Bind(wxEVT_BUTTON, [set](wxCommandEvent&) { set("no"); });
    never->Bind(wxEVT_BUTTON, [set](wxCommandEvent&) { set("never"); });
    clear->Bind(wxEVT_BUTTON, [set](wxCommandEvent&) { set(""); });
    add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxArrayString names;
        std::vector<std::string> ids;
        for (const VisualConceptDef& d : _concepts.concepts) {
            if (!Has(_rows, d.id)) {
                names.Add(VisualConceptName(d.id));
                ids.push_back(d.id);
            }
        }
        wxSingleChoiceDialog pick(this, _("What does it show that the program missed?"), _("Add What Is Missing"), names);
        if (!ids.empty() && pick.ShowModal() == wxID_OK) {
            AddConcept(ids[static_cast<size_t>(pick.GetSelection())]);
        }
    });
    _scores->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { Fill(); });
    _uses->Bind(wxEVT_CHECKLISTBOX, [this](wxCommandEvent&) {
        std::vector<std::string> u;
        for (size_t i = 0; i < MediaUses().size(); ++i) {
            if (_uses->IsChecked(static_cast<unsigned>(i))) {
                u.push_back(MediaUses()[i]);
            }
        }
        _labels.notFor = u;
    });
    Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
        SetComplete(_complete->GetValue(), S(_reviewer->GetValue().Trim()));
        e.Skip();
    }, wxID_OK);
    Fill();
}

void MediaLabelsDialog::SetDecision(const std::string& c, const std::string& d) {
    for (std::vector<std::string>* l : { &_labels.confirmed, &_labels.rejected, &_labels.excluded }) {
        std::erase(*l, c);
    }
    if (d == "yes") {
        if (!Has(_labels.added, c) && std::none_of(_item.analysis.concepts.begin(), _item.analysis.concepts.end(), [&](const MediaConcept& m) { return m.id == c; })) {
            _labels.added.push_back(c);
        } else if (!Has(_labels.added, c)) {
            _labels.confirmed.push_back(c);
        }
    } else {
        std::erase(_labels.added, c);
        if (d == "no") {
            _labels.rejected.push_back(c);
        } else if (d == "never") {
            _labels.excluded.push_back(c);
        }
    }
    Fill();
}

void MediaLabelsDialog::AddConcept(const std::string& c) {
    SetDecision(c, "yes");
}

void MediaLabelsDialog::SetNotFor(const std::vector<std::string>& uses) {
    _labels.notFor = uses;
    for (size_t i = 0; i < MediaUses().size(); ++i) {
        _uses->Check(static_cast<unsigned>(i), Has(uses, MediaUses()[i]));
    }
}

void MediaLabelsDialog::SetComplete(bool complete, const std::string& reviewer) {
    _labels.complete = complete;
    _labels.reviewer = reviewer;
    _labels.reviewedAt = Now();
    _complete->SetValue(complete);
}

std::string MediaLabelsDialog::Selected() const {
    const long i = _list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    return i >= 0 && i < static_cast<long>(_rows.size()) ? _rows[static_cast<size_t>(i)] : std::string();
}

void MediaLabelsDialog::Fill() {
    const std::string keep = Selected();
    _rows.clear();
    _list->DeleteAllItems();
    long row = 0;
    const auto line = [&](const std::string& id, const MediaConcept* m) {
        _list->InsertItem(row, VisualConceptName(id));
        wxString said = m == nullptr ? _("did not see it")
                        : m->level == "strong" ? _("clearly")
                        : m->level == "probable" ? _("probably")
                                                 : _("doubtfully");
        if (m != nullptr && _scores->GetValue()) {
            said += wxString::Format(" (%.3f)", m->score);
        }
        _list->SetItem(row, 1, said);
        const wxString you = Has(_labels.confirmed, id) || Has(_labels.added, id) ? _("It is there") : Has(_labels.rejected, id) ? _("It is not") : Has(_labels.excluded, id) ? _("Never for this file") : wxString("-");
        _list->SetItem(row, 2, you);
        if (id == keep) {
            _list->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        }
        _rows.push_back(id);
        ++row;
    };
    for (const MediaConcept& c : _item.analysis.concepts) {
        line(c.id, &c);
    }
    for (const std::string& a : _labels.added) {
        if (!Has(_rows, a)) {
            line(a, nullptr);
        }
    }
}

// ------------------------------------------------------------------ preferences

PreferencesDialog::PreferencesDialog(wxWindow* parent, ArtisticPreferences prefs, const MediaLibrary* library) :
    wxDialog(parent, wxID_ANY, _("What Has Been Learned of Your Taste"), wxDefaultPosition, wxSize(900, 600), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    _prefs(std::move(prefs)),
    _library(library) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(new wxStaticText(this, wxID_ANY, _("Learned from your reviews, on this computer. Nothing counts until it comes from the reviews of two shows; you can change, turn off or forget anything.")), 0, wxALL, 8);
    _learning = new wxCheckBox(this, wxID_ANY, _("Use what has been learned when composing"));
    _learning->SetValue(_prefs.learning);
    sizer->Add(_learning, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    _sentences = new wxStaticText(this, wxID_ANY, "");
    sizer->Add(_sentences, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    _list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 300), wxLC_REPORT | wxLC_SINGLE_SEL);
    for (const auto& [label, width] : std::vector<std::pair<wxString, int>>{ { _("What"), 300 }, { _("Value"), 120 }, { _("Backed by"), 110 }, { _("From"), 200 }, { _("In use"), 80 } }) {
        _list->AppendColumn(label, wxLIST_FORMAT_LEFT, width);
    }
    sizer->Add(_list, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    auto* change = new wxButton(this, wxID_ANY, _("Change the Value..."));
    auto* toggle = new wxButton(this, wxID_ANY, _("Turn On or Off"));
    auto* reset = new wxButton(this, wxID_ANY, _("Forget It"));
    auto* amount = new wxButton(this, wxID_ANY, _("More or Fewer Pictures..."));
    auto* all = new wxButton(this, wxID_ANY, _("Forget Everything"));
    for (wxButton* b : { change, toggle, reset, amount, all }) {
        actions->Add(b, 0, wxRIGHT, 4);
    }
    sizer->Add(actions, 0, wxALL, 8);
    sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
    SetSizer(sizer);

    _learning->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { SetLearning(_learning->GetValue()); });
    const auto ask = [this](const std::string& key) {
        double now = 0;
        for (const Preference& p : ComputePreferences(_prefs)) {
            now = p.key == key ? (p.source == "person" ? p.value : p.learned) : now;
        }
        const long v = wxGetNumberFromUser(_("From -100 (much less) to 100 (much more); 0 is neutral."), PreferenceName(key, _library), _("Change the Value"), std::lround(now * 100), -100, 100, this);
        if (v != -1) { // -1 is also what a cancelled box gives: -1% is not worth telling apart
            SetValue(key, v / 100.0);
        }
    };
    change->Bind(wxEVT_BUTTON, [this, ask](wxCommandEvent&) {
        if (!Selected().empty()) {
            ask(Selected());
        }
    });
    amount->Bind(wxEVT_BUTTON, [ask](wxCommandEvent&) { ask("amount"); });
    toggle->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const std::string key = Selected();
        for (const Preference& p : ComputePreferences(_prefs)) {
            if (p.key == key) {
                Enable(key, !p.enabled);
            }
        }
    });
    reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!Selected().empty()) {
            Reset(Selected());
        }
    });
    all->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (wxMessageBox(_("Forget everything learned and every value you set?"), _("Forget Everything"), wxYES_NO | wxICON_QUESTION, this) == wxYES) {
            ResetAll();
        }
    });
    Fill();
}

void PreferencesDialog::SetValue(const std::string& key, double value) {
    SetPreference(_prefs, key, value);
    Fill();
}

void PreferencesDialog::Enable(const std::string& key, bool enabled) {
    EnablePreference(_prefs, key, enabled);
    Fill();
}

void PreferencesDialog::Reset(const std::string& key) {
    ResetPreference(_prefs, key);
    Fill();
}

void PreferencesDialog::ResetAll() {
    ResetAllPreferences(_prefs);
    Fill();
}

void PreferencesDialog::SetLearning(bool on) {
    _prefs.learning = on;
    _learning->SetValue(on);
    Fill();
}

std::string PreferencesDialog::Selected() const {
    const long i = _list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    return i >= 0 && i < static_cast<long>(_rows.size()) ? _rows[static_cast<size_t>(i)] : std::string();
}

std::vector<wxString> PreferencesDialog::Sentences() const {
    std::vector<wxString> out;
    for (const Preference& p : ComputePreferences(_prefs)) {
        if (!p.active || p.source != "learned") {
            continue;
        }
        if (p.key == "light.dark" && p.value < 0) {
            out.push_back(wxString::Format(_("You have turned down dark pictures and videos several times (%d decisions)."), p.decisions));
        } else if (p.key.find("motion.moving") != std::string::npos) {
            out.push_back(wxString::Format(p.value > 0 ? _("You seem to like slow camera movements: %s (%d decisions).") : _("You seem to prefer still pictures: %s (%d decisions)."), PreferenceName(p.key, _library), p.decisions));
        } else if (p.key.rfind("screen.", 0) == 0 && p.key.find(".presence") != std::string::npos) {
            out.push_back(wxString::Format(p.value > 0 ? _("You often approve pictures on a screen: %s (%d decisions).") : _("You find fewer pictures fit a screen: %s (%d decisions)."), PreferenceName(p.key, _library), p.decisions));
        } else {
            out.push_back(wxString::Format(_("%s: %s (%d decisions)."), PreferenceName(p.key, _library), PreferenceValueText(p.value), p.decisions));
        }
    }
    return out;
}

void PreferencesDialog::Fill() {
    const std::string keep = Selected();
    _rows.clear();
    _list->DeleteAllItems();
    long row = 0;
    for (const Preference& p : ComputePreferences(_prefs)) {
        _list->InsertItem(row, PreferenceName(p.key, _library));
        _list->SetItem(row, 1, p.source == "person" ? PreferenceValueText(p.value) : PreferenceValueText(p.learned));
        _list->SetItem(row, 2, wxString::Format(_("decisions: %d, shows: %d"), p.decisions, p.reviews));
        _list->SetItem(row, 3, p.source == "person" ? _("your value") : p.source == "learned" ? _("learned") : _("learned, not yet enough to count"));
        _list->SetItem(row, 4, !p.enabled ? _("off") : p.active ? _("yes") : _("not yet"));
        if (p.key == keep) {
            _list->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        }
        _rows.push_back(p.key);
        ++row;
    }
    wxString text;
    for (const wxString& s : Sentences()) {
        text += (text.empty() ? "" : "\n") + s;
    }
    if (!_prefs.learning) {
        text = _("Learning is off: shows are composed with only the values you set.") + (text.empty() ? wxString() : "\n" + text);
    }
    _sentences->SetLabel(text.empty() ? _("Nothing learned yet: review the pictures and videos of a show to teach the program your taste.") : text);
    _sentences->Wrap(860);
    Layout();
}

// ------------------------------------------------------------------ evaluation

wxString CriterionName(const std::string& c) {
    static const std::map<std::string, wxString> NAMES = { { "musicality", _("Follows the music") }, { "variety", _("Variety") }, { "complexity", _("Richness") },
                                                           { "coordination", _("The house moves together") }, { "colour", _("Colour") }, { "direction", _("Artistic direction") },
                                                           { "continuity", _("Flows without jumps") }, { "personality", _("Personality") }, { "multimedia", _("Pictures and videos") } };
    const auto it = NAMES.find(c);
    return it != NAMES.end() ? it->second : U(c);
}

wxString EvaluationTagName(const std::string& t) {
    static const std::map<std::string, wxString> NAMES = { { "liked", _("I liked it") }, { "monotonous", _("Monotonous") }, { "too-busy", _("Too busy") }, { "too-calm", _("Too calm") },
                                                           { "too-christmas", _("Too Christmas-coloured") }, { "not-christmas", _("Not Christmas enough") } };
    const auto it = NAMES.find(t);
    return it != NAMES.end() ? it->second : U(t);
}

wxString EvaluationRuleName(const std::string& r) {
    static const std::map<std::string, wxString> NAMES = { { "more-variety", _("Change the looks more from scene to scene") }, { "less-variety", _("Keep an idea longer") },
                                                           { "more-christmas", _("Use more Christmas colours") }, { "more-song-colours", _("Use more of the song's own colours") },
                                                           { "keep-recipes", _("Mark this show's recipes as favourites") } };
    const auto it = NAMES.find(r);
    return it != NAMES.end() ? it->second : U(r);
}

EvaluationDialog::EvaluationDialog(wxWindow* parent, ShowEvaluation evaluation, const std::map<std::string, double>& measured) :
    wxDialog(parent, wxID_ANY, _("Evaluate the Show"), wxDefaultPosition, wxSize(820, 760), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    _measured(measured),
    _ruleIds(EvaluationRules()) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(new wxStaticText(this, wxID_ANY, _("How did the show turn out? Only what you saw in xLights counts. The measures below are the program's, not a judgement.")), 0, wxALL, 8);
    auto* who = new wxBoxSizer(wxHORIZONTAL);
    who->Add(new wxStaticText(this, wxID_ANY, _("Who evaluates:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    _reviewer = new wxTextCtrl(this, wxID_ANY);
    who->Add(_reviewer, 1);
    sizer->Add(who, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    auto* score = new wxBoxSizer(wxHORIZONTAL);
    score->Add(new wxStaticText(this, wxID_ANY, _("Your score:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    _overall = new wxSlider(this, wxID_ANY, 0, 0, 100, wxDefaultPosition, wxSize(300, -1));
    score->Add(_overall, 0, wxRIGHT, 8);
    _overallText = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxSize(120, -1));
    score->Add(_overallText, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(score, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    _quick = new wxCheckBox(this, wxID_ANY, _("Quick evaluation: only the score and a few words"));
    sizer->Add(_quick, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    auto* grid = new wxFlexGridSizer(2, 4, 12);
    wxArrayString levels;
    for (const wxString& l : { _("Not rated"), _("1 - Poor"), wxString("2"), wxString("3"), wxString("4"), _("5 - Very good") }) {
        levels.Add(l);
    }
    for (const std::string& c : EvaluationCriteria()) {
        grid->Add(new wxStaticText(this, wxID_ANY, CriterionName(c)), 0, wxALIGN_CENTER_VERTICAL);
        auto* choice = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, levels);
        choice->SetSelection(0);
        grid->Add(choice, 0);
        _ratings[c] = choice;
    }
    sizer->Add(grid, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    sizer->Add(new wxStaticText(this, wxID_ANY, _("In a few words:")), 0, wxLEFT | wxRIGHT, 8);
    wxArrayString tags;
    for (const std::string& t : EvaluationTags()) {
        tags.Add(EvaluationTagName(t));
    }
    _tags = new wxCheckListBox(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 100), tags);
    sizer->Add(_tags, 0, wxEXPAND | wxALL, 8);
    sizer->Add(new wxStaticText(this, wxID_ANY, _("Notes:")), 0, wxLEFT | wxRIGHT, 8);
    _notes = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 60), wxTE_MULTILINE);
    sizer->Add(_notes, 0, wxEXPAND | wxALL, 8);
    sizer->Add(new wxStaticText(this, wxID_ANY, _("What should the next shows take from this one? Only what you tick becomes a rule (marked: suggested by your answers).")), 0, wxLEFT | wxRIGHT, 8);
    _rules = new wxCheckListBox(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 100));
    sizer->Add(_rules, 0, wxEXPAND | wxALL, 8);
    wxString measures;
    for (const auto& [k, v] : _measured) {
        measures += wxString::Format("%s: %.2f   ", U(k), v);
    }
    auto* m = new wxTextCtrl(this, wxID_ANY, measures.empty() ? _("No measures for this show.") : measures, wxDefaultPosition, wxSize(-1, 50), wxTE_MULTILINE | wxTE_READONLY);
    sizer->Add(m, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    sizer->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
    SetSizer(sizer);

    const auto refresh = [this]() {
        _overallText->SetLabel(_overall->GetValue() == 0 ? _("not given") : wxString::Format("%d%%", _overall->GetValue()));
        for (auto& [c, choice] : _ratings) {
            choice->Enable(!_quick->GetValue());
        }
        Suggest();
    };
    _overall->Bind(wxEVT_SLIDER, [refresh](wxCommandEvent&) { refresh(); });
    _quick->Bind(wxEVT_CHECKBOX, [refresh](wxCommandEvent&) { refresh(); });
    _tags->Bind(wxEVT_CHECKLISTBOX, [refresh](wxCommandEvent&) { refresh(); });
    for (auto& [c, choice] : _ratings) {
        choice->Bind(wxEVT_CHOICE, [refresh](wxCommandEvent&) { refresh(); });
    }
    Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
        VisualReview probe;
        std::string why;
        if (!Accept(probe, why)) {
            wxMessageBox(why == "no-reviewer" ? _("Say who evaluates the show.") : why == "rules-contradict" ? _("Two of the rules you ticked contradict each other.") : _("The evaluation is not complete."),
                         _("Evaluate the Show"), wxOK | wxICON_INFORMATION, this);
            return;
        }
        e.Skip();
    }, wxID_OK);
    Set(evaluation);
}

void EvaluationDialog::Suggest() {
    const ShowEvaluation now = Evaluation();
    const std::vector<std::string> suggested = SuggestRules(now);
    for (size_t i = 0; i < _ruleIds.size(); ++i) {
        const bool exists = i < _rules->GetCount();
        const bool ticked = exists && _rules->IsChecked(static_cast<unsigned>(i));
        const bool s = std::find(suggested.begin(), suggested.end(), _ruleIds[i]) != suggested.end();
        const wxString label = EvaluationRuleName(_ruleIds[i]) + (s ? wxString(" ") + _("(suggested)") : wxString());
        if (exists) {
            _rules->SetString(static_cast<unsigned>(i), label);
        } else {
            _rules->Append(label);
        }
        _rules->Check(static_cast<unsigned>(i), ticked);
    }
}

void EvaluationDialog::Set(const ShowEvaluation& e) {
    _reviewer->SetValue(U(e.reviewer));
    _overall->SetValue(e.overall);
    _quick->SetValue(e.quick);
    for (auto& [c, choice] : _ratings) {
        const auto it = e.ratings.find(c);
        choice->SetSelection(it == e.ratings.end() ? 0 : it->second);
        choice->Enable(!e.quick);
    }
    for (size_t i = 0; i < EvaluationTags().size(); ++i) {
        _tags->Check(static_cast<unsigned>(i), std::find(e.tags.begin(), e.tags.end(), EvaluationTags()[i]) != e.tags.end());
    }
    _notes->SetValue(U(e.notes));
    Suggest();
    for (size_t i = 0; i < _ruleIds.size(); ++i) {
        _rules->Check(static_cast<unsigned>(i), std::find(e.rules.begin(), e.rules.end(), _ruleIds[i]) != e.rules.end());
    }
    _overallText->SetLabel(e.overall == 0 ? _("not given") : wxString::Format("%d%%", e.overall));
}

ShowEvaluation EvaluationDialog::Evaluation() const {
    ShowEvaluation e;
    e.reviewer = std::string(wxString(_reviewer->GetValue()).Trim().ToUTF8());
    e.overall = _overall->GetValue();
    e.quick = _quick->GetValue();
    if (!e.quick) {
        for (const auto& [c, choice] : _ratings) {
            if (choice->GetSelection() > 0) {
                e.ratings[c] = choice->GetSelection();
            }
        }
    }
    for (size_t i = 0; i < EvaluationTags().size(); ++i) {
        if (_tags->IsChecked(static_cast<unsigned>(i))) {
            e.tags.push_back(EvaluationTags()[i]);
        }
    }
    e.notes = std::string(_notes->GetValue().ToUTF8());
    for (size_t i = 0; i < _ruleIds.size() && i < _rules->GetCount(); ++i) {
        if (_rules->IsChecked(static_cast<unsigned>(i))) {
            e.rules.push_back(_ruleIds[i]);
        }
    }
    e.measured = _measured;
    e.at = std::string(wxDateTime::Now().FormatISOCombined(' ').ToUTF8());
    return e;
}

bool EvaluationDialog::Accept(VisualReview& review, std::string& why) const {
    return EvaluateShow(review, Evaluation(), why);
}
