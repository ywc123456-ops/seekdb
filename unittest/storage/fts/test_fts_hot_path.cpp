/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "storage/fts/dict/ob_ft_dat_dict.h"
#include "storage/fts/dict/ob_ft_trie.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_beng_ft_parser.h"
#include "data_plane/fts/ob_fts_parser_property.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "storage/retrieval/ob_sparse_retrieval_util.h"
#include "storage/blocksstable/encoding/ob_string_diff_decoder.h"
#include "storage/fts/ik/ob_ik_cjk_processor.h"
#include "share/text_analysis/ob_token_stream.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <tuple>
#include "lib/utility/pdqsort.h"

namespace oceanbase {
namespace storage {
using namespace common;

class CharsetEnvironment final : public testing::Environment
{
public:
  void SetUp() override { ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset()); }
};
static testing::Environment *charset_environment =
    testing::AddGlobalTestEnvironment(new CharsetEnvironment);

TEST(FTSHotPath, OldPrimeAndNewPowerOfTwoDictionaryBuckets)
{
  for (uint32_t capacity : {101U, 128U}) {
    // The map is a serialized header followed by entries. Retain a fixture
    // with the previous prime capacity to verify existing dictionary caches.
    const size_t bytes = sizeof(ObArrayHashMap::Header)
        + (capacity - 1) * sizeof(ObArrayHashMap::Entry);
    void *buffer = std::calloc(1, bytes);
    ASSERT_NE(nullptr, buffer);
    auto *map = new (buffer) ObArrayHashMap;
    auto *header = reinterpret_cast<ObArrayHashMap::Header *>(map);
    header->capacity_ = capacity;
    std::vector<std::string> keys;
    keys.reserve(70);
    for (int i = 0; i < 70; ++i) {
      keys.push_back(std::to_string(1000 + i));
      ASSERT_EQ(OB_SUCCESS, map->insert(ObString(keys.back().size(), keys.back().data()), i + 1));
    }
    for (int i = 0; i < 70; ++i) {
      ObFTSingleWord word;
      ASSERT_EQ(OB_SUCCESS, word.set_word(keys[i].data(), keys[i].size()));
      ObFTWordCode code = 0;
      ASSERT_EQ(OB_SUCCESS, map->find(word, code));
      ASSERT_EQ(i + 1, code);
    }
    ObFTSingleWord missing;
    ASSERT_EQ(OB_SUCCESS, missing.set_word("none", 4));
    ObFTWordCode code = 0;
    EXPECT_EQ(OB_ENTRY_NOT_EXIST, map->find(missing, code));
    map->~ObArrayHashMap();
    std::free(buffer);
  }
}

TEST(FTSHotPath, DirectUtf8DictionaryCodesPreserveTrieHitsAndFallbacks)
{
  ObArenaAllocator allocator("FTSCodesTest");
  ObFTTrie<void> trie(allocator, CS_TYPE_UTF8MB4_BIN);
  const std::vector<std::string> terms = {"a", "ab", "abc", "数据库", "数据", "中文",
      "école", "Ä", "\xEF\xBF\xBF", "😀", "😀x", "𠀀", std::string("\0z", 2)};
  for (const auto &term : terms) {
    ASSERT_EQ(OB_SUCCESS, trie.insert(ObString(term.size(), term.data()), {}));
  }
  ObFTDATBuilder<void> builder(allocator);
  ASSERT_EQ(OB_SUCCESS, builder.init(trie));
  ASSERT_EQ(OB_SUCCESS, builder.build_from_trie(trie));
  ObFTDAT *dat = nullptr;
  size_t bytes = 0;
  ASSERT_EQ(OB_SUCCESS, builder.get_mem_block(dat, bytes));
  ObFTDATReader<void> reference(dat), accelerated(dat);
  ASSERT_EQ(OB_SUCCESS, accelerated.init_utf8_codes(allocator));
  ASSERT_EQ(OB_SUCCESS, accelerated.init_utf8_codes(allocator)); // idempotent
  auto compare = [&](const ObString &word, ObDATrieHit &expected, ObDATrieHit &actual) {
    const int ret = reference.match_with_hit(word, expected, expected);
    EXPECT_EQ(ret, accelerated.match_with_hit(word, actual, actual));
    EXPECT_TRUE(expected == actual);
    EXPECT_EQ(expected.is_match(), actual.is_match());
    EXPECT_EQ(expected.is_prefix(), actual.is_prefix());
    // Also exercise distinct source and destination hit objects.
    ObDATrieHit copied_expected(nullptr, 7), copied_actual(nullptr, 7);
    EXPECT_EQ(reference.match_with_hit(word, expected, copied_expected),
        accelerated.match_with_hit(word, actual, copied_actual));
    EXPECT_TRUE(copied_expected == copied_actual);
    EXPECT_EQ(copied_expected.is_match(), copied_actual.is_match());
    EXPECT_EQ(copied_expected.is_prefix(), copied_actual.is_prefix());
  };
  for (const auto &term : terms) {
    ObDATrieHit expected(nullptr, 0), actual(nullptr, 0);
    for (size_t offset = 0; offset < term.size();) {
      int64_t length = 0;
      ASSERT_EQ(OB_SUCCESS, ObCharset::first_valid_char(CS_TYPE_UTF8MB4_BIN,
          term.data() + offset, term.size() - offset, length));
      compare(ObString(length, term.data() + offset), expected, actual);
      offset += length;
    }
  }
  // Every BMP character exercises both presence and absence; malformed,
  // multi-character and supplementary sequences retain the original map path.
  const auto *cs = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
  for (uint32_t cp = 0; cp < 65536; ++cp) {
    unsigned char bytes[8] = {};
    const int len = cs->cset->wc_mb(cs, cp, bytes, bytes + sizeof(bytes));
    if (len > 0) {
      ObDATrieHit expected(nullptr, 0), actual(nullptr, 0);
      compare(ObString(len, reinterpret_cast<char *>(bytes)), expected, actual);
    }
  }
  for (const std::string word : {std::string("ab"), std::string("\xC0\xAF"),
      std::string("\xE4\xB8"), std::string("\xED\xA0\x80"), std::string("\xFF")}) {
    ObDATrieHit expected(nullptr, 0), actual(nullptr, 0);
    compare(ObString(word.size(), word.data()), expected, actual);
  }
}

TEST(FTSHotPath, CaseFoldingMatchesCharsetImplementation)
{
  const std::string inputs[] = {
    "ASCII Upper And lower", "中文数据库", "\xC3\x84\xC3\x96\xC3\x9C",
    std::string("\0\x41\0\x42\0\x43", 6),
    std::string("\x41\0\x42\0\x43\0", 6),
    std::string("\xC4\xD6\xDC", 3)
  };
  const ObCollationType collations[] = {
    CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI,
    CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF16_BIN, CS_TYPE_UTF16LE_BIN,
    CS_TYPE_LATIN1_BIN
  };
  for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    SCOPED_TRACE(i);
    ObArenaAllocator allocator("FTSCaseTest");
    ObFTWordMap words;
    ASSERT_EQ(OB_SUCCESS, words.create(16, "FTSCaseTest"));
    ObObjMeta meta;
    meta.set_varchar();
    meta.set_collation_type(collations[i]);
    ObFTParserProperty property;
    ObAddWordFlag flags;
    flags.set_casedown();
    flags.set_groupby_word();
    ObAddWord add(property, meta, flags, allocator, words);
    ObString expected;
    const int expected_ret = ObCharset::tolower(collations[i],
        ObString(inputs[i].size(), inputs[i].data()), expected, allocator);
    ASSERT_EQ(expected_ret, add.process_word(inputs[i].data(), inputs[i].size(), 1, 1));
    ASSERT_EQ(expected_ret, add.process_word(inputs[i].data(), inputs[i].size(), 1, 2));
    if (OB_SUCCESS == expected_ret) {
      ASSERT_EQ(1, words.size());
      EXPECT_EQ(expected, words.begin()->first.get_word().get_string());
      EXPECT_EQ(3, words.begin()->second);
    } else {
      EXPECT_EQ(0, words.size());
    }
    words.destroy();
  }
}

