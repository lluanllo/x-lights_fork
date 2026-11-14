/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "ComposerUnderstandingDialogs.h"

#include <wx/button.h>
#include <wx/checklst.h>
#include <wx/choice.h>
#include <wx/choicdlg.h>
#include <wx/collpane.h>
#include <wx/colordlg.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/imaglist.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/numdlg.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/wrapsizer.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include <spdlog/fmt/fmt.h>

using namespace composer;

namespace {

wxString U(const std::string& s) {
    return wxString::FromUTF8(s);
}

std::string S(const wxString& s) {
    return s.ToStdString(wxConvUTF8);
}

wxStaticText* Muted(wxWindow* parent, const wxString& text, int wrap = 860) {
    auto* t = new wxStaticText(parent, wxID_ANY, text);
    t->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    t->Wrap(wrap);
    return t;
}

wxColour LaneColour(const std::string& lane) {
    static const std::map<std::string, wxColour> C = {
        { "structure", wxColour(120, 120, 160) }, { "beats", wxColour(150, 150, 150) }, { "bars", wxColour(90, 90, 90) },
        { "energy", wxColour(230, 160, 40) }, { "events", wxColour(210, 90, 60) }, { "narration", wxColour(60, 140, 220) },
        { "lead", wxColour(220, 60, 120) }, { "chorus", wxColour(150, 80, 200) }, { "secondary", wxColour(240, 120, 170) },
        { "instrumental", wxColour(40, 170, 110) }, { "words", wxColour(110, 110, 110) }, { "figures", wxColour(200, 170, 40) }
    };
    const auto it = C.find(lane);
    return it != C.end() ? it->second : wxColour(128, 128, 128);
}

wxString SourceWords(const std::string& source) {
    if (source.rfind("face-definition", 0) == 0) {
        return _("the face drawn in xLights");
    }
    if (source.rfind("face-images", 0) == 0) {
        return _("the face's pictures in xLights");
    }
    if (source.rfind("submodel", 0) == 0) {
        return _("a submodel");
    }
    if (source.rfind("name-hint", 0) == 0) {
        return _("suggested by a submodel's name: to confirm");
    }
    if (source == "manual") {
        return _("chosen by you");
    }
    if (source == "geometry") {
        return _("the figure's shape");
    }
    return U(source);
}

wxString NoteWords(const std::string& note) {
    if (note.rfind("no-mouth", 0) == 0) {
        return _("It has no mouth: it cannot speak or sing until one is defined.");
    }
    if (note.rfind("no-controller", 0) == 0) {
        return _("It has no controller: it is only seen in the preview.");
    }
    if (note.rfind("region-to-confirm:", 0) == 0) {
        return wxString::Format(_("The part \"%s\" is only suggested by a name: confirm it if it is right."), UnderstandingWords(RegionPhrase(note.substr(18))));
    }
    if (note.rfind("region-without-lights:", 0) == 0) {
        return wxString::Format(_("The part \"%s\" is not tied to any light."), UnderstandingWords(RegionPhrase(note.substr(22))));
    }
    return U(note);
}

const std::vector<std::string>& AllRoles() {
    static const std::vector<std::string> R = { "narrator", "character", "lead-singer", "secondary-singer", "chorus-member", "instrumental", "percussion", "decoration", "background", "accent" };
    return R;
}

const std::vector<std::string>& AllRegions() {
    static const std::vector<std::string> R = { "mouth", "eyes", "face", "head", "body", "outline", "trunk", "foliage", "branches", "top", "background", "text", "image" };
    return R;
}

} // namespace

wxString UnderstandingWords(const std::string& coreText) {
    return wxGetTranslation(U(coreText));
}

wxString ItemWords(const TimelineItem& i) {
    // Words are the item's own text, never translated.
    const wxString what = i.kind == "word" ? U(i.label) : UnderstandingWords(i.label);
    return i.who.empty() ? what : U(i.who) + ": " + what;
}

wxString ClockText(int ms) {
    ms = std::max(0, ms);
    return wxString::Format("%d:%02d.%d", ms / 60000, (ms / 1000) % 60, (ms % 1000) / 100);
}

wxString UnderstandingSummary(const SongUnderstanding& song) {
    const AudioUnderstanding& a = song.audio;
    const auto n = [&](const char* kind) { return static_cast<int>(a.Of(kind).size()); };
    wxString text = wxString::Format(_("Heard - speech: %d, singing: %d, the choir: %d, the instruments alone: %d."), n("speech"), n("singing"), n("chorus"), n("instrumental"));
    int people = 0;
    for (const VoiceTrack& t : song.voices.tracks) {
        people += t.id != "chorus" ? 1 : 0;
    }
    text += " " + wxString::Format(_("Voices told apart by how they sound (not separated): %d."), people);
    if (!a.corrections.empty()) {
        text += " " + wxString::Format(_("Corrections of yours applied: %d."), static_cast<int>(a.corrections.size()));
    }
    return text;
}

// ---------------------------------------------------------------- lanes view

class LanesView : public wxScrolledWindow {
public:
    LanesView(wxWindow* parent, std::function<void(const std::string&, int)> clicked) : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(900, 420), wxHSCROLL | wxVSCROLL | wxBORDER_SIMPLE), _clicked(std::move(clicked)) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetScrollRate(20, 20);
        Bind(wxEVT_PAINT, &LanesView::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, &LanesView::OnClick, this);
    }

    void Set(const MultimodalTimeline* t, const std::string* selected, const int* cursor) {
        _t = t;
        _selected = selected;
        _cursor = cursor;
        Resize();
    }

    void SetZoom(double pixelsPerSecond) {
        _pps = pixelsPerSecond;
        Resize();
    }

    void Resize() {
        if (_t != nullptr) {
            SetVirtualSize(NAME_W + static_cast<int>(_t->durationMS / 1000.0 * _pps) + 40, TOP + LANE_H * static_cast<int>(_t->lanes.size()) + 10);
        }
        Refresh();
    }

