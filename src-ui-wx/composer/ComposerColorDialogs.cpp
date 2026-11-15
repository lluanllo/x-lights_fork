/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "ComposerColorDialogs.h"

#include <wx/bitmap.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/choice.h>
#include <wx/choicdlg.h>
#include <wx/colordlg.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/imaglist.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include "ComposerStageDialogs.h"
#include "ComposerTimeline.h"
#include "composer/ColorSpace.h"
#include "composer/UnderstandingStore.h"

namespace fs = std::filesystem;
using namespace composer;

namespace {

wxString U(const std::string& s) {
    return wxString::FromUTF8(s);
}

std::string S(const wxString& s) {
    return s.ToStdString(wxConvUTF8);
}

wxString ClockText(int ms) {
    const int s = std::max(0, ms) / 1000;
    return wxString::Format("%d:%02d", s / 60, s % 60);
}

// The core's own sentences, in the person's language.
wxString Words(const std::string& coreText) {
    return wxGetTranslation(U(coreText));
}

bool WriteAll(const std::string& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream out(fs::path(path), std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

wxStaticText* Muted(wxWindow* parent, const wxString& text, int wrap = 760) {
    auto* t = new wxStaticText(parent, wxID_ANY, text);
    t->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    t->Wrap(wrap);
    return t;
}

wxBitmap PaletteBitmap(const std::vector<std::string>& colors, int w, int h) {
    wxBitmap bmp(w, h);
    wxMemoryDC dc(bmp);
    dc.SetBackground(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)));
    dc.Clear();
    if (!colors.empty()) {
        const int band = std::max(1, w / static_cast<int>(colors.size()));
        for (size_t i = 0; i < colors.size(); ++i) {
            const wxColour c(U(colors[i]));
            dc.SetBrush(wxBrush(c));
            dc.SetPen(wxPen(c));
            dc.DrawRectangle(static_cast<int>(i) * band, 0, i + 1 == colors.size() ? w - static_cast<int>(i) * band : band, h);
        }
    }
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.SetPen(wxPen(wxColour(120, 120, 120)));
    dc.DrawRectangle(0, 0, w, h);
    dc.SelectObject(wxNullBitmap);
    return bmp;
}

std::vector<std::string> MainColors(const ColorPaletteProfile& p) {
    std::vector<std::string> out;
    for (const char* role : { "anchor", "secondary", "accent", "contrast" }) {
        const std::string hex = p.Hex(role);
        if (!hex.empty()) {
            out.push_back(hex);
        }
    }
    return out;
}

wxString EvolutionWords(const std::string& e) {
    static const std::map<std::string, wxString> W = {
        { "establish", _("The colours are presented, contained") }, { "develop", _("The family develops") }, { "expand", _("More contrast and saturation") },
        { "climax", _("The widest palette of the song") },         { "shift", _("Another temperature or family") }, { "resolve", _("Back to the main colour, resolved") },
        { "return", _("A part that comes back to its colours") },    { "hold", _("The palette holds") },             { "rest", _("Quiet") }
    };
    const auto it = W.find(e);
    return it != W.end() ? it->second : U(e);
}

// The parts of a song, with the names the timeline uses.
wxString PartWords(const std::string& kind) {
    static const std::map<std::string, wxString> W = { { "intro", _("Intro") }, { "verse", _("Verse") }, { "chorus", _("Chorus") }, { "bridge", _("Bridge") },
                                                       { "instrumental", _("Instrumental") }, { "outro", _("Outro") }, { "pre-chorus", _("Pre-chorus") }, { "interlude", _("Interlude") } };
    const auto it = W.find(kind);
    return it != W.end() ? it->second : _("Section");
}

wxString FeelingWords(const std::string& f) {
    static const std::map<std::string, wxString> W = { { "joyful", _("joyful") }, { "tender", _("tender") }, { "epic", _("epic") }, { "playful", _("playful") },
                                                       { "nostalgic", _("nostalgic") }, { "dark", _("dark") }, { "calm", _("calm") }, { "festive", _("festive") } };
    const auto it = W.find(f);
    return it != W.end() ? it->second : U(f);
}

wxString SourceWords(const std::string& source) {
    // "groq:<model>:<origin>" or "groq:<model> (...)" or "local"
    if (source.rfind("groq:", 0) == 0) {
        if (source.find(":cache") != std::string::npos || source.find("kept answer") != std::string::npos) {
            return _("Groq (an answer kept from before)");
        }
        if (source.find(":file") != std::string::npos || source.find("read from a file") != std::string::npos) {
            return _("Groq (an answer saved in a file)");
        }
        if (source.find(":request") != std::string::npos || source.find("asked now") != std::string::npos) {
            return _("Groq (asked now)");
        }
        return _("Groq");
    }
    if (source == "local" || source.rfind("local", 0) == 0) {
        return _("the lyrics, read on this computer");
    }
    if (source.empty()) {
        return _("nothing: the song's key and your choices");
    }
    return U(source);
}

wxString GestureIntentWords(const std::string& intent) {
    if (intent == "reinforce") return _("it reinforces the scene's colour");
    if (intent == "accent") return _("an accent for a strong moment");
    if (intent == "transform") return _("it prepares the next scene's colour");
    if (intent == "resolve") return _("it resolves to the identity");
    return U(intent);
}

wxString ColorWarningWords(const std::string& w) {
    const std::string code = w.substr(0, w.find(':'));
    static const std::map<std::string, wxString> W = {
        { "house-in-one-colour", _("For much of the show, one colour holds most of the house.") },
        { "long-stretch-in-one-colour", _("There is a long stretch with the house in the same colour.") },
        { "main-colour-never-changes", _("Every scene leads with the same colour.") },
        { "scenes-repeat-their-colours", _("The scenes repeat their colours from one to the next.") },
        { "figures-alike", _("The figures are almost the same colour.") },
        { "few-accents", _("Few accents: a minority colour rarely appears.") },
        { "gestures-blend-in", _("Many gestures are lit in colours close to what was already there.") },
        { "too-many-colours-at-once", _("In some scenes more than four colour families share the house at once.") },
        { "colours-scattered", _("Much of the light goes to colours that are each used only a little: the colours look scattered.") },
        { "figures-in-many-colours", _("Each figure carries many colours at once.") },
    };
    const auto it = W.find(code);
    return it != W.end() ? it->second : U(w);
}

} // namespace

wxString ColorFamilyWords(const std::string& family);

