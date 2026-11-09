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

#include <map>
#include <string>
#include <vector>

#include "composer/ArtisticPreferences.h"
#include "composer/MediaLibrary.h"
#include "composer/VisualModel.h"
#include "composer/VisualReview.h"

class wxButton;
class wxCheckBox;
class wxCheckListBox;
class wxChoice;
class wxListCtrl;
class wxStaticBitmap;
class wxStaticText;
class wxTextCtrl;

// What the review window shows about the show as a whole.
struct ReviewContext {
    std::string sequence;
    std::string song;
    int durationMS = 0;
    std::vector<std::string> screens;   // the screens with pictures, with their part
    std::vector<wxString> warnings;     // already said simply
    std::map<std::string, int> transitions;
};

// The pictures and videos of a show, one row per shot: where, when, which file, what it
// shows, why it was chosen, how it moves and comes in, and what the person thinks of it.
// Below, what the person checked in xLights, and the approval, which only they can give.
class ReviewDialog : public wxDialog {
public:
    enum { ID_APPLY_REVIEW = 1 };
    ReviewDialog(wxWindow* parent, composer::VisualReview review, const ReviewContext& context, const composer::MediaLibrary& library, const std::string& showDir);

    [[nodiscard]] const composer::VisualReview& Review() const { return _review; }
    [[nodiscard]] bool ApplyAsked() const { return _apply; }
    // Files the person never wants chosen automatically.
    [[nodiscard]] const std::vector<std::string>& NeverUse() const { return _neverUse; }

    // The same calls the controls make, for the UI test.
    bool Decide(const std::string& decisionId, composer::ClipState state, const std::vector<std::string>& flags = {}, const std::string& note = "");
    bool Replace(const std::string& decisionId, const std::string& mediaId);
    void SetChecklist(const composer::ReviewChecklist& checklist);
    bool Approve(const std::string& reviewer, std::string& why);
    void SetOrderByScreen(bool byScreen);
    [[nodiscard]] std::vector<std::string> RowOrder() const; // decision ids, as listed

private:
    void Fill();
    void ShowState();
    [[nodiscard]] std::string Selected() const;
    void Flag(const std::string& flag);

    composer::VisualReview _review;
    ReviewContext _context;
    const composer::MediaLibrary& _library;
    std::string _showDir;
    std::vector<std::string> _rows; // decision ids, in list order
    std::vector<std::string> _neverUse;
    bool _byScreen = false;
    bool _apply = false;

    wxStaticText* _summary = nullptr;
    wxChoice* _order = nullptr;
    wxListCtrl* _list = nullptr;
    wxStaticText* _detail = nullptr;
    wxCheckBox* _steps[5] = {};
    wxTextCtrl* _reviewer = nullptr;
    wxButton* _approve = nullptr;
    wxStaticText* _state = nullptr;
};

// What a person says one file shows: each concept the model proposed confirmed, turned
// down or never to be attached, the ones it missed added, and the uses the file is not
// for. The model's own words are shown beside, and never changed.
class MediaLabelsDialog : public wxDialog {
public:
    MediaLabelsDialog(wxWindow* parent, const composer::MediaItem& item, const std::string& showDir, const composer::VisualConcepts& concepts);

    [[nodiscard]] const composer::MediaHumanLabels& Labels() const { return _labels; }

    // The same calls the controls make, for the UI test. `decision`: "yes", "no",
    // "never" or "" (not said).
    void SetDecision(const std::string& conceptId, const std::string& decision);
    void AddConcept(const std::string& conceptId);
    void SetNotFor(const std::vector<std::string>& uses);
    void SetComplete(bool complete, const std::string& reviewer);

private:
    void Fill();
    [[nodiscard]] std::string Selected() const;

    composer::MediaItem _item;
    const composer::VisualConcepts& _concepts;
    composer::MediaHumanLabels _labels;
    std::vector<std::string> _rows;

    wxListCtrl* _list = nullptr;
    wxCheckBox* _scores = nullptr;
    wxCheckListBox* _uses = nullptr;
    wxCheckBox* _complete = nullptr;
    wxTextCtrl* _reviewer = nullptr;
};

// What the program has learned of the maker's taste: each preference with its value, how
// many decisions back it, where it comes from, and a way to change, turn off or forget it.
class PreferencesDialog : public wxDialog {
public:
    PreferencesDialog(wxWindow* parent, composer::ArtisticPreferences prefs, const composer::MediaLibrary* library);

    [[nodiscard]] const composer::ArtisticPreferences& Preferences() const { return _prefs; }

    // The same calls the controls make, for the UI test.
    void SetValue(const std::string& key, double value);
    void Enable(const std::string& key, bool enabled);
    void Reset(const std::string& key);
    void ResetAll();
    void SetLearning(bool on);
    [[nodiscard]] std::vector<wxString> Sentences() const;

private:
    void Fill();
    [[nodiscard]] std::string Selected() const;

    composer::ArtisticPreferences _prefs;
    const composer::MediaLibrary* _library = nullptr;
    std::vector<std::string> _rows;

    wxCheckBox* _learning = nullptr;
    wxStaticText* _sentences = nullptr;
    wxListCtrl* _list = nullptr;
};

// A preference said simply: "Videos rather than pictures", "Dark pictures and videos"...
[[nodiscard]] wxString PreferenceName(const std::string& key, const composer::MediaLibrary* library);
// What its value means: "more (+40%)", "less (-25%)".
[[nodiscard]] wxString PreferenceValueText(double value);
// A clip's state for a person.
[[nodiscard]] wxString ClipStateName(composer::ClipState state);
// Why a clip does not work, for a person.
[[nodiscard]] wxString ClipFlagName(const std::string& flag);

class wxSlider;

// What a person thinks of the whole show: a score (their own, like "60%"), the nine
// criteria or only a few words, notes, and which observations become rules for the next
// shows. The measures of the same show are shown beside it, never mixed with it.
class EvaluationDialog : public wxDialog {
public:
    EvaluationDialog(wxWindow* parent, composer::ShowEvaluation evaluation, const std::map<std::string, double>& measured);

    // The evaluation as the controls hold it; Accept() checks it like the OK button.
    [[nodiscard]] composer::ShowEvaluation Evaluation() const;
    void Set(const composer::ShowEvaluation& evaluation);
    bool Accept(composer::VisualReview& review, std::string& why) const;

private:
    void Suggest();

    std::map<std::string, double> _measured;
    std::vector<std::string> _ruleIds;
    wxTextCtrl* _reviewer = nullptr;
    wxCheckBox* _quick = nullptr;
    wxSlider* _overall = nullptr;
    wxStaticText* _overallText = nullptr;
    std::map<std::string, wxChoice*> _ratings;
    wxCheckListBox* _tags = nullptr;
    wxTextCtrl* _notes = nullptr;
    wxCheckListBox* _rules = nullptr;
};

[[nodiscard]] wxString CriterionName(const std::string& criterion);
[[nodiscard]] wxString EvaluationTagName(const std::string& tag);
[[nodiscard]] wxString EvaluationRuleName(const std::string& rule);