private:
    static constexpr int NAME_W = 170;
    static constexpr int LANE_H = 30;
    static constexpr int TOP = 22;

    int X(int ms) const { return NAME_W + static_cast<int>(ms / 1000.0 * _pps); }

    void OnPaint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        DoPrepareDC(dc);
        dc.SetBackground(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)));
        dc.Clear();
        if (_t == nullptr) {
            return;
        }
        dc.SetFont(GetFont());
        const wxColour text = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
        dc.SetTextForeground(text);
        const int step = _pps >= 40 ? 1 : _pps >= 10 ? 5 : 15;
        dc.SetPen(wxPen(wxColour(200, 200, 200)));
        for (int s = 0; s * 1000 <= _t->durationMS; s += step) {
            dc.DrawLine(X(s * 1000), TOP - 4, X(s * 1000), TOP + LANE_H * static_cast<int>(_t->lanes.size()));
            dc.DrawText(ClockText(s * 1000), X(s * 1000) + 2, 2);
        }
        for (size_t l = 0; l < _t->lanes.size(); ++l) {
            const TimelineLane& lane = _t->lanes[l];
            const int y = TOP + static_cast<int>(l) * LANE_H;
            dc.SetPen(wxPen(wxColour(220, 220, 220)));
            dc.DrawLine(0, y + LANE_H - 1, X(_t->durationMS) + 20, y + LANE_H - 1);
            dc.DrawText(UnderstandingWords(lane.title), 4, y + 7);
            const wxColour c = LaneColour(lane.id);
            for (const TimelineItem& i : lane.items) {
                const int x0 = X(i.startMS);
                const int w = std::max(2, X(i.endMS) - x0);
                const bool selected = _selected != nullptr && *_selected == i.id;
                dc.SetBrush(wxBrush(selected ? c.ChangeLightness(70) : (i.provenance == "corrected" || i.provenance == "added" || i.provenance == "manual" ? c.ChangeLightness(85) : c)));
                dc.SetPen(selected ? wxPen(*wxBLACK, 2) : wxPen(c.ChangeLightness(60)));
                dc.DrawRectangle(x0, y + 4, w, LANE_H - 9);
                if (w > 50 && lane.id != "beats" && lane.id != "bars") {
                    dc.SetClippingRegion(x0, y + 4, w, LANE_H - 9);
                    dc.SetTextForeground(*wxWHITE);
                    dc.DrawText(ItemWords(i), x0 + 3, y + 7);
                    dc.SetTextForeground(text);
                    dc.DestroyClippingRegion();
                }
            }
        }
        if (_cursor != nullptr) {
            dc.SetPen(wxPen(*wxRED, 1));
            dc.DrawLine(X(*_cursor), 0, X(*_cursor), TOP + LANE_H * static_cast<int>(_t->lanes.size()));
        }
    }

    void OnClick(wxMouseEvent& e) {
        if (_t == nullptr) {
            return;
        }
        int x = 0, y = 0;
        CalcUnscrolledPosition(e.GetX(), e.GetY(), &x, &y);
        const int lane = (y - TOP) / LANE_H;
        const int ms = std::max(0, static_cast<int>((x - NAME_W) * 1000.0 / _pps));
        std::string hit;
        if (lane >= 0 && lane < static_cast<int>(_t->lanes.size())) {
            for (const TimelineItem& i : _t->lanes[lane].items) {
                if (x >= X(i.startMS) - 2 && x <= std::max(X(i.endMS), X(i.startMS) + 2) + 2) {
                    hit = i.id;
                }
            }
        }
        _clicked(hit, ms);
    }

    const MultimodalTimeline* _t = nullptr;
    const std::string* _selected = nullptr;
    const int* _cursor = nullptr;
    double _pps = 20;
    std::function<void(const std::string&, int)> _clicked;
};

// ---------------------------------------------------------------- recording

namespace {
enum {
    ID_MOVE = wxID_HIGHEST + 1600,
    ID_SPLIT,
    ID_MERGE,
    ID_KIND,
    ID_VOICE,
    ID_FIGURES,
    ID_REMOVE,
    ID_CONFIRM,
    ID_REVERT,
    ID_RESTORE
};
}

RecordingDialog::RecordingDialog(wxWindow* parent, SongUnderstanding song, UnderstandRequest request, const SongAnalysis* analysis, const FigureCatalog* geometry, const FigureSemantics* semantics) :
    wxDialog(parent, wxID_ANY, _("What happens in the recording"), wxDefaultPosition, wxSize(1100, 760), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    _song(std::move(song)), _request(std::move(request)), _analysis(analysis), _geometry(geometry), _semantics(semantics) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    _summary = new wxStaticText(this, wxID_ANY, "");
    sizer->Add(_summary, 0, wxEXPAND | wxALL, 8);
    sizer->Add(Muted(this, _("Click a piece to choose it, or a moment to place the red line. Your corrections are kept apart and applied again after every new analysis. "
                             "Voices are told apart by how they sound: nobody is identified and no voice is separated from the music.")),
               0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    _lanes = new LanesView(this, [this](const std::string& id, int ms) {
        _cursorMS = ms;
        if (!id.empty()) {
            _selected = id;
        }
        ShowSelection();
    });
    sizer->Add(_lanes, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
    auto* zoomRow = new wxBoxSizer(wxHORIZONTAL);
    zoomRow->Add(new wxStaticText(this, wxID_ANY, _("Zoom:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    _zoom = new wxSlider(this, wxID_ANY, 20, 4, 120, wxDefaultPosition, wxSize(240, -1));
    zoomRow->Add(_zoom, 0);
    sizer->Add(zoomRow, 0, wxALL, 6);
    _detail = new wxStaticText(this, wxID_ANY, "");
    sizer->Add(_detail, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
    auto* buttons = new wxWrapSizer(wxHORIZONTAL, 0);
    const auto button = [&](int id, const wxString& label) { buttons->Add(new wxButton(this, id, label), 0, wxALL, 3); };
    button(ID_MOVE, _("Change Its Times..."));
    button(ID_SPLIT, _("Split at the Red Line"));
    button(ID_MERGE, _("Join with the Next One"));
    button(ID_KIND, _("It Is..."));
    button(ID_VOICE, _("It Is the Voice of..."));
    button(ID_FIGURES, _("Who Shows It..."));
    button(ID_REMOVE, _("Remove"));
    button(ID_CONFIRM, _("It Is Right"));
    button(ID_REVERT, _("Back to the Automatic Reading"));
    button(ID_RESTORE, _("Bring My Changes Back"));
    sizer->Add(buttons, 0, wxEXPAND | wxALL, 6);
    _status = new wxStaticText(this, wxID_ANY, "");
    sizer->Add(_status, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
    auto* advanced = new wxCollapsiblePane(this, wxID_ANY, _("Advanced options"));
    auto* pane = advanced->GetPane();
    auto* advSizer = new wxBoxSizer(wxVERTICAL);
    _technical = new wxTextCtrl(pane, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 110), wxTE_MULTILINE | wxTE_READONLY);
    advSizer->Add(_technical, 1, wxEXPAND);
    pane->SetSizer(advSizer);
    sizer->Add(advanced, 0, wxEXPAND | wxALL, 6);
    sizer->Add(CreateStdDialogButtonSizer(wxOK), 0, wxEXPAND | wxALL, 8);
    SetSizer(sizer);

    _zoom->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { _lanes->SetZoom(_zoom->GetValue()); });
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AskMove(); }, ID_MOVE);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        TimelineEdit e;
        e.action = TimelineEdit::Action::Split;
        e.itemId = _selected;
        e.atMS = _cursorMS;
        Edit(e);
    }, ID_SPLIT);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const TimelineItem* item = _timeline.Find(_selected);
        const TimelineLane* lane = item != nullptr ? _timeline.Lane(_selected.substr(0, _selected.find('/'))) : nullptr;
        TimelineEdit e;
        e.action = TimelineEdit::Action::Merge;
        e.itemId = _selected;
        for (const TimelineItem& i : lane != nullptr ? lane->items : std::vector<TimelineItem>()) {
            if (e.otherItemId.empty() && i.startMS >= item->endMS - 50 && i.id != item->id) {
                e.otherItemId = i.id;
            }
        }
        Edit(e);
    }, ID_MERGE);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AskKind(); }, ID_KIND);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AskVoice(); }, ID_VOICE);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AskFigures(); }, ID_FIGURES);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        TimelineEdit e;
        e.action = TimelineEdit::Action::Remove;
        e.itemId = _selected;
        Edit(e);
    }, ID_REMOVE);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        TimelineEdit e;
        e.action = TimelineEdit::Action::Confirm;
        e.itemId = _selected;
        Edit(e);
    }, ID_CONFIRM);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Revert(false); }, ID_REVERT);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Revert(true); }, ID_RESTORE);
    advanced->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [this](wxCollapsiblePaneEvent&) { Layout(); });

    Rebuild();
    _lanes->Set(&_timeline, &_selected, &_cursorMS);
}

