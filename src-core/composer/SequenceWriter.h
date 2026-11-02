#pragma once

#include "composer/ComposerTypes.h"

#include <string>
#include <vector>

class xLightsShowContext;

namespace composer {

// Builds the spec into the context's sequence elements and writes it with
// SequenceFile::Save. Save truncates its target, so `path` must be disposable:
// callers wanting a safe write use WriteSequenceSafely.
[[nodiscard]] bool BuildAndSave(xLightsShowContext& ctx, const SequenceSpec& spec, const std::string& path, std::string& error);

// Compares the sequence open in `ctx` with the spec and returns every mismatch.
// xLights loads and renders broken references without reporting them, so a
// successful open proves nothing on its own.
[[nodiscard]] std::vector<std::string> VerifySequence(xLightsShowContext& ctx, const SequenceSpec& spec);

// Validates the plan, writes to <showDir>/ShowComposer/tmp, reopens with new
// instances, validates again and only then publishes as <showDir>/<destinationName>.
// Never overwrites a sequence or its .xbkp, and removes only the files this call
// created. Anything GenerationValidator calls an error stops the publication.
[[nodiscard]] WriteResult WriteSequenceSafely(const std::string& showDir, const SequenceSpec& spec, const std::string& destinationName);

} // namespace composer
