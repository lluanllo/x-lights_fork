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
#include <wx/timer.h>

#include <string>
#include <vector>

#include "composer/LyricAligner.h"
#include "composer/LyricDocument.h"

class AudioManager;
class SequenceElements;
class TimingElement;
class wxButton;
class wxListCtrl;
class wxListEvent;
class wxStaticText;

// Marks lyric phrases by ear and turns them into the phrases, words and phonemes a
// singing face reads. Marks are stored apart from the track, next to the sequence, so
// generating again never loses what was marked.
class LyricSyncDialog : public wxDialog {
public:
    LyricSyncDialog(wxWindow* parent, SequenceElements* elements, TimingElement* element, AudioManager* audio, const std::string& sessionPath);
    // Only the marks, with no sequence open: Show Composer hands them to the generation,
    // which builds the track itself. `lyrics` is used when no session is saved yet.
    LyricSyncDialog(wxWindow* parent, AudioManager* audio, int durationMS, int frameMS, const std::string& sessionPath, const std::string& lyrics, const std::string& trackName);
    ~LyricSyncDialog() override;

    [[nodiscard]] bool TrackChanged() const { return _trackChanged; }

private:
    void BuildControls();
    void LoadSession();
    void SaveSession();
    void SetLyrics(const std::string& lyrics);
    void RefreshRows();
    void UpdateMode();
    int SelectedRow() const;
    void SelectRow(int row);
    int CurrentTime() const;
    const composer::LyricCorrection* MarkOf(int row) const;
    composer::LyricCorrection MarkFor(int row) const;

    void OnImportText(wxCommandEvent& event);
    void OnOpenFile(wxCommandEvent& event);
    void OnPlayPause(wxCommandEvent& event);
    void OnPlayPhrase(wxCommandEvent& event);
    void OnPlayFromHere(wxCommandEvent& event);
    void OnMarkStart(wxCommandEvent& event);
    void OnMarkEnd(wxCommandEvent& event);
    void OnConfirm(wxCommandEvent& event);
    void OnNudge(wxCommandEvent& event);
    void OnGenerate(wxCommandEvent& event);
    void OnClose(wxCommandEvent& event);
    void OnTimer(wxTimerEvent& event);

    SequenceElements* _elements = nullptr;
    TimingElement* _element = nullptr;
    AudioManager* _audio = nullptr;
    int _durationMS = 0;
    int _frameMS = 25;
    bool _sessionOnly = false;
    std::string _sessionPath;
    composer::LyricSession _session;
    composer::LyricDocument _document;
    std::vector<const composer::LyricLine*> _rows;
    int _lastTime = 0;
    bool _startedPlayback = false;
    bool _trackChanged = false;

    wxStaticText* _mode = nullptr;
    wxStaticText* _clock = nullptr;
    wxListCtrl* _list = nullptr;
    wxTimer _timer;
};
