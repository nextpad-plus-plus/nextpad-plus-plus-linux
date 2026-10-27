/*
 * WordPieceTokenizer.h
 *
 * BERT WordPiece, matching Hugging Face BertTokenizer for an uncased
 * vocabulary: clean, CJK spacing, lowercase, accent strip, punctuation
 * split, then greedy WordPiece. Output is [CLS] + pieces + [SEP],
 * truncated to max_tokens with the final id kept as [SEP].
 */
#ifndef NPP_WORDPIECE_TOKENIZER_H
#define NPP_WORDPIECE_TOKENIZER_H

#include <string>
#include <unordered_map>
#include <vector>

class WordPieceTokenizer {
public:
    bool load(const std::string &vocab_path, std::string &error);
    bool loaded() const { return loaded_; }

    /* Includes [CLS] and [SEP]. `max_tokens` must be at least 2. */
    bool encode(const std::string &text, int max_tokens, std::vector<int32_t> &ids) const;

    int cls_id() const { return cls_; }
    int sep_id() const { return sep_; }
    int unk_id() const { return unk_; }
    int pad_id() const { return pad_; }

private:
    std::unordered_map<std::string, int> vocab_;
    bool loaded_ = false;
    int cls_ = 101;
    int sep_ = 102;
    int unk_ = 100;
    int pad_ = 0;
};

#endif