namespace {
// The colours a scene really uses (its grammar), main first.
std::vector<std::string> UsedColors(const ChromaticScenePlan& s) {
    std::vector<std::string> out;
    for (const char* role : { "anchor", "secondary", "contrast", "accent", "transition", "highlight", "neutral" }) {
        if (std::find(s.used.begin(), s.used.end(), role) != s.used.end() || s.used.empty()) {
            const std::string hex = s.palette.Hex(role);
            if (!hex.empty() && std::find(out.begin(), out.end(), hex) == out.end()) {
                out.push_back(hex);
            }
        }
        if (s.used.empty() && out.size() >= 4) {
            break;
        }
    }
    return out;
}

wxString MotifKindWords(const std::string& kind) {
    if (kind == "identity") return _("the song's identity");
    if (kind == "contrast") return _("the identity with a contrast");
    if (kind == "transformed") return _("the identity transformed");
    return U(kind);
}

wxString OperationWords(const std::string& op) {
    static const std::map<std::string, wxString> W = { { "present", _("Presents the identity") }, { "develop", _("Develops it") }, { "introduce", _("Brings a contrast") },
                                                       { "recall", _("Brings an idea back") }, { "intensify", _("Brings it back stronger") }, { "transform", _("Transforms it") },
                                                       { "resolve", _("Resolves to the identity") }, { "hold", _("Keeps the colours before") }, { "rest", _("Rests") } };
    const auto it = W.find(op);
    return it != W.end() ? it->second : U(op);
}

wxString IntentWords(const std::string& intent) {
    static const std::map<std::string, wxString> W = { { "open", _("opens the song and presents its colours") }, { "tell", _("tells, with the song's own colours") },
                                                       { "lift", _("lifts: a contrast comes in") }, { "celebrate", _("celebrates: the fullest moment") },
                                                       { "answer", _("call and answer between figures") }, { "turn", _("turns somewhere else") },
                                                       { "close", _("closes and resolves") }, { "rest", _("rests") } };
    const auto it = W.find(intent);
    return it != W.end() ? it->second : U(intent);
}

wxString Names(const std::vector<std::string>& names, size_t most = 4) {
    wxString out;
    for (size_t k = 0; k < names.size() && k < most; ++k) {
        out += (k == 0 ? "" : ", ") + U(names[k]);
    }
    if (names.size() > most) {
        out += wxString::Format(_(" and %d more"), static_cast<int>(names.size() - most));
    }
    return out;
}

wxString ColourNames(const std::vector<std::string>& hexes) {
    wxString out;
    std::set<std::string> seen;
    for (const std::string& h : hexes) {
        const std::string family = ColorFamilyId(ColorFamily(OklchOfHex(h)));
        if (seen.insert(family).second) {
            out += (out.empty() ? "" : ", ") + ColorFamilyWords(family);
        }
    }
    return out;
}

// A scene in plain words: what it wants to say, its colours, who carries them, what it keeps.
wxString SceneText(const ChromaticScenePlan& s, const ChromaticScore& score) {
    (void)score;
    wxString out = wxString::Format(_("From %s to %s (%s): it %s."), ClockText(s.startMS), ClockText(s.endMS), PartWords(s.sectionKind), IntentWords(s.intent)) + "\n";
    if (!s.motif.empty()) {
        out += wxString::Format(_("Colour idea %s: %s."), U(s.motif), OperationWords(s.motifOperation)) + "\n";
    }
    const wxString ranges = s.hueBudget <= 1 ? _("all from one range of neighbouring hues") : s.hueBudget == 2 ? _("from two ranges of hues") : _("from three ranges of hues");
    out += wxString::Format(_("Colours in use: %s (%s)"), ColourNames(UsedColors(s)), ranges) + "\n";
    if (!s.kept.empty()) {
        out += wxString::Format(_("It keeps: %s"), ColourNames(s.kept)) + "\n";
    }
    if (!s.introduced.empty()) {
        out += wxString::Format(_("It brings: %s"), ColourNames(s.introduced)) + "\n";
    }
    if (!s.protagonist.empty()) {
        out += wxString::Format(_("Carried by: %s"), U(s.protagonist)) + "\n";
    }
    if (!s.companions.empty()) {
        out += wxString::Format(_("Accompanied by: %s"), Names(s.companions)) + "\n";
    }
    if (!s.discreet.empty()) {
        out += wxString::Format(_("Discreet: %s"), Names(s.discreet)) + "\n";
    }
    if (!s.feeling.empty()) {
        out += wxString::Format(_("According to %s, this part feels %s."), SourceWords(s.feelingSource), FeelingWords(s.feeling)) + "\n";
    }
    std::map<std::string, std::vector<std::string>> byRole;
    for (const FigureColorRole& r : s.figures) {
        byRole[r.role].push_back(r.figure);
    }
    for (const char* role : { "main", "complement", "accent", "response", "decorative", "background", "identity", "manual" }) {
        if (const auto it = byRole.find(role); it != byRole.end()) {
            out += Words(ColorRolePhrase(role)) + ": " + Names(it->second, 5) + "\n";
        }
    }
    for (const GestureChromaticProfile& g : s.gestures) {
        out += wxString::Format(_("Gesture at %s: %s, in %s"), ClockText(std::strtol(g.gesture.substr(g.gesture.find('@') + 1).c_str(), nullptr, 10)), GestureIntentWords(g.intent),
                                ColorFamilyWords(ColorFamilyId(ColorFamily(OklchOfHex(g.front))))) +
               "\n";
    }
    out += wxString::Format(_("Into the next scene: %s."), Words(ChromaticTransitionPhrase(s.toNext.kind))) + "\n";
    return out;
}

} // namespace

wxString ColorFamilyWords(const std::string& family) {
    static const std::map<std::string, wxString> W = { { "red", _("red") },         { "orange", _("orange") },   { "amber", _("amber") }, { "yellow", _("yellow") },
                                                       { "green", _("green") },     { "emerald", _("emerald") }, { "cyan", _("cyan") },   { "azure", _("light blue") },
                                                       { "blue", _("blue") },       { "violet", _("violet") },   { "magenta", _("magenta") }, { "pink", _("pink") },
                                                       { "white", _("white") },     { "neutral", _("grey") } };
    const auto it = W.find(family);
    return it != W.end() ? it->second : U(family);
}

wxString ArtisticStatusText(const ArtisticCheck& c) {
    if (c.status == "REAL_REQUEST") {
        return _("Groq was asked now and answered.");
    }
    if (c.status == "CACHE_HIT") {
        return _("An answer Groq gave before is used: nothing was sent now.");
    }
    if (c.status == "LOCAL_FALLBACK") {
        return _("Groq was not asked: the colours read the lyrics on this computer.");
    }
    if (c.status == "NOT_CONFIGURED") {
        return _("There is no Groq key: the colours read the lyrics on this computer.");
    }
    if (c.status == "ERROR") {
        return _("Groq was asked but did not give a usable answer: the colours read the lyrics on this computer.");
    }
    return U(c.status);
}

// ============================================================ swatches

ColorSwatches::ColorSwatches(wxWindow* parent, const wxSize& size) :
    wxPanel(parent, wxID_ANY, wxDefaultPosition, size, wxBORDER_SIMPLE) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        const wxSize s = GetClientSize();
        dc.SetBackground(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE)));
        dc.Clear();
        if (_colors.empty()) {
            return;
        }
        const int band = std::max(1, s.x / static_cast<int>(_colors.size()));
        for (size_t i = 0; i < _colors.size(); ++i) {
            const wxColour c(U(_colors[i]));
            dc.SetBrush(wxBrush(c));
            dc.SetPen(wxPen(c));
            dc.DrawRectangle(static_cast<int>(i) * band, 0, i + 1 == _colors.size() ? s.x - static_cast<int>(i) * band : band, s.y);
        }
    });
}

void ColorSwatches::SetColors(const std::vector<std::string>& colors) {
    _colors = colors;
    Refresh();
}

std::vector<TimelineColors> TimelineColorsOf(const ChromaticScore& score) {
    std::vector<TimelineColors> out;
    for (size_t i = 0; i < score.scenes.size(); ++i) {
        const ChromaticScenePlan& sp = score.scenes[i];
        TimelineColors c;
        c.startMS = sp.startMS;
        c.endMS = sp.endMS;
        for (const std::string& hex : MainColors(sp.palette)) {
            c.colors.push_back(wxColour(U(hex)));
        }
        if (sp.toNext.kind != "cut" && sp.toNext.kind != "hold" && sp.toNext.startMS < sp.endMS && i + 1 < score.scenes.size()) {
            c.transitionMS = sp.toNext.startMS;
            for (const std::string& hex : MainColors(score.scenes[i + 1].palette)) {
                c.next.push_back(wxColour(U(hex)));
            }
        }
        c.manual = sp.manual;
        c.label = sp.motif.empty() ? std::string() : sp.motif + (sp.motifOperation == "recall" ? "=" : sp.motifOperation == "intensify" ? "+" : sp.motifOperation == "transform" ? "~" : "");
        out.push_back(c);
    }
    return out;
}

// ============================================================ director panel

