#pragma once

#include <string>

// Internal entry point behind --composer-assist-test. Every service is played by a
// stand-in that answers from memory: nothing is sent anywhere and no key is needed.
namespace composer {

// LRCLIB, Groq and Gemini requests and answers, retries, keys, consent, the cache and
// transcript alignment, under a new folder of `workDir`. 0 when every check passed.
[[nodiscard]] int RunAssistTest(const std::string& workDir);

} // namespace composer
