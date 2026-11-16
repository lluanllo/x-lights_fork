#include "ComposerProductionDialogs.h"

#include <wx/button.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <spdlog/fmt/fmt.h>

#include "ComposerUnderstandingDialogs.h"
#include "composer/UnderstandingStore.h"

namespace fs = std::filesystem;
using namespace composer;

namespace {

wxString U(const std::string& s) {
    return wxString::FromUTF8(s.c_str());
}

std::string S(const wxString& s) {
    return std::string(s.utf8_str());
}

wxString T(const std::string& coreText) {
    return wxGetTranslation(U(coreText));
}

ProductionTranslate Translator() {
    return [](const char* msgid) { return S(wxGetTranslation(wxString::FromUTF8(msgid))); };
}

wxStaticText* Muted(wxWindow* parent, const wxString& text, int wrap = 760) {
    auto* t = new wxStaticText(parent, wxID_ANY, text);
    t->SetForegroundColour(wxColour(110, 110, 110));
    t->Wrap(wrap);
    return t;
}

bool WriteAll(const std::string& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream out(fs::path(path), std::ios::binary | std::ios::trunc);
    out << text;
    return out.good();
}

} // namespace

wxString ProductionStatusText(const ProductionReport& r) {
    const auto mark = [](bool ok) { return ok ? wxString::FromUTF8("\xE2\x9C\x93 ") : wxString::FromUTF8("\xE2\x9C\x97 "); };
    wxString out;
    out += mark(r.status.technical) + _("Technical generation correct") + "\n";
    out += mark(r.status.rendered) + _("Render correct") + "\n";
    out += mark(r.status.mediaComplete) + _("Pictures and videos complete") + "\n";
    out += mark(r.status.roundTrip) + _("Saved again by xLights and the same") + "\n";
    out += (r.status.review == "approved" ? mark(true) : wxString("- ")) + _("Visual review") + ": " + T(ProductionReviewPhrase(r.status.review));
    if (r.status.review == "approved" && !r.status.reviewer.empty()) {
        out += " (" + U(r.status.reviewer) + ")";
    }
    return out;
}