ColorDirectorPanel::ColorDirectorPanel(wxWindow* parent) :
    wxPanel(parent) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    _on = new wxCheckBox(this, wxID_ANY, _("Direct the colours scene by scene and figure by figure"));
    _on->SetValue(true);
    sizer->Add(_on, 0, wxBOTTOM, 4);
    _state = new wxStaticText(this, wxID_ANY, "");
    sizer->Add(_state, 0, wxEXPAND | wxBOTTOM, 4);
    auto* song = new wxBoxSizer(wxHORIZONTAL);
    song->Add(new wxStaticText(this, wxID_ANY, _("The song's colours:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    _song = new ColorSwatches(this, wxSize(200, 20));
    song->Add(_song, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    _songText = new wxStaticText(this, wxID_ANY, "");
    song->Add(_songText, 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(song, 0, wxEXPAND | wxBOTTOM, 4);
    auto* body = new wxBoxSizer(wxHORIZONTAL);
    _list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(500, 180), wxLC_REPORT | wxLC_SINGLE_SEL);
    _images = new wxImageList(64, 14, false);
    _list->SetImageList(_images, wxIMAGE_LIST_SMALL);
    for (const auto& [label, width] : std::vector<std::pair<wxString, int>>{ { _("Colours"), 90 }, { _("From"), 50 }, { _("Part"), 80 }, { _("Motif"), 55 }, { _("What it does"), 150 }, { _("Changed"), 65 } }) {
        _list->AppendColumn(label, wxLIST_FORMAT_LEFT, width);
    }
    body->Add(_list, 3, wxEXPAND | wxRIGHT, 6);
    _detail = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(300, 180), wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
    body->Add(_detail, 2, wxEXPAND);
    sizer->Add(body, 1, wxEXPAND | wxBOTTOM, 4);
    const auto button = [&](wxSizer* row, const wxString& label, std::function<void()> action) {
        auto* b = new wxButton(this, wxID_ANY, label);
        b->Bind(wxEVT_BUTTON, [action](wxCommandEvent&) { action(); });
        row->Add(b, 0, wxRIGHT, 4);
    };
    auto* edits = new wxBoxSizer(wxHORIZONTAL);
    button(edits, _("Keep This Identity"), [this]() { (void)KeepIdentity(); });
    button(edits, _("Bring a Motif Back..."), [this]() {
        if (_score.motifDefs.empty()) {
            return;
        }
        wxArrayString choices;
        for (const ChromaticMotif& m : _score.motifDefs) {
            choices.Add(wxString::Format("%s - %s", U(m.id), MotifKindWords(m.kind)));
        }
        const int pick = wxGetSingleChoiceIndex(_("Which colour idea should come back in this scene?"), _("Art Direction"), choices, this);
        if (pick >= 0) {
            (void)RecallMotif(_score.motifDefs[static_cast<size_t>(pick)].id);
        }
    });
    button(edits, _("Fewer Colours"), [this]() { (void)FewerColours(); });
    button(edits, _("More Contrast"), [this]() {
        if (const ChromaticScenePlan* s = Scene()) {
            (void)Edit({ "more-contrast", s->startMS, "", "", "", "" });
        }
    });
    button(edits, _("Fewer Accents"), [this]() { (void)FewerAccents(); });
    sizer->Add(edits, 0, wxBOTTOM, 4);
    auto* edits2 = new wxBoxSizer(wxHORIZONTAL);
    button(edits2, _("Change the Main Colour..."), [this]() { AskColor(); });
    button(edits2, _("Fix This Combination"), [this]() { (void)FixCombination(); });
    button(edits2, _("Undo the Last Change"), [this]() { (void)Undo(); });
    button(edits2, _("Back to the Original Plan"), [this]() {
        if (wxMessageBox(_("Drop every change you made to the colours of this song?"), _("Art Direction"), wxYES_NO | wxICON_QUESTION, this) == wxYES) {
            (void)Reset();
        }
    });
    button(edits2, _("Apply the Changes"), [this]() {
        if (onRecompose) {
            onRecompose();
        }
    });
    sizer->Add(edits2, 0, wxBOTTOM, 4);
    auto* tools = new wxBoxSizer(wxHORIZONTAL);
    button(tools, _("Compare Scenes..."), [this]() { CompareScenes(); });
    button(tools, _("Colours of the Figures..."), [this]() { OpenFigures(); });
    button(tools, _("Colours of the Show..."), [this]() { OpenReview(); });
    sizer->Add(tools, 0);
    SetSizer(sizer);
    _list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& e) {
        _scene = static_cast<int>(e.GetIndex());
        ShowScene();
    });
    _on->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        Fill();
        if (onDirectorChanged) {
            onDirectorChanged();
        }
    });
}

bool ColorDirectorPanel::SetReport(const ReportFolder* report, const std::string& showDir) {
    const std::string hash = report != nullptr ? report->analysis.audioHash : std::string();
    const std::string editsPath = !showDir.empty() && !hash.empty() ? ChromaticEditsPath(showDir, hash) : std::string();
    const std::string text = report != nullptr ? report->colorScore : std::string();
    if (report == _report && text == _scoreText && editsPath == _editsPath) {
        return false;
    }
    _report = report;
    _showDir = showDir;
    _scoreText = text;
    _score = ChromaticScore();
    std::string error;
    if (!text.empty()) {
        (void)FromJson(text, _score, error);
    }
    if (editsPath != _editsPath) {
        _editsPath = editsPath;
        _undo.clear();
        (void)LoadEdits();
    }
    Fill();
    return true;
}

bool ColorDirectorPanel::DirectorOn() const {
    return _on->GetValue();
}

void ColorDirectorPanel::SetDirectorOn(bool on) {
    _on->SetValue(on);
    Fill();
}

std::string ColorDirectorPanel::EditsText() const {
    return _edits.edits.empty() ? std::string() : ToJson(_edits);
}

bool ColorDirectorPanel::LoadEdits() {
    _edits = ChromaticEdits();
    std::string text, error;
    return _editsPath.empty() || !ReadTextFile(_editsPath, text) || FromJson(text, _edits, error);
}

bool ColorDirectorPanel::SaveEdits() {
    if (_editsPath.empty()) {
        return false;
    }
    if (_edits.edits.empty()) {
        std::error_code ec;
        fs::remove(fs::path(_editsPath), ec);
        return true;
    }
    return WriteAll(_editsPath, ToJson(_edits));
}

const ChromaticScenePlan* ColorDirectorPanel::Scene() const {
    return _scene >= 0 && _scene < static_cast<int>(_score.scenes.size()) ? &_score.scenes[static_cast<size_t>(_scene)] : nullptr;
}

