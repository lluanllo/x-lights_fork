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

#include <functional>
#include <string>
#include <vector>

#include "composer/LyricAligner.h"
#include "composer/LyricDocument.h"
#include "composer/MediaLibrary.h"

class wxCheckBox;
class wxCheckListBox;
class wxChoice;
class wxImageList;
class wxListBox;
class wxListCtrl;
class wxSpinCtrl;
class wxStaticBitmap;
class wxStaticText;
class wxTextCtrl;

// The show's pictures and videos: the folders searched, what was found in them, and a
// person's categories, tags and "not automatically" for each. Files are only read: the
// library is kept in the show's Show Composer folder.
class MediaLibraryDialog : public wxDialog {
public:
    MediaLibraryDialog(wxWindow* parent, const std::string& showDir, const std::vector<std::string>& folders);
    ~MediaLibraryDialog() override;

    [[nodiscard]] const std::vector<std::string>& Folders() const { return _folders; }
    [[nodiscard]] const composer::MediaLibrary& Library() const { return _library; }

    // The same calls the controls make, for the UI test.
    bool AddFolder(const std::string& folder, std::string& refused);
    bool Scan();
    void SelectItem(const std::string& id);
    void SetItemCategories(const std::vector<std::string>& categories);
    void SetItemDisabled(bool disabled);
    // Several files at once: the categories ticked go to every one selected.
    void SelectItems(const std::vector<std::string>& ids);
    int ApplyCategoriesToSelected();
    bool SaveLibrary();
    void SetQuiet(bool quiet) { _quiet = quiet; }

private:
    void Refill();
    void ShowSelected();
    void StoreSelected();
    [[nodiscard]] std::vector<std::string> SelectedIds() const;

    std::string _showDir;
    std::vector<std::string> _folders;
    composer::MediaLibrary _library;
    std::vector<std::string> _shown; // item ids, in list order
    std::string _selected;
    bool _quiet = false;

    wxListBox* _folderList = nullptr;
    wxListCtrl* _items = nullptr;
    wxImageList* _images = nullptr;
    wxChoice* _filter = nullptr;
    wxCheckListBox* _categories = nullptr;
    wxTextCtrl* _tags = nullptr;
    wxCheckBox* _disabled = nullptr;
    wxStaticBitmap* _preview = nullptr;
    wxStaticText* _info = nullptr;
    wxStaticText* _summary = nullptr;
};

// One file from the library, chosen by a person.
class MediaChooserDialog : public wxDialog {
public:
    MediaChooserDialog(wxWindow* parent, const std::string& showDir, const composer::MediaLibrary& library);
    ~MediaChooserDialog() override;
    [[nodiscard]] std::string Chosen() const;

private:
    std::vector<std::string> _ids;
    wxListCtrl* _items = nullptr;
    wxImageList* _images = nullptr;
};

// A phrase as the voice editor shows it.
struct VoicePhrase {
    std::string id;
    std::string text;
    int startMS = -1;
    int endMS = -1;
    std::vector<std::string> voices;
    bool spoken = false;
    int words = 0;
};

// Who sings or says each phrase, and which face each voice moves. Every change becomes
// a correction the next generation applies; nothing is guessed about who a singer is.
class VoicesDialog : public wxDialog {
public:
    VoicesDialog(wxWindow* parent, std::vector<composer::VoiceDef> cast, std::vector<VoicePhrase> phrases, std::vector<std::string> faces, std::function<void(int, int)> play);

    [[nodiscard]] std::vector<composer::VoiceDef> Voices() const;
    [[nodiscard]] const std::vector<composer::LyricCorrection>& Corrections() const { return _corrections; }

    // The same calls the controls make, for the UI test.
    void SetPhraseVoices(int phrase, const std::vector<std::string>& voices, bool spoken);
    void SetVoiceFacesOff(const std::string& voice, bool off);
    void SetVoiceRole(const std::string& voice, composer::VoiceRole role);
    void AddVoice(const std::string& name);
    void Split(int phrase, int beforeWord);
    void Merge(int phrase);

private:
    void RefillVoices();
    void RefillPhrases();
    void ShowVoice();
    void StoreVoice();
    [[nodiscard]] composer::VoiceDef* Voice(const std::string& id);
    [[nodiscard]] int SelectedPhrase() const;

    std::vector<composer::VoiceDef> _cast;
    std::vector<std::string> _edited;
    std::vector<VoicePhrase> _phrases;
    std::vector<std::string> _faces;
    std::function<void(int, int)> _play;
    std::vector<composer::LyricCorrection> _corrections;
    std::string _voice;
    int _shownFace = -1;

    wxListCtrl* _voiceList = nullptr;
    wxChoice* _role = nullptr;
    wxCheckBox* _facesOff = nullptr;
    wxChoice* _face = nullptr;
    wxListCtrl* _phraseList = nullptr;
    wxCheckListBox* _phraseVoices = nullptr;
    wxCheckBox* _spoken = nullptr;
    wxSpinCtrl* _splitAt = nullptr;
};

// The category as a person reads it.
[[nodiscard]] wxString MediaCategoryName(const std::string& id);
[[nodiscard]] wxString VoiceRoleName(composer::VoiceRole role);