TEST(FTSHotPath, StopwordLookupPreservesCharsetConversion)
{
  ObStopWordChecker checker;
  ASSERT_EQ(OB_SUCCESS, checker.init());
  const ObCollationType collations[] = {
    CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_GBK_BIN
  };
  for (auto collation : collations) {
    ObArenaAllocator allocator("FTSStopTest");
    ObObjMeta meta;
    meta.set_varchar();
    meta.set_collation_type(collation);
    for (const char *text : {"the", "database", "中文"}) {
      SCOPED_TRACE(collation);
      SCOPED_TRACE(text);
      const ObString input = ObString::make_string(text);
      ObString converted;
      const int expected_ret = ObCharset::charset_convert(allocator,
          input, collation, CS_TYPE_UTF8MB4_GENERAL_CI, converted);
      bool is_stopword = false;
      ASSERT_EQ(expected_ret, checker.check_stopword(
          ObFTWord(input.length(), input.ptr(), meta), is_stopword));
      if (OB_SUCCESS == expected_ret) {
        EXPECT_EQ(0 == std::strcmp(text, "the"), is_stopword);
      }
    }
  }
}

TEST(FTSHotPath, GroupingRetainsLastCollationEquivalentRepresentation)
{
  ObArenaAllocator allocator("FTSGroupTest");
  ObFTWordMap words;
  ASSERT_EQ(OB_SUCCESS, words.create(16, "FTSGroupTest"));
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  ObFTParserProperty property;
  ObAddWordFlag flags;
  flags.set_groupby_word();
  ObAddWord add(property, meta, flags, allocator, words);
  // Supplementary code points have equal weights under this collation.
  const ObString first = ObString::make_string("😀🎉");
  const ObString last = ObString::make_string("𠀀𠀁");
  ASSERT_EQ(OB_SUCCESS, add.process_word(first.ptr(), first.length(), 2, 1));
  ASSERT_EQ(OB_SUCCESS, add.process_word(last.ptr(), last.length(), 2, 1));
  ASSERT_EQ(1, words.size());
  EXPECT_EQ(last, words.begin()->first.get_word().get_string());
  EXPECT_EQ(2, words.begin()->second);
  words.destroy();
}

