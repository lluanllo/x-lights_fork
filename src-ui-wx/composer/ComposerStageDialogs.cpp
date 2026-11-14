/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "ComposerStageDialogs.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/choicdlg.h>
#include <wx/dcbuffer.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include "ComposerTimeline.h"
#include "ComposerUnderstandingDialogs.h"
#include "composer/UnderstandingStore.h"
#include "render/FSEQFile.h"

namespace fs = std::filesystem;
using namespace composer;

namespace {

wxString U(const std::string& s) {
    return wxString::FromUTF8(s);
}

std::string S(const wxString& s) {
    return s.ToStdString(wxConvUTF8);
}

wxStaticText* Muted(wxWindow* parent, const wxString& text, int wrap = 760) {
    auto* t = new wxStaticText(parent, wxID_ANY, text);
    t->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    t->Wrap(wrap);
    return t;
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

bool ReadScore(const std::string& text, GlobalStageScore& score) {
    std::string error;
    return !text.empty() && FromJson(text, score, error);
}

std::vector<std::string> RealFigures(const GlobalStageScore& score) {
    std::vector<std::string> out;
    for (const StageScene& s : score.scenes) {
        for (const SceneRole& r : s.roles) {
            if (std::find(out.begin(), out.end(), r.figure) == out.end()) {
                out.push_back(r.figure);
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

wxString StageModeWords(StageMode mode) {
    return Words(StageModePhrase(mode));
}

wxString GestureWords(GestureKind kind) {
    return Words(GestureKindPhrase(kind));
}

wxString StageRoleWords(const std::string& role) {
    return Words(SceneRolePhrase(role));
}

std::vector<TimelineGesture> TimelineGestures(const GlobalStageScore& score) {
    std::vector<TimelineGesture> out;
    for (const GlobalGesture& g : score.gestures) {
        out.push_back({ g.startMS, g.endMS, GestureWords(g.kind), g.manual });
    }
    return out;
}

// ------------------------------------------------------------------ the map

StageMapView::StageMapView(wxWindow* parent) : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(720, 380)) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &StageMapView::OnPaint, this);
}

StageMapView::~StageMapView() = default;

void StageMapView::SetLights(std::vector<StageFigureLights> lights) {
    _lights = std::move(lights);
    Refresh();
}

void StageMapView::SetScoreEffects(const Choreography& choreography) {
    _fields.clear();
    std::map<std::string, size_t> count;
    for (const StageFigureLights& f : _lights) {
        count[f.figure] = f.lights.size();
    }
    for (const ChoreographyEvent& e : choreography.events) {
        if (e.provenance.rfind("stage:", 0) != 0 || e.settings.empty()) {
            continue;
        }
        Field f;
        std::string error;
        if (!FromText(e.settings.front().second, f.spec, error)) {
            continue;
        }
        f.figure = e.target;
        f.startMS = e.startMS;
        f.endMS = e.endMS;
        f.colors = e.colors.empty() ? std::vector<std::string>{ "#FFFFFF" } : e.colors;
        const int n = static_cast<int>(count[e.target]);
        const std::vector<char> only = LightSet(f.spec.only, n, true);
        const std::vector<char> skip = LightSet(f.spec.skip, n, false);
        f.lit.resize(static_cast<size_t>(n));
        for (int k = 0; k < n; ++k) {
            f.lit[k] = only[k] && !skip[k];
        }
        _fields.push_back(std::move(f));
    }
    Refresh();
}

bool StageMapView::SetRender(const std::string& fseqPath) {
    _fseq.reset();
    _frameRead = -1;
    if (fseqPath.empty()) {
        return false;
    }
    _fseq.reset(FSEQFile::openFSEQFile(fseqPath));
    if (_fseq == nullptr) {
        return false;
    }
    _channels = static_cast<uint32_t>(_fseq->getMaxChannel());
    _frameMS = std::max(1, static_cast<int>(_fseq->getStepTime()));
    _fseq->prepareRead({ { 0, _channels } });
    _frame.assign(_channels, 0);
    return true;
}

void StageMapView::UseRender(bool render) {
    _useRender = render && _fseq != nullptr;
    Refresh();
}

void StageMapView::SetTime(int ms) {
    _timeMS = std::max(0, ms);
    Refresh();
}

void StageMapView::Mark(double x, double y) {
    _markX = x;
    _markY = y;
    Refresh();
}

void StageMapView::ReadFrame() {
    const int frame = _timeMS / _frameMS;
    if (_fseq == nullptr || frame == _frameRead || frame >= static_cast<int>(_fseq->getNumFrames())) {
        return;
    }
    std::fill(_frame.begin(), _frame.end(), 0);
    std::unique_ptr<FSEQFile::FrameData> data(_fseq->getFrame(static_cast<uint32_t>(frame)));
    if (data != nullptr) {
        (void)data->readFrame(_frame.data(), _channels);
    }
    _frameRead = frame;
}

void StageMapView::OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    const wxSize size = GetClientSize();
    dc.SetBrush(wxBrush(wxColour(14, 14, 20)));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, 0, size.x, size.y);
    _lit = 0;
    if (_lights.empty()) {
        dc.SetTextForeground(wxColour(170, 170, 170));
        dc.DrawText(_("Work out the composition to see the installation here."), 12, size.y / 2 - 8);
        return;
    }
    double minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
    for (const StageFigureLights& f : _lights) {
        for (const StageLight& l : f.lights) {
            minX = std::min<double>(minX, l.x);
            maxX = std::max<double>(maxX, l.x);
            minY = std::min<double>(minY, l.y);
            maxY = std::max<double>(maxY, l.y);
        }
    }
    const double k = std::min((size.x - 24) / std::max(0.01, maxX - minX), (size.y - 24) / std::max(0.01, maxY - minY));
    const auto px = [&](double x) { return 12 + static_cast<int>((x - minX) * k); };
    const auto py = [&](double y) { return size.y - 12 - static_cast<int>((y - minY) * k); }; // the layout's y goes up
    if (_useRender) {
        ReadFrame();
    }
    std::map<std::string, std::vector<const Field*>> fields;
    if (!_useRender) {
        for (const Field& f : _fields) {
            if (_timeMS >= f.startMS && _timeMS < f.endMS) {
                fields[f.figure].push_back(&f);
            }
        }
    }
    for (const StageFigureLights& f : _lights) {
        const auto active = fields.find(f.figure);
        for (size_t n = 0; n < f.lights.size(); ++n) {
            const StageLight& l = f.lights[n];
            wxColour colour(46, 46, 56);
            if (_useRender) {
                if (l.channel >= 0 && static_cast<uint32_t>(l.channel) + 2 < _channels) {
                    const uint8_t r = _frame[l.channel], g = _frame[l.channel + 1], b = _frame[l.channel + 2];
                    if (std::max({ r, g, b }) > 24) {
                        colour = wxColour(r, g, b);
                        ++_lit;
                    }
                }
            } else if (active != fields.end()) {
                float best = 0;
                for (const Field* field : active->second) {
                    if (n >= field->lit.size() || !field->lit[n]) {
                        continue;
                    }
                    const double t = (_timeMS - field->startMS) / 1000.0 + field->spec.at;
                    const FieldSample s = EvaluateField(field->spec, l.x, l.y, t);
                    if (s.intensity > best) {
                        best = s.intensity;
                        const size_t c = std::min(field->colors.size() - 1, static_cast<size_t>(std::lround(s.position * (field->colors.size() - 1))));
                        const wxColour base(U(field->colors[c]));
                        colour = wxColour(static_cast<unsigned char>(46 + (base.Red() - 46) * best), static_cast<unsigned char>(46 + (base.Green() - 46) * best),
                                          static_cast<unsigned char>(56 + (base.Blue() - 56) * best));
                    }
                }
                _lit += best > 0.1F ? 1 : 0;
            }
            dc.SetBrush(wxBrush(colour));
            dc.DrawRectangle(px(l.x) - 1, py(l.y) - 1, 3, 3);
        }
    }
    if (_markX >= 0) {
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.SetPen(wxPen(wxColour(255, 255, 255), 2));
        dc.DrawCircle(px(_markX), py(_markY), 9);
    }
}

// ------------------------------------------------------------------ the installation's choreography

StageMapDialog::StageMapDialog(wxWindow* parent, const ReportFolder& report, const std::string& fseqPath, int startMS) :
    wxDialog(parent, wxID_ANY, _("Choreography of the Installation"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), _timer(this) {
    (void)ReadScore(report.stageScore, _score);
    _durationMS = std::max(_score.durationMS, report.analysis.durationMS);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(Muted(this, _("Every light of the installation where the layout puts it. The layout is a drawing, not a measurement: distances are those of the drawing.")), 0, wxALL, 8);
    _map = new StageMapView(this);
    std::vector<StageFigureLights> lights;
    std::string error;
    if (!report.stageLights.empty()) {
        (void)StageLightsFromJson(report.stageLights, lights, error);
    }
    _map->SetLights(std::move(lights));
    if (report.hasChoreography) {
        _map->SetScoreEffects(report.choreography);
    }
    const bool render = _map->SetRender(fseqPath);
    sizer->Add(_map, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    _play = new wxButton(this, wxID_ANY, _("Play"));
    row->Add(_play, 0, wxRIGHT, 6);
    auto* next = new wxButton(this, wxID_ANY, _("Next Gesture"));
    row->Add(next, 0, wxRIGHT, 6);
    _slider = new wxSlider(this, wxID_ANY, 0, 0, std::max(1, _durationMS / 100));
    row->Add(_slider, 1, wxEXPAND | wxRIGHT, 6);
    _clock = new wxStaticText(this, wxID_ANY, "0:00.0", wxDefaultPosition, wxSize(70, -1));
    row->Add(_clock, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    _source = new wxChoice(this, wxID_ANY);
    _source->Append(_("What the score plans"));
    if (render) {
        _source->Append(_("What xLights drew"));
    }
    _source->SetSelection(render ? 1 : 0);
    _map->UseRender(render);
    row->Add(_source, 0);
    sizer->Add(row, 0, wxEXPAND | wxALL, 8);
    _now = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 90), wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
    sizer->Add(_now, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
    sizer->Add(CreateStdDialogButtonSizer(wxCLOSE), 0, wxEXPAND | wxALL, 8);
    SetSizerAndFit(sizer);
    SetSize(wxSize(900, 640));
    SetEscapeId(wxID_CLOSE);
    _slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { ShowAt(_slider->GetValue() * 100); });
    _source->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        _map->UseRender(_source->GetSelection() == 1);
        ShowAt(_map->Time());
    });
    next->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { NextGesture(); });
    _play->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (_timer.IsRunning()) {
            _timer.Stop();
            _play->SetLabel(_("Play"));
        } else {
            _timer.Start(50);
            _play->SetLabel(_("Pause"));
        }
    });
    // Played slowly, a quarter of real time: a wave is seen travelling.
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        const int ms = _map->Time() + 12;
        if (ms >= _durationMS) {
            _timer.Stop();
            _play->SetLabel(_("Play"));
            return;
        }
        ShowAt(ms);
    });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
        _timer.Stop();
        e.Skip();
    });
    ShowAt(startMS);
}