bool KeepComposition(const std::string& showDir, const StageLibraryEntry& entry, std::string& error) {
    StageLibrary personal;
    std::string text;
    if (ReadTextFile(StageLibraryPath(showDir), text) && !FromJson(text, personal, error)) {
        return false;
    }
    std::erase_if(personal.entries, [&](const StageLibraryEntry& e) { return e.id == entry.id; });
    personal.entries.push_back(entry);
    if (!WriteAll(StageLibraryPath(showDir), ToJson(personal))) {
        error = "library-not-saved";
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ the check before it goes out

ProductionDialog::ProductionDialog(wxWindow* parent, const ProductionReport& report, const std::string& showDir) :
    wxDialog(parent, wxID_ANY, _("Prepare the Show"), wxDefaultPosition, wxSize(860, 660), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    _report(report),
    _showDir(showDir) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    wxString head;
    if (report.cancelled) {
        head = _("The check was stopped before the end: check the show again before it goes out.");
    } else if (report.Count("blocker") > 0) {
        head = wxString::Format(_("%d things stop this show from going out. Correct them and check it again."), report.Count("blocker"));
    } else if (report.Count("risk") > 0) {
        head = wxString::Format(_("The show can go out, but %d things may make it look wrong. Look at them first."), report.Count("risk"));
    } else {
        head = _("Nothing stops this show from going out. It still has to be seen in xLights before the lights go on.");
    }
    auto* title = new wxStaticText(this, wxID_ANY, head);
    wxFont font = title->GetFont();
    font.MakeBold();
    title->SetFont(font);
    title->Wrap(820);
    sizer->Add(title, 0, wxALL, 8);
    sizer->Add(new wxStaticText(this, wxID_ANY, ProductionStatusText(report)), 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    _text = new wxTextCtrl(this, wxID_ANY, U(ProductionText(report, Translator())), wxDefaultPosition, wxSize(-1, 360), wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
    sizer->Add(_text, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
    sizer->Add(Muted(this, _("The check opens the show without switching any controller on and changes none of its files. Nothing here approves the show: only you do, after seeing it in xLights.")),
               0, wxALL, 8);
    _packageState = new wxStaticText(this, wxID_ANY, "");
    sizer->Add(_packageState, 0, wxLEFT | wxRIGHT, 8);
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* package = new wxButton(this, wxID_ANY, _("Prepare the Production Folder"));
    auto* open = new wxButton(this, wxID_ANY, _("Open in xLights"));
    buttons->Add(package, 0, wxRIGHT, 4);
    buttons->Add(open, 0, wxRIGHT, 4);
    buttons->AddStretchSpacer();
    buttons->Add(new wxButton(this, wxID_CANCEL, _("Close")), 0);
    sizer->Add(buttons, 0, wxEXPAND | wxALL, 8);
    SetSizer(sizer);
    package->Enable(report.ran && !report.cancelled);
    package->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const std::string problem = MakePackage();
        if (!problem.empty()) {
            wxMessageBox(U(problem), _("Prepare the Show"), wxOK | wxICON_WARNING, this);
        }
    });
    open->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (openInXLights) {
            openInXLights();
        }
    });
}

std::string ProductionDialog::MakePackage() {
    ProductionPackageInput in;
    in.showDir = _showDir;
    in.sequencePath = _report.sequence;
    in.report = &_report;
    _package = BuildProductionPackage(in);
    if (!_package.ok && _package.folder.empty()) {
        return _package.problems.empty() ? std::string("package-not-made") : _package.problems.front();
    }
    _packageState->SetLabel(wxString::Format(_("Production folder ready: %s (%d files copied, %d already where the show is)."), U(_package.folder), static_cast<int>(_package.copied.size()),
                                             static_cast<int>(_package.referred.size())));
    _packageState->Wrap(820);
    Layout();
    return _package.ok ? std::string() : _package.problems.front();
}

wxString ProductionDialog::Text() const {
    return _text->GetValue();
}

// ------------------------------------------------------------------ the song's visual ideas

MotifsDialog::MotifsDialog(wxWindow* parent, const GlobalStageScore& score, const std::string& showDir, const std::string& song) :
    wxDialog(parent, wxID_ANY, _("Visual Ideas and How They Develop"), wxDefaultPosition, wxSize(900, 640), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    _score(score),
    _showDir(showDir),
    _song(song) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(Muted(this, _("A visual idea is who carries it, with which gesture, from where and in what order. The song presents it, develops it, transforms it, brings it back and closes it. Choose an idea to see every scene that uses it.")),
               0, wxALL, 8);
    auto* body = new wxBoxSizer(wxHORIZONTAL);
    _motifs = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(300, 220), wxLC_REPORT | wxLC_SINGLE_SEL);
    _motifs->AppendColumn(_("Idea"), wxLIST_FORMAT_LEFT, 50);
    _motifs->AppendColumn(_("What it says"), wxLIST_FORMAT_LEFT, 180);
    _motifs->AppendColumn(_("Scenes"), wxLIST_FORMAT_LEFT, 60);
    body->Add(_motifs, 0, wxEXPAND | wxRIGHT, 8);
    _uses = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(540, 220), wxLC_REPORT | wxLC_SINGLE_SEL);
    for (const auto& [label, width] : std::vector<std::pair<wxString, int>>{ { _("From"), 60 }, { _("In the song"), 150 }, { _("What it does"), 210 }, { _("Figures"), 60 } }) {
        _uses->AppendColumn(label, wxLIST_FORMAT_LEFT, width);
    }
    body->Add(_uses, 1, wxEXPAND);
    sizer->Add(body, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
    _about = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 220), wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
    sizer->Add(_about, 1, wxEXPAND | wxALL, 8);
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* keep = new wxButton(this, wxID_ANY, _("Keep This Idea as My Composition..."));
    buttons->Add(keep, 0);
    buttons->AddStretchSpacer();
    buttons->Add(new wxButton(this, wxID_CANCEL, _("Close")), 0);
    sizer->Add(buttons, 0, wxEXPAND | wxALL, 8);
    SetSizer(sizer);
    for (const VisualMotif& m : score.motifs) {
        const long row = _motifs->InsertItem(_motifs->GetItemCount(), U(m.id));
        _motifs->SetItem(row, 1, T(m.intent));
        _motifs->SetItem(row, 2, wxString::Format("%d", static_cast<int>(m.variants.size())));
    }
    _motifs->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& e) { ShowMotif(static_cast<int>(e.GetIndex())); });
    keep->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (_shown < 0) {
            return;
        }
        if (wxMessageBox(wxString::Format(_("Keep the idea %s as a composition of your own? The director may then use it in other songs; you can exclude it again in \"Compositions and Gestures\"."),
                                          U(_score.motifs[static_cast<size_t>(_shown)].id)),
                         _("Visual Ideas and How They Develop"), wxYES_NO | wxICON_QUESTION, this) != wxYES) {
            return;
        }
        const std::string problem = KeepAsComposition();
        wxMessageBox(problem.empty() ? _("Kept in \"Compositions and Gestures\".") : _("The composition could not be kept."), _("Visual Ideas and How They Develop"),
                     wxOK | (problem.empty() ? wxICON_INFORMATION : wxICON_WARNING), this);
    });
    if (score.motifs.empty()) {
        _about->SetValue(_("This composition has no visual ideas yet: work it out on step 3 with the idea development on (step 1)."));
    } else {
        SelectMotif(0);
    }
}

