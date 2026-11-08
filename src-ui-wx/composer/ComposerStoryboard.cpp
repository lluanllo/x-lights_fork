/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "ComposerStoryboard.h"

#include <wx/dcbuffer.h>
#include <wx/intl.h>
#include <wx/settings.h>

#include <algorithm>
#include <cmath>

#include "composer/VisualModel.h"
#include "utils/ExternalHooks.h"

using namespace composer;

namespace {

wxString U(const std::string& s) {
    return wxString::FromUTF8(s);
}

wxString Clock(int ms) {
    return wxString::Format("%d:%02d", ms / 60000, ms / 1000 % 60);
}

} // namespace

// ------------------------------------------------------------------ analysis

MediaAnalysisRunner::~MediaAnalysisRunner() {
    _cancel = true;
    Join();
}

void MediaAnalysisRunner::Join() {
    if (_thread.joinable()) {
        _thread.join();
    }
}

std::string MediaAnalysisRunner::Current() const {
    std::lock_guard<std::mutex> l(_lock);
    return _current;
}

MediaAnalysisSummary MediaAnalysisRunner::Summary() const {
    std::lock_guard<std::mutex> l(_lock);
    return _summary;
}

std::string MediaAnalysisRunner::Error() const {
    std::lock_guard<std::mutex> l(_lock);
    return _error;
}

void MediaAnalysisRunner::Start(const std::string& showDir, const std::vector<std::string>& folders, const std::string& modelDir) {
    Join();
    _cancel = false;
    _finished = false;
    _running = true;
    _done = 0;
    _total = 0;
    _thread = std::thread([this, showDir, folders, modelDir]() {
        std::string error;
        MediaLibrary saved;
        const bool hasSaved = ReadMediaLibrary(MediaLibraryPath(showDir), saved, error);
        MediaScanOptions so;
        so.showDir = showDir;
        so.folders = folders;
        so.cancelled = [this]() { return _cancel.load(); };
        so.progress = [this](int done, const std::string& name) {
            std::lock_guard<std::mutex> l(_lock);
            _current = name;
            _done = done;
        };
        MediaLibrary library = ScanMediaFolders(so, hasSaved ? &saved : nullptr);
        VisualConcepts concepts;
        (void)ReadVisualConcepts(DefaultVisualConceptsPath(), concepts, error);
        MediaAnalysisOptions o;
        o.showDir = showDir;
        o.concepts = concepts.concepts.empty() ? nullptr : &concepts;
        o.cancelled = [this]() { return _cancel.load(); };
        o.progress = [this](int done, int total, const std::string& name) {
            std::lock_guard<std::mutex> l(_lock);
            _current = name;
            _done = done;
            _total = total;
        };
        std::unique_ptr<VisualModel> model;
        ConceptIndex index;
        if (!modelDir.empty() && o.concepts != nullptr) {
            model = OpenVisualModel(modelDir, error);
            if (model != nullptr && BuildConceptIndex(*model, concepts, MediaCacheDir(showDir), index, error)) {
                o.model = model.get();
                o.index = &index;
            }
        }
        const MediaAnalysisSummary summary = AnalyzeMediaLibrary(library, o);
        std::string saveError;
        // A cancelled run keeps what it finished: the rest is looked at next time.
        if (!SaveMediaLibrary(MediaLibraryPath(showDir), library, saveError)) {
            error = saveError;
        }
        {
            std::lock_guard<std::mutex> l(_lock);
            _summary = summary;
            _error = o.model == nullptr && !modelDir.empty() ? error : saveError;
            _current.clear();
        }
        _running = false;
        _finished = true;
    });
}

// ------------------------------------------------------------------ model download

ModelDownloadRunner::~ModelDownloadRunner() {
    _cancel = true;
    Join();
}

void ModelDownloadRunner::Join() {
    if (_thread.joinable()) {
        _thread.join();
    }
}

std::string ModelDownloadRunner::Error() const {
    std::lock_guard<std::mutex> l(_lock);
    return _error;
}

void ModelDownloadRunner::Start(const std::string& root) {
    Join();
    _cancel = false;
    _finished = false;
    _ok = false;
    _running = true;
    _percent = 0;
    _thread = std::thread([this, root]() {
        std::string dir, error;
        const bool ok = InstallVisualModel(root, [this](int pct, const std::string&) {
            _percent = pct;
            return !_cancel.load();
        }, dir, error);
        {
            std::lock_guard<std::mutex> l(_lock);
            _error = error;
        }
        _ok = ok;
        _running = false;
        _finished = true;
    });
}

// ------------------------------------------------------------------ timeline

namespace {

constexpr int LABEL_W = 110;
constexpr int SCENE_H = 18;
constexpr int LANE_H = 58;
constexpr int GAP = 4;

} // namespace

StoryboardView::StoryboardView(wxWindow* parent) :
    wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, SCENE_H + 2 * (LANE_H + GAP) + 18)) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &StoryboardView::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &StoryboardView::OnClick, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        Refresh();
        e.Skip();
    });
}