void RecordingDialog::Rebuild() {
    if (_geometry != nullptr && _semantics != nullptr) {
        MappingInput in;
        in.audio = &_song.audio;
        in.voices = &_song.voices;
        in.geometry = _geometry;
        in.semantics = _semantics;
        in.manual = &_song.manual;
        _performers = MapPerformance(in);
    }
    TimelineInput t;
    t.song = _analysis;
    t.audio = &_song.audio;
    t.voices = &_song.voices;
    t.alignment = _request.alignment;
    t.performers = _geometry != nullptr ? &_performers : nullptr;
    _timeline = BuildTimeline(t);
    _summary->SetLabel(UnderstandingSummary(_song));
    if (_lanes != nullptr) {
        _lanes->Resize();
    }
    ShowSelection();
}

wxString RecordingDialog::Detail() const {
    const TimelineItem* i = _timeline.Find(_selected);
    if (i == nullptr) {
        return wxString::Format(_("Red line at %s. Choose a piece to correct it."), ClockText(_cursorMS));
    }
    wxString text = wxString::Format(_("%s, from %s to %s (%d%% sure)"), ItemWords(*i), ClockText(i->startMS), ClockText(i->endMS), static_cast<int>(std::lround(i->confidence * 100)));
    if (i->provenance == "corrected" || i->provenance == "added" || i->provenance == "manual") {
        text += " - " + _("corrected by you");
    } else if (i->provenance.rfind("stem:", 0) == 0) {
        text += " - " + _("heard on its own recording");
    } else if (i->provenance == "lyrics") {
        text += " - " + _("placed by the lyrics");
    }
    return text;
}

void RecordingDialog::ShowSelection() {
    _detail->SetLabel(Detail());
    const TimelineItem* i = _timeline.Find(_selected);
    wxString tech;
    if (i != nullptr) {
        tech = U(fmt::format("id {}\nsource {}\nkind {}\nprovenance {}\ntrack {}\n", i->id, i->source, i->kind, i->provenance, i->track));
        if (const ActivitySegment* s = _song.audio.Find(i->source)) {
            for (const std::string& e : s->evidence) {
                tech += U(e) + "\n";
            }
        }
    }
    _technical->SetValue(tech);
    _lanes->Refresh();
    Layout();
}

bool RecordingDialog::Select(const std::string& itemId) {
    if (_timeline.Find(itemId) == nullptr) {
        return false;
    }
    _selected = itemId;
    ShowSelection();
    return true;
}

void RecordingDialog::SetCursor(int ms) {
    _cursorMS = ms;
    ShowSelection();
}

std::string RecordingDialog::Edit(const TimelineEdit& edit) {
    const TimelineChange change = TranslateEdit(_timeline, edit, _song.audio, _song.voices, &_performers);
    if (!change.ok) {
        _status->SetLabel(UnderstandingWords(TimelineEditError(change.error)));
        return change.error;
    }
    ApplyChange(change, _song.audio, _song.manual);
    std::string error;
    if (change.isCorrection && !SaveCorrections(_song.folder, _song.audio.corrections, error)) {
        _status->SetLabel(wxString::Format(_("The correction could not be saved: %s"), U(error)));
        return "not-saved";
    }
    if (change.isAssignment && !SaveManualAssignments(_song.folder, _song.manual, error)) {
        _status->SetLabel(wxString::Format(_("The choice could not be saved: %s"), U(error)));
        return "not-saved";
    }
    RefreshVoices(_song, _request);
    (void)SaveDerived(_song, error);
    _changed = true;
    _status->SetLabel(_("Saved. It will be used every time the show is made."));
    Rebuild();
    if (_timeline.Find(_selected) == nullptr) {
        _selected.clear();
        ShowSelection();
    }
    return "";
}