void ColorDirectorPanel::Fill() {
    _list->DeleteAllItems();
    _images->RemoveAll();
    if (!DirectorOn()) {
        _state->SetLabel(_("The colours are not directed: each scene uses its palette on every figure, as before."));
        _song->SetColors({});
        _songText->SetLabel("");
        _detail->SetValue("");
        return;
    }
    if (_score.scenes.empty()) {
        _state->SetLabel(_("Work out the composition on step 3 to see the colours of each scene."));
        _song->SetColors({});
        _songText->SetLabel("");
        _detail->SetValue("");
        return;
    }
    _song->SetColors(MainColors(_score.song));
    _songText->SetLabel(wxString::Format(_("reading of the song: %s"), SourceWords(_score.briefSource)));
    for (const ChromaticScenePlan& s : _score.scenes) {
        const int image = _images->Add(PaletteBitmap(UsedColors(s), 64, 14));
        const long row = _list->InsertItem(_list->GetItemCount(), "", image);
        _list->SetItem(row, 1, ClockText(s.startMS));
        _list->SetItem(row, 2, PartWords(s.sectionKind));
        _list->SetItem(row, 3, U(s.motif));
        _list->SetItem(row, 4, OperationWords(s.motifOperation));
        _list->SetItem(row, 5, s.manual ? _("By you") : "");
    }
    wxString state = wxString::Format(_("%d scenes; %d colour ideas that come and go through the song."), static_cast<int>(_score.scenes.size()), static_cast<int>(_score.motifDefs.size()));
    if (!_edits.edits.empty()) {
        state += " " + wxString::Format(_("%d changes of yours are saved; press \"Apply the Changes\" to see them."), static_cast<int>(_edits.edits.size()));
    }
    _state->SetLabel(state);
    if (_scene >= 0 && _scene < _list->GetItemCount()) {
        _list->SetItemState(_scene, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    }
    ShowScene();
}

wxString ColorDirectorPanel::Detail() const {
    return _detail->GetValue();
}

void ColorDirectorPanel::ShowScene() {
    const ChromaticScenePlan* s = Scene();
    if (s == nullptr) {
        _detail->SetValue(_("Choose a scene to see what it wants to say, its colours, who carries them and why."));
        return;
    }
    _detail->SetValue(SceneText(*s, _score));
}

std::string ColorDirectorPanel::KeepIdentity() {
    const ChromaticScenePlan* s = Scene();
    return s == nullptr ? std::string("no-scene") : Edit({ "keep-identity", s->startMS, "", "", "", "" });
}

std::string ColorDirectorPanel::RecallMotif(const std::string& motif) {
    const ChromaticScenePlan* s = Scene();
    return s == nullptr ? std::string("no-scene") : Edit({ "recall-motif", s->startMS, "", "", motif, "" });
}

std::string ColorDirectorPanel::FewerColours() {
    const ChromaticScenePlan* s = Scene();
    return s == nullptr ? std::string("no-scene") : Edit({ "fewer-colours", s->startMS, "", "", "", "" });
}

std::string ColorDirectorPanel::FewerAccents() {
    const ChromaticScenePlan* s = Scene();
    return s == nullptr ? std::string("no-scene") : Edit({ "fewer-accents", s->startMS, "", "", "", "" });
}

std::string ColorDirectorPanel::FixCombination() {
    const ChromaticScenePlan* s = Scene();
    if (s == nullptr) {
        return "no-scene";
    }
    std::string value;
    for (const PaletteColor& c : s->palette.colors) {
        value += (value.empty() ? "" : ";") + c.role + "=" + c.hex;
    }
    return Edit({ "lock-palette", s->startMS, "", "", value, "" });
}

std::string ColorDirectorPanel::ChangeMainColour(const std::string& hex) {
    const ChromaticScenePlan* s = Scene();
    return s == nullptr ? std::string("no-scene") : Edit({ "color", s->startMS, "", "anchor", hex, "" });
}

void ColorDirectorPanel::CompareScenes() {
    if (_score.scenes.size() < 2) {
        return;
    }
    const int first = std::max(0, _scene);
    SceneCompareDialog d(this, _score, first, first + 1 < static_cast<int>(_score.scenes.size()) ? first + 1 : first - 1);
    d.ShowModal();
}

bool ColorDirectorPanel::SelectScene(int index) {
    if (index < 0 || index >= static_cast<int>(_score.scenes.size())) {
        return false;
    }
    _scene = index;
    if (index < _list->GetItemCount()) {
        _list->SetItemState(index, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
        _list->EnsureVisible(index);
    }
    ShowScene();
    return true;
}

std::string ColorDirectorPanel::Edit(const ChromaticEdit& edit) {
    if (_editsPath.empty()) {
        return "no-song";
    }
    _undo.push_back(ToJson(_edits));
    // A new choice of the same kind for the same scene, role or figure replaces the earlier one.
    std::erase_if(_edits.edits, [&](const ChromaticEdit& e) {
        const bool contrast = (e.action == "more-contrast" || e.action == "less-contrast") && (edit.action == "more-contrast" || edit.action == "less-contrast");
        const bool family = (e.action == "monochrome" || e.action == "no-monochrome") && (edit.action == "monochrome" || edit.action == "no-monochrome");
        const bool own = (e.action == "identity" || e.action == "no-identity" || e.action == "figure-color") && (edit.action == "identity" || edit.action == "no-identity" || edit.action == "figure-color");
        const bool clear = edit.action == "clear" && !edit.figure.empty() && e.figure == edit.figure;
        return clear || (e.sceneStartMS == edit.sceneStartMS && e.figure == edit.figure && e.role == edit.role && (e.action == edit.action || contrast || family || own));
    });
    if (edit.action != "clear") {
        _edits.edits.push_back(edit);
    }
    if (!SaveEdits()) {
        return "edits-not-saved";
    }
    Fill();
    return "";
}

std::string ColorDirectorPanel::Undo() {
    if (_undo.empty()) {
        return "nothing-to-undo";
    }
    std::string error;
    ChromaticEdits before;
    if (!FromJson(_undo.back(), before, error)) {
        return "undo-unreadable";
    }
    _undo.pop_back();
    _edits = before;
    if (!SaveEdits()) {
        return "edits-not-saved";
    }
    Fill();
    return "";
}

std::string ColorDirectorPanel::Reset() {
    if (_editsPath.empty()) {
        return "no-song";
    }
    _undo.push_back(ToJson(_edits));
    _edits = ChromaticEdits();
    if (!SaveEdits()) {
        return "edits-not-saved";
    }
    Fill();
    return "";
}

void ColorDirectorPanel::AskColor() {
    const ChromaticScenePlan* s = Scene();
    if (s == nullptr) {
        wxMessageBox(_("Choose a scene first."), _("Art Direction"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    wxColourData data;
    data.SetColour(wxColour(U(s->palette.Hex("anchor"))));
    wxColourDialog dialog(this, &data);
    if (dialog.ShowModal() != wxID_OK) {
        return;
    }
    (void)ChangeMainColour(S(dialog.GetColourData().GetColour().GetAsString(wxC2S_HTML_SYNTAX)));
}

void ColorDirectorPanel::OpenFigures() {
    auto* d = new FigureColorsDialog(this, this);
    d->ShowScene(std::max(0, _scene));
    d->ShowModal();
    d->Destroy();
}

void ColorDirectorPanel::OpenReview() {
    const ReportFolder* r = resultReport ? resultReport() : nullptr;
    if (r == nullptr || r->colorCheck.empty()) {
        wxMessageBox(_("Generate the show first: its colours are measured on xLights' own render."), _("Colours of the Show"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    ColorReviewDialog d(this, *r, resultFseq ? resultFseq() : std::string());
    d.ShowModal();
}

// ============================================================ figures

FigureColorsDialog::FigureColorsDialog(wxWindow* parent, ColorDirectorPanel* panel) :
    wxDialog(parent, wxID_ANY, _("Colours of the Figures"), wxDefaultPosition, wxSize(760, 520), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    _panel(panel) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(Muted(this, _("Which colour each figure carries in a scene, and why. A figure can keep its own colours (a trunk brown, foliage green), take the scene's palette, or have colours you choose. Your choices are applied when the show is composed again.")), 0, wxALL, 8);
    auto* nav = new wxBoxSizer(wxHORIZONTAL);
    auto* prev = new wxButton(this, wxID_ANY, _("< Previous Scene"));
    auto* next = new wxButton(this, wxID_ANY, _("Next Scene >"));
    _sceneText = new wxStaticText(this, wxID_ANY, "");
    nav->Add(prev, 0, wxRIGHT, 6);
    nav->Add(next, 0, wxRIGHT, 10);
    nav->Add(_sceneText, 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(nav, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
    _list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 320), wxLC_REPORT | wxLC_SINGLE_SEL);
    _images = new wxImageList(48, 14, false);
    _list->SetImageList(_images, wxIMAGE_LIST_SMALL);
    for (const auto& [label, width] : std::vector<std::pair<wxString, int>>{ { _("Colours"), 70 }, { _("Figure"), 190 }, { _("Its colour role"), 150 }, { _("Why"), 300 } }) {
        _list->AppendColumn(label, wxLIST_FORMAT_LEFT, width);
    }
    sizer->Add(_list, 1, wxEXPAND | wxALL, 8);
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    const auto button = [&](const wxString& label, std::function<void()> action) {
        auto* b = new wxButton(this, wxID_ANY, label);
        b->Bind(wxEVT_BUTTON, [action](wxCommandEvent&) { action(); });
        buttons->Add(b, 0, wxRIGHT, 4);
    };
    button(_("Keep Its Own Colours"), [this]() { (void)KeepOwn(Selected(), true); });
    button(_("Let It Take the Palette"), [this]() { (void)KeepOwn(Selected(), false); });
    button(_("Choose Its Colours..."), [this]() {
        const std::string f = Selected();
        if (f.empty()) {
            return;
        }
        wxColourDialog dialog(this);
        if (dialog.ShowModal() == wxID_OK) {
            (void)Choose(f, { S(dialog.GetColourData().GetColour().GetAsString(wxC2S_HTML_SYNTAX)) });
        }
    });
    button(_("Remove My Choice"), [this]() { (void)Clear(Selected()); });
    buttons->AddStretchSpacer();
    buttons->Add(new wxButton(this, wxID_OK, _("Close")), 0);
    sizer->Add(buttons, 0, wxEXPAND | wxALL, 8);
    SetSizer(sizer);
    prev->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShowScene(_scene - 1); });
    next->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShowScene(_scene + 1); });
}

void FigureColorsDialog::ShowScene(int index) {
    const int count = static_cast<int>(_panel->Score().scenes.size());
    _scene = count == 0 ? 0 : std::clamp(index, 0, count - 1);
    Fill();
}

int FigureColorsDialog::Rows() const {
    return _list->GetItemCount();
}

std::string FigureColorsDialog::Selected() const {
    const long i = _list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    return i >= 0 ? S(_list->GetItemText(i, 1)) : std::string();
}

void FigureColorsDialog::Fill() {
    _list->DeleteAllItems();
    _images->RemoveAll();
    const ChromaticScore& score = _panel->Score();
    if (score.scenes.empty()) {
        _sceneText->SetLabel(_("Work out the composition first."));
        return;
    }
    const ChromaticScenePlan& s = score.scenes[static_cast<size_t>(_scene)];
    _sceneText->SetLabel(wxString::Format(_("Scene %d of %d: %s, from %s"), _scene + 1, static_cast<int>(score.scenes.size()), PartWords(s.sectionKind), ClockText(s.startMS)));
    std::set<std::string> chosen;
    for (const ChromaticEdit& e : _panel->Edits().edits) {
        if (!e.figure.empty()) {
            chosen.insert(e.figure);
        }
    }
    for (const FigureColorRole& r : s.figures) {
        const long row = _list->InsertItem(_list->GetItemCount(), "", _images->Add(PaletteBitmap(r.colors, 48, 14)));
        _list->SetItem(row, 1, U(r.figure));
        _list->SetItem(row, 2, Words(ColorRolePhrase(r.role)) + (chosen.contains(r.figure) ? wxString(" *") : wxString()));
        _list->SetItem(row, 3, Words(r.why));
    }
}

std::string FigureColorsDialog::KeepOwn(const std::string& figure, bool keep) {
    if (figure.empty()) {
        return "no-figure";
    }
    const std::string r = _panel->Edit({ keep ? "identity" : "no-identity", -1, figure, "", "", "" });
    Fill();
    return r;
}

std::string FigureColorsDialog::Choose(const std::string& figure, const std::vector<std::string>& colors) {
    if (figure.empty() || colors.empty()) {
        return "no-figure";
    }
    std::string value;
    for (const std::string& c : colors) {
        value += (value.empty() ? "" : ",") + c;
    }
    const std::string r = _panel->Edit({ "figure-color", -1, figure, "", value, "" });
    Fill();
    return r;
}

std::string FigureColorsDialog::Clear(const std::string& figure) {
    if (figure.empty()) {
        return "no-figure";
    }
    const std::string r = _panel->Edit({ "clear", -1, figure, "", "", "" });
    Fill();
    return r;
}

// ============================================================ the show's colours

namespace {

// How the colour moved through the song: what the plan said and what the render showed.
class EvolutionStrip : public wxPanel {
public:
    EvolutionStrip(wxWindow* parent, const ChromaticScore& score, const ColorCheck& check) :
        wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 46), wxBORDER_SIMPLE), _score(score), _check(check) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE)));
            dc.Clear();
            const wxSize s = GetClientSize();
            int end = 1;
            for (const SceneColor& c : _check.scenes) {
                end = std::max(end, c.endMS);
            }
            for (const ChromaticScenePlan& p : _score.scenes) {
                end = std::max(end, p.endMS);
            }
            const auto x = [&](int ms) { return static_cast<int>(static_cast<double>(ms) / end * s.x); };
            dc.SetPen(*wxTRANSPARENT_PEN);
            for (const ChromaticScenePlan& p : _score.scenes) {
                const std::vector<std::string> colors = MainColors(p.palette);
                const int h = 20 / std::max<int>(1, static_cast<int>(colors.size()));
                for (size_t k = 0; k < colors.size(); ++k) {
                    dc.SetBrush(wxBrush(wxColour(U(colors[k]))));
                    dc.DrawRectangle(x(p.startMS), 2 + static_cast<int>(k) * h, std::max(1, x(p.endMS) - x(p.startMS)), h);
                }
            }
            for (const SceneColor& c : _check.scenes) {
                if (c.meanHex.empty()) {
                    continue;
                }
                dc.SetBrush(wxBrush(wxColour(U(c.meanHex))));
                dc.DrawRectangle(x(c.startMS), 24, std::max(1, x(c.endMS) - x(c.startMS)), 20);
            }
        });
    }