void StageMapDialog::SetTime(int ms) {
    ShowAt(ms);
}

void StageMapDialog::NextGesture() {
    for (const GlobalGesture& g : _score.gestures) {
        if (g.startMS > _map->Time() + 50) {
            ShowAt(std::max(0, g.startMS - 200));
            return;
        }
    }
}

void StageMapDialog::ShowAt(int ms) {
    _map->SetTime(ms);
    _slider->SetValue(ms / 100);
    _clock->SetLabel(wxString::Format("%d:%04.1f", ms / 60000, (ms % 60000) / 1000.0));
    double mx = -1, my = -1;
    for (const GlobalGesture& g : _score.gestures) {
        if (ms >= g.startMS - 200 && ms < g.endMS + 400 && !g.origin.empty()) {
            mx = g.ox;
            my = g.oy;
        }
    }
    _map->Mark(mx, my);
    _now->SetValue(Describe());
}

wxString StageMapDialog::Describe() const {
    const int ms = _map->Time();
    wxString out;
    if (const StageScene* s = _score.SceneAt(ms)) {
        out += wxString::Format(_("Now: %s."), StageModeWords(s->mode));
        if (!s->reasons.empty()) {
            out += wxString::FromUTF8(" \xE2\x80\x94 ") + Words(s->reasons.front());
        }
        out += "\n";
    }
    for (const GlobalGesture& g : _score.gestures) {
        if (ms >= g.startMS - 200 && ms < g.endMS + 400) {
            out += wxString::Format(_("%s at %s, %d figures"), GestureWords(g.kind), ClockText(g.startMS), static_cast<int>(g.participants.size()));
            if (!g.reasons.empty()) {
                out += ": " + Words(g.reasons.back());
            }
            out += "\n";
        }
    }
    return out.IsEmpty() ? _("No gesture of the installation at this moment.") : out;
}

