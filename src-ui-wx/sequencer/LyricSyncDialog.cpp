/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "LyricSyncDialog.h"

#include <wx/accel.h>
#include <wx/button.h>
#include <wx/filedlg.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "composer/GenerationValidator.h"
#include "media/AudioManager.h"
#include "render/Effect.h"
#include "render/EffectLayer.h"
#include "render/Element.h"
#include "render/SequenceElements.h"
#include "utils/ExternalHooks.h"

namespace {

enum {
    ID_IMPORT_TEXT = wxID_HIGHEST + 1,
    ID_OPEN_FILE,
    ID_PLAY_PAUSE,
    ID_PLAY_PHRASE,
    ID_PLAY_FROM_HERE,
    ID_MARK_START,
    ID_MARK_END,
    ID_CONFIRM,
    ID_START_EARLIER,
    ID_START_LATER,
    ID_END_EARLIER,
    ID_END_LATER,
    ID_GENERATE,
};

constexpr int NUDGE_MS = 50;
constexpr int REPLAY_LEAD_MS = 1000;

wxString FormatTime(int ms) {
    if (ms < 0) {
        return "-";
    }
    return wxString::Format("%d:%02d.%03d", ms / 60000, (ms / 1000) % 60, ms % 1000);
}

std::string Translate(const char* msgid) {
    return wxGetTranslation(wxString::FromUTF8(msgid)).ToStdString(wxConvUTF8);
}

bool HasTimes(const composer::LyricCorrection* mark) {
    return mark != nullptr && mark->startMS >= 0 && mark->endMS > mark->startMS;
}

} // namespace