private:
    const ChromaticScore& _score;
    const ColorCheck& _check;
};

ColorCheck ReadCheck(const std::string& text) {
    ColorCheck c;
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (!root.is_object() || root.value("kind", std::string()) != "composer-color-check") {
        return c;
    }
    c.ran = root.value("ran", false);
    c.frames = root.value("frames", 0);
    c.litFrames = root.value("litFrames", 0);
    c.dominant = root.value("dominant", std::string());
    c.dominantShare = root.value("dominantShare", 0.0);
    c.effectiveColors = root.value("effectiveColors", 0.0);
    c.monotoneShare = root.value("monotoneShare", 0.0);
    c.longestMonotoneMS = root.value("longestMonotoneMS", 0);
    c.longestMonotoneFamily = root.value("longestMonotoneFamily", std::string());
    c.sceneDominants = root.value("sceneDominants", 0);
    c.sceneVariation = root.value("sceneVariation", 0.0);
    c.figureDifference = root.value("figureDifference", 0.0);
    c.figureFamilies = root.value("figureFamilies", 0.0);
    c.accentFrames = root.value("accentFrames", 0.0);
    c.gestureContrast = root.value("gestureContrast", 0.0);
    c.gestureHouseContrast = root.value("gestureHouseContrast", 0.0);
    c.gesturesFaint = root.value("gesturesFaint", 0);
    c.warnings = root.value("warnings", std::vector<std::string>());
    for (const nlohmann::json& s : root.value("scenes", nlohmann::json::array())) {
        SceneColor sc;
        sc.id = s.value("id", std::string());
        sc.startMS = s.value("startMS", 0);
        sc.endMS = s.value("endMS", 0);
        sc.dominant = s.value("dominant", std::string());
        sc.dominantShare = s.value("dominantShare", 0.0);
        sc.monotoneShare = s.value("monotoneShare", 0.0);
        sc.effectiveColors = s.value("effectiveColors", 0.0);
        sc.figureDifference = s.value("figureDifference", 0.0);
        sc.meanHex = s.value("meanHex", std::string());
        sc.intended = s.value("intended", false);
        c.scenes.push_back(sc);
    }
    for (const nlohmann::json& f : root.value("figureColors", nlohmann::json::array())) {
        FigureColorCheck fc;
        fc.name = f.value("name", std::string());
        fc.dominant = f.value("dominant", std::string());
        fc.dominantShare = f.value("dominantShare", 0.0);
        fc.litShare = f.value("litShare", 0.0);
        fc.sceneDominants = f.value("sceneDominants", 0);
        for (const nlohmann::json& s : f.value("families", nlohmann::json::array())) {
            fc.families.push_back({ s.value("family", std::string()), s.value("share", 0.0) });
        }
        c.figureChecks.push_back(fc);
    }
    for (const nlohmann::json& g : root.value("gestures", nlohmann::json::array())) {
        GestureColorCheck gc;
        gc.gesture = g.value("gesture", std::string());
        gc.againstBefore = g.value("againstBefore", 0.0);
        c.gestures.push_back(gc);
    }
    return c;
}

} // namespace