// ------------------------------------------------------------------ a figure's layers

FigureLayersDialog::FigureLayersDialog(wxWindow* parent, const ReportFolder& report, const std::string& editsPath, const std::string& figure) :
    wxDialog(parent, wxID_ANY, _("Layers of This Figure"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), _report(report), _editsPath(editsPath) {
    (void)ReadScore(report.stageScore, _score);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(Muted(this, _("What the figure has, top to bottom, during a scene. The top layer is seen over the ones below where it is lit; where it is dark, the one below shows.")), 0, wxALL, 8);
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(new wxStaticText(this, wxID_ANY, _("Figure:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    _figures = new wxChoice(this, wxID_ANY);
    for (const std::string& f : RealFigures(_score)) {
        _figures->Append(U(f));
    }
    row->Add(_figures, 1, wxRIGHT, 8);
    row->Add(new wxStaticText(this, wxID_ANY, _("Scene:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    _scenes = new wxChoice(this, wxID_ANY);
    for (const StageScene& s : _score.scenes) {
        _scenes->Append(wxString::Format("%s  %s", ClockText(s.startMS), StageModeWords(s.mode)));
    }
    row->Add(_scenes, 1);
    sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
    _list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(640, 220), wxLC_REPORT | wxLC_SINGLE_SEL);
    for (const auto& [label, width] : std::vector<std::pair<wxString, int>>{ { _("Layer"), 50 }, { _("From"), 70 }, { _("To"), 70 }, { _("What it is"), 230 }, { _("Effect"), 120 }, { _("Mix"), 80 } }) {
        _list->AppendColumn(label, wxLIST_FORMAT_LEFT, width);
    }
    sizer->Add(_list, 1, wxEXPAND | wxALL, 8);
    _colours = new wxStaticText(this, wxID_ANY, "");
    sizer->Add(_colours, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* keep = new wxButton(this, wxID_ANY, _("Keep Its Own Colours"));
    auto* never = new wxButton(this, wxID_ANY, _("Never Its Own Colours"));
    auto* director = new wxButton(this, wxID_ANY, _("As the Director Decides"));
    buttons->Add(keep, 0, wxRIGHT, 4);
    buttons->Add(never, 0, wxRIGHT, 4);
    buttons->Add(director, 0);
    sizer->Add(buttons, 0, wxALL, 8);
    sizer->Add(Muted(this, _("Its own colours come only from parts known for sure (a submodel, a name, a face definition or your confirmation). The mouth and the eyes are always left to the face. The change is applied when the composition is worked out again.")), 0, wxLEFT | wxRIGHT, 8);
    sizer->Add(CreateStdDialogButtonSizer(wxCLOSE), 0, wxEXPAND | wxALL, 8);
    SetSizerAndFit(sizer);
    SetEscapeId(wxID_CLOSE);
    const auto refresh = [this]() {
        const int s = _scenes->GetSelection();
        ShowFigure(S(_figures->GetStringSelection()), s >= 0 && s < static_cast<int>(_score.scenes.size()) ? _score.scenes[s].startMS : 0);
    };
    _figures->Bind(wxEVT_CHOICE, [refresh](wxCommandEvent&) { refresh(); });
    _scenes->Bind(wxEVT_CHOICE, [refresh](wxCommandEvent&) { refresh(); });
    keep->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { (void)KeepColours(1); });
    never->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { (void)KeepColours(-1); });
    director->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { (void)KeepColours(0); });
    _figures->SetStringSelection(U(figure));
    if (_figures->GetSelection() < 0 && _figures->GetCount() > 0) {
        _figures->SetSelection(0);
    }
    if (_scenes->GetCount() > 0) {
        _scenes->SetSelection(0);
    }
    refresh();
}

void FigureLayersDialog::ShowFigure(const std::string& figure, int ms) {
    _figure = figure;
    _list->DeleteAllItems();
    const StageScene* scene = _score.SceneAt(ms);
    const int from = scene != nullptr ? scene->startMS : ms;
    const int to = scene != nullptr ? scene->endMS : ms + 1;
    std::vector<const ChoreographyEvent*> events;
    for (const ChoreographyEvent& e : _report.choreography.events) {
        if (e.target == figure && e.startMS < to && e.endMS > from) {
            events.push_back(&e);
        }
    }
    std::stable_sort(events.begin(), events.end(), [](const ChoreographyEvent* a, const ChoreographyEvent* b) { return a->layer != b->layer ? a->layer < b->layer : a->startMS < b->startMS; });
    for (const ChoreographyEvent* e : events) {
        const long row = _list->InsertItem(_list->GetItemCount(), wxString::Format("%d", e->layer + 1));
        _list->SetItem(row, 1, ClockText(e->startMS));
        _list->SetItem(row, 2, ClockText(e->endMS));
        wxString what = e->intent == "stage-gesture" ? _("A gesture of the installation") : e->intent == "stage-identity" ? _("Its own colours") : e->intent == "face-sing" ? _("Its face singing") : _("Its own effect");
        if (e->intent == "stage-identity" && !e->reasons.empty()) {
            what += " (" + Words(RegionPhrase(e->reasons.front().detail)) + ")";
        }
        _list->SetItem(row, 3, what);
        _list->SetItem(row, 4, U(e->effect));
        std::string mix = "Normal";
        for (const auto& [k, v] : e->settings) {
            if (k == "T_CHOICE_LayerMethod") {
                mix = v;
            }
        }
        _list->SetItem(row, 5, U(mix));
    }
    int layers = 0;
    for (const StageLayer& l : _score.layers) {
        layers += l.figure == figure ? 1 : 0;
    }
    _colours->SetLabel(layers > 0 ? wxString::Format(_("It keeps its own colours in %d scenes and parts."), layers) : _("It has no layer of its own colours in this score."));
}

std::string FigureLayersDialog::KeepColours(int mode) {
    if (_figure.empty() || _editsPath.empty()) {
        return "no-figure";
    }
    StageEdits edits;
    std::string text, error;
    if (ReadTextFile(_editsPath, text) && !FromJson(text, edits, error)) {
        return "edits-unreadable";
    }
    std::erase_if(edits.edits, [&](const StageEdit& e) { return (e.action == "identity" || e.action == "no-identity") && e.value == _figure && e.sceneStartMS < 0; });
    if (mode != 0) {
        edits.edits.push_back({ mode > 0 ? "identity" : "no-identity", -1, -1, _figure, "" });
    }
    if (!WriteAll(_editsPath, ToJson(edits))) {
        return "edits-not-saved";
    }
    _changed = true;
    _colours->SetLabel(mode > 0 ? _("Saved: it will keep its own colours wherever it takes part.") : mode < 0 ? _("Saved: it will never get its own colours.") : _("Saved: the director decides."));
    return "";
}

int FigureLayersDialog::Rows() const {
    return _list->GetItemCount();
}

// ------------------------------------------------------------------ compositions and gestures

StageLibraryDialog::StageLibraryDialog(wxWindow* parent, const std::string& showDir) :
    wxDialog(parent, wxID_ANY, _("Compositions and Gestures"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), _showDir(showDir) {
    StageLibrary personal;
    std::string text, error;
    if (ReadTextFile(StageLibraryPath(showDir), text)) {
        (void)FromJson(text, personal, error);
    }
    _library = MergeStageLibrary(personal);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(Muted(this, _("The gestures the director can give the whole installation, and the compositions a figure can keep. A favourite is tried first; an excluded one is never chosen on its own (you can still place it).")), 0, wxALL, 8);
    _list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(640, 260), wxLC_REPORT | wxLC_SINGLE_SEL);
    for (const auto& [label, width] : std::vector<std::pair<wxString, int>>{ { _("Name"), 280 }, { _("Kind"), 150 }, { _("Favourite"), 80 }, { _("Excluded"), 80 } }) {
        _list->AppendColumn(label, wxLIST_FORMAT_LEFT, width);
    }
    sizer->Add(_list, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
    _about = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 60), wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
    sizer->Add(_about, 0, wxEXPAND | wxALL, 8);
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* favourite = new wxButton(this, wxID_ANY, _("Favourite"));
    auto* exclude = new wxButton(this, wxID_ANY, _("Exclude"));
    auto* include = new wxButton(this, wxID_ANY, _("Include Again"));
    auto* own = new wxButton(this, wxID_ANY, _("New Composition..."));
    buttons->Add(favourite, 0, wxRIGHT, 4);
    buttons->Add(exclude, 0, wxRIGHT, 4);
    buttons->Add(include, 0, wxRIGHT, 12);
    buttons->Add(own, 0);
    sizer->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    // The recipes taken from the reference shows stay on this computer; only their count is shown.
    int recipes = 0;
    if (ReadTextFile((fs::path(showDir) / "ShowComposer" / "recipes.json").string(), text)) {
        try {
            recipes = static_cast<int>(nlohmann::json::parse(text).value("recipes", nlohmann::json::array()).size());
        } catch (const nlohmann::json::exception&) {
            recipes = 0;
        }
    }
    sizer->Add(Muted(this, recipes > 0 ? wxString::Format(_("Besides these, %d figure recipes from the reference shows, kept only on this computer, are used for each figure's own effects."), recipes)
                                       : _("No figure recipes from reference shows in this installation.")),
               0, wxLEFT | wxRIGHT, 8);
    sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
    SetSizerAndFit(sizer);
    Fill();
    _list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) {
        const StageLibraryEntry* e = _library.Find(Selected());
        _about->SetValue(e != nullptr ? Words(e->description) : wxString());
    });
    favourite->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const StageLibraryEntry* e = _library.Find(Selected());
        (void)Toggle(Selected(), true, e != nullptr && !e->favorite);
    });
    exclude->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { (void)Toggle(Selected(), false, true); });
    include->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { (void)Toggle(Selected(), false, false); });
    own->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const wxString name = wxGetTextFromUser(_("Name of your composition:"), _("New Composition"), "", this);
        if (name.IsEmpty()) {
            return;
        }
        wxArrayString choices;
        const std::vector<FieldFunction>& functions = AllFieldFunctions();
        for (FieldFunction f : functions) {
            choices.Add(Words(FieldFunctionPhrase(f)));
        }
        const int pick = wxGetSingleChoiceIndex(_("How its light moves:"), _("New Composition"), choices, this);
        if (pick >= 0) {
            (void)AddOwn(S(name), FieldFunctionId(functions[static_cast<size_t>(pick)]));
        }
    });
    Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
        if (Save()) {
            e.Skip();
        } else {
            wxMessageBox(_("Your choices could not be saved."), _("Compositions and Gestures"), wxOK | wxICON_WARNING, this);
        }
    }, wxID_OK);
}

