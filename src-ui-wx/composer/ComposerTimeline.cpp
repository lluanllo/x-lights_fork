/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "ComposerTimeline.h"

#include <wx/dcbuffer.h>
#include <wx/settings.h>

#include <algorithm>
#include <cmath>

#include "media/AudioManager.h"

namespace {

constexpr int PEAK_MS = 10;
constexpr int LABEL_WIDTH = 0;

// Row tops and heights, from the top of the window.
constexpr int RULER_H = 18;
constexpr int SECTIONS_Y = RULER_H;
constexpr int SECTIONS_H = 22;
constexpr int WAVE_Y = SECTIONS_Y + SECTIONS_H + 2;
constexpr int WAVE_H = 70;
constexpr int BEATS_Y = WAVE_Y + WAVE_H + 2;
constexpr int BEATS_H = 16;
constexpr int ONSETS_Y = BEATS_Y + BEATS_H + 2;
constexpr int ONSETS_H = 10;
constexpr int PHRASES_Y = ONSETS_Y + ONSETS_H + 6;
constexpr int PHRASES_H = 22;
constexpr int WORDS_Y = PHRASES_Y + PHRASES_H + 2;
constexpr int WORDS_H = 18;
constexpr int SHAPES_Y = WORDS_Y + WORDS_H + 2;
constexpr int SHAPES_H = 16;
constexpr int TOTAL_H = SHAPES_Y + SHAPES_H + 4;

wxColour SectionColour(composer::SectionKind kind) {
    switch (kind) {
    case composer::SectionKind::Intro:
        return wxColour(120, 160, 200);
    case composer::SectionKind::Verse:
        return wxColour(110, 170, 120);
    case composer::SectionKind::Chorus:
        return wxColour(220, 140, 70);
    case composer::SectionKind::Bridge:
        return wxColour(170, 120, 190);
    case composer::SectionKind::Instrumental:
        return wxColour(150, 150, 110);
    case composer::SectionKind::Outro:
        return wxColour(120, 140, 170);
    default:
        return wxColour(140, 140, 140);
    }
}

wxString SectionName(composer::SectionKind kind) {
    switch (kind) {
    case composer::SectionKind::Intro:
        return _("Intro");
    case composer::SectionKind::Verse:
        return _("Verse");
    case composer::SectionKind::Chorus:
        return _("Chorus");
    case composer::SectionKind::Bridge:
        return _("Bridge");
    case composer::SectionKind::Instrumental:
        return _("Instrumental");
    case composer::SectionKind::Outro:
        return _("Outro");
    default:
        return _("Section");
    }
}

} // namespace

ComposerTimeline::ComposerTimeline(wxWindow* parent) :
    wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, TOTAL_H + 20), wxHSCROLL | wxBORDER_SUNKEN) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetScrollRate(20, 0);
    SetMinSize(wxSize(400, TOTAL_H + 20));
    Bind(wxEVT_PAINT, &ComposerTimeline::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &ComposerTimeline::OnMouse, this);
    Bind(wxEVT_LEFT_UP, &ComposerTimeline::OnMouse, this);
    Bind(wxEVT_MOTION, &ComposerTimeline::OnMouse, this);
    Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& e) {
        if (e.ControlDown()) {
            SetPixelsPerSecond(_pixelsPerSecond * (e.GetWheelRotation() > 0 ? 1.25 : 0.8));
        } else {
            e.Skip();
        }
    });
}

void ComposerTimeline::SetSong(const composer::SongAnalysis* analysis, AudioManager* audio) {
    _analysis = analysis;
    _audio = audio;
    ComputePeaks();
    UpdateVirtualSize();
    Refresh();
}

void ComposerTimeline::SetLyrics(const composer::LyricAlignment* alignment) {
    _alignment = alignment;
    Refresh();
}

void ComposerTimeline::SetPixelsPerSecond(double pixels) {
    _pixelsPerSecond = std::clamp(pixels, 2.0, 800.0);
    UpdateVirtualSize();
    Refresh();
}

void ComposerTimeline::SetPlayhead(int ms) {
    if (ms == _playheadMS) {
        return;
    }
    _playheadMS = ms;
    if (ms >= 0) {
        // Keep the playhead in view while the song plays.
        int x0, y0;
        GetViewStart(&x0, &y0);
        int ux, uy;
        GetScrollPixelsPerUnit(&ux, &uy);
        const int left = x0 * std::max(1, ux);
        const int width = GetClientSize().GetWidth();
        const int x = ToX(ms);
        if (x < left || x > left + width - 40) {
            Scroll(std::max(0, x - width / 4) / std::max(1, ux), -1);
        }
    }
    Refresh(false);
}

int ComposerTimeline::ToX(int ms) const {
    return LABEL_WIDTH + static_cast<int>(std::lround(ms * _pixelsPerSecond / 1000.0));
}

int ComposerTimeline::ToMS(int x) const {
    return std::max(0, static_cast<int>(std::lround((x - LABEL_WIDTH) * 1000.0 / _pixelsPerSecond)));
}

