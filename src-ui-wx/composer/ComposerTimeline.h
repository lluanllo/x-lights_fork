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

#include <wx/scrolwin.h>

#include <algorithm>
#include <functional>
#include <vector>

#include "composer/LyricAligner.h"
#include "composer/SongAnalysis.h"

class AudioManager;

// The song on one time scale: its waveform, sections, beats, bars, onsets, silences and
// climax, and under them the lyrics' phrases, words and mouth shapes, so a phrase can be
// read against the moment it is sung. Click sets the cursor, drag selects a stretch.
class ComposerTimeline : public wxScrolledWindow {
public:
    explicit ComposerTimeline(wxWindow* parent);

    void SetSong(const composer::SongAnalysis* analysis, AudioManager* audio);
    void SetLyrics(const composer::LyricAlignment* alignment);
    void SetPixelsPerSecond(double pixels);
    [[nodiscard]] double PixelsPerSecond() const { return _pixelsPerSecond; }
    void SetPlayhead(int ms);
    [[nodiscard]] int Cursor() const { return _cursorMS; }
    [[nodiscard]] int SelectionStart() const { return std::min(_selectFromMS, _selectToMS); }
    [[nodiscard]] int SelectionEnd() const { return std::max(_selectFromMS, _selectToMS); }
    [[nodiscard]] bool HasSelection() const { return SelectionEnd() - SelectionStart() > 50; }
    void SetCursorListener(std::function<void(int ms)> listener) { _onCursor = std::move(listener); }

private:
    void OnPaint(wxPaintEvent& event);
    void OnMouse(wxMouseEvent& event);
    void UpdateVirtualSize();
    void ComputePeaks();
    [[nodiscard]] int ToX(int ms) const;
    [[nodiscard]] int ToMS(int x) const;

    const composer::SongAnalysis* _analysis = nullptr;
    const composer::LyricAlignment* _alignment = nullptr;
    AudioManager* _audio = nullptr;
    std::vector<std::pair<float, float>> _peaks; // min and max per 10 ms
    double _pixelsPerSecond = 40;
    int _cursorMS = 0;
    int _selectFromMS = 0;
    int _selectToMS = 0;
    int _playheadMS = -1;
    bool _dragging = false;
    std::function<void(int)> _onCursor;
};