void StageLibraryDialog::Fill() {
    _list->DeleteAllItems();
    for (const StageLibraryEntry& e : _library.entries) {
        const long row = _list->InsertItem(_list->GetItemCount(), e.custom ? U(e.title) : Words(e.title));
        _list->SetItem(row, 1, e.custom ? _("Yours") : e.kind == "gesture" ? _("Whole installation") : _("One figure"));
        _list->SetItem(row, 2, e.favorite ? _("Yes") : "");
        _list->SetItem(row, 3, e.excluded ? _("Yes") : "");
    }
}

std::string StageLibraryDialog::Selected() const {
    const long i = _list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    return i >= 0 && i < static_cast<long>(_library.entries.size()) ? _library.entries[static_cast<size_t>(i)].id : std::string();
}

bool StageLibraryDialog::Toggle(const std::string& id, bool favourite, bool value) {
    for (StageLibraryEntry& e : _library.entries) {
        if (e.id == id) {
            (favourite ? e.favorite : e.excluded) = value;
            Fill();
            return true;
        }
    }
    return false;
}

bool StageLibraryDialog::AddOwn(const std::string& title, const std::string& function) {
    FieldFunction f;
    if (title.empty() || !FieldFunctionFromId(function, f)) {
        return false;
    }
    std::string id = "own:" + title;
    _library.entries.push_back({ id, "local", title, "", { function }, {}, true, false, true, "person" });
    Fill();
    return true;
}