void ComposerTimeline::UpdateVirtualSize() {
    const int duration = _analysis != nullptr ? _analysis->durationMS : 0;
    SetVirtualSize(ToX(duration) + 20, TOTAL_H);
}

void ComposerTimeline::ComputePeaks() {
    _peaks.clear();
    if (_audio == nullptr || _audio->GetRate() <= 0) {
        return;
    }
    const long rate = _audio->GetRate();
    const long samples = _audio->GetTrackSize();
    const long step = std::max<long>(1, rate * PEAK_MS / 1000);
    for (long start = 0; start < samples; start += step) {
        float low = 0;
        float high = 0;
        _audio->GetLeftDataMinMax(start, std::min(samples, start + step), low, high);
        _peaks.emplace_back(low, high);
    }
}

void ComposerTimeline::OnMouse(wxMouseEvent& event) {
    const wxPoint p = CalcUnscrolledPosition(event.GetPosition());
    const int ms = ToMS(p.x);
    if (event.LeftDown()) {
        _dragging = true;
        _cursorMS = ms;
        _selectFromMS = ms;
        _selectToMS = ms;
        CaptureMouse();
    } else if (event.Dragging() && _dragging) {
        _selectToMS = ms;
    } else if (event.LeftUp() && _dragging) {
        _dragging = false;
        _selectToMS = ms;
        if (HasCapture()) {
            ReleaseMouse();
        }
        if (_onCursor) {
            _onCursor(_cursorMS);
        }
    } else {
        event.Skip();
        return;
    }
    Refresh(false);
}