ColorReviewDialog::ColorReviewDialog(wxWindow* parent, const ReportFolder& report, const std::string& fseqPath) :
    wxDialog(parent, wxID_ANY, _("Colours of the Show"), wxDefaultPosition, wxSize(900, 720), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
    _check = ReadCheck(report.colorCheck);
    std::string error;
    if (!report.colorScore.empty()) {
        (void)FromJson(report.colorScore, _score, error);
    }
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(Muted(this, _("What xLights' own render put on the house, against what the colour plan meant. These are measurements, not a judgement: whether it looks good is for you to say, watching it in xLights.")), 0, wxALL, 8);
    sizer->Add(new wxStaticText(this, wxID_ANY, _("How the colour moves through the song (above: the plan; below: what the render showed)")), 0, wxLEFT | wxRIGHT, 8);
    sizer->Add(new EvolutionStrip(this, _score, _check), 0, wxEXPAND | wxALL, 8);
    _summary = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 130), wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
    sizer->Add(_summary, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
    sizer->Add(new wxStaticText(this, wxID_ANY, _("The composition, in words (signs, never a grade):")), 0, wxLEFT | wxRIGHT | wxTOP, 8);
    _composition = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 90), wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
    sizer->Add(_composition, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
    auto* lists = new wxBoxSizer(wxHORIZONTAL);
    _scenes = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 300), wxLC_REPORT | wxLC_SINGLE_SEL);
    auto* sceneImages = new wxImageList(60, 14, false);
    _scenes->AssignImageList(sceneImages, wxIMAGE_LIST_SMALL);
    for (const auto& [label, width] : std::vector<std::pair<wxString, int>>{ { _("Plan / render"), 80 }, { _("From"), 50 }, { _("Main colour"), 90 }, { _("In one colour"), 90 }, { _("Figures differ"), 90 } }) {
        _scenes->AppendColumn(label, wxLIST_FORMAT_LEFT, width);
    }
    for (const SceneColor& s : _check.scenes) {
        std::vector<std::string> both;
        const ChromaticScenePlan* p = _score.Scene(s.id);
        if (p != nullptr) {
            both = { p->palette.Hex("anchor"), p->palette.Hex("secondary"), p->palette.Hex("accent") };
        }
        both.push_back(s.meanHex.empty() ? "#000000" : s.meanHex);
        const long row = _scenes->InsertItem(_scenes->GetItemCount(), "", sceneImages->Add(PaletteBitmap(both, 60, 14)));
        _scenes->SetItem(row, 1, ClockText(s.startMS));
        _scenes->SetItem(row, 2, s.dominant.empty() ? wxString("-") : wxString::Format("%s %.0f%%", ColorFamilyWords(s.dominant), s.dominantShare * 100));
        _scenes->SetItem(row, 3, wxString::Format("%.0f%%%s", s.monotoneShare * 100, s.intended ? wxString(" ") + _("(on purpose)") : wxString()));
        _scenes->SetItem(row, 4, wxString::Format("%.0f", s.figureDifference));
    }
    lists->Add(_scenes, 1, wxEXPAND | wxRIGHT, 6);
    _figures = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 300), wxLC_REPORT | wxLC_SINGLE_SEL);
    for (const auto& [label, width] : std::vector<std::pair<wxString, int>>{ { _("Figure"), 160 }, { _("Its colours"), 220 } }) {
        _figures->AppendColumn(label, wxLIST_FORMAT_LEFT, width);
    }
    for (const FigureColorCheck& f : _check.figureChecks) {
        if (f.litShare <= 0) {
            continue;
        }
        const long row = _figures->InsertItem(_figures->GetItemCount(), U(f.name));
        wxString mix;
        for (size_t k = 0; k < f.families.size() && k < 3; ++k) {
            mix += wxString::Format("%s%s %.0f%%", k == 0 ? "" : ", ", ColorFamilyWords(f.families[k].family), f.families[k].share * 100);
        }
        _figures->SetItem(row, 1, mix);
    }
    lists->Add(_figures, 1, wxEXPAND);
    sizer->Add(lists, 1, wxEXPAND | wxALL, 8);
    auto* end = new wxBoxSizer(wxHORIZONTAL);
    auto* house = new wxButton(this, wxID_ANY, _("See This Scene in the House..."));
    house->Enable(!fseqPath.empty() && !report.stageLights.empty());
    end->Add(house, 0);
    end->AddStretchSpacer();
    end->Add(new wxButton(this, wxID_OK, _("Close")), 0);
    sizer->Add(end, 0, wxEXPAND | wxALL, 8);
    SetSizer(sizer);
    house->Bind(wxEVT_BUTTON, [this, &report, fseqPath](wxCommandEvent&) {
        const long i = _scenes->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        const SceneColor* s = i >= 0 && i < static_cast<long>(_check.scenes.size()) ? &_check.scenes[static_cast<size_t>(i)] : nullptr;
        StageMapDialog d(this, report, fseqPath, s == nullptr ? 0 : (s->startMS + s->endMS) / 2);
        d.ShowModal();
    });
    {
        const nlohmann::json root = nlohmann::json::parse(report.composition, nullptr, false);
        wxString words;
        static const std::map<std::string, wxString> NAMES = { { "cohesion", _("Cohesion") }, { "variety", _("Variety") }, { "contrast", _("Contrast") }, { "hierarchy", _("Hierarchy") },
                                                               { "continuity", _("Continuity") }, { "identity", _("Identity of the song") }, { "motifs", _("Motifs") },
                                                               { "distribution", _("Colours per figure") }, { "overload", _("Overload") }, { "gestures", _("Gestures") } };
        static const std::map<std::string, wxString> READINGS = {
            { "cohesive", _("cohesive") }, { "related", _("related colours") }, { "scattered", _("scattered") }, { "repetitive", _("repetitive") }, { "evolving", _("evolving") },
            { "restless", _("restless") }, { "flat", _("flat") }, { "clear", _("clear") }, { "harsh", _("harsh") }, { "clear lead", _("a clear lead") }, { "shared", _("shared") },
            { "no lead", _("no lead") }, { "one idea per figure", _("one idea per figure") }, { "some busy figures", _("some busy figures") }, { "busy figures", _("busy figures") },
            { "none", _("none") }, { "a few scenes", _("a few scenes") }, { "many scenes", _("many scenes") }, { "blend in", _("blend in") }, { "read", _("can be read") },
            { "jump out", _("jump out") }, { "a steady thread", _("a steady thread") }, { "a thread with turns", _("a thread with turns") }, { "many new ideas", _("many new ideas") },
            { "recurring", _("recurring") }, { "not recalled", _("never recalled") }, { "present", _("present") }, { "lost", _("lost") }
        };
        for (const nlohmann::json& sign : root.is_object() ? root.value("signs", nlohmann::json::array()) : nlohmann::json::array()) {
            const std::string id = sign.value("id", std::string());
            const std::string reading = sign.value("reading", std::string());
            const auto n = NAMES.find(id);
            const auto r = READINGS.find(reading);
            words += (n != NAMES.end() ? n->second : U(id)) + ": " + (r != READINGS.end() ? r->second : U(reading)) + "\n";
        }
        _composition->SetValue(words.IsEmpty() ? _("No reading of the composition for this show.") : words);
    }
    if (!_check.ran) {
        _summary->SetValue(_("The colours of this show were not measured: generate it with the render check on."));
        return;
    }
    wxString out;
    out += wxString::Format(_("About %.1f families of colour share the light; the most used is %s (%.0f%%)."), _check.effectiveColors, ColorFamilyWords(_check.dominant), _check.dominantShare * 100) + "\n";
    out += wxString::Format(_("One colour holds most of the house %.0f%% of the time; the longest stretch is %.0f s."), _check.monotoneShare * 100, _check.longestMonotoneMS / 1000.0) + "\n";
    out += wxString::Format(_("The figures wear %.1f families at once and differ by %.0f points; a minority accent appears %.0f%% of the time."), _check.figureFamilies,
                            _check.figureDifference, _check.accentFrames * 100) +
           "\n";
    if (!_check.gestures.empty()) {
        out += wxString::Format(_("The gestures differ from what was lit before them by %.0f points (half of them more); %d of %d barely stand out."), _check.gestureContrast,
                                _check.gesturesFaint, static_cast<int>(_check.gestures.size())) +
               "\n";
    }
    for (const std::string& w : _check.warnings) {
        out += "- " + ColorWarningWords(w) + "\n";
    }
    if (_check.warnings.empty()) {
        out += _("No sign of monotony was found. That is not an approval: look at it in xLights.") + wxString("\n");
    }
    _summary->SetValue(out);
}

wxString ColorReviewDialog::Summary() const {
    return _summary->GetValue();
}

int ColorReviewDialog::SceneRows() const {
    return _scenes->GetItemCount();
}

int ColorReviewDialog::FigureRows() const {
    return _figures->GetItemCount();
}

wxString ColorReviewDialog::Composition() const {
    return _composition->GetValue();
}

// ============================================================ Groq check

ArtisticCheckDialog::ArtisticCheckDialog(wxWindow* parent, const ArtisticCheck& c) :
    wxDialog(parent, wxID_ANY, _("Artistic Analysis with Groq"), wxDefaultPosition, wxSize(640, 520), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* status = new wxStaticText(this, wxID_ANY, ArtisticStatusText(c));
    wxFont bold = status->GetFont();
    bold.MakeBold();
    status->SetFont(bold);
    sizer->Add(status, 0, wxALL, 8);
    wxString out;
    out += wxString::Format(_("Result: %s"), U(c.status)) + "\n";
    out += (c.configured ? wxString::Format(_("A Groq key is set (in %s). It is never shown."),
                                            c.credentialSource == "secure-store" ? _("xLights' secure store") : c.credentialSource == "environment" ? _("the GROQ_API_KEY variable") : U(c.credentialSource))
                         : _("No Groq key is set.")) +
           "\n";
    out += (c.realRequest ? _("Something was sent to Groq in this check.") : _("Nothing was sent to Groq in this check.")) + wxString("\n");
    if (c.cacheHit) {
        out += _("An answer kept from before was used.") + wxString("\n");
    }
    if (!c.model.empty()) {
        out += wxString::Format(_("Model: %s"), U(c.model)) + "\n";
    }
    if (!c.modelsOffered.empty()) {
        out += wxString::Format(_("Chat models the key offers now: %d"), static_cast<int>(c.modelsOffered.size())) + "\n";
    }
    if (!c.analyzedAt.empty()) {
        out += wxString::Format(_("Last analysis: %s"), U(c.analyzedAt)) + "\n";
    }
    if (!c.received.empty()) {
        wxString what;
        for (const std::string& r : c.received) {
            what += (what.empty() ? "" : ", ") + (r == "lyrics" ? _("the text of the lyrics") : r == "title" ? _("the title") : r == "acoustic-summary" ? _("a summary measured on this computer (tempo, key, parts)") : U(r));
        }
        out += wxString::Format(_("What Groq received: %s. Never the audio."), what) + "\n";
    }
    out += (c.valid ? _("The answer passed the checks.") : c.realRequest || c.cacheHit ? _("The answer did not pass the checks.") : wxString()) + "\n";
    if (c.fallback) {
        out += _("The local reading of the lyrics is used instead.") + wxString("\n");
    }
    if (!c.error.empty()) {
        out += wxString::Format(_("What went wrong: %s"), U(c.error)) + "\n";
    }
    if (!c.brief.mood.empty()) {
        out += wxString::Format(_("Reading of the song: %s"), FeelingWords(c.brief.mood)) + "\n";
    }
    _text = new wxTextCtrl(this, wxID_ANY, out, wxDefaultPosition, wxSize(-1, 300), wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
    sizer->Add(_text, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
    auto* swatches = new ColorSwatches(this, wxSize(240, 22));
    swatches->SetColors(c.brief.colors);
    sizer->Add(swatches, 0, wxALL, 8);
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* again = new wxButton(this, wxID_ANY, _("Ask Groq Again..."));
    again->Enable(c.configured);
    buttons->Add(again, 0, wxRIGHT, 4);
    buttons->AddStretchSpacer();
    buttons->Add(new wxButton(this, wxID_OK, _("Close")), 0);
    sizer->Add(buttons, 0, wxEXPAND | wxALL, 8);
    SetSizer(sizer);
    again->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        EndModal(wxID_OK);
        if (onAskAgain) {
            onAskAgain();
        }
    });
}