std::string RecordingDialog::Revert(bool restore) {
    std::string error;
    if (!RevertUnderstanding(_song.folder, restore, error)) {
        _status->SetLabel(U(error));
        return error;
    }
    // The reading again from its file, with whatever corrections are now in place.
    std::string text;
    AudioUnderstanding reading;
    if (ReadTextFile(_song.folder + "/reading.json", text) && FromJson(text, reading, error)) {
        reading.corrections.clear();
        _song.audio = reading;
    }
    std::vector<UnderstandingCorrection> corrections;
    AudioUnderstanding holder;
    if (ReadTextFile(_song.folder + "/corrections.json", text) && FromJson(text, holder, error)) {
        corrections = holder.corrections;
    }
    _song.audio.corrections = corrections;
    ApplyUnderstandingCorrections(_song.audio);
    _song.manual.clear();
    if (ReadTextFile(_song.folder + "/performers.json", text)) {
        (void)FromJson(text, _song.manual, error);
    }
    RefreshVoices(_song, _request);
    _changed = true;
    _status->SetLabel(restore ? _("Your corrections are back.") : _("Back to the automatic reading. Your corrections are kept aside and can be brought back."));
    _selected.clear();
    Rebuild();
    return "";
}

void RecordingDialog::AskMove() {
    const TimelineItem* i = _timeline.Find(_selected);
    if (i == nullptr) {
        _status->SetLabel(_("Choose a piece first."));
        return;
    }
    wxTextEntryDialog ask(this, _("New start and end, in seconds, separated by a space (for example: 12.5 18)"), _("Change Its Times"),
                          wxString::Format("%.1f %.1f", i->startMS / 1000.0, i->endMS / 1000.0));
    if (ask.ShowModal() != wxID_OK) {
        return;
    }
    double a = 0, b = 0;
    if (std::sscanf(S(ask.GetValue()).c_str(), "%lf %lf", &a, &b) != 2) {
        _status->SetLabel(UnderstandingWords(TimelineEditError("times-not-valid")));
        return;
    }
    TimelineEdit e;
    e.action = TimelineEdit::Action::Move;
    e.itemId = _selected;
    e.startMS = static_cast<int>(std::lround(a * 1000));
    e.endMS = static_cast<int>(std::lround(b * 1000));
    Edit(e);
}

void RecordingDialog::AskKind() {
    const std::vector<std::string> kinds = { "speech", "singing", "chorus", "instrumental", "percussion", "silence", "pause", "noise" };
    wxArrayString names;
    for (const std::string& k : kinds) {
        names.Add(UnderstandingWords(ActivityPhrase(k)));
    }
    wxSingleChoiceDialog ask(this, _("What is it?"), _("It Is..."), names);
    if (ask.ShowModal() != wxID_OK) {
        return;
    }
    TimelineEdit e;
    e.action = TimelineEdit::Action::Reclassify;
    e.itemId = _selected;
    e.kind = kinds[static_cast<size_t>(ask.GetSelection())];
    Edit(e);
}

void RecordingDialog::AskVoice() {
    std::vector<std::string> ids;
    wxArrayString names;
    for (const VoiceTrack& t : _song.voices.tracks) {
        ids.push_back(t.id);
        names.Add(U(t.label));
    }
    // A voice nobody heard yet: the next free letter.
    std::set<std::string> used(ids.begin(), ids.end());
    for (char c = 'A'; c <= 'Z'; ++c) {
        const std::string id = std::string("voice:") + c;
        if (!used.contains(id)) {
            ids.push_back(id);
            names.Add(wxString::Format(_("A new voice (Voz %c)"), c));
            break;
        }
    }
    wxSingleChoiceDialog ask(this, _("Whose voice is it?"), _("It Is the Voice of..."), names);
    if (ask.ShowModal() != wxID_OK) {
        return;
    }
    TimelineEdit e;
    e.action = TimelineEdit::Action::Reassign;
    e.itemId = _selected;
    e.track = ids[static_cast<size_t>(ask.GetSelection())];
    Edit(e);
}

void RecordingDialog::AskFigures() {
    if (_geometry == nullptr || _selected.rfind("figures/", 0) != 0) {
        _status->SetLabel(_("Choose a piece of the Figures lane: the figures that show a voice or a moment."));
        return;
    }
    wxArrayString names;
    std::vector<std::string> figures;
    for (const FigureGeometry& g : _geometry->figures) {
        if (g.kind == "model") {
            figures.push_back(g.name);
            names.Add(U(g.name));
        }
    }
    wxMultiChoiceDialog ask(this, _("Which figures show it?"), _("Who Shows It..."), names);
    if (ask.ShowModal() != wxID_OK) {
        return;
    }
    TimelineEdit e;
    e.action = TimelineEdit::Action::Reassign;
    e.itemId = _selected;
    for (int i : ask.GetSelections()) {
        e.figures.push_back(figures[static_cast<size_t>(i)]);
    }
    Edit(e);
}

// ---------------------------------------------------------------- figures

class NodesView : public wxPanel {
public:
    explicit NodesView(wxWindow* parent) : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(320, 320), wxBORDER_SIMPLE) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(*wxBLACK_BRUSH);
            dc.Clear();
            if (_g == nullptr || _g->nodeMap.empty()) {
                dc.SetTextForeground(*wxWHITE);
                dc.DrawText(_("No light positions known"), 8, 8);
                return;
            }
            const wxSize size = GetClientSize();
            const int m = 10;
            const double sx = size.x - 2 * m, sy = size.y - 2 * m;
            std::map<int, wxColour> colour;
            std::set<int> selected;
            if (_p != nullptr) {
                for (const FigureRegion& r : _p->regions) {
                    if (r.nodes.size() == _g->nodeMap.size() && r.id != *_region) {
                        continue; // the whole figure: only shown when chosen
                    }
                    const wxColour c(U(r.color.empty() ? "#B0B0B0" : r.color));
                    for (int n : r.nodes) {
                        colour[n] = c;
                        if (r.id == *_region) {
                            selected.insert(n);
                        }
                    }
                }
            }
            const int dot = std::max(2, std::min(8, static_cast<int>(std::min(sx, sy) / std::sqrt(static_cast<double>(_g->nodeMap.size())) / 2)));
            for (const FigureNode& n : _g->nodeMap) {
                const auto c = colour.find(n.index);
                const wxColour fill = c != colour.end() ? c->second : wxColour(70, 70, 70);
                dc.SetBrush(wxBrush(fill));
                dc.SetPen(selected.contains(n.index) ? wxPen(*wxWHITE, 2) : wxPen(fill));
                dc.DrawCircle(m + static_cast<int>(n.x * sx), m + static_cast<int>((1 - n.y) * sy), dot);
            }
        });
    }

    void Set(const FigureGeometry* g, const FigureProfile* p, const std::string* region) {
        _g = g;
        _p = p;
        _region = region;
        Refresh();
    }

