/*
 * SentenceSplitter.h
 *
 * Sentence spans in UTF-8 byte offsets, using Pango log attributes
 * (is_sentence_start). Spans with no real words are dropped, so a
 * bare number or a bullet is neither embedded nor painted. The text
 * field is the raw slice, including its surrounding whitespace, so a
 * cache key matches the bytes that get painted.
 */
#ifndef NPP_SENTENCE_SPLITTER_H
#define NPP_SENTENCE_SPLITTER_H

#include <cstdint>
#include <string>
#include <vector>

struct SentenceSpan {
    int64_t byte_start;
    int64_t byte_length;
    std::string text;
};

/* A span worth an embedding: at least two words, where a word is a
 * run of letters. Digits, punctuation, bullets, and list markers do
 * not count. */
bool segment_has_content(const std::string &utf8);

std::vector<SentenceSpan> split_sentences(const std::string &utf8);

#endif