bool StageLibraryDialog::Save() {
    // Only what a person changed is kept: the built-in entries come from the program.
    StageLibrary personal;
    const StageLibrary builtIn = BuiltInStageLibrary();
    for (const StageLibraryEntry& e : _library.entries) {
        const StageLibraryEntry* b = builtIn.Find(e.id);
        if (e.custom || b == nullptr || b->favorite != e.favorite || b->excluded != e.excluded) {
            personal.entries.push_back(e);
        }
    }
    return WriteAll(StageLibraryPath(_showDir), ToJson(personal));
}

// ------------------------------------------------------------------ step 4: the director

StageDirectorPanel::StageDirectorPanel(wxWindow* parent) : wxPanel(parent) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    _on = new wxCheckBox(this, wxID_ANY, _("Direct the whole installation as one stage"));
    _on->SetValue(true);
    sizer->Add(_on, 0, wxBOTTOM, 4);
    _state = new wxStaticText(this, wxID_ANY, "");
    sizer->Add(_state, 0, wxEXPAND | wxBOTTOM, 4);
    auto* body = new wxBoxSizer(wxHORIZONTAL);
    _list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(470, 170), wxLC_REPORT | wxLC_SINGLE_SEL);
    for (const auto& [label, width] : std::vector<std::pair<wxString, int>>{ { _("From"), 60 }, { _("How the house is used"), 190 }, { _("Figures"), 60 }, { _("Gestures"), 70 }, { _("Changed"), 70 } }) {
        _list->AppendColumn(label, wxLIST_FORMAT_LEFT, width);
    }
    body->Add(_list, 3, wxEXPAND | wxRIGHT, 6);
    _detail = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(300, 170), wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
    body->Add(_detail, 2, wxEXPAND);
    sizer->Add(body, 1, wxEXPAND | wxBOTTOM, 4);
    auto* edits = new wxBoxSizer(wxHORIZONTAL);
    const auto button = [&](wxSizer* row, const wxString& label, std::function<void()> action) {
        auto* b = new wxButton(this, wxID_ANY, label);
        b->Bind(wxEVT_BUTTON, [action](wxCommandEvent&) { action(); });
        row->Add(b, 0, wxRIGHT, 4);
    };
    button(edits, _("Change How..."), [this]() { AskMode(); });
    button(edits, _("Take a Figure Out..."), [this]() { AskFigure(true); });
    button(edits, _("Bring a Figure In..."), [this]() { AskFigure(false); });
    button(edits, _("No Gestures Here"), [this]() {
        if (const StageScene* s = Scene()) {
            (void)Edit({ "no-gestures", s->startMS, -1, "", "" });
        }
    });
    button(edits, _("Add a Gesture..."), [this]() { AskGesture(); });
    button(edits, _("Faster"), [this]() { ChangeSpeed(1.5); });
    button(edits, _("Slower"), [this]() { ChangeSpeed(1 / 1.5); });
    sizer->Add(edits, 0, wxBOTTOM, 4);
    auto* tools = new wxBoxSizer(wxHORIZONTAL);
    button(tools, _("Undo the Last Change"), [this]() { (void)Undo(); });
    button(tools, _("Apply the Changes"), [this]() {
        if (onRecompose) {
            onRecompose();
        }
    });
    button(tools, _("Choreography of the Installation..."), [this]() { OpenMap(); });
    button(tools, _("Layers of This Figure..."), [this]() { OpenLayers(); });
    button(tools, _("Compositions and Gestures..."), [this]() { OpenLibrary(); });
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