wxString ArtisticCheckDialog::Text() const {
    return _text->GetValue();
}

// ============================================================ phase 19 dialogs

SceneCompareDialog::SceneCompareDialog(wxWindow* parent, const ChromaticScore& score, int first, int second) :
    wxDialog(parent, wxID_ANY, _("Compare Scenes"), wxDefaultPosition, wxSize(820, 560), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    _score(score) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    wxArrayString names;
    for (const ChromaticScenePlan& s : score.scenes) {
        names.Add(wxString::Format("%s %s", ClockText(s.startMS), PartWords(s.sectionKind)));
    }
    _a = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, names);
    _b = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, names);
    row->Add(_a, 1, wxRIGHT, 8);
    row->Add(_b, 1);
    sizer->Add(row, 0, wxEXPAND | wxALL, 8);
    _text = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 420), wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
    sizer->Add(_text, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
    sizer->Add(new wxButton(this, wxID_OK, _("Close")), 0, wxALIGN_RIGHT | wxALL, 8);
    SetSizer(sizer);
    const auto changed = [this](wxCommandEvent&) { ShowScenes(_a->GetSelection(), _b->GetSelection()); };
    _a->Bind(wxEVT_CHOICE, changed);
    _b->Bind(wxEVT_CHOICE, changed);
    ShowScenes(first, second);
}

void SceneCompareDialog::ShowScenes(int first, int second) {
    const int n = static_cast<int>(_score.scenes.size());
    if (n == 0) {
        return;
    }
    first = std::clamp(first, 0, n - 1);
    second = std::clamp(second, 0, n - 1);
    _a->SetSelection(first);
    _b->SetSelection(second);
    const ChromaticScenePlan& a = _score.scenes[static_cast<size_t>(first)];
    const ChromaticScenePlan& b = _score.scenes[static_cast<size_t>(second)];
    wxString out = SceneText(a, _score) + "\n----------------------------------------\n\n" + SceneText(b, _score) + "\n";
    out += a.motif == b.motif && !a.motif.empty() ? wxString::Format(_("Both scenes use the colour idea %s."), U(a.motif)) : _("Each scene has its own colour idea.");
    out += "\n";
    int shared = 0;
    for (const std::string& x : UsedColors(a)) {
        for (const std::string& y : UsedColors(b)) {
            shared += DeltaE(x, y) < 8 ? 1 : 0;
        }
    }
    out += wxString::Format(_("They share %d colours."), shared) + "\n";
    _text->SetValue(out);
}

wxString SceneCompareDialog::Text() const {
    return _text->GetValue();
}

wxString ChangeWords(const std::string& kind) {
    static const std::map<std::string, wxString> W = { { "colour", _("Colours changed") }, { "effect", _("Effect replaced or added") }, { "layer-added", _("Layer added") },
                                                       { "layer-removed", _("Effect taken out") }, { "intensity", _("Brighter or dimmer") }, { "timing", _("Moved in time") },
                                                       { "figure-added", _("Figure brought in") }, { "figure-removed", _("Figure left dark") } };
    const auto it = W.find(kind);
    return it != W.end() ? it->second : U(kind);
}

// Why a change was found, from what it holds (the core's own reason is for the logs).
wxString ChangeReasonWords(const SequenceChange& c) {
    const auto split = [](const std::string& joined) {
        std::vector<std::string> out;
        std::istringstream in(joined);
        for (std::string hex; in >> hex;) {
            out.push_back(hex);
        }
        return out;
    };
    if (c.kind == "colour") {
        const wxString before = ColourNames(split(c.before));
        const wxString after = ColourNames(split(c.after));
        const wxString lesson = c.sign > 0 ? _("fewer colours together") : c.sign < 0 ? _("more colours together") : _("other colours, as many");
        return wxString::Format(_("before %s, now %s (%s)"), before, after, lesson);
    }
    if (c.kind == "intensity") {
        return c.sign > 0 ? _("brighter") : _("dimmer");
    }
    if (c.kind == "timing") {
        return _("moved or made longer or shorter");
    }
    if (c.kind == "effect") {
        return c.before.empty() ? wxString::Format(_("%s was added"), U(c.after)) : wxString::Format(_("%s instead of %s"), U(c.after), U(c.before));
    }
    if (c.kind == "layer-added") {
        return wxString::Format(_("%s on top"), U(c.after));
    }
    if (c.kind == "layer-removed" || c.kind == "figure-removed") {
        return wxString::Format(_("%s taken out"), U(c.before));
    }
    if (c.kind == "figure-added") {
        return wxString::Format(_("%s, where the show left it dark"), U(c.after));
    }
    return U(c.why);
}

wxString ObservationWords(const std::string& kind) {
    static const std::map<std::string, wxString> W = { { "too-many-colours", _("Too many colours") }, { "incoherent", _("Colours do not belong together") },
                                                       { "wrong-main-colour", _("Wrong main colour") }, { "good-contrast", _("Good contrast") },
                                                       { "accents-excessive", _("Too many accents") }, { "inelegant-transition", _("Clumsy change of colour") },
                                                       { "keep-combination", _("Keep this combination") }, { "recall-motif", _("Bring back an earlier motif") },
                                                       { "figure-stand-out", _("This figure should stand out") }, { "figure-rest", _("This figure should rest") } };
    const auto it = W.find(kind);
    return it != W.end() ? it->second : U(kind);
}

namespace {

wxString CategoryWords(const std::string& c) {
    if (c == "technical") return _("a nudge in time");
    if (c == "necessary") return _("something that should not light");
    return _("a matter of taste");
}

wxString DecisionWords(const std::string& d) {
    if (d == "learn") return _("Learn it");
    if (d == "song") return _("Only this song");
    if (d == "general") return _("General preference");
    if (d == "reject") return _("Reject it");
    return _("Ignore it");
}

} // namespace