private:
    const FigureGeometry* _g = nullptr;
    const FigureProfile* _p = nullptr;
    const std::string* _region = nullptr;
};

namespace {
enum {
    ID_CONFIRM_REGION = wxID_HIGHEST + 1650,
    ID_DROP_REGION,
    ID_ADD_REGION,
    ID_COLOR_REGION,
    ID_REVIEWED
};
}

FiguresDialog::FiguresDialog(wxWindow* parent, FigureCatalog geometry, FigureSemantics found, FigureChoices choices, std::string showDir) :
    wxDialog(parent, wxID_ANY, _("Know My Figures"), wxDefaultPosition, wxSize(1150, 720), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    _geometry(std::move(geometry)), _found(std::move(found)), _choices(std::move(choices)), _showDir(std::move(showDir)) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(Muted(this, _("Each figure as xLights draws it. A part (a mouth, a trunk...) is only known when it is tied to real lights; a name alone only suggests it. "
                             "Each part keeps its own colour, whatever the song's colours.")),
               0, wxEXPAND | wxALL, 8);
    auto* body = new wxBoxSizer(wxHORIZONTAL);
    _figures = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(430, -1), wxLC_REPORT | wxLC_SINGLE_SEL);
    _figures->AppendColumn(_("Figure"), wxLIST_FORMAT_LEFT, 150);
    _figures->AppendColumn(_("Kind"), wxLIST_FORMAT_LEFT, 90);
    _figures->AppendColumn(_("Lights"), wxLIST_FORMAT_RIGHT, 55);
    _figures->AppendColumn(_("Mouth"), wxLIST_FORMAT_LEFT, 120);
    body->Add(_figures, 0, wxEXPAND | wxRIGHT, 8);
    auto* right = new wxBoxSizer(wxVERTICAL);
    _about = new wxStaticText(this, wxID_ANY, "");
    right->Add(_about, 0, wxEXPAND | wxBOTTOM, 6);
    auto* middle = new wxBoxSizer(wxHORIZONTAL);
    _nodes = new NodesView(this);
    middle->Add(_nodes, 0, wxRIGHT, 8);
    auto* partCol = new wxBoxSizer(wxVERTICAL);
    _regions = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 180), wxLC_REPORT | wxLC_SINGLE_SEL);
    _regions->AppendColumn(_("Part"), wxLIST_FORMAT_LEFT, 100);
    _regions->AppendColumn(_("Lights"), wxLIST_FORMAT_RIGHT, 55);
    _regions->AppendColumn(_("Colour"), wxLIST_FORMAT_LEFT, 75);
    _regions->AppendColumn(_("Where it comes from"), wxLIST_FORMAT_LEFT, 230);
    partCol->Add(_regions, 1, wxEXPAND | wxBOTTOM, 4);
    auto* partButtons = new wxWrapSizer(wxHORIZONTAL, 0);
    partButtons->Add(new wxButton(this, ID_CONFIRM_REGION, _("It Is Right")), 0, wxALL, 2);
    partButtons->Add(new wxButton(this, ID_DROP_REGION, _("Remove This Part")), 0, wxALL, 2);
    partButtons->Add(new wxButton(this, ID_ADD_REGION, _("Add a Part...")), 0, wxALL, 2);
    partButtons->Add(new wxButton(this, ID_COLOR_REGION, _("Its Colour...")), 0, wxALL, 2);
    partCol->Add(partButtons, 0, wxEXPAND);
    middle->Add(partCol, 1, wxEXPAND);
    right->Add(middle, 1, wxEXPAND | wxBOTTOM, 6);
    right->Add(new wxStaticText(this, wxID_ANY, _("It can be:")), 0, wxBOTTOM, 2);
    wxArrayString roles;
    for (const std::string& r : AllRoles()) {
        roles.Add(UnderstandingWords(RolePhrase(r)));
    }
    _roles = new wxCheckListBox(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 110), roles, wxLB_MULTIPLE);
    right->Add(_roles, 0, wxEXPAND | wxBOTTOM, 4);
    right->Add(new wxButton(this, ID_REVIEWED, _("I Have Looked at This Figure")), 0, wxBOTTOM, 4);
    body->Add(right, 1, wxEXPAND);
    sizer->Add(body, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
    _status = new wxStaticText(this, wxID_ANY, "");
    sizer->Add(_status, 0, wxEXPAND | wxALL, 8);
    sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
    SetSizer(sizer);

    _figures->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& e) {
        _figure = _figureRows[static_cast<size_t>(e.GetIndex())];
        _region.clear();
        FillRegions();
    });
    _regions->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& e) {
        const FigureProfile* p = Profile();
        if (p != nullptr && e.GetIndex() < static_cast<long>(p->regions.size())) {
            _region = p->regions[static_cast<size_t>(e.GetIndex())].id;
            _nodes->Refresh();
        }
    });
    _roles->Bind(wxEVT_CHECKLISTBOX, [this](wxCommandEvent&) {
        std::vector<std::string> roles;
        for (size_t i = 0; i < AllRoles().size(); ++i) {
            if (_roles->IsChecked(static_cast<unsigned>(i))) {
                roles.push_back(AllRoles()[i]);
            }
        }
        SetRoles(roles);
    });
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ConfirmRegion(); }, ID_CONFIRM_REGION);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { DropRegion(); }, ID_DROP_REGION);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxArrayString names;
        for (const std::string& r : AllRegions()) {
            names.Add(UnderstandingWords(RegionPhrase(r)));
        }
        wxSingleChoiceDialog which(this, _("Which part?"), _("Add a Part"), names);
        if (which.ShowModal() != wxID_OK) {
            return;
        }
        wxTextEntryDialog lights(this, _("Its lights, as xLights numbers them (for example: 1-20,25,30-34)"), _("Add a Part"));
        if (lights.ShowModal() != wxID_OK) {
            return;
        }
        std::string error;
        if (!AddRegion(AllRegions()[static_cast<size_t>(which.GetSelection())], S(lights.GetValue()), error)) {
            _status->SetLabel(U(error));
        }
    }, ID_ADD_REGION);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (_region.empty()) {
            _status->SetLabel(_("Choose a part first."));
            return;
        }
        wxColourData data;
        const FigureRegion* r = Profile() != nullptr ? Profile()->Region(_region) : nullptr;
        if (r != nullptr && !r->color.empty()) {
            data.SetColour(wxColour(U(r->color)));
        }
        wxColourDialog dialog(this, &data);
        if (dialog.ShowModal() == wxID_OK) {
            const wxColour c = dialog.GetColourData().GetColour();
            SetRegionColor(S(c.GetAsString(wxC2S_HTML_SYNTAX)));
        }
    }, ID_COLOR_REGION);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { MarkReviewed(true); }, ID_REVIEWED);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
        std::string error;
        if (_changed && !Save(error)) {
            wxMessageBox(wxString::Format(_("Your choices could not be saved: %s"), U(error)), _("Know My Figures"), wxOK | wxICON_WARNING, this);
            return;
        }
        e.Skip();
    }, wxID_OK);

    Recompute();
    FillFigures();
    if (!_figureRows.empty()) {
        SelectFigure(_figureRows.front());
    }
}