bool StageDirectorPanel::SetReport(const ReportFolder* report, const std::string& showDir, const std::string& fseqPath) {
    _fseqPath = fseqPath;
    const std::string hash = report != nullptr ? report->analysis.audioHash : std::string();
    const std::string editsPath = !showDir.empty() && !hash.empty() ? StageEditsPath(showDir, hash) : std::string();
    const std::string text = report != nullptr ? report->stageScore : std::string();
    if (report == _report && text == _scoreText && editsPath == _editsPath) {
        return false; // nothing new: a refresh of the window costs nothing here
    }
    _report = report;
    _showDir = showDir;
    _scoreText = text;
    _score = GlobalStageScore();
    (void)ReadScore(text, _score);
    if (editsPath != _editsPath) {
        _editsPath = editsPath;
        _undo.clear();
        (void)LoadEdits();
    }
    Fill();
    return true;
}

bool StageDirectorPanel::DirectorOn() const {
    return _on->GetValue();
}

void StageDirectorPanel::SetDirectorOn(bool on) {
    _on->SetValue(on);
    Fill();
}

std::string StageDirectorPanel::EditsText() const {
    return _edits.edits.empty() ? std::string() : ToJson(_edits);
}

bool StageDirectorPanel::LoadEdits() {
    _edits = StageEdits();
    std::string text, error;
    return _editsPath.empty() || !ReadTextFile(_editsPath, text) || FromJson(text, _edits, error);
}