TEST(FTSHotPath, BinaryIdsMatchGenericComparison)
{
  auto reference = ObDatumFuncs::get_basic_func(ObVarcharType, CS_TYPE_BINARY)->null_first_cmp_;
  uint32_t state = 17;
  for (int sample = 0; sample < 4096; ++sample) {
    std::string left(sample % 33, '\0'), right((sample / 33) % 33, '\0');
    for (char &c : left) { state = state * 1664525U + 1013904223U; c = state >> 24; }
    for (char &c : right) { state = state * 1664525U + 1013904223U; c = state >> 24; }
    if (sample % 7 == 0) right = left + " ";
    if (sample % 11 == 0) right = left;
    const ObCharsetInfo *binary = ObCharset::get_charset(CS_TYPE_BINARY);
    for (bool endspace : {false, true}) {
      const int expected = binary->coll->strnncollsp(binary,
          reinterpret_cast<const unsigned char *>(left.data()), left.size(),
          reinterpret_cast<const unsigned char *>(right.data()), right.size(), endspace);
      EXPECT_EQ(expected, ObCharset::strcmpsp(CS_TYPE_BINARY, left.data(), left.size(),
          right.data(), right.size(), endspace));
    }
    ObDatum lhs, rhs;
    lhs.set_string(left.data(), left.size());
    rhs.set_string(right.data(), right.size());
    if (sample % 13 == 0) lhs.set_null();
    if (sample % 19 == 0) rhs.set_null();
    int expected = 0, actual = 0;
    ASSERT_EQ(OB_SUCCESS, reference(lhs, rhs, expected, nullptr));
    ASSERT_EQ(OB_SUCCESS, compare_binary_retrieval_ids(lhs, rhs, actual, nullptr));
    EXPECT_EQ((expected > 0) - (expected < 0), (actual > 0) - (actual < 0));
  }
}

static std::vector<std::string> reference_english(const std::string &doc, const ObCharsetInfo *cs)
{
  std::vector<std::string> result;
  ObArenaAllocator allocator("FTSEnglishRef");
  size_t pos = 0;
  bool ended = false;
  auto delimiter = [&](size_t at) {
    return ob_isspace(cs, doc[at]) || ob_iscntrl(cs, doc[at]) || ob_ispunct(cs, doc[at]);
  };
  while (pos < doc.size() && !ended) {
    while (pos < doc.size() && delimiter(pos)) {
      const int64_t len = ob_mbcharlen_ptr(cs, doc.data() + pos, doc.data() + doc.size());
      if (len == 0) return result;
      pos += len;
    }
    const size_t start = pos;
    while (pos < doc.size() && !delimiter(pos)) {
      const int64_t len = ob_mbcharlen_ptr(cs, doc.data() + pos, doc.data() + doc.size());
      if (len == 0) { ended = true; break; }
      pos += len;
    }
    size_t first = start, last = pos;
    while (first < last && !ob_isalnum(cs, doc[first])) ++first;
    while (first < last && !ob_isalnum(cs, doc[last - 1])) --last;
    if (first < last) {
      ObString lower;
      const int ret = ObCharset::tolower(cs, ObString(last - first, doc.data() + first), lower, allocator);
      EXPECT_EQ(OB_SUCCESS, ret);
      result.emplace_back(lower.ptr(), lower.length());
    }
  }
  return result;
}