void FiguresDialog::Recompute() {
    _semantics = ApplyFigureChoices(_found, _geometry, &_choices);
}

const FigureProfile* FiguresDialog::Profile() const {
    return _semantics.Find(_figure);
}

FigureChoice& FiguresDialog::ChoiceFor(const std::string& figure) {
    if (FigureChoice* c = _choices.Find(figure)) {
        return *c;
    }
    FigureChoice c;
    c.figure = figure;
    _choices.figures.push_back(c);
    return _choices.figures.back();
}

void FiguresDialog::FillFigures() {
    _figures->DeleteAllItems();
    _figureRows.clear();
    for (const FigureProfile& p : _semantics.profiles) {
        const FigureGeometry* g = _geometry.Find(p.figure);
        if (g == nullptr) {
            continue;
        }
        const long row = _figures->InsertItem(_figures->GetItemCount(), U(p.figure));
        _figures->SetItem(row, 1, UnderstandingWords(TopologyPhrase(g->topology)));
        _figures->SetItem(row, 2, wxString::Format("%d", g->nodes));
        _figures->SetItem(row, 3, p.mouth == "nodes" ? _("Yes, with its lights") : p.mouth == "images" ? _("Yes, with pictures") : p.mouth == "suggested" ? _("To confirm") : _("No"));
        _figureRows.push_back(p.figure);
    }
}

wxString FiguresDialog::FigureText() const {
    const FigureGeometry* g = _geometry.Find(_figure);
    const FigureProfile* p = Profile();
    if (g == nullptr || p == nullptr) {
        return wxString();
    }
    wxString text = wxString::Format(_("%s: %s, %d lights (%d channels), %d x %d in xLights."), U(g->name), UnderstandingWords(TopologyPhrase(g->topology)), g->nodes, g->channels, g->bufferWi, g->bufferHt);
    text += " " + (g->noController ? _("Without a controller: only in the preview.") : wxString::Format(_("Controller: %s."), U(g->controller)));
    for (const std::string& n : p->notes) {
        text += "\n" + NoteWords(n);
    }
    for (const std::string& l : g->limits) {
        if (l.rfind("irregular-shape", 0) == 0) {
            text += "\n" + _("An irregular figure: its lights' places are known, not rows or branches.");
        } else if (l.rfind("shape-unknown", 0) == 0) {
            text += "\n" + _("A shape the composer does not know: it is used as a set of points.");
        }
    }
    if (p->reviewed) {
        text += "\n" + _("You have looked at this figure.");
    }
    return text;
}

void FiguresDialog::FillRegions() {
    _regions->DeleteAllItems();
    const FigureProfile* p = Profile();
    _about->SetLabel(FigureText());
    _about->Wrap(620);
    if (p == nullptr) {
        _nodes->Set(nullptr, nullptr, &_region);
        return;
    }
    auto* swatches = new wxImageList(14, 14, true);
    for (const FigureRegion& r : p->regions) {
        wxBitmap swatch(14, 14);
        {
            wxMemoryDC dc(swatch);
            dc.SetBackground(wxBrush(r.color.empty() ? wxColour(128, 128, 128) : wxColour(U(r.color))));
            dc.Clear();
            dc.SetPen(*wxBLACK_PEN);
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRectangle(0, 0, 14, 14);
        }
        const int image = swatches->Add(swatch);
        const long row = _regions->InsertItem(_regions->GetItemCount(), UnderstandingWords(RegionPhrase(r.id)), image);
        _regions->SetItem(row, 1, wxString::Format("%d", static_cast<int>(r.nodes.size())));
        _regions->SetItem(row, 2, r.color.empty() ? _("none") : U(r.color));
        _regions->SetItem(row, 3, SourceWords(r.source) + (r.confirmed ? " - " + _("confirmed") : wxString()));
    }
    _regions->AssignImageList(swatches, wxIMAGE_LIST_SMALL);
    for (size_t i = 0; i < AllRoles().size(); ++i) {
        _roles->Check(static_cast<unsigned>(i), std::find(p->roles.begin(), p->roles.end(), AllRoles()[i]) != p->roles.end());
    }
    _nodes->Set(_geometry.Find(_figure), p, &_region);
    Layout();
}