bool StageDirectorPanel::SaveEdits() {
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

const StageScene* StageDirectorPanel::Scene() const {
    return _scene >= 0 && _scene < static_cast<int>(_score.scenes.size()) ? &_score.scenes[static_cast<size_t>(_scene)] : nullptr;
}

void StageDirectorPanel::Fill() {
    _list->DeleteAllItems();
    if (!DirectorOn()) {
        _state->SetLabel(_("The installation is not directed as one stage: each figure follows its own plan, as before."));
        _detail->SetValue("");
        return;
    }
    if (_score.scenes.empty()) {
        _state->SetLabel(_("Work out the composition on step 3 to see how the director uses the installation."));
        _detail->SetValue("");
        return;
    }
    int changed = 0;
    for (const StageScene& s : _score.scenes) {
        const long row = _list->InsertItem(_list->GetItemCount(), ClockText(s.startMS));
        int taking = 0;
        for (const SceneRole& r : s.roles) {
            taking += r.role != "rest" ? 1 : 0;
        }
        _list->SetItem(row, 1, StageModeWords(s.mode));
        _list->SetItem(row, 2, wxString::Format("%d", taking));
        _list->SetItem(row, 3, wxString::Format("%d", static_cast<int>(s.gestures.size())));
        _list->SetItem(row, 4, s.manual ? _("By you") : "");
        changed += s.manual ? 1 : 0;
    }
    wxString state = wxString::Format(_("%d scenes, %d gestures across the installation."), static_cast<int>(_score.scenes.size()), static_cast<int>(_score.gestures.size()));
    if (!_edits.edits.empty()) {
        state += " " + wxString::Format(_("%d changes of yours are saved; press \"Apply the Changes\" to see them in the score."), static_cast<int>(_edits.edits.size()));
    }
    _state->SetLabel(state);
    if (_scene >= 0 && _scene < _list->GetItemCount()) {
        _list->SetItemState(_scene, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    }
    ShowScene();
}

wxString StageDirectorPanel::Detail() const {
    return _detail->GetValue();
}

void StageDirectorPanel::ShowScene() {
    const StageScene* s = Scene();
    if (s == nullptr) {
        _detail->SetValue(_("Choose a scene to see who takes part and why."));
        return;
    }
    wxString out = wxString::Format(_("%s, from %s to %s."), StageModeWords(s->mode), ClockText(s->startMS), ClockText(s->endMS)) + "\n";
    for (const std::string& r : s->reasons) {
        out += "- " + Words(r) + "\n";
    }
    std::map<std::string, std::vector<std::string>> byRole;
    for (const SceneRole& r : s->roles) {
        byRole[r.role].push_back(r.figure);
    }
    for (const char* role : { "protagonist", "complement", "responder", "accent", "background" }) {
        const auto it = byRole.find(role);
        if (it == byRole.end()) {
            continue;
        }
        wxString names;
        for (size_t k = 0; k < it->second.size() && k < 6; ++k) {
            names += (k == 0 ? "" : ", ") + U(it->second[k]);
        }
        if (it->second.size() > 6) {
            names += wxString::Format(_(" and %d more"), static_cast<int>(it->second.size() - 6));
        }
        out += StageRoleWords(role) + ": " + names + "\n";
    }
    if (byRole.contains("rest")) {
        out += wxString::Format(_("%d figures rest."), static_cast<int>(byRole["rest"].size())) + "\n";
    }
    for (const std::string& id : s->gestures) {
        if (const GlobalGesture* g = _score.Gesture(id)) {
            out += wxString::Format(_("%s at %s (%d figures)"), GestureWords(g->kind), ClockText(g->startMS), static_cast<int>(g->participants.size()));
            if (!g->reasons.empty()) {
                out += ": " + Words(g->reasons.back());
            }
            out += "\n";
        }
    }
    _detail->SetValue(out);
}

bool StageDirectorPanel::SelectScene(int index) {
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

std::string StageDirectorPanel::Edit(const StageEdit& edit) {
    if (_editsPath.empty()) {
        return "no-song";
    }
    _undo.push_back(ToJson(_edits));
    // A new choice of the same kind for the same scene replaces the earlier one.
    std::erase_if(_edits.edits, [&](const StageEdit& e) {
        return e.action == edit.action && e.sceneStartMS == edit.sceneStartMS && e.atMS == edit.atMS && (edit.action == "mode" || edit.action == "no-gestures" || edit.action == "speed" || e.value == edit.value);
    });
    _edits.edits.push_back(edit);
    if (!SaveEdits()) {
        return "edits-not-saved";
    }
    Fill();
    return "";
}

std::string StageDirectorPanel::Undo() {
    if (_undo.empty()) {
        return "nothing-to-undo";
    }
    std::string error;
    StageEdits before;
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

void StageDirectorPanel::AskMode() {
    const StageScene* s = Scene();
    if (s == nullptr) {
        wxMessageBox(_("Choose a scene first."), _("Stage Director"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    const std::vector<StageMode> modes = { StageMode::Protagonist, StageMode::Duo, StageMode::Group, StageMode::Unison, StageMode::Propagation, StageMode::LocalWaves,
                                           StageMode::Cascade, StageMode::CallResponse, StageMode::Counterpoint, StageMode::FullEnsemble, StageMode::Rest };
    wxArrayString choices;
    for (StageMode m : modes) {
        choices.Add(StageModeWords(m));
    }
    const int pick = wxGetSingleChoiceIndex(_("How should the installation be used in this scene?"), _("Stage Director"), choices, this);
    if (pick >= 0) {
        (void)Edit({ "mode", s->startMS, -1, StageModeId(modes[static_cast<size_t>(pick)]), "" });
    }
}

void StageDirectorPanel::AskFigure(bool exclude) {
    const StageScene* s = Scene();
    if (s == nullptr) {
        wxMessageBox(_("Choose a scene first."), _("Stage Director"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    wxArrayString choices;
    for (const SceneRole& r : s->roles) {
        if ((r.role != "rest") == exclude) {
            choices.Add(U(r.figure));
        }
    }
    if (choices.IsEmpty()) {
        wxMessageBox(exclude ? _("No figure takes part in this scene.") : _("Every figure already takes part in this scene."), _("Stage Director"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    const int pick = wxGetSingleChoiceIndex(exclude ? _("Which figure should rest in this scene?") : _("Which figure should take part in this scene?"), _("Stage Director"), choices, this);
    if (pick >= 0) {
        (void)Edit({ exclude ? "exclude" : "include", s->startMS, -1, S(choices[pick]), "" });
    }
}

void StageDirectorPanel::AskGesture() {
    const StageScene* s = Scene();
    if (s == nullptr) {
        wxMessageBox(_("Choose a scene first."), _("Stage Director"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    const std::vector<GestureKind> kinds = { GestureKind::PropagatedWave, GestureKind::LocalWaves, GestureKind::JointPulse, GestureKind::CallResponse, GestureKind::Echo,
                                             GestureKind::SpatialSweep, GestureKind::Expansion, GestureKind::Contraction, GestureKind::Counterpoint, GestureKind::AmbientField };
    wxArrayString choices;
    for (GestureKind k : kinds) {
        choices.Add(GestureWords(k));
    }
    const int pick = wxGetSingleChoiceIndex(_("Which gesture, in the middle of this scene?"), _("Stage Director"), choices, this);
    if (pick >= 0) {
        const int at = ((s->startMS + s->endMS) / 2 / 25) * 25;
        (void)Edit({ "gesture", s->startMS, at, GestureKindId(kinds[static_cast<size_t>(pick)]), "" });
    }
}

void StageDirectorPanel::ChangeSpeed(double factor) {
    const StageScene* s = Scene();
    if (s == nullptr) {
        return;
    }
    for (const std::string& id : s->gestures) {
        const GlobalGesture* g = _score.Gesture(id);
        if (g != nullptr && (g->kind == GestureKind::PropagatedWave || g->kind == GestureKind::Echo || g->kind == GestureKind::Expansion || g->kind == GestureKind::SpatialSweep)) {
            (void)Edit({ "speed", s->startMS, g->startMS, fmt::format("{:.3f}", std::clamp(g->speed * factor, 0.06, 9.0)), "" });
            return;
        }
    }
    wxMessageBox(_("This scene has no travelling gesture to change."), _("Stage Director"), wxOK | wxICON_INFORMATION, this);
}

void StageDirectorPanel::OpenMap() {
    if (_report == nullptr || _report->stageLights.empty()) {
        wxMessageBox(_("Work out the composition on step 3 first."), _("Choreography of the Installation"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    const StageScene* s = Scene();
    StageMapDialog dialog(this, *_report, _fseqPath, s != nullptr ? s->startMS : 0);
    dialog.ShowModal();
}

void StageDirectorPanel::OpenLayers() {
    if (_report == nullptr || _score.scenes.empty()) {
        wxMessageBox(_("Work out the composition on step 3 first."), _("Layers of This Figure"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    std::string figure;
    if (const StageScene* s = Scene()) {
        for (const SceneRole& r : s->roles) {
            if (r.role == "protagonist") {
                figure = r.figure;
                break;
            }
        }
    }
    FigureLayersDialog dialog(this, *_report, _editsPath, figure);
    dialog.ShowModal();
    if (dialog.Changed()) {
        (void)LoadEdits();
        Fill();
    }
}

void StageDirectorPanel::OpenLibrary() {
    if (_showDir.empty()) {
        return;
    }
    StageLibraryDialog dialog(this, _showDir);
    dialog.ShowModal();
}
