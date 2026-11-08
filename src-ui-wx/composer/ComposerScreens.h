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

#include <string>
#include <vector>

#include "composer/CompositionPlan.h"
#include "composer/MediaSurfaces.h"
#include "composer/ModelCapabilities.h"

class wxCheckBox;
class wxChoice;
class wxListCtrl;
class wxSpinCtrl;
class wxStaticText;

// The display's screens and the part each plays: the main screen, a second one, the
// mega tree, the words, extra pictures, only lights, or nothing; how the words and the
// pictures share a screen; and, for a round tree, where its front is and which way it
// turns. Every change is shown at once with what the program would then do.
class ScreensDialog : public wxDialog {
public:
    ScreensDialog(wxWindow* parent, const composer::ModelCapabilities& caps, const composer::CompositionPlan* plan, const composer::ScreenSettings& settings);

    [[nodiscard]] const composer::ScreenSettings& Settings() const { return _settings; }
    [[nodiscard]] const composer::SurfacePlan& Preview() const { return _preview; }

    // The same calls the controls make, for the UI test.
    void SetMode(composer::LyricsMode mode);
    void SetPreview(bool preview);
    bool SetRole(const std::string& target, composer::SurfaceRole role);
    bool SetAutomatic(const std::string& target);
    bool SetTreeFront(const std::string& target, double frontOffset, int turn);

private:
    void Replan();
    void Fill();
    void ShowSelected();
    [[nodiscard]] std::string Selected() const;
    composer::ScreenChoice& ChoiceFor(const std::string& target);

    const composer::ModelCapabilities& _caps;
    const composer::CompositionPlan* _plan = nullptr;
    composer::ScreenSettings _settings;
    composer::SurfacePlan _preview;
    bool _filling = false;

    wxChoice* _mode = nullptr;
    wxStaticText* _modeInfo = nullptr;
    wxCheckBox* _forPreview = nullptr;
    wxListCtrl* _list = nullptr;
    wxChoice* _role = nullptr;
    wxSpinCtrl* _front = nullptr;
    wxChoice* _turn = nullptr;
    wxStaticText* _details = nullptr;
    wxStaticText* _warnings = nullptr;
};

// What a screen's role is called for a person.
[[nodiscard]] wxString ScreenRoleName(composer::SurfaceRole role);
// The same from the id kept in a storyboard lane ("main", "tree", ...).
[[nodiscard]] wxString ScreenRoleNameOf(const std::string& id);
// How words and pictures share the screens, said simply.
[[nodiscard]] wxString LyricsModeName(composer::LyricsMode mode);
// A plan's warnings about the screens, said simply, each once.
[[nodiscard]] std::vector<wxString> ScreenWarnings(const composer::SurfacePlan& plan);
