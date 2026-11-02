#pragma once

#include "composer/ComposerTypes.h"

#include <functional>
#include <string>

class HeadlessRenderContext;
class xLightsShowContext;

namespace composer {
struct LyricAlignment;
}

// Decides whether a sequence does what the Composer meant, which xLights itself
// never checks: a render exits 0 with missing models, faces that never move and
// models filled with red. Nothing here modifies a sequence, a layout or any file.
namespace composer {

namespace issue {
// Structural
inline constexpr const char* ModelNotFound = "COMPOSER_MODEL_NOT_FOUND";
inline constexpr const char* GroupNotFound = "COMPOSER_GROUP_NOT_FOUND";
inline constexpr const char* SubModelNotFound = "COMPOSER_SUBMODEL_NOT_FOUND";
inline constexpr const char* TargetKindMismatch = "COMPOSER_TARGET_KIND_MISMATCH";
inline constexpr const char* GroupMemberInvalid = "COMPOSER_GROUP_MEMBER_INVALID";
inline constexpr const char* GroupEmpty = "COMPOSER_GROUP_EMPTY";
inline constexpr const char* LayoutIncompatible = "COMPOSER_LAYOUT_INCOMPATIBLE";
inline constexpr const char* ShowFolderMismatch = "COMPOSER_SHOW_FOLDER_MISMATCH";
inline constexpr const char* SequenceFileUnreadable = "COMPOSER_SEQUENCE_FILE_UNREADABLE";
inline constexpr const char* SequenceMismatch = "COMPOSER_SEQUENCE_MISMATCH";
inline constexpr const char* EffectUnknown = "COMPOSER_EFFECT_UNKNOWN";
inline constexpr const char* EffectIntervalInvalid = "COMPOSER_EFFECT_INTERVAL_INVALID";
inline constexpr const char* EffectOverlapInLayer = "COMPOSER_EFFECT_OVERLAP_IN_LAYER";
inline constexpr const char* TimingInvalid = "COMPOSER_TIMING_INVALID";
inline constexpr const char* TimingOffGrid = "COMPOSER_TIMING_OFF_GRID";
// Configuration
inline constexpr const char* EffectSettingsInvalid = "COMPOSER_EFFECT_SETTINGS_INVALID";
inline constexpr const char* EffectSettingUnknown = "COMPOSER_EFFECT_SETTING_UNKNOWN";
inline constexpr const char* EffectSettingsWarning = "COMPOSER_EFFECT_SETTINGS_WARNING";
inline constexpr const char* PaletteInvalid = "COMPOSER_PALETTE_INVALID";
inline constexpr const char* PaletteEmpty = "COMPOSER_PALETTE_EMPTY";
inline constexpr const char* BufferStyleInvalid = "COMPOSER_BUFFER_STYLE_INVALID";
inline constexpr const char* MediaFileMissing = "COMPOSER_MEDIA_FILE_MISSING";
inline constexpr const char* FaceTrackMissing = "COMPOSER_FACE_TRACK_MISSING";
inline constexpr const char* FaceInsufficientLayers = "COMPOSER_FACE_INSUFFICIENT_LAYERS";
inline constexpr const char* FaceNoPhonemes = "COMPOSER_FACE_NO_PHONEMES";
inline constexpr const char* FaceNoWords = "COMPOSER_FACE_NO_WORDS";
inline constexpr const char* FacePhonemeUnknown = "COMPOSER_FACE_PHONEME_UNKNOWN";
inline constexpr const char* FacePhonemeOutsideWord = "COMPOSER_FACE_PHONEME_OUTSIDE_WORD";
inline constexpr const char* FaceDefinitionNotFound = "COMPOSER_FACE_DEFINITION_NOT_FOUND";
inline constexpr const char* FaceDefinitionIncompatible = "COMPOSER_FACE_DEFINITION_INCOMPATIBLE";
inline constexpr const char* FaceDefinitionEmpty = "COMPOSER_FACE_DEFINITION_EMPTY";
inline constexpr const char* FaceImageMissing = "COMPOSER_FACE_IMAGE_MISSING";
inline constexpr const char* TextEmpty = "COMPOSER_TEXT_EMPTY";
inline constexpr const char* TextUtf8Issue = "COMPOSER_TEXT_UTF8_ISSUE";
inline constexpr const char* TextLyricTrackInvalid = "COMPOSER_TEXT_LYRIC_TRACK_INVALID";
inline constexpr const char* GroupMemberConflict = "COMPOSER_GROUP_MEMBER_CONFLICT";
inline constexpr const char* MarkerInvalid = "COMPOSER_MARKER_INVALID";
inline constexpr const char* MarkerProducesLight = "COMPOSER_MARKER_PRODUCES_LIGHT";
// Render
inline constexpr const char* RenderFileUnreadable = "COMPOSER_RENDER_FILE_UNREADABLE";
inline constexpr const char* RenderTimingMismatch = "COMPOSER_RENDER_TIMING_MISMATCH";
inline constexpr const char* RenderChannelsUnavailable = "COMPOSER_RENDER_CHANNELS_UNAVAILABLE";
inline constexpr const char* RenderTargetSilent = "COMPOSER_RENDER_TARGET_SILENT";
inline constexpr const char* RenderMemberSilent = "COMPOSER_RENDER_MEMBER_SILENT";
inline constexpr const char* RenderPlaceholderOutput = "COMPOSER_RENDER_PLACEHOLDER_OUTPUT";
inline constexpr const char* UnexpectedOutput = "COMPOSER_UNEXPECTED_OUTPUT";
inline constexpr const char* FaceMouthStatic = "COMPOSER_FACE_MOUTH_STATIC";
inline constexpr const char* FaceMouthMismatch = "COMPOSER_FACE_MOUTH_MISMATCH";
inline constexpr const char* FaceRestMismatch = "COMPOSER_FACE_REST_MISMATCH";
// Lyrics
inline constexpr const char* LyricsWordNoPhonemes = "COMPOSER_LYRICS_WORD_NO_PHONEMES";
inline constexpr const char* LyricsLanguageUnsupported = "COMPOSER_LYRICS_LANGUAGE_UNSUPPORTED";
inline constexpr const char* LyricsNotSynchronized = "COMPOSER_LYRICS_NOT_SYNCHRONIZED";
inline constexpr const char* LyricsTrackIncomplete = "COMPOSER_LYRICS_TRACK_INCOMPLETE";
inline constexpr const char* LyricsTimeInvalid = "COMPOSER_LYRICS_TIME_INVALID";
inline constexpr const char* LyricsOverlap = "COMPOSER_LYRICS_OVERLAP";
inline constexpr const char* LyricsRestMissing = "COMPOSER_LYRICS_REST_MISSING";
inline constexpr const char* LyricsLayerOrder = "COMPOSER_LYRICS_LAYER_ORDER";
inline constexpr const char* LyricsLowConfidence = "COMPOSER_LYRICS_LOW_CONFIDENCE";
inline constexpr const char* LyricsCorrectionLost = "COMPOSER_LYRICS_CORRECTION_LOST";
inline constexpr const char* LyricsCorrectionConflict = "COMPOSER_LYRICS_CORRECTION_CONFLICT";
} // namespace issue

// Values of the "classification" argument of GroupMemberConflict.
namespace overlap {
inline constexpr const char* Confirmed = "confirmed";
inline constexpr const char* Possible = "possible";
inline constexpr const char* Intentional = "intentional";
inline constexpr const char* Allowed = "allowed";
} // namespace overlap

// Polled between effects and between frames. Returning true stops the stage, which
// then reports Cancelled rather than a partial pass.
using CancelCheck = std::function<bool()>;

// A generated spec is held to everything: an unknown setting or a value the metadata
// does not list is an error. A sequence read from disk may come from any xLights
// version or vendor, so the same findings are only warnings.
enum class Strictness {
    Generated,
    FromFile
};

// Stage 1: the plan against the loaded layout, before anything is built.
[[nodiscard]] ValidationReport ValidatePlan(xLightsShowContext& ctx, const SequenceSpec& spec, const CancelCheck& cancel = {});

// Stage 2: the objects SequenceWriter left in the context's sequence elements.
[[nodiscard]] ValidationReport ValidateBuilt(xLightsShowContext& ctx, const SequenceSpec& spec, const CancelCheck& cancel = {});

// Stage 3: a saved file reopened in `ctx`. The file itself is read too, because the
// loader drops what it cannot resolve and the elements alone would look clean.
// `spec` may be null: a sequence without its sidecar is still a valid sequence.
[[nodiscard]] ValidationReport ValidateSequenceFile(xLightsShowContext& ctx, const std::string& xsqPath, const SequenceSpec* spec, Strictness strictness, const CancelCheck& cancel = {});

// Stage 4: a rendered .fseq against the intervals and targets `spec` expects.
[[nodiscard]] ValidationReport ValidateRender(xLightsShowContext& ctx, const std::string& fseqPath, const SequenceSpec& spec, Strictness strictness, const CancelCheck& cancel = {});

// A lyrics track on its own, before any face uses it: three layers in the order
// FacesEffect reads them, times that nest and never overlap, a rest in every pause and
// only shapes the renderer knows. `durationMS` 0 skips the check against the song.
[[nodiscard]] ValidationReport ValidateLyricTrack(const TimingTrackSpec& track, int durationMS, int frameMS);

// An alignment against the track built from it: what could not be pronounced or
// synchronized, what is too uncertain to trust unseen, and whether every correction a
// person made is still where they put it.
[[nodiscard]] ValidationReport ValidateLyrics(const LyricAlignment& alignment, const TimingTrackSpec& track, int durationMS, int frameMS);

// The sequence open in `ctx` as plain data, leaving out disabled effects and
// strand or node effects, which have no target name of their own.
[[nodiscard]] SequenceSpec SpecFromSequence(xLightsShowContext& ctx);

// Identifies a layout by model names, node counts and start channels, to catch one
// that keeps the names of another but no longer lights the same channels.
[[nodiscard]] std::string LayoutFingerprint(const xLightsShowContext& ctx);

// English text with the arguments filled in. The template is meant as a gettext
// msgid for the UI layer; the core never translates it.
[[nodiscard]] std::string DescribeIssue(const ValidationIssue& issue);
[[nodiscard]] std::string DescribeSuggestion(const ValidationIssue& issue);

// The same, with the template translated before its arguments are filled in, so a UI
// can pass its own gettext lookup.
using TranslateText = std::function<std::string(const char* msgid)>;
[[nodiscard]] std::string DescribeIssue(const ValidationIssue& issue, const TranslateText& translate);
[[nodiscard]] std::string DescribeSuggestion(const ValidationIssue& issue, const TranslateText& translate);
[[nodiscard]] const char* StatusName(CheckStatus status);

} // namespace composer