bool FiguresDialog::SelectFigure(const std::string& figure) {
    const auto it = std::find(_figureRows.begin(), _figureRows.end(), figure);
    if (it == _figureRows.end()) {
        return false;
    }
    _figure = figure;
    _region.clear();
    _figures->SetItemState(it - _figureRows.begin(), wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    FillRegions();
    return true;
}

bool FiguresDialog::SelectRegion(const std::string& region) {
    const FigureProfile* p = Profile();
    if (p == nullptr || p->Region(region) == nullptr) {
        return false;
    }
    _region = region;
    _nodes->Refresh();
    return true;
}

bool FiguresDialog::ConfirmRegion() {
    const FigureProfile* p = Profile();
    const FigureRegion* r = p != nullptr && !_region.empty() ? p->Region(_region) : nullptr;
    if (r == nullptr || !r->real) {
        _status->SetLabel(_("Choose a part tied to lights first."));
        return false;
    }
    FigureRegion kept = *r;
    kept.source = r->source.rfind("name-hint", 0) == 0 ? "submodel:" + r->subModel : r->source;
    FigureChoice& c = ChoiceFor(_figure);
    std::erase_if(c.regions, [&](const FigureRegion& o) { return o.id == kept.id; });
    std::erase(c.dropped, kept.id);
    c.regions.push_back(kept);
    _changed = true;
    Recompute();
    FillFigures();
    SelectFigure(_figure);
    _region = kept.id;
    _status->SetLabel(_("Confirmed."));
    return true;
}

bool FiguresDialog::DropRegion() {
    if (_region.empty() || Profile() == nullptr) {
        _status->SetLabel(_("Choose a part first."));
        return false;
    }
    FigureChoice& c = ChoiceFor(_figure);
    std::erase_if(c.regions, [&](const FigureRegion& o) { return o.id == _region; });
    if (std::find(c.dropped.begin(), c.dropped.end(), _region) == c.dropped.end()) {
        c.dropped.push_back(_region);
    }
    _changed = true;
    Recompute();
    FillFigures();
    SelectFigure(_figure);
    _status->SetLabel(_("Removed. It will stay removed after a new analysis."));
    return true;
}

bool FiguresDialog::AddRegion(const std::string& region, const std::string& ranges, std::string& error) {
    const FigureGeometry* g = _geometry.Find(_figure);
    if (g == nullptr) {
        error = S(_("Choose a figure first."));
        return false;
    }
    FigureRegion r;
    r.id = region;
    r.source = "manual";
    r.nodes = ParseNodeRanges(ranges, g->nodes);
    if (r.nodes.empty()) {
        error = S(wxString::Format(_("No light of this figure in \"%s\": it has lights 1 to %d."), U(ranges), g->nodes));
        return false;
    }
    FigureChoice& c = ChoiceFor(_figure);
    std::erase_if(c.regions, [&](const FigureRegion& o) { return o.id == region; });
    std::erase(c.dropped, region);
    c.regions.push_back(r);
    _changed = true;
    Recompute();
    FillFigures();
    SelectFigure(_figure);
    _region = region;
    _status->SetLabel(wxString::Format(_("%s: %d lights."), UnderstandingWords(RegionPhrase(region)), static_cast<int>(r.nodes.size())));
    return true;
}

bool FiguresDialog::SetRegionColor(const std::string& hex) {
    if (_region.empty() || hex.size() != 7 || hex[0] != '#') {
        return false;
    }
    ChoiceFor(_figure).colors[_region] = hex;
    _changed = true;
    const std::string keep = _region;
    Recompute();
    FillRegions();
    _region = keep;
    return true;
}

bool FiguresDialog::SetRoles(const std::vector<std::string>& roles) {
    if (Profile() == nullptr) {
        return false;
    }
    ChoiceFor(_figure).roles = roles;
    _changed = true;
    Recompute();
    return true;
}

bool FiguresDialog::MarkReviewed(bool reviewed) {
    if (Profile() == nullptr) {
        return false;
    }
    ChoiceFor(_figure).reviewed = reviewed;
    _changed = true;
    Recompute();
    FillRegions();
    return true;
}

bool FiguresDialog::Save(std::string& error) {
    if (!WriteTextFileAtomically(FigureChoicesPath(_showDir), ToJson(_choices), error)) {
        return false;
    }
    _changed = false;
    return true;
}

// ---------------------------------------------------------------- performers

namespace {
enum {
    ID_PICK = wxID_HIGHEST + 1700,
    ID_AUTOMATIC
};
}

PerformersDialog::PerformersDialog(wxWindow* parent, SongUnderstanding song, FigureCatalog geometry, FigureSemantics semantics) :
    wxDialog(parent, wxID_ANY, _("Assign Voices and Characters"), wxDefaultPosition, wxSize(960, 600), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    _song(std::move(song)), _geometry(std::move(geometry)), _semantics(std::move(semantics)) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(Muted(this, _("Who performs each voice and each moment of the music. A figure only performs while its voice is heard: when the narration ends the tree stops "
                             "speaking, and no mouth moves while the instruments play alone. What you choose always wins.")),
               0, wxEXPAND | wxALL, 8);
    _list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 260), wxLC_REPORT | wxLC_SINGLE_SEL);
    _list->AppendColumn(_("Who"), wxLIST_FORMAT_LEFT, 150);
    _list->AppendColumn(_("Does"), wxLIST_FORMAT_LEFT, 190);
    _list->AppendColumn(_("Figures"), wxLIST_FORMAT_LEFT, 330);
    _list->AppendColumn(_("Chosen"), wxLIST_FORMAT_LEFT, 150);
    sizer->Add(_list, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->Add(new wxButton(this, ID_PICK, _("Choose the Figures...")), 0, wxRIGHT, 4);
    buttons->Add(new wxButton(this, ID_AUTOMATIC, _("Let the Composer Choose")), 0);
    sizer->Add(buttons, 0, wxALL, 8);
    sizer->Add(new wxStaticText(this, wxID_ANY, _("To look at:")), 0, wxLEFT | wxRIGHT, 8);
    _problems = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 130), wxTE_MULTILINE | wxTE_READONLY);
    sizer->Add(_problems, 0, wxEXPAND | wxALL, 8);
    sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
    SetSizer(sizer);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AskFigures(); }, ID_PICK);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!Selected().empty()) {
            Automatic(Selected());
        }
    }, ID_AUTOMATIC);
    _list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) { AskFigures(); });
    Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
        std::string error;
        if (_changed && !Save(error)) {
            wxMessageBox(wxString::Format(_("Your choices could not be saved: %s"), U(error)), _("Assign Voices and Characters"), wxOK | wxICON_WARNING, this);
            return;
        }
        e.Skip();
    }, wxID_OK);
    Recompute();
}

void PerformersDialog::Recompute() {
    MappingInput in;
    in.audio = &_song.audio;
    in.voices = &_song.voices;
    in.geometry = &_geometry;
    in.semantics = &_semantics;
    in.manual = &_song.manual;
    _map = MapPerformance(in);
    Fill();
}

void PerformersDialog::Fill() {
    const std::string keep = Selected();
    _list->DeleteAllItems();
    _rows.clear();
    for (const PerformerAssignment& a : _map.assignments) {
        const long row = _list->InsertItem(_list->GetItemCount(), a.sourceLabel.empty() ? U(a.source) : UnderstandingWords(a.sourceLabel));
        _list->SetItem(row, 1, UnderstandingWords(FunctionPhrase(a.function)));
        wxString figures;
        for (const std::string& f : a.figures) {
            figures += (figures.empty() ? "" : ", ") + U(f);
        }
        _list->SetItem(row, 2, figures.empty() ? _("(nobody)") : figures);
        _list->SetItem(row, 3, a.state == "manual" ? _("by you") : _("by the composer"));
        if (!a.conflicts.empty()) {
            _list->SetItemTextColour(row, wxColour(170, 60, 0));
        }
        _rows.push_back(a.source);
        if (a.source == keep) {
            _list->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        }
    }
    _problems->SetValue(ProblemsText());
}