TEST(FTSHotPath, EnglishFastPathsMatchOriginalByteAndCharsetRules)
{
  const ObCharsetInfo *cs = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
  uint32_t state = 29;
  for (int sample = 0; sample < 2048; ++sample) {
    std::string doc = "ASCII lower Upper 123 foo-bar 中文 ＡＢＣ 😀 Straße ";
    for (int i = 0; i < 64; ++i) {
      state = state * 1664525U + 1013904223U;
      doc += static_cast<char>(state >> 24);
    }
    share::ObTextWhitespaceTokenizer tokenizer;
    share::ObBasicEnglishNormalizer normalizer;
    ObDatum document;
    document.set_string(doc.data(), doc.size());
    ASSERT_EQ(OB_SUCCESS, tokenizer.open(document, cs));
    ASSERT_EQ(OB_SUCCESS, normalizer.init(cs, tokenizer));
    std::vector<std::string> actual;
    int ret = OB_SUCCESS;
    ObDatum token;
    int64_t frequency = 0;
    while ((ret = normalizer.get_next(token, frequency)) == OB_SUCCESS) {
      actual.emplace_back(token.ptr_, static_cast<uint32_t>(token.len_));
    }
    ASSERT_EQ(OB_ITER_END, ret);
    EXPECT_EQ(reference_english(doc, cs), actual);
  }
}
TEST(FTSHotPath, FusedEnglishParserMatchesOriginalRulesAndResetLifetimes)
{
  std::string all_ascii;
  for (int i = 0; i < 128; ++i) all_ascii += static_cast<char>(i);
  std::vector<std::string> docs = {all_ascii, " ,.!\t\n", "lower UPPER 123 foo-bar a_b can't",
      "ASCII 中文 ＡＢＣ 😀 Straße", std::string(65537, 'A'), std::string("a\0B", 3)};
  uint32_t state = 1701;
  for (int sample = 0; sample < 1024; ++sample) {
    std::string text;
    for (int i = 0; i < 1 + sample % 257; ++i) {
      state = state * 1664525U + 1013904223U;
      text += static_cast<char>((state >> 24) & 127);
    }
    docs.push_back(text);
  }
  ObArenaAllocator allocator("FusedBengTest");
  ObBEngFTParser parser(allocator);
  for (auto collation : {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI,
      CS_TYPE_UTF8MB4_UNICODE_CI}) {
    const auto *cs = ObCharset::get_charset(collation);
    ASSERT_NE(nullptr, cs) << collation;
    for (const auto &doc : docs) {
      const auto expected = reference_english(doc, cs);
      ObFTParserParam param;
      param.allocator_ = &allocator; param.cs_ = cs;
      param.fulltext_ = doc.data(); param.ft_length_ = doc.size();
      ASSERT_EQ(OB_SUCCESS, parser.init(&param));
      if (std::all_of(doc.begin(), doc.end(), [](unsigned char c) { return c < 128; })) {
        EXPECT_EQ(0, allocator.used()); // ASCII setup must not build the legacy pipeline.
      }
      std::vector<ObString> words;
      const char *word = nullptr;
      int64_t length = 0, chars = 0, frequency = 0;
      int ret = OB_SUCCESS;
      while ((ret = parser.get_next_token(word, length, chars, frequency)) == OB_SUCCESS) {
        ASSERT_EQ(1, frequency); ASSERT_EQ(length, chars);
        words.emplace_back(length, word);
      }
      ASSERT_EQ(OB_ITER_END, ret);
      parser.reset();
      ASSERT_EQ(expected.size(), words.size());
      for (size_t i = 0; i < words.size(); ++i) {
        ASSERT_EQ(expected[i], std::string(words[i].ptr(), words[i].length()));
      }
      allocator.reuse();
    }
  }
}

