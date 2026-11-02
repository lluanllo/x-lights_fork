#pragma once

#include <string>
#include <vector>

// Text by code point rather than by byte, for the Composer's lyrics. Covers what
// Spanish and English lyrics use: Latin-1, Latin Extended-A and common punctuation.
// Anything else passes through unchanged, so nothing a person wrote is lost.
namespace composer::text {

[[nodiscard]] std::vector<char32_t> Decode(const std::string& utf8);
[[nodiscard]] std::string Encode(const std::vector<char32_t>& codePoints);
[[nodiscard]] std::string Encode(char32_t codePoint);

[[nodiscard]] char32_t ToLower(char32_t c);
[[nodiscard]] bool IsLetter(char32_t c);
[[nodiscard]] bool IsDigit(char32_t c);
[[nodiscard]] bool IsSpace(char32_t c);
// Letters and the apostrophes that join them, as in "pa'l": what a word is made of.
[[nodiscard]] bool IsWordChar(char32_t c);

// Lowercase, accents kept: "Navidad" and "NAVIDAD" share a key, "año" and "ano" do not.
[[nodiscard]] std::string LowerKey(const std::string& utf8);
// Lowercase without accents or diaeresis, for matching tags like "[Estribillo]".
[[nodiscard]] std::string Fold(const std::string& utf8);
[[nodiscard]] std::string Trim(const std::string& utf8);

} // namespace composer::text