bool MotifsDialog::SelectMotif(int index) {
    if (index < 0 || index >= static_cast<int>(_score.motifs.size())) {
        return false;
    }
    _motifs->SetItemState(index, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    ShowMotif(index);
    return true;
}

void MotifsDialog::ShowMotif(int index) {
    if (index < 0 || index >= static_cast<int>(_score.motifs.size())) {
        return;
    }
    _shown = index;
    const VisualMotif& m = _score.motifs[static_cast<size_t>(index)];
    _uses->DeleteAllItems();
    for (const MotifVariant& v : m.variants) {
        const long row = _uses->InsertItem(_uses->GetItemCount(), ClockText(v.startMS));
        _uses->SetItem(row, 1, T(NarrativePhrase(v.narrative)));
        _uses->SetItem(row, 2, T(MotifOperationPhrase(v.operation)));
        _uses->SetItem(row, 3, wxString::Format("%d", static_cast<int>(v.figures.size())));
    }
    const auto names = [](const std::vector<std::string>& list, size_t most) {
        wxString out;
        for (size_t k = 0; k < list.size() && k < most; ++k) {
            out += (k == 0 ? "" : ", ") + U(list[k]);
        }
        if (list.size() > most) {
            out += wxString::Format(_(" and %d more"), static_cast<int>(list.size() - most));
        }
        return out;
    };
    wxString out = wxString::Format(_("Idea %s: %s."), U(m.id), T(m.intent)) + "\n";
    if (!m.explanation.empty()) {
        wxString e = T(m.explanation);
        e[0] = wxToupper(e[0]);
        out += e + ".\n";
    }
    out += wxString::Format(_("It is carried by %s"), m.protagonist.empty() ? _("the figures below") : U(m.protagonist));
    out += m.companions.empty() ? wxString(".\n") : wxString::Format(_(", with %s.\n"), names(m.companions, 6));
    GestureKind kind;
    if (!m.gestureKind.empty() && GestureKindFromId(m.gestureKind, kind)) {
        wxString gesture = T(GestureKindPhrase(kind));
        gesture[0] = wxTolower(gesture[0]);
        out += wxString::Format(_("Its gesture: %s, %s"), gesture, T(RelationPhrase(m.temporal)));
        if (!m.spatial.empty()) {
            const size_t from = m.spatial.rfind("outwards from ", 0) == 0 ? 14 : std::string::npos;
            out += ", " + (from != std::string::npos ? wxString::Format(_("outwards from %s"), U(m.spatial.substr(from))) : T(m.spatial));
        }
        out += ".\n";
    } else {
        out += _("It has no gesture of its own yet: it is carried by the figures' own effects.") + "\n";
    }
    if (!m.colorMotif.empty()) {
        out += wxString::Format(_("It is dressed in the colour idea %s."), U(m.colorMotif)) + "\n";
    }
    if (!m.recipes.empty()) {
        out += _("What each figure does when it is presented:") + "\n";
        int shown = 0;
        for (const auto& [figure, effect] : m.recipes) {
            if (++shown > 8) {
                break;
            }
            out += "  " + U(figure) + ": " + U(effect) + "\n";
        }
    }
    for (const std::string& c : m.constraints) {
        if (c.rfind("face-sings:", 0) == 0) {
            out += wxString::Format(_("%s sings: no gesture goes over its face while it does."), U(c.substr(11))) + "\n";
        } else if (c.rfind("locked:", 0) == 0) {
            out += wxString::Format(_("%s was locked by you: it is left as it is."), U(c.substr(7))) + "\n";
        }
    }
    _about->SetValue(out);
}

std::string MotifsDialog::KeepAsComposition() {
    if (_shown < 0 || _shown >= static_cast<int>(_score.motifs.size()) || _showDir.empty()) {
        return "no-motif";
    }
    std::string error;
    return KeepComposition(_showDir, CompositionFromMotif(_score.motifs[static_cast<size_t>(_shown)], _score, _song), error) ? std::string() : error;
}

wxString MotifsDialog::Detail() const {
    return _about->GetValue();
}