TEST(FTSHotPath, DefaultPropertiesMatchValidatedJsonAndPreserveReusedFields)
{
  for (const char *name : {"space.1", "beng.1", "ngram.1", "ngram2.1"}) {
    ObFTParser parser;
    ASSERT_EQ(OB_SUCCESS, parser.parse_from_str(name, std::strlen(name)));
    ObFTParserJsonProps props;
    ASSERT_EQ(OB_SUCCESS, props.init());
    ASSERT_EQ(OB_SUCCESS, props.rebuild_props_for_ddl(ObString::make_string(name),
        CS_TYPE_UTF8MB4_BIN, false));
    ObArenaAllocator allocator;
    ObString json;
    ASSERT_EQ(OB_SUCCESS, props.to_format_json(allocator, json));
    ASSERT_FALSE(json.empty());
    for (bool reused : {false, true}) {
      ObFTParserProperty fast, reference;
      if (reused) {
        fast.min_token_size_ = 7;
        fast.max_token_size_ = 9;
        fast.ngram_token_size_ = 3;
        fast.min_ngram_token_size_ = 2;
        fast.max_ngram_token_size_ = 6;
        fast.ik_mode_smart_ = false;
        fast.dict_table_ = ObString::make_string("retained_dictionary");
        fast.stopword_table_ = ObString::make_string("retained_stopwords");
        fast.quantifier_table_ = ObString::make_string("retained_quantifiers");
      }
      reference = fast;
      ASSERT_EQ(OB_SUCCESS, fast.parse_for_parser_helper(parser, ObString()));
      ASSERT_EQ(OB_SUCCESS, reference.parse_for_parser_helper(parser, json));
      EXPECT_TRUE(fast.is_equal(reference)) << name << " reused=" << reused;
    }
  }
}
TEST(FTSHotPath, UpstreamPdqsortMatchesReferenceForCollatedRowPointers)
{
  using Row = std::tuple<bool, std::string, uint64_t>;
  const std::vector<std::string> words = {"", "a", "A", "database", "数据库", "😀",
      "full text", "full text ", "Straße", "strasse", "é", "e", std::string("a\0b", 3)};
  uint64_t state = 7349;
  for (int size : {0, 1, 2, 23, 24, 127, 128, 8192}) {
    for (int pattern = 0; pattern < 4; ++pattern) {
      std::vector<Row> rows;
      for (int i = 0; i < size; ++i) {
        state = state * 6364136223846793005ULL + 1;
        rows.emplace_back(i % 13 == 0, pattern == 3 ? "database" : words[(state >> 32) % words.size()], i);
      }
      for (ObCollationType collation : {CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_UTF8MB4_BIN}) {
        for (bool descending : {false, true}) {
          auto compare = [collation, descending](const Row *left, const Row *right) {
            if (std::get<0>(*left) != std::get<0>(*right)) return std::get<0>(*left);
            if (!std::get<0>(*left)) {
              const auto &a = std::get<1>(*left), &b = std::get<1>(*right);
              const int cmp = ObCharset::strcmpsp(collation, a.data(), a.size(), b.data(), b.size(), false);
              if (cmp != 0) return descending ? cmp > 0 : cmp < 0;
            }
            return std::get<2>(*left) < std::get<2>(*right);
          };
          std::vector<const Row *> actual;
          for (const auto &row : rows) actual.push_back(&row);
          if (pattern == 1 || pattern == 2) {
            std::sort(actual.begin(), actual.end(), compare);
            if (pattern == 2) std::reverse(actual.begin(), actual.end());
          }
          auto expected = actual;
          std::sort(expected.begin(), expected.end(), compare);
          ::pdqsort(actual.begin(), actual.end(), compare);
          EXPECT_EQ(expected, actual) << "size=" << size << " pattern=" << pattern;
        }
      }
    }
  }
}
TEST(FTSHotPath, IdenticalUtf8KeysMatchOriginalCollationForArbitraryBytes)
{
  const auto *charset = ObCharset::get_charset(CS_TYPE_UTF8MB4_GENERAL_CI);
  uint64_t state = 963;
  for (int sample = 0; sample < 16384; ++sample) {
    state = state * 6364136223846793005ULL + 1;
    std::string left((state >> 32) % 80, '\0');
    for (char &c : left) { state = state * 6364136223846793005ULL + 1; c = state >> 56; }
    std::string right = left;
    if (sample % 3 == 0) right += ' ';
    if (sample % 5 == 0 && !right.empty()) right.back() ^= 0x20;
    for (bool endspace : {false, true}) {
      const int expected = charset->coll->strnncollsp(charset,
          reinterpret_cast<const unsigned char *>(left.data()), left.size(),
          reinterpret_cast<const unsigned char *>(right.data()), right.size(), endspace);
      EXPECT_EQ(expected, ObCharset::strcmpsp(CS_TYPE_UTF8MB4_GENERAL_CI,
          left.data(), left.size(), right.data(), right.size(), endspace));
    }
  }
}
TEST(FTSHotPath, CanonicalIKDefaultsMatchValidatedJsonForReusedProperties)
{
  ObFTParser parser;
  ASSERT_EQ(OB_SUCCESS, parser.parse_from_str("ik.1", 4));
  for (bool reused : {false, true}) {
    ObFTParserProperty fast, reference;
    if (reused) {
      fast.ik_mode_smart_ = false;
      fast.min_token_size_ = 17;
      fast.max_token_size_ = 31;
      fast.dict_table_ = ObString::make_string("previous_dictionary");
      fast.stopword_table_ = ObString::make_string("previous_stopwords");
      fast.quantifier_table_ = ObString::make_string("previous_quantifiers");
    }
    reference = fast;
    ASSERT_EQ(OB_SUCCESS, fast.parse_for_parser_helper(parser,
        ObString::make_string("{\"ik_mode\":\"smart\"}")));
    ASSERT_EQ(OB_SUCCESS, reference.parse_for_parser_helper(parser,
        ObString::make_string(" { \"ik_mode\" : \"smart\" } ")));
    EXPECT_TRUE(fast.is_equal(reference));
    ASSERT_EQ(OB_SUCCESS, fast.parse_for_parser_helper(parser,
        ObString::make_string("{\"ik_mode\":\"max_word\"}")));
    EXPECT_FALSE(fast.ik_mode_smart_);
  }
}
TEST(FTSHotPath, EnglishBorrowedAndNormalizedTokensSurviveParserReset)
{
  ObArenaAllocator allocator("FTSBorrowTest");
  const std::string document = "lower UPPER MiXeD database \xC3\x84\xC3\x96\xC3\x9C more WORDS";
  ObFTParserParam param;
  param.allocator_ = &allocator;
  param.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
  param.fulltext_ = document.data();
  param.ft_length_ = document.size();
  const auto expected = reference_english(document, param.cs_);
  std::vector<ObString> words;
  {
    ObBEngFTParser parser(allocator);
    ASSERT_EQ(OB_SUCCESS, parser.init(&param));
    int ret = OB_SUCCESS;
    const char *word = nullptr;
    int64_t len = 0, chars = 0, frequency = 0;
    while ((ret = parser.get_next_token(word, len, chars, frequency)) == OB_SUCCESS) {
      words.emplace_back(len, word);
      EXPECT_EQ(1, frequency);
    }
    ASSERT_EQ(OB_ITER_END, ret);
  }
  ASSERT_EQ(expected.size(), words.size());
  for (size_t i = 0; i < words.size(); ++i) {
    EXPECT_EQ(expected[i], std::string(words[i].ptr(), words[i].length()));
  }
  // The first lowercase token should actually take the borrowing path.
  EXPECT_EQ(document.data(), words.front().ptr());
}