void StoryboardView::SetStoryboard(const VisualMediaStoryboard* storyboard, int durationMS, std::map<std::string, wxBitmap> thumbnails) {
    _board = storyboard;
    _durationMS = durationMS;
    _thumbs = std::move(thumbnails);
    if (_board == nullptr || _selected >= static_cast<int>(_board->shots.size())) {
        _selected = -1;
    }
    Refresh();
}

void StoryboardView::Select(int shot) {
    _selected = shot;
    Refresh();
}

wxRect StoryboardView::ShotRect(int i) const {
    if (_board == nullptr || i < 0 || i >= static_cast<int>(_board->shots.size()) || _durationMS <= 0) {
        return {};
    }
    const StoryboardShot& s = _board->shots[static_cast<size_t>(i)];
    int lane = 0;
    for (size_t k = 0; k < _board->lanes.size(); ++k) {
        if (_board->lanes[k].target == s.lane) {
            lane = static_cast<int>(k);
        }
    }
    const int w = std::max(1, GetClientSize().x - LABEL_W - 4);
    const int x0 = LABEL_W + static_cast<int>(static_cast<double>(s.startMS) / _durationMS * w);
    const int x1 = LABEL_W + static_cast<int>(static_cast<double>(s.endMS) / _durationMS * w);
    return wxRect(x0, SCENE_H + GAP + lane * (LANE_H + GAP), std::max(2, x1 - x0 - 1), LANE_H);
}

