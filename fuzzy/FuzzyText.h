// FuzzyText.h — library-agnostic Unicode helpers for fuzzy matching.
//
// Kept separate from FuzzyMatcher so the Unicode policy (decode + case-fold)
// is identical no matter which matcher library is plugged in behind the facade.

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstddef>

namespace npp { namespace fuzzy {

/// Decode a UTF-8 byte string into UTF-32 code points.
/// On return `out` holds the code points and `byteOffsets` has size out.size()+1:
/// byteOffsets[i] = the byte offset in `utf8` where code point i begins; the final
/// entry equals utf8.size(). Invalid bytes decode to U+FFFD (1 byte consumed), so
/// the offset map always stays valid for mapping a code-point span back to bytes.
void decodeUTF8(std::string_view utf8,
                std::u32string& out,
                std::vector<std::size_t>& byteOffsets);

/// Index-preserving simple lowercase: returns a string the SAME length as `in`
/// (so positions still map 1:1 to the original code points). Code points whose
/// lowercase is not exactly one code point (e.g. U+00DF 'ß') are left unchanged.
/// Covers the full Unicode repertoire via CoreFoundation. Used for case-insensitive
/// fuzzy matching; accent/precomposition differences are absorbed by the matcher's
/// edit-distance tolerance, so no length-changing normalization is applied here.
std::u32string lowercasePreservingLength(std::u32string_view in);

}} // namespace npp::fuzzy