TEST(FTSHotPath, BatchedStringDiffTemplatesMatchScalarDecoder)
{
  using namespace blocksstable;
  for (int length : {1, 15, 16, 17, 127, 128, 129, 257}) {
    for (bool hex : {false, true}) {
      for (bool nulls : {false, true}) {
        SCOPED_TRACE(std::to_string(length) + "/" + std::to_string(hex) + "/" + std::to_string(nulls));
        constexpr int ROWS = 37;
        std::vector<ObStringDiffHeader::DiffDesc> runs;
        int diff_bytes = 0, common_bytes = 0;
        for (int pos = 0; pos < length;) {
          ObStringDiffHeader::DiffDesc run;
          run.diff_ = runs.size() % 2;
          run.count_ = std::min(1 + static_cast<int>(runs.size() % 11), length - pos);
          (run.diff_ ? diff_bytes : common_bytes) += run.count_;
          pos += run.count_;
          runs.push_back(run);
        }
        const int cell_length = hex ? (diff_bytes + 1) / 2 : diff_bytes;
        const int meta_length = sizeof(ObStringDiffHeader) + runs.size() * sizeof(runs[0])
            + (hex ? 16 : 0) + common_bytes;
        const int ext_length = nulls ? (ROWS * 2 + 7) / 8 : 0;
        std::vector<char> block(meta_length + ext_length + ROWS * cell_length + 8, 0);
        auto *header = new (block.data()) ObStringDiffHeader;
        header->string_size_ = length;
        header->length_ = cell_length;
        header->diff_desc_cnt_ = runs.size();
        header->hex_char_array_size_ = hex ? 16 : 0;
        std::memcpy(header->diff_descs_, runs.data(), runs.size() * sizeof(runs[0]));
        for (int i = 0; i < 16 && hex; ++i) header->hex_char_array()[i] = 'a' + i;
        for (int i = 0; i < common_bytes; ++i) header->common_data()[i] = i * 11;
        for (int i = 0; i < ROWS * cell_length; ++i) block[meta_length + ext_length + i] = i * 29;
        ObMicroBlockHeader micro;
        micro.row_count_ = ROWS;
        micro.extend_value_bit_ = nulls ? 2 : 0;
        ObColumnHeader column;
        column.type_ = ObColumnHeader::STRING_DIFF;
        column.obj_type_ = ObVarcharType;
        column.length_ = meta_length;
        column.set_fix_lenght_attr();
        ObBitStream bitstream(reinterpret_cast<unsigned char *>(block.data() + meta_length), ext_length);
        if (nulls) {
          column.set_has_extend_value_attr();
          for (int i = 0; i < ROWS; ++i) {
            ASSERT_EQ(OB_SUCCESS, bitstream.set(i * 2, 2,
                static_cast<int64_t>(i % 5 == 0 ? STORED_NULL : STORED_NOT_EXT)));
          }
        }
        ObArenaAllocator allocator("FTSDecodeTest");
        ObObjMeta meta;
        meta.set_varchar();
        meta.set_collation_type(CS_TYPE_BINARY);
        ObColumnDecoderCtx ctx;
        ctx.fill(meta, &micro, &column, &allocator, 0);
        ObStringDiffDecoder decoder;
        ASSERT_EQ(OB_SUCCESS, decoder.init(micro, column, block.data()));
        std::vector<int32_t> row_ids(ROWS);
        std::vector<ObDatum> actual(ROWS);
        for (int i = 0; i < ROWS; ++i) row_ids[i] = (i * 13) % ROWS;
        ASSERT_EQ(OB_SUCCESS, decoder.batch_decode(ctx, nullptr, row_ids.data(),
            nullptr, ROWS, actual.data()));
        for (int i = 0; i < ROWS; ++i) {
          ObDatum expected;
          ASSERT_EQ(OB_SUCCESS, decoder.decode(ctx, expected, row_ids[i], bitstream,
              block.data(), block.size()));
          ASSERT_EQ(expected.is_null(), actual[i].is_null());
          if (!expected.is_null()) EXPECT_EQ(expected.get_string(), actual[i].get_string());
        }
      }
    }
  }
}