void StoryboardView::OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    const wxSize size = GetClientSize();
    dc.SetBackground(wxBrush(wxColour(30, 30, 34)));
    dc.Clear();
    dc.SetFont(wxFont(wxFontInfo(8)));
    if (_board == nullptr || _board->lanes.empty() || _durationMS <= 0) {
        dc.SetTextForeground(wxColour(170, 170, 170));
        dc.DrawText(_("No visual storyboard yet: choose a folder of pictures and videos, then compose."), 8, 8);
        return;
    }
    const int w = std::max(1, size.x - LABEL_W - 4);
    const auto X = [&](int ms) { return LABEL_W + static_cast<int>(static_cast<double>(ms) / _durationMS * w); };
    // Scenes: the part each plays, alternating shades.
    int n = 0;
    for (const StoryboardScene& sc : _board->scenes) {
        const int x0 = X(sc.startMS), x1 = X(sc.endMS);
        const wxColour shade = n++ % 2 == 0 ? wxColour(48, 48, 56) : wxColour(40, 40, 46);
        dc.SetBrush(wxBrush(shade));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(x0, 0, x1 - x0, size.y);
        dc.SetTextForeground(wxColour(190, 190, 200));
        dc.SetClippingRegion(x0, 0, std::max(1, x1 - x0 - 2), SCENE_H);
        const wxString role = sc.role == "establish" ? _("opening") : sc.role == "verse" ? _("verse") : sc.role == "chorus" ? _("chorus") : sc.role == "instrumental" ? _("instrumental")
                              : sc.role == "bridge" ? _("bridge") : sc.role == "climax" ? _("climax") : sc.role == "ending" ? _("ending") : _("calm");
        dc.DrawText(role, x0 + 3, 2);
        dc.DestroyClippingRegion();
    }
    for (size_t k = 0; k < _board->lanes.size(); ++k) {
        const int y = SCENE_H + GAP + static_cast<int>(k) * (LANE_H + GAP);
        dc.SetBrush(wxBrush(wxColour(22, 22, 26)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, y, LABEL_W - 4, LANE_H);
        dc.SetTextForeground(*wxWHITE);
        dc.SetClippingRegion(2, y, LABEL_W - 8, LANE_H);
        dc.DrawText(U(_board->lanes[k].target), 4, y + 4);
        dc.SetTextForeground(wxColour(160, 160, 170));
        dc.DrawText(_board->lanes[k].role == "mega-tree" ? _("mega tree") : _("matrix"), 4, y + 20);
        dc.DestroyClippingRegion();
        // Gaps: the lights carry on there.
        for (const StoryboardGap& g : _board->gaps) {
            if (g.lane != _board->lanes[k].target) {
                continue;
            }
            dc.SetBrush(wxBrush(wxColour(70, 70, 70), wxBRUSHSTYLE_BDIAGONAL_HATCH));
            dc.DrawRectangle(X(g.startMS), y, X(g.endMS) - X(g.startMS), LANE_H);
        }
    }
    for (int i = 0; i < static_cast<int>(_board->shots.size()); ++i) {
        const StoryboardShot& s = _board->shots[static_cast<size_t>(i)];
        const wxRect r = ShotRect(i);
        const wxColour edge = s.state == DecisionState::Locked ? wxColour(240, 200, 60) : s.state == DecisionState::ManualEdit ? wxColour(120, 200, 255) : wxColour(110, 110, 120);
        dc.SetBrush(wxBrush(wxColour(15, 15, 18)));
        dc.SetPen(wxPen(edge));
        dc.DrawRectangle(r);
        dc.SetClippingRegion(wxRect(r).Deflate(1));
        const auto t = _thumbs.find(s.mediaId);
        if (t != _thumbs.end() && t->second.IsOk()) {
            // The picture repeated along the shot, as a film strip.
            const int tw = t->second.GetWidth();
            for (int x = r.x + 1; x < r.GetRight(); x += tw + 2) {
                dc.DrawBitmap(t->second, x, r.y + 1 + (r.height - 2 - t->second.GetHeight()) / 2);
            }
        }
        dc.SetBrush(wxBrush(wxColour(0, 0, 0, 160)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(r.x + 1, r.GetBottom() - 13, r.width - 2, 13);
        dc.SetTextForeground(*wxWHITE);
        dc.DrawText(U(s.name), r.x + 3, r.GetBottom() - 13);
        dc.DestroyClippingRegion();
        // Cuts and fades at the edges.
        dc.SetPen(wxPen(wxColour(255, 255, 255, 120)));
        if (s.fadeInMS > 0) {
            dc.DrawLine(r.x, r.GetBottom(), X(s.startMS + s.fadeInMS), r.y);
        }
        if (s.fadeOutMS > 0) {
            dc.DrawLine(X(s.endMS - s.fadeOutMS), r.y, r.GetRight(), r.GetBottom());
        }
        if (i == _selected) {
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(wxColour(255, 255, 255), 2));
            dc.DrawRectangle(wxRect(r).Inflate(1));
        }
    }
    // Time marks.
    dc.SetTextForeground(wxColour(150, 150, 160));
    const int bottom = SCENE_H + GAP + static_cast<int>(_board->lanes.size()) * (LANE_H + GAP);
    const int step = _durationMS > 240000 ? 30000 : _durationMS > 60000 ? 15000 : 5000;
    for (int ms = 0; ms <= _durationMS; ms += step) {
        dc.DrawText(Clock(ms), X(ms) + 2, bottom);
    }
}

void StoryboardView::OnClick(wxMouseEvent& e) {
    if (_board == nullptr) {
        return;
    }
    for (int i = 0; i < static_cast<int>(_board->shots.size()); ++i) {
        if (ShotRect(i).Contains(e.GetPosition())) {
            Select(i);
            if (onSelect) {
                onSelect(i);
            }
            return;
        }
    }
}

// ------------------------------------------------------------------ words

wxString VisualConceptName(const std::string& id) {
    static VisualConcepts concepts;
    static bool read = false;
    if (!read) {
        std::string error;
        read = true;
        (void)ReadVisualConcepts(DefaultVisualConceptsPath(), concepts, error);
    }
    const VisualConceptDef* d = concepts.Find(id);
    return d != nullptr ? wxGetTranslation(U(d->label)) : U(id);
}

wxString FramingName(const std::string& s) {
    if (s == "fill" || s == "video-fill") {
        return _("fills the figure");
    }
    if (s == "subject") {
        return _("framed on its subject");
    }
    if (s == "fit" || s == "video-fit" || s == "gif-fit") {
        return _("whole, with bands");
    }
    if (s == "pan-x" || s == "pan-y") {
        return _("slow pan");
    }
    if (s == "zoom-in") {
        return _("slow zoom in");
    }
    if (s == "zoom-out") {
        return _("slow zoom out");
    }
    if (s == "tree-faces") {
        return _("on both sides of the tree");
    }
    if (s == "tree-turn") {
        return _("turning round the tree");
    }
    if (s == "tree-wrap" || s == "video-wrap") {
        return _("all the way round the tree");
    }
    if (s == "video-crop" || s == "gif-fill") {
        return _("cropped to the figure");
    }
    return U(s);
}

wxString ShotWhy(const StoryboardShot& s) {
    wxString out;
    const auto add = [&](const wxString& part) {
        if (!part.empty()) {
            out += (out.empty() ? "" : "; ") + part;
        }
    };
    for (const Reason& r : s.reasons) {
        const wxString detail = U(r.detail);
        if (r.code == "theme-category") {
            add(r.detail == "neutral" || r.detail == "no-theme" ? _("fits any theme") : wxString::Format(_("theme: %s"), detail));
        } else if (r.code == "content") {
            wxString seen;
            const std::string ids = r.detail.substr(0, r.detail.find(" ("));
            size_t at = 0;
            while (at < ids.size()) {
                const size_t end = std::min(ids.find(", ", at), ids.size());
                seen += (seen.empty() ? "" : ", ") + VisualConceptName(ids.substr(at, end - at));
                at = end + 2;
            }
            add(wxString::Format(_("shows %s"), seen));
        } else if (r.code == "scene-role") {
            add(wxString::Format(_("suits the %s"), U(r.detail.substr(0, r.detail.find(' ')))));
        } else if (r.code == "repeat-keeps-media") {
            add(_("same picture as the repeated section"));
        } else if (r.code == "fragment") {
            add(wxString::Format(_("from %s of the video"), detail));
        } else if (r.code == "chosen-by-person" || r.code == "locked-by-person") {
            add(_("your choice"));
        } else if (r.code == "copied-into-show") {
            add(_("copied into the show"));
        }
    }
    add(FramingName(s.framing));
    if (s.state == DecisionState::Automatic) {
        add(s.level == "very-suitable" ? _("very suitable") : s.level == "acceptable" ? _("acceptable") : _("doubtful"));
    }
    return out;
}