wxString PerformersDialog::ProblemsText() const {
    wxString text;
    for (const PerformerAssignment& a : _map.assignments) {
        for (const std::string& c : a.conflicts) {
            const size_t colon = c.find(':');
            const std::string subject = colon == std::string::npos ? std::string() : c.substr(colon + 1);
            text += (a.sourceLabel.empty() ? U(a.source) : UnderstandingWords(a.sourceLabel)) + (subject.empty() ? wxString() : " (" + U(subject.substr(0, subject.find(':'))) + ")") + ": " +
                    UnderstandingWords(ConflictPhrase(c)) + "\n";
        }
    }
    return text.empty() ? _("Nothing to look at.") : text;
}

std::string PerformersDialog::Selected() const {
    const long i = _list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    return i >= 0 && i < static_cast<long>(_rows.size()) ? _rows[static_cast<size_t>(i)] : std::string();
}

bool PerformersDialog::Assign(const std::string& source, const std::vector<std::string>& figures) {
    const PerformerAssignment* current = _map.For(source);
    if (current == nullptr) {
        return false;
    }
    PerformerAssignment a = *current;
    a.figures = figures;
    a.state = "manual";
    a.confidence = 1;
    a.conflicts.clear();
    a.reasons = { "chosen by a person" };
    std::erase_if(_song.manual, [&](const PerformerAssignment& m) { return m.source == source; });
    _song.manual.push_back(a);
    _changed = true;
    Recompute();
    return true;
}

bool PerformersDialog::Automatic(const std::string& source) {
    const size_t before = _song.manual.size();
    std::erase_if(_song.manual, [&](const PerformerAssignment& m) { return m.source == source; });
    _changed = _changed || before != _song.manual.size();
    Recompute();
    return before != _song.manual.size();
}

bool PerformersDialog::Save(std::string& error) {
    if (!SaveManualAssignments(_song.folder, _song.manual, error)) {
        return false;
    }
    _changed = false;
    return true;
}

void PerformersDialog::AskFigures() {
    const std::string source = Selected();
    const PerformerAssignment* a = _map.For(source);
    if (a == nullptr) {
        return;
    }
    const bool speaks = a->function == "speak" || a->function == "sing" || a->function == "choral-response";
    wxArrayString names;
    std::vector<std::string> figures;
    wxArrayInt chosen;
    for (const FigureProfile& p : _semantics.profiles) {
        const FigureGeometry* g = _geometry.Find(p.figure);
        wxString label = U(p.figure);
        if (speaks && !p.CanSpeak()) {
            label += " - " + _("no mouth");
        }
        if (g != nullptr && g->noController) {
            label += " - " + _("no controller");
        }
        if (std::find(a->figures.begin(), a->figures.end(), p.figure) != a->figures.end()) {
            chosen.Add(static_cast<int>(figures.size()));
        }
        names.Add(label);
        figures.push_back(p.figure);
    }
    wxMultiChoiceDialog ask(this, wxString::Format(_("Which figures perform \"%s\"?"), a->sourceLabel.empty() ? U(a->source) : UnderstandingWords(a->sourceLabel)), _("Choose the Figures"), names);
    ask.SetSelections(chosen);
    if (ask.ShowModal() != wxID_OK) {
        return;
    }
    std::vector<std::string> picked;
    for (int i : ask.GetSelections()) {
        picked.push_back(figures[static_cast<size_t>(i)]);
    }
    Assign(source, picked);
}

// ---------------------------------------------------------------- render check

RenderCheckDialog::RenderCheckDialog(wxWindow* parent, RenderUnderstanding check, std::function<void(const std::vector<int>&, const std::vector<std::string>&)> takePictures) :
    wxDialog(parent, wxID_ANY, _("What the Figures Did"), wxDefaultPosition, wxSize(900, 560), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    _check(std::move(check)), _takePictures(std::move(takePictures)) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    int windows = 0, honoured = 0;
    for (const IntentionCheck& c : _check.checks) {
        windows += c.windows;
        honoured += c.honoured;
    }
    sizer->Add(new wxStaticText(this, wxID_ANY, wxString::Format(_("The figures did what was meant in %d of %d moments. Below, the moments to look at in xLights."), honoured, windows)), 0,
               wxEXPAND | wxALL, 8);
    sizer->Add(Muted(this, _("This reads how much and which lights shine, not whether a picture or a word is the right one. Nothing is approved here: you decide, in xLights.")), 0,
               wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    _list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 300), wxLC_REPORT);
    _list->AppendColumn(_("When"), wxLIST_FORMAT_LEFT, 120);
    _list->AppendColumn(_("Figure"), wxLIST_FORMAT_LEFT, 160);
    _list->AppendColumn(_("What to look at"), wxLIST_FORMAT_LEFT, 520);
    for (const RenderFinding& f : _check.findings) {
        const long row = _list->InsertItem(_list->GetItemCount(), ClockText(f.startMS) + " - " + ClockText(f.endMS));
        _list->SetItem(row, 1, U(f.figure) + (f.region.empty() ? wxString() : " (" + UnderstandingWords(RegionPhrase(f.region)) + ")"));
        _list->SetItem(row, 2, UnderstandingWords(FindingPhrase(f.kind)));
        if (f.severity == "warning") {
            _list->SetItemTextColour(row, wxColour(170, 60, 0));
        }
    }
    sizer->Add(_list, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* pictures = new wxButton(this, wxID_ANY, wxString::Format(_("Pictures of the Moments (%d)"), static_cast<int>(_check.captures.size())));
    pictures->Enable(_takePictures != nullptr && !_check.captures.empty());
    row->Add(pictures, 0);
    sizer->Add(row, 0, wxALL, 8);
    sizer->Add(CreateStdDialogButtonSizer(wxOK), 0, wxEXPAND | wxALL, 8);
    SetSizer(sizer);
    pictures->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        std::vector<int> times;
        std::set<std::string> figures;
        for (const CaptureMoment& c : _check.captures) {
            times.push_back(c.timeMS);
            figures.insert(c.figures.begin(), c.figures.end());
        }
        _takePictures(times, std::vector<std::string>(figures.begin(), figures.end()));
    });
}

int RenderCheckDialog::Rows() const {
    return _list->GetItemCount();
}