class CJKOracleDictionary final : public ObIFTDict
{
public:
  explicit CJKOracleDictionary(std::vector<std::string> words = {}) : words_(std::move(words))
  {
    for (const auto &word : words_) {
      for (size_t end = 0; end < word.size();) {
        int64_t length = 0;
        if (ObCharset::first_valid_char(CS_TYPE_UTF8MB4_BIN, word.data() + end,
            word.size() - end, length) != OB_SUCCESS) break;
        end += length;
        prefixes_.push_back(word.substr(0, end));
      }
    }
    std::sort(prefixes_.begin(), prefixes_.end());
    prefixes_.erase(std::unique(prefixes_.begin(), prefixes_.end()), prefixes_.end());
  }
  int init() override { return OB_SUCCESS; }
  int match(const ObString &word, bool &found) const override
  {
    found = std::find(words_.begin(), words_.end(), std::string(word.ptr(), word.length())) != words_.end();
    return OB_SUCCESS;
  }
  int match(const ObString &word, ObDATrieHit &hit) const override
  {
    return advance(std::string(word.ptr(), word.length()), word.length(), hit);
  }
  int match_with_hit(const ObString &word, const ObDATrieHit &last, ObDATrieHit &hit) const override
  {
    if (fail_) return OB_ERR_UNEXPECTED;
    if (last.base_idx_ == 0 || last.base_idx_ > prefixes_.size()) return OB_ERR_UNEXPECTED;
    const std::string prefix = prefixes_[last.base_idx_ - 1];
    if (last.dict_ != identity(prefix)) return OB_ERR_UNEXPECTED;
    const ObDATrieHit saved(last);
    hit = saved;
    hit.dict_ = saved.dict_;
    return advance(prefix + std::string(word.ptr(), word.length()), word.length(), hit);
  }
  const ObIFTDict *other_ = nullptr;
  bool fail_ = false;
private:
  const ObIFTDict *identity(const std::string &prefix) const
  {
    return other_ != nullptr && prefix.compare(0, std::strlen("中"), "中") == 0 ? other_ : this;
  }
  int advance(const std::string &prefix, const int length, ObDATrieHit &hit) const
  {
    hit.set_unmatch();
    const auto pos = std::lower_bound(prefixes_.begin(), prefixes_.end(), prefix);
    if (pos != prefixes_.end() && *pos == prefix) {
      hit.base_idx_ = pos - prefixes_.begin() + 1;
      hit.end_pos_ += length;
      ++hit.char_cnt_;
      hit.dict_ = identity(prefix);
      hit.set_prefix();
      if (std::find(words_.begin(), words_.end(), prefix) != words_.end()) hit.set_match();
    }
    return OB_SUCCESS;
  }
  std::vector<std::string> words_, prefixes_;
};