LyricSyncDialog::LyricSyncDialog(wxWindow* parent, SequenceElements* elements, TimingElement* element, AudioManager* audio, const std::string& sessionPath) :
    wxDialog(parent, wxID_ANY, _("Sync Lyrics"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    _elements(elements),
    _element(element),
    _audio(audio),
    _sessionPath(sessionPath),
    _timer(this) {
    BuildControls();
    LoadSession();
    Bind(wxEVT_TIMER, &LyricSyncDialog::OnTimer, this);
    _timer.Start(50);
}

LyricSyncDialog::~LyricSyncDialog() {
    _timer.Stop();
}

void LyricSyncDialog::BuildControls() {
    auto* main = new wxBoxSizer(wxVERTICAL);
    _mode = new wxStaticText(this, wxID_ANY, "");
    wxFont bold = _mode->GetFont();
    bold.MakeBold();
    _mode->SetFont(bold);
    main->Add(_mode, 0, wxALL, 8);

    auto* lyrics = new wxBoxSizer(wxHORIZONTAL);
    lyrics->Add(new wxButton(this, ID_IMPORT_TEXT, _("Import Lyrics Text...")), 0, wxRIGHT, 6);
    lyrics->Add(new wxButton(this, ID_OPEN_FILE, _("Open Lyrics File...")), 0);
    main->Add(lyrics, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

    _list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(760, 320), wxLC_REPORT | wxLC_SINGLE_SEL);
    _list->AppendColumn("#", wxLIST_FORMAT_RIGHT, 40);
    _list->AppendColumn(_("Phrase"), wxLIST_FORMAT_LEFT, 380);
    _list->AppendColumn(_("Start (m:ss)"), wxLIST_FORMAT_RIGHT, 100);
    _list->AppendColumn(_("End (m:ss)"), wxLIST_FORMAT_RIGHT, 100);
    _list->AppendColumn(_("Status"), wxLIST_FORMAT_LEFT, 120);
    main->Add(_list, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);

    auto* play = new wxBoxSizer(wxHORIZONTAL);
    _clock = new wxStaticText(this, wxID_ANY, FormatTime(0), wxDefaultPosition, wxSize(90, -1));
    play->Add(_clock, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    play->Add(new wxButton(this, ID_PLAY_PAUSE, _("Play / Pause")), 0, wxRIGHT, 6);
    play->Add(new wxButton(this, ID_PLAY_PHRASE, _("Play Phrase")), 0, wxRIGHT, 6);
    play->Add(new wxButton(this, ID_PLAY_FROM_HERE, _("Play From Here")), 0);
    main->Add(play, 0, wxALL, 8);

    auto* mark = new wxBoxSizer(wxHORIZONTAL);
    mark->Add(new wxButton(this, ID_MARK_START, _("Mark Start")), 0, wxRIGHT, 6);
    mark->Add(new wxButton(this, ID_MARK_END, _("Mark End")), 0, wxRIGHT, 6);
    mark->Add(new wxButton(this, ID_CONFIRM, _("Confirm Phrase")), 0);
    main->Add(mark, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

    auto* adjust = new wxBoxSizer(wxHORIZONTAL);
    adjust->Add(new wxStaticText(this, wxID_ANY, _("Adjust Time:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    adjust->Add(new wxButton(this, ID_START_EARLIER, _("Start -50 ms")), 0, wxRIGHT, 4);
    adjust->Add(new wxButton(this, ID_START_LATER, _("Start +50 ms")), 0, wxRIGHT, 12);
    adjust->Add(new wxButton(this, ID_END_EARLIER, _("End -50 ms")), 0, wxRIGHT, 4);
    adjust->Add(new wxButton(this, ID_END_LATER, _("End +50 ms")), 0);
    main->Add(adjust, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

    main->Add(new wxStaticText(this, wxID_ANY, _("Keys: Space plays or pauses, Z marks the start, X marks the end, C confirms the phrase.")), 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->Add(new wxButton(this, ID_GENERATE, _("Generate Words and Phonemes")), 0, wxRIGHT, 6);
    buttons->AddStretchSpacer();
    buttons->Add(new wxButton(this, wxID_CLOSE, _("Close")), 0);
    main->Add(buttons, 0, wxEXPAND | wxALL, 8);
    SetSizerAndFit(main);

    wxAcceleratorEntry keys[] = {
        wxAcceleratorEntry(wxACCEL_NORMAL, WXK_SPACE, ID_PLAY_PAUSE),
        wxAcceleratorEntry(wxACCEL_NORMAL, 'Z', ID_MARK_START),
        wxAcceleratorEntry(wxACCEL_NORMAL, 'X', ID_MARK_END),
        wxAcceleratorEntry(wxACCEL_NORMAL, 'C', ID_CONFIRM),
    };
    SetAcceleratorTable(wxAcceleratorTable(static_cast<int>(std::size(keys)), keys));

    Bind(wxEVT_BUTTON, &LyricSyncDialog::OnImportText, this, ID_IMPORT_TEXT);
    Bind(wxEVT_BUTTON, &LyricSyncDialog::OnOpenFile, this, ID_OPEN_FILE);
    Bind(wxEVT_BUTTON, &LyricSyncDialog::OnPlayPause, this, ID_PLAY_PAUSE);
    Bind(wxEVT_MENU, &LyricSyncDialog::OnPlayPause, this, ID_PLAY_PAUSE);
    Bind(wxEVT_BUTTON, &LyricSyncDialog::OnPlayPhrase, this, ID_PLAY_PHRASE);
    Bind(wxEVT_BUTTON, &LyricSyncDialog::OnPlayFromHere, this, ID_PLAY_FROM_HERE);
    Bind(wxEVT_BUTTON, &LyricSyncDialog::OnMarkStart, this, ID_MARK_START);
    Bind(wxEVT_MENU, &LyricSyncDialog::OnMarkStart, this, ID_MARK_START);
    Bind(wxEVT_BUTTON, &LyricSyncDialog::OnMarkEnd, this, ID_MARK_END);
    Bind(wxEVT_MENU, &LyricSyncDialog::OnMarkEnd, this, ID_MARK_END);
    Bind(wxEVT_BUTTON, &LyricSyncDialog::OnConfirm, this, ID_CONFIRM);
    Bind(wxEVT_MENU, &LyricSyncDialog::OnConfirm, this, ID_CONFIRM);
    Bind(wxEVT_BUTTON, &LyricSyncDialog::OnNudge, this, ID_START_EARLIER, ID_END_LATER);
    Bind(wxEVT_BUTTON, &LyricSyncDialog::OnGenerate, this, ID_GENERATE);
    Bind(wxEVT_BUTTON, &LyricSyncDialog::OnClose, this, wxID_CLOSE);
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
        wxCommandEvent unused;
        OnClose(unused);
    });

    if (_audio == nullptr) {
        for (int id : { ID_PLAY_PAUSE, ID_PLAY_PHRASE, ID_PLAY_FROM_HERE, ID_MARK_START, ID_MARK_END }) {
            FindWindow(id)->Enable(false);
        }
    }
}

void LyricSyncDialog::LoadSession() {
    std::string error;
    if (FileExists(_sessionPath) && composer::LoadLyricSession(_sessionPath, _session, error)) {
        SetLyrics(_session.lyrics);
        return;
    }
    if (!error.empty()) {
        wxMessageBox(wxString::Format(_("The saved marks could not be read and have been left untouched:\n%s"), wxString::FromUTF8(error)), _("Sync Lyrics"), wxOK | wxICON_WARNING, this);
        _sessionPath += ".unreadable-kept";
    }
    // A track that already has phrases is someone's earlier work: its times are kept
    // as corrections, so nothing has to be marked again.
    if (_element != nullptr && _element->GetEffectLayerCount() > 0) {
        std::string lyrics;
        std::vector<composer::LyricCorrection> marks;
        EffectLayer* phrases = _element->GetEffectLayer(0);
        for (int i = 0; i < phrases->GetEffectCount(); ++i) {
            lyrics += phrases->GetEffect(i)->GetEffectName() + "\n";
        }
        const composer::LyricDocument document = composer::ParsePlainLyrics(lyrics);
        int i = 0;
        for (const composer::LyricLine* line : document.Lines()) {
            while (i < phrases->GetEffectCount() && composer::LyricKey(phrases->GetEffect(i)->GetEffectName()).empty()) {
                ++i;
            }
            if (i >= phrases->GetEffectCount()) {
                break;
            }
            composer::LyricCorrection mark;
            mark.targetId = line->id;
            mark.state = composer::Provenance::Corrected;
            mark.startMS = phrases->GetEffect(i)->GetStartTimeMS();
            mark.endMS = phrases->GetEffect(i)->GetEndTimeMS();
            mark.originalStartMS = mark.startMS;
            mark.originalEndMS = mark.endMS;
            marks.push_back(mark);
            ++i;
        }
        _session.corrections = marks;
        _session.lyrics = lyrics;
    }
    _session.trackName = _element != nullptr ? _element->GetName() : "";
    SetLyrics(_session.lyrics);
}

void LyricSyncDialog::SaveSession() {
    std::string error;
    if (!composer::SaveLyricSession(_sessionPath, _session, error)) {
        wxMessageBox(wxString::Format(_("The marks could not be saved:\n%s"), wxString::FromUTF8(error)), _("Sync Lyrics"), wxOK | wxICON_WARNING, this);
    }
}

void LyricSyncDialog::SetLyrics(const std::string& lyrics) {
    _session.lyrics = lyrics;
    _document = composer::ParseLyrics(lyrics);
    _rows.clear();
    for (const composer::LyricLine* line : _document.Lines()) {
        if (!line->instrumental && !line->words.empty()) {
            _rows.push_back(line);
        }
    }
    RefreshRows();
}

const composer::LyricCorrection* LyricSyncDialog::MarkOf(int row) const {
    if (row < 0 || row >= static_cast<int>(_rows.size())) {
        return nullptr;
    }
    for (const composer::LyricCorrection& c : _session.corrections) {
        if (c.level == composer::LyricCorrection::Level::Phrase && c.targetId == _rows[row]->id) {
            return &c;
        }
    }
    return nullptr;
}

composer::LyricCorrection LyricSyncDialog::MarkFor(int row) const {
    if (const composer::LyricCorrection* existing = MarkOf(row)) {
        return *existing;
    }
    composer::LyricCorrection mark;
    mark.targetId = _rows[row]->id;
    mark.originalStartMS = _rows[row]->startMS;
    mark.originalEndMS = _rows[row]->endMS;
    return mark;
}

void LyricSyncDialog::RefreshRows() {
    const int selected = SelectedRow();
    _list->DeleteAllItems();
    for (size_t r = 0; r < _rows.size(); ++r) {
        const composer::LyricLine& line = *_rows[r];
        const composer::LyricCorrection* mark = MarkOf(static_cast<int>(r));
        int start = line.startMS;
        int end = line.endMS;
        wxString status = start >= 0 ? _("approximate") : _("not marked");
        if (mark != nullptr) {
            start = mark->startMS;
            end = mark->endMS;
            switch (mark->state) {
            case composer::Provenance::Locked:
                status = _("confirmed");
                break;
            case composer::Provenance::Corrected:
                status = _("adjusted");
                break;
            default:
                status = end > start ? _("marked") : _("start marked");
                break;
            }
        }
        const long item = _list->InsertItem(static_cast<long>(r), wxString::Format("%d", static_cast<int>(r) + 1));
        _list->SetItem(item, 1, wxString::FromUTF8(line.text));
        _list->SetItem(item, 2, FormatTime(start));
        _list->SetItem(item, 3, FormatTime(end));
        _list->SetItem(item, 4, status);
    }
    SelectRow(selected < 0 ? 0 : std::min(selected, static_cast<int>(_rows.size()) - 1));
    UpdateMode();
}

void LyricSyncDialog::UpdateMode() {
    int marked = 0;
    int approximate = 0;
    for (size_t r = 0; r < _rows.size(); ++r) {
        if (HasTimes(MarkOf(static_cast<int>(r)))) {
            ++marked;
        } else if (_rows[r]->startMS >= 0) {
            ++approximate;
        }
    }
    wxString mode;
    if (_rows.empty()) {
        mode = _("No lyrics yet: import or write them first.");
    } else if (marked == static_cast<int>(_rows.size())) {
        mode = _("Manual sync");
    } else if (marked + approximate == static_cast<int>(_rows.size())) {
        mode = _("Approximate sync");
    } else {
        mode = _("Not synchronized");
    }
    _mode->SetLabel(wxString::Format(_("%s: %d of %d phrases marked by hand"), mode, marked, static_cast<int>(_rows.size())));
}

int LyricSyncDialog::SelectedRow() const {
    return static_cast<int>(_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED));
}

void LyricSyncDialog::SelectRow(int row) {
    if (row < 0 || row >= _list->GetItemCount()) {
        return;
    }
    _list->SetItemState(row, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    _list->EnsureVisible(row);
}

int LyricSyncDialog::CurrentTime() const {
    if (_audio != nullptr && _audio->GetPlayingState() == MEDIAPLAYINGSTATE::PLAYING) {
        return static_cast<int>(_audio->Tell());
    }
    return _lastTime;
}

void LyricSyncDialog::OnImportText(wxCommandEvent&) {
    wxTextEntryDialog dialog(this, _("Paste or write the lyrics. Optional tags such as [Chorus], and blank lines between verses, are kept."), _("Sync Lyrics"),
                             wxString::FromUTF8(_session.lyrics), wxOK | wxCANCEL | wxTE_MULTILINE);
    dialog.SetSize(wxSize(600, 500));
    if (dialog.ShowModal() == wxID_OK) {
        SetLyrics(dialog.GetValue().ToStdString(wxConvUTF8));
        SaveSession();
    }
}

void LyricSyncDialog::OnOpenFile(wxCommandEvent&) {
    wxFileDialog dialog(this, _("Open Lyrics File..."), "", "", _("Lyrics files (*.txt;*.lrc;*.srt)|*.txt;*.lrc;*.srt"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) {
        return;
    }
    const std::string path = dialog.GetPath().ToStdString(wxConvUTF8);
    ObtainAccessToURL(path, false);
    std::ifstream in(std::filesystem::path(dialog.GetPath().ToStdWstring()), std::ios::binary);
    std::ostringstream content;
    content << in.rdbuf();
    std::string text = content.str();
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) {
        text.erase(0, 3);
    }
    SetLyrics(text);
    SaveSession();
}

void LyricSyncDialog::OnPlayPause(wxCommandEvent&) {
    if (_audio == nullptr) {
        return;
    }
    switch (_audio->GetPlayingState()) {
    case MEDIAPLAYINGSTATE::PLAYING:
        _audio->Pause();
        break;
    case MEDIAPLAYINGSTATE::PAUSED:
        _audio->Play();
        break;
    default:
        _audio->Seek(_lastTime);
        _audio->Play();
        _startedPlayback = true;
        break;
    }
}

void LyricSyncDialog::OnPlayPhrase(wxCommandEvent&) {
    const int row = SelectedRow();
    const composer::LyricCorrection* mark = MarkOf(row);
    if (_audio == nullptr || row < 0) {
        return;
    }
    const int start = mark != nullptr ? mark->startMS : _rows[row]->startMS;
    const int end = mark != nullptr ? mark->endMS : _rows[row]->endMS;
    if (start < 0 || end <= start) {
        wxMessageBox(_("Mark the start and end of this phrase first."), _("Sync Lyrics"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    _audio->Play(start, end - start);
    _startedPlayback = true;
}

void LyricSyncDialog::OnPlayFromHere(wxCommandEvent&) {
    const int row = SelectedRow();
    if (_audio == nullptr || row < 0) {
        return;
    }
    const composer::LyricCorrection* mark = MarkOf(row);
    int start = mark != nullptr ? mark->startMS : _rows[row]->startMS;
    if (start < 0) {
        start = _lastTime;
    }
    _lastTime = std::max(0, start - REPLAY_LEAD_MS);
    _audio->Stop();
    _audio->Seek(_lastTime);
    _audio->Play();
    _startedPlayback = true;
}

void LyricSyncDialog::OnMarkStart(wxCommandEvent&) {
    const int row = SelectedRow();
    if (row < 0) {
        return;
    }
    const int now = CurrentTime();
    composer::LyricCorrection mark = MarkFor(row);
    mark.startMS = now;
    if (mark.endMS <= now) {
        mark.endMS = -1;
    }
    mark.state = composer::Provenance::Confirmed;
    composer::UpsertCorrection(_session.corrections, mark);
    // Starting a phrase ends the one before, if it was still open.
    if (row > 0) {
        const composer::LyricCorrection* previous = MarkOf(row - 1);
        if (previous != nullptr && previous->startMS >= 0 && previous->startMS < now && previous->endMS < 0) {
            composer::LyricCorrection closed = *previous;
            closed.endMS = now;
            composer::UpsertCorrection(_session.corrections, closed);
        }
    }
    SaveSession();
    RefreshRows();
}

void LyricSyncDialog::OnMarkEnd(wxCommandEvent&) {
    const int row = SelectedRow();
    if (row < 0) {
        return;
    }
    const int now = CurrentTime();
    composer::LyricCorrection mark = MarkFor(row);
    if (mark.startMS < 0 || now <= mark.startMS) {
        wxMessageBox(_("Mark the start of this phrase first."), _("Sync Lyrics"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    mark.endMS = now;
    mark.state = composer::Provenance::Confirmed;
    composer::UpsertCorrection(_session.corrections, mark);
    SaveSession();
    RefreshRows();
    SelectRow(row + 1);
}

void LyricSyncDialog::OnConfirm(wxCommandEvent&) {
    const int row = SelectedRow();
    if (row < 0) {
        return;
    }
    composer::LyricCorrection mark = MarkFor(row);
    if (mark.startMS < 0 || mark.endMS <= mark.startMS) {
        wxMessageBox(_("Mark the start and end of this phrase first."), _("Sync Lyrics"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    mark.state = composer::Provenance::Locked;
    composer::UpsertCorrection(_session.corrections, mark);
    SaveSession();
    RefreshRows();
}

void LyricSyncDialog::OnNudge(wxCommandEvent& event) {
    const int row = SelectedRow();
    if (row < 0) {
        return;
    }
    composer::LyricCorrection mark = MarkFor(row);
    if (mark.startMS < 0) {
        mark.startMS = _rows[row]->startMS;
        mark.endMS = _rows[row]->endMS;
    }
    if (mark.startMS < 0) {
        return;
    }
    const bool start = event.GetId() == ID_START_EARLIER || event.GetId() == ID_START_LATER;
    const int delta = (event.GetId() == ID_START_EARLIER || event.GetId() == ID_END_EARLIER) ? -NUDGE_MS : NUDGE_MS;
    int& value = start ? mark.startMS : mark.endMS;
    if (value < 0) {
        return;
    }
    value = std::max(0, value + delta);
    if (mark.endMS >= 0 && mark.endMS <= mark.startMS) {
        return;
    }
    if (mark.state != composer::Provenance::Locked) {
        mark.state = composer::Provenance::Corrected;
    }
    composer::UpsertCorrection(_session.corrections, mark);
    SaveSession();
    RefreshRows();
}

void LyricSyncDialog::OnGenerate(wxCommandEvent&) {
    composer::AlignerInput input;
    input.document = &_document;
    input.durationMS = _elements->GetSequenceEnd();
    int incomplete = 0;
    for (const composer::LyricCorrection& c : _session.corrections) {
        // A phrase with only its start marked is not given an end it does not have.
        if (c.level == composer::LyricCorrection::Level::Phrase && !HasTimes(&c)) {
            ++incomplete;
            continue;
        }
        input.corrections.push_back(c);
    }
    composer::AlignerOptions options;
    options.frameMS = static_cast<int>(std::lround(1000.0 / _elements->GetFrequency()));
    const composer::LyricAlignment alignment = composer::AlignLyrics(input, options);
    const composer::TimingTrackSpec track = composer::BuildLyricTrack(alignment, _element->GetName());
    if (track.layers[0].empty()) {
        wxMessageBox(_("Mark the start and end of at least one phrase first."), _("Sync Lyrics"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    const composer::ValidationReport report = composer::ValidateLyrics(alignment, track, _elements->GetSequenceEnd(), options.frameMS);

    wxString summary = wxString::Format(_("Replace the words and phonemes of \"%s\" with %d phrases, %d words and %d mouth shapes?"), wxString::FromUTF8(_element->GetName()),
                                        static_cast<int>(track.layers[0].size()), static_cast<int>(track.layers[1].size()), static_cast<int>(track.layers[2].size()));
    if (incomplete > 0) {
        summary += "\n\n" + wxString::Format(_("%d phrases have only their start marked and will be left out."), incomplete);
    }
    int shown = 0;
    for (const composer::ValidationIssue& issue : report.issues) {
        if (issue.severity == composer::IssueSeverity::Info || shown >= 8) {
            continue;
        }
        summary += "\n\n- " + wxString::FromUTF8(composer::DescribeIssue(issue, Translate)) + "\n  " + wxString::FromUTF8(composer::DescribeSuggestion(issue, Translate));
        ++shown;
    }
    if (report.ErrorCount() > 0) {
        wxMessageBox(summary, _("Sync Lyrics"), wxOK | wxICON_ERROR, this);
        return;
    }
    if (wxMessageBox(summary, _("Sync Lyrics"), wxYES_NO | wxICON_QUESTION, this) != wxYES) {
        return;
    }

    _elements->get_undo_mgr().CreateUndoStep();
    _element->SetFixedTiming(0);
    for (int k = _element->GetEffectLayerCount() - 1; k >= 0; --k) {
        _element->RemoveEffectLayer(k);
    }
    for (const auto& marks : track.layers) {
        EffectLayer* layer = _element->AddEffectLayer();
        for (const composer::TimingMark& mark : marks) {
            Effect* effect = layer->AddEffect(0, mark.label, "", "", mark.startMS, mark.endMS, EFFECT_NOT_SELECTED, false);
            if (effect != nullptr) {
                _elements->get_undo_mgr().CaptureAddedEffect(_element->GetName(), layer->GetIndex(), effect->GetID());
            }
        }
    }
    _trackChanged = true;
    SaveSession();
}

void LyricSyncDialog::OnClose(wxCommandEvent&) {
    if (_audio != nullptr && _startedPlayback && _audio->GetPlayingState() != MEDIAPLAYINGSTATE::STOPPED) {
        _audio->Stop();
    }
    EndModal(_trackChanged ? wxID_OK : wxID_CANCEL);
}

void LyricSyncDialog::OnTimer(wxTimerEvent&) {
    if (_audio != nullptr && _audio->GetPlayingState() == MEDIAPLAYINGSTATE::PLAYING) {
        _lastTime = static_cast<int>(_audio->Tell());
    }
    _clock->SetLabel(FormatTime(_lastTime));
}