void ComposerTimeline::OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    DoPrepareDC(dc);
    const wxColour background = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    const wxColour text = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    const bool dark = background.GetLuminance() < 0.5;
    const wxColour faint = dark ? wxColour(70, 70, 70) : wxColour(215, 215, 215);
    dc.SetBackground(wxBrush(background));
    dc.Clear();
    if (_analysis == nullptr) {
        dc.SetTextForeground(text);
        dc.DrawText(_("Analyze the song to see it here."), 10, TOTAL_H / 2);
        return;
    }
    int vx, vy;
    GetViewStart(&vx, &vy);
    int ux, uy;
    GetScrollPixelsPerUnit(&ux, &uy);
    const int left = vx * std::max(1, ux);
    const int right = left + GetClientSize().GetWidth();
    const int fromMS = ToMS(left);
    const int toMS = ToMS(right) + 1;
    const auto visible = [&](int start, int end) { return end >= fromMS && start <= toMS; };
    wxFont labels = GetFont();
    labels.SetPointSize(std::max(7, labels.GetPointSize() - 1));
    dc.SetFont(labels);

    // Ruler, one mark a second, labelled where there is room.
    dc.SetPen(wxPen(faint));
    dc.SetTextForeground(text);
    const int labelEvery = _pixelsPerSecond >= 60 ? 1 : _pixelsPerSecond >= 20 ? 5 : _pixelsPerSecond >= 6 ? 15 : 30;
    for (int s = std::max(0, fromMS / 1000); s * 1000 <= std::min(toMS, _analysis->durationMS); ++s) {
        const int x = ToX(s * 1000);
        dc.DrawLine(x, RULER_H - 5, x, RULER_H);
        if (s % labelEvery == 0) {
            dc.DrawText(wxString::Format("%d:%02d", s / 60, s % 60), x + 2, 1);
        }
    }

    // Silences and the climax lie under everything else.
    for (const composer::MusicalEvent& e : _analysis->events) {
        if ((e.type != composer::MusicalEventType::Silence && e.type != composer::MusicalEventType::Climax) || !visible(e.timeMS, e.timeMS + e.durationMS)) {
            continue;
        }
        const bool silence = e.type == composer::MusicalEventType::Silence;
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(silence ? (dark ? wxColour(40, 40, 70) : wxColour(220, 225, 245)) : (dark ? wxColour(90, 40, 40) : wxColour(250, 215, 210))));
        dc.DrawRectangle(ToX(e.timeMS), WAVE_Y, std::max(2, ToX(e.timeMS + e.durationMS) - ToX(e.timeMS)), WAVE_H);
        dc.SetTextForeground(text);
        dc.DrawText(silence ? _("silence") : _("climax"), ToX(e.timeMS) + 2, WAVE_Y + 1);
    }

    for (const composer::SongSection& s : _analysis->sections) {
        if (!visible(s.startMS, s.endMS)) {
            continue;
        }
        const wxColour c = SectionColour(s.kind);
        dc.SetPen(wxPen(c.ChangeLightness(70)));
        dc.SetBrush(wxBrush(c));
        const int x = ToX(s.startMS);
        const int w = std::max(1, ToX(s.endMS) - x);
        dc.DrawRectangle(x, SECTIONS_Y, w, SECTIONS_H);
        dc.SetTextForeground(*wxBLACK);
        wxString label = SectionName(s.kind);
        if (s.repeatGroup >= 0) {
            label += wxString::Format(" %d", s.repeatGroup + 1);
        }
        dc.SetClippingRegion(x, SECTIONS_Y, w, SECTIONS_H);
        dc.DrawText(label, x + 4, SECTIONS_Y + 3);
        dc.DestroyClippingRegion();
    }

    // The waveform, one min-max line a pixel.
    const int middle = WAVE_Y + WAVE_H / 2;
    dc.SetPen(wxPen(dark ? wxColour(120, 180, 230) : wxColour(40, 90, 160)));
    if (!_peaks.empty()) {
        for (int x = std::max(left, LABEL_WIDTH); x < right; ++x) {
            const size_t a = static_cast<size_t>(ToMS(x) / PEAK_MS);
            const size_t b = std::max(a + 1, static_cast<size_t>(ToMS(x + 1) / PEAK_MS));
            if (a >= _peaks.size()) {
                break;
            }
            float low = 0;
            float high = 0;
            for (size_t i = a; i < std::min(b, _peaks.size()); ++i) {
                low = std::min(low, _peaks[i].first);
                high = std::max(high, _peaks[i].second);
            }
            dc.DrawLine(x, middle - static_cast<int>(high * WAVE_H / 2), x, middle - static_cast<int>(low * WAVE_H / 2) + 1);
        }
    } else {
        dc.SetTextForeground(text);
        dc.DrawText(_("(no waveform: the song could not be read here)"), left + 10, middle - 8);
    }

    // Beats, with the downbeats taller and stronger; onsets in their own row.
    for (const composer::MusicalEvent& e : _analysis->events) {
        if (!visible(e.timeMS, e.timeMS)) {
            continue;
        }
        const int x = ToX(e.timeMS);
        const bool manual = e.provenance != composer::Provenance::Detected;
        if (e.type == composer::MusicalEventType::Beat) {
            dc.SetPen(wxPen(manual ? wxColour(0, 150, 0) : text, 1));
            dc.DrawLine(x, BEATS_Y + BEATS_H / 2, x, BEATS_Y + BEATS_H);
        } else if (e.type == composer::MusicalEventType::Downbeat) {
            dc.SetPen(wxPen(manual ? wxColour(0, 170, 0) : wxColour(210, 40, 40), 2));
            dc.DrawLine(x, BEATS_Y, x, BEATS_Y + BEATS_H);
        } else if (e.type == composer::MusicalEventType::Onset) {
            dc.SetPen(wxPen(faint.ChangeLightness(dark ? 160 : 70)));
            dc.DrawLine(x, ONSETS_Y, x, ONSETS_Y + ONSETS_H);
        }
    }

    // Lyrics: phrases, then words, then the mouth shapes faces will draw.
    if (_alignment != nullptr) {
        const auto box = [&](int start, int end, int y, int h, const wxString& label, const wxColour& fill) {
            if (start < 0 || end <= start || !visible(start, end)) {
                return;
            }
            const int x = ToX(start);
            const int w = std::max(2, ToX(end) - x);
            dc.SetPen(wxPen(fill.ChangeLightness(60)));
            dc.SetBrush(wxBrush(fill));
            dc.DrawRectangle(x, y, w, h);
            dc.SetClippingRegion(x, y, w, h);
            dc.SetTextForeground(*wxBLACK);
            dc.DrawText(label, x + 3, y + 2);
            dc.DestroyClippingRegion();
        };
        for (const composer::AlignedPhrase& p : _alignment->phrases) {
            if (!p.instrumental) {
                box(p.startMS, p.endMS, PHRASES_Y, PHRASES_H, wxString::FromUTF8(p.text), p.state == composer::Provenance::Detected ? wxColour(250, 230, 150) : wxColour(170, 230, 170));
            }
        }
        for (const composer::AlignedWord& w : _alignment->words) {
            box(w.startMS, w.endMS, WORDS_Y, WORDS_H, wxString::FromUTF8(w.text), wxColour(235, 235, 200));
        }
        for (const composer::AlignedPhoneme& p : _alignment->phonemes) {
            if (p.shape != "rest") {
                box(p.startMS, p.endMS, SHAPES_Y, SHAPES_H, wxString::FromUTF8(p.shape), wxColour(205, 220, 245));
            }
        }
    }

    if (HasSelection()) {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(wxColour(60, 120, 255), wxBRUSHSTYLE_BDIAGONAL_HATCH)); // plain DCs draw no alpha
        dc.DrawRectangle(ToX(SelectionStart()), RULER_H, ToX(SelectionEnd()) - ToX(SelectionStart()), TOTAL_H - RULER_H);
    }
    dc.SetPen(wxPen(wxColour(30, 120, 255), 1));
    dc.DrawLine(ToX(_cursorMS), RULER_H, ToX(_cursorMS), TOTAL_H);
    if (_playheadMS >= 0) {
        dc.SetPen(wxPen(wxColour(0, 180, 0), 2));
        dc.DrawLine(ToX(_playheadMS), 0, ToX(_playheadMS), TOTAL_H);
    }
}