TEST(FTSHotPath, CJKPendingHitsMatchAllDictionaryOccurrencesAcrossGrowthAndReuse)
{
  std::vector<std::string> words = {"哈中", "中哈", "中中", "中文", "中文分词", "数据", "数据库", "a", "ab", "abc"};
  std::string repeated;
  for (int i = 0; i < 24; ++i) { repeated += "哈"; words.push_back(repeated); }
  CJKOracleDictionary alternate, dictionary(words);
  dictionary.other_ = &alternate;
  ObArenaAllocator allocator("CJKOracle");
  ObIKCJKProcessor processor(dictionary, allocator);
  std::vector<std::string> documents = {repeated + repeated, "哈中哈哈中文分词中哈中中 数据库",
      "abc ab a 😀\t中文\n哈中中哈", " "};
  uint64_t state = 59;
  const char *parts[] = {"哈", "中", "文", "数据", "a", "b", " "};
  for (int doc = 0; doc < 128; ++doc) {
    std::string text;
    for (int i = 0; i < 80; ++i) {
      state = state * 6364136223846793005ULL + 1;
      text += parts[(state >> 32) % 7];
    }
    documents.push_back(text);
  }
  // An error must discard pending state before reuse with a new document.
  {
    TokenizeContext ctx(CS_TYPE_UTF8MB4_BIN, allocator, "哈哈", 6, true);
    ASSERT_EQ(OB_SUCCESS, ctx.init());
    const char *ch = nullptr; uint8_t length = 0;
    ObFTCharUtil::CharType type;
    ASSERT_EQ(OB_SUCCESS, ctx.current_char(ch, length));
    ASSERT_EQ(OB_SUCCESS, ctx.current_char_type(type));
    ASSERT_EQ(OB_SUCCESS, processor.do_process(ctx, ch, length, type));
    ASSERT_EQ(OB_SUCCESS, ctx.step_next());
    dictionary.fail_ = true;
    ASSERT_EQ(OB_SUCCESS, ctx.current_char(ch, length));
    EXPECT_EQ(OB_ERR_UNEXPECTED, processor.do_process(ctx, ch, length, type));
    dictionary.fail_ = false;
  }
  for (const auto &document : documents) {
    TokenizeContext ctx(CS_TYPE_UTF8MB4_BIN, allocator, document.data(), document.size(), true);
    ASSERT_EQ(OB_SUCCESS, ctx.init());
    while (true) {
      const char *ch = nullptr; uint8_t length = 0;
      ObFTCharUtil::CharType type;
      ASSERT_EQ(OB_SUCCESS, ctx.current_char(ch, length));
      ASSERT_EQ(OB_SUCCESS, ctx.current_char_type(type));
      ASSERT_EQ(OB_SUCCESS, processor.do_process(ctx, ch, length, type));
      const int ret = ctx.step_next();
      if (ret == OB_ITER_END) break;
      ASSERT_EQ(OB_SUCCESS, ret);
    }
    using Span = std::tuple<int64_t, int64_t, int64_t>;
    std::vector<Span> expected, actual;
    for (const auto &word : words) {
      int64_t chars = 0;
      for (size_t offset = 0; offset < word.size(); ++chars) {
        int64_t length = 0;
        ASSERT_EQ(OB_SUCCESS, ObCharset::first_valid_char(CS_TYPE_UTF8MB4_BIN,
            word.data() + offset, word.size() - offset, length));
        offset += length;
      }
      for (size_t pos = document.find(word); pos != std::string::npos; pos = document.find(word, pos + 1)) {
        expected.emplace_back(pos, word.size(), chars);
      }
    }
    for (const auto &token : ctx.token_list().tokens()) {
      actual.emplace_back(token.offset_, token.length_, token.char_cnt_);
    }
    std::sort(expected.begin(), expected.end()); std::sort(actual.begin(), actual.end());
    ASSERT_EQ(expected, actual);
  }
}

TEST(FTSHotPath, CombinedNextCharacterMatchesLegacyValidationAndClassification)
{
  ObArenaAllocator allocator("CharCtxTest");
  std::vector<std::string> documents = {"", "a中文😀Ａ１２", std::string("a\0b", 3),
      std::string("\xED\xA0\x80", 3), std::string("\xC0\xAF", 2),
      std::string("\xF4\x90\x80\x80", 4), std::string("a\xE4\xB8", 3),
      std::string("\xD8\0\xDC\0", 4), std::string("\0\xD8\0\xDC", 4)};
  uint64_t state = 137;
  for (int sample = 0; sample < 8192; ++sample) {
    std::string text(sample % 31 + 1, '\0');
    for (char &c : text) { state = state * 6364136223846793005ULL + 1; c = state >> 56; }
    documents.push_back(text);
  }
  for (auto collation : {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_UTF8MB4_UNICODE_CI,
      CS_TYPE_UTF16_BIN, CS_TYPE_UTF16LE_BIN}) {
    for (const auto &text : documents) {
      TokenizeContext ctx(collation, allocator, text.data(), text.size(), true);
      int actual = ctx.init();
      size_t offset = 0;
      while (true) {
        int64_t length = 0;
        ObFTCharUtil::CharType expected_type = ObFTCharUtil::CharType::USELESS;
        int expected = text.empty() ? OB_INVALID_ARGUMENT : ObCharset::first_valid_char(collation,
            text.data() + offset, text.size() - offset, length);
        if (expected == OB_SUCCESS) expected = ObFTCharUtil::classify_first_char(collation,
            text.data() + offset, length, expected_type);
        ASSERT_EQ(expected, actual) << collation << ":" << offset;
        if (expected != OB_SUCCESS) break;
        const char *ch = nullptr; uint8_t got_length = 0;
        ObFTCharUtil::CharType got_type;
        ASSERT_EQ(OB_SUCCESS, ctx.current_char(ch, got_length));
        ASSERT_EQ(OB_SUCCESS, ctx.current_char_type(got_type));
        ASSERT_EQ(text.data() + offset, ch);
        ASSERT_EQ(length, got_length); ASSERT_EQ(expected_type, got_type);
        offset += length;
        actual = ctx.step_next();
        if (offset == text.size()) { ASSERT_EQ(OB_ITER_END, actual); break; }
      }
    }
  }
}
} // namespace storage
} // namespace oceanbase