CorrectionsDialog::CorrectionsDialog(wxWindow* parent, const SequenceComparison& comparison, const ComparisonDecisions* record) :
    wxDialog(parent, wxID_ANY, _("Your Corrected Version"), wxDefaultPosition, wxSize(960, 640), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    _comparison(comparison) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(Muted(this, wxString::Format(_("%d effects in the generated show, %d in yours, %d kept as they were. Not every change is a taste: choose what each one becomes. Nothing is learned unless you choose it."),
                                            comparison.generatedEffects, comparison.correctedEffects, comparison.matched)),
               0, wxALL, 8);
    sizer->Add(Muted(this, _("Learn it: a taste for the next shows, once a second show agrees. Only this song: kept as a decision of this song. General preference: your own value now, for every show (it can be undone in \"What Has Been Learned\"). Ignore: nothing. Reject: nothing, and it is not offered again.")),
               0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    _list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 400), wxLC_REPORT);
    for (const auto& [label, width] : std::vector<std::pair<wxString, int>>{ { _("When"), 60 }, { _("Figure"), 150 }, { _("What changed"), 340 }, { _("Kind"), 170 }, { _("Decision"), 140 } }) {
        _list->AppendColumn(label, wxLIST_FORMAT_LEFT, width);
    }
    const std::vector<size_t> offered = record != nullptr ? OfferedChanges(comparison, *record) : std::vector<size_t>();
    for (size_t i = 0; i < comparison.changes.size(); ++i) {
        if (record != nullptr && std::find(offered.begin(), offered.end(), i) == offered.end()) {
            continue;
        }
        const SequenceChange& c = comparison.changes[i];
        const long row = _list->InsertItem(_list->GetItemCount(), ClockText(c.startMS));
        _list->SetItem(row, 1, U(c.figure));
        _list->SetItem(row, 2, wxString::Format("%s: %s%s", ChangeWords(c.kind), ChangeReasonWords(c), c.suggestion.empty() ? wxString(" ") + _("(nothing to learn for other songs)") : wxString()));
        _list->SetItem(row, 3, CategoryWords(c.category));
        _rows.push_back(i);
        _choices.push_back("ignore");
        ShowDecision(_rows.size() - 1);
    }
    sizer->Add(_list, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
    auto* choose = new wxBoxSizer(wxHORIZONTAL);
    choose->Add(new wxStaticText(this, wxID_ANY, _("The changes chosen above:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    for (const char* d : { "learn", "song", "general", "ignore", "reject" }) {
        auto* b = new wxButton(this, wxID_ANY, DecisionWords(d));
        choose->Add(b, 0, wxRIGHT, 4);
        const std::string choice = d;
        b->Bind(wxEVT_BUTTON, [this, choice](wxCommandEvent&) {
            for (long i = _list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED); i >= 0; i = _list->GetNextItem(i, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) {
                Decide(static_cast<size_t>(i), choice);
            }
        });
    }
    sizer->Add(choose, 0, wxALL, 8);
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();
    buttons->Add(new wxButton(this, wxID_OK, _("Apply My Decisions")), 0, wxRIGHT, 4);
    buttons->Add(new wxButton(this, wxID_CANCEL, _("Close")), 0);
    sizer->Add(buttons, 0, wxEXPAND | wxALL, 8);
    SetSizer(sizer);
}

void CorrectionsDialog::ShowDecision(size_t row) {
    if (row < _choices.size()) {
        _list->SetItem(static_cast<long>(row), 4, DecisionWords(_choices[row]));
    }
}

int CorrectionsDialog::Rows() const {
    return _list->GetItemCount();
}

void CorrectionsDialog::Decide(size_t index, const std::string& choice) {
    if (index < _choices.size()) {
        _choices[index] = choice;
        ShowDecision(index);
    }
}

void CorrectionsDialog::Pick(size_t index, bool picked) {
    Decide(index, picked ? "learn" : "ignore");
}

std::vector<size_t> CorrectionsDialog::Picked() const {
    std::vector<size_t> out;
    for (size_t i = 0; i < _choices.size(); ++i) {
        if (_choices[i] == "learn") {
            out.push_back(_rows[i]);
        }
    }
    return out;
}

std::vector<ComparisonDecision> CorrectionsDialog::Decisions() const {
    std::vector<ComparisonDecision> out;
    for (size_t i = 0; i < _choices.size(); ++i) {
        out.push_back({ _rows[i], _choices[i] });
    }
    return out;
}

ObservationsDialog::ObservationsDialog(wxWindow* parent, const ChromaticScore& score, std::vector<ArtisticObservation> observations, const std::string& reviewer) :
    wxDialog(parent, wxID_ANY, _("Observations with Their Place"), wxDefaultPosition, wxSize(820, 600), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    _score(score),
    _observations(std::move(observations)) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(Muted(this, _("Say what you noticed and where. Some observations teach the next shows (after two shows); others are decisions for this song only, applied to its colours when you confirm them.")),
               0, wxALL, 8);
    auto* who = new wxBoxSizer(wxHORIZONTAL);
    who->Add(new wxStaticText(this, wxID_ANY, _("Who observes:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    _reviewer = new wxTextCtrl(this, wxID_ANY, U(reviewer), wxDefaultPosition, wxSize(200, -1));
    who->Add(_reviewer, 0);
    sizer->Add(who, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    auto* grid = new wxFlexGridSizer(2, 6, 8);
    wxArrayString kinds;
    for (const std::string& k : ArtisticObservationKinds()) {
        kinds.Add(ObservationWords(k));
    }
    _kind = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, kinds);
    _kind->SetSelection(0);
    wxArrayString scenes;
    scenes.Add(_("The whole song"));
    for (const ChromaticScenePlan& s : score.scenes) {
        scenes.Add(wxString::Format("%s %s", ClockText(s.startMS), PartWords(s.sectionKind)));
    }
    _scene = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, scenes);
    _scene->SetSelection(0);
    std::set<std::string> names;
    for (const ChromaticScenePlan& s : score.scenes) {
        for (const FigureColorRole& r : s.figures) {
            names.insert(r.figure);
        }
    }
    wxArrayString figures;
    figures.Add(_("No figure"));
    for (const std::string& n : names) {
        figures.Add(U(n));
    }
    _figure = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, figures);
    _figure->SetSelection(0);
    _note = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(360, -1));
    grid->Add(new wxStaticText(this, wxID_ANY, _("What:")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(_kind, 0);
    grid->Add(new wxStaticText(this, wxID_ANY, _("Where in the song:")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(_scene, 0);
    grid->Add(new wxStaticText(this, wxID_ANY, _("Which figure:")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(_figure, 0);
    grid->Add(new wxStaticText(this, wxID_ANY, _("Note:")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(_note, 0);
    sizer->Add(grid, 0, wxLEFT | wxRIGHT, 8);
    auto* add = new wxButton(this, wxID_ANY, _("Add the Observation"));
    sizer->Add(add, 0, wxALL, 8);
    _list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 220), wxLC_REPORT | wxLC_SINGLE_SEL);
    for (const auto& [label, width] : std::vector<std::pair<wxString, int>>{ { _("What"), 230 }, { _("Where"), 120 }, { _("Figure"), 160 }, { _("Teaches or decides"), 220 } }) {
        _list->AppendColumn(label, wxLIST_FORMAT_LEFT, width);
    }
    sizer->Add(_list, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* remove = new wxButton(this, wxID_ANY, _("Remove the Selected One"));
    buttons->Add(remove, 0);
    buttons->AddStretchSpacer();
    buttons->Add(new wxButton(this, wxID_OK, _("Save")), 0, wxRIGHT, 4);
    buttons->Add(new wxButton(this, wxID_CANCEL, _("Cancel")), 0);
    sizer->Add(buttons, 0, wxEXPAND | wxALL, 8);
    SetSizer(sizer);
    add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        ArtisticObservation o;
        o.kind = ArtisticObservationKinds()[static_cast<size_t>(std::max(0, _kind->GetSelection()))];
        o.sceneStartMS = _scene->GetSelection() <= 0 ? -1 : _score.scenes[static_cast<size_t>(_scene->GetSelection() - 1)].startMS;
        o.figure = _figure->GetSelection() <= 0 ? std::string() : S(_figure->GetStringSelection());
        o.note = S(_note->GetValue());
        if (!Add(o)) {
            wxMessageBox(o.kind == "keep-combination" ? _("Say in which scene.") : _("Say which figure."), _("Observations with Their Place"), wxOK | wxICON_INFORMATION, this);
        }
    });
    remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const long i = _list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (i >= 0 && i < static_cast<long>(_observations.size())) {
            _observations.erase(_observations.begin() + i);
            Fill();
        }
    });
    Fill();
}

bool ObservationsDialog::Add(const ArtisticObservation& o) {
    if ((o.kind == "figure-stand-out" || o.kind == "figure-rest") && o.figure.empty()) {
        return false;
    }
    if (o.kind == "keep-combination" && o.sceneStartMS < 0) {
        return false;
    }
    _observations.push_back(o);
    Fill();
    return true;
}

std::string ObservationsDialog::Reviewer() const {
    return S(_reviewer->GetValue());
}

void ObservationsDialog::Fill() {
    _list->DeleteAllItems();
    for (const ArtisticObservation& o : _observations) {
        const long row = _list->InsertItem(_list->GetItemCount(), ObservationWords(o.kind));
        _list->SetItem(row, 1, o.sceneStartMS < 0 ? _("The whole song") : ClockText(o.sceneStartMS));
        _list->SetItem(row, 2, U(o.figure));
        _list->SetItem(row, 3, ObservationIsSongDecision(o) ? _("A decision for this song") : _("Teaches the next shows"));
    }
}

