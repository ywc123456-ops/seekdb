/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "object/ob_object.h"
#include "storage/fts/ob_fts_struct.h"
#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/ob_fts_parser_helper.h"
#include <cinttypes>

#include "common/json_type/ob_json_tree.h"
#include "share/ob_force_print_log.h"
#include "storage/fts/dict/ob_ft_dict_hub.h"
#include "storage/fts/ob_beng_ft_parser.h"
#include "storage/fts/ob_ik_ft_parser.h"
#include "storage/fts/ob_ngram2_ft_parser.h"
#include "storage/fts/ob_ngram_ft_parser.h"
#include "data_plane/fts/ob_fts_parser_property.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_whitespace_ft_parser.h"
#include "lib/allocator/ob_malloc.h"
#include "lib/lock/ob_mutex.h"
#include <atomic>

namespace oceanbase
{
namespace storage
{

namespace
{
/**
 * Thread local cache of the parsed fulltext parser context.
 *
 * ObFTParseHelper::init() parses the parser name and the parser properties
 * (a JSON document) on every call, although the result only depends on the
 * two input strings.  The cache keeps the parsed result of the most recently
 * used (parser name, parser properties) pairs per thread, so that the hot
 * tokenize path does not have to rebuild and re-parse the JSON document again
 * and again.
 *
 * The cached context only references immutable data:
 *  - ObFTParserName owns a copy of the parser name,
 *  - the parser descriptor is a process wide constant,
 *  - ObFTParserProperty only holds plain values and string literals.
 */
class ObFTParserContextCache final
{
public:
  static constexpr int64_t MAX_ENTRY_CNT = 4;
  static constexpr int64_t MAX_PROPERTIES_LENGTH = 1024;

  struct ParserContext
  {
    ObFTParser parser_;
    ObFTParserProperty property_;
    const ObIFTParserDesc *parser_desc_;
    ObAddWordFlag add_word_flag_;

    ParserContext() : parser_(), property_(), parser_desc_(nullptr), add_word_flag_() {}
  };

  struct Entry
  {
    char parser_name_[OB_FT_PARSER_NAME_LENGTH];
    char parser_properties_[MAX_PROPERTIES_LENGTH];
    int64_t parser_name_len_;
    int64_t parser_properties_len_;
    int64_t hit_cnt_;
    ParserContext context_;

    Entry()
      : parser_name_len_(0),
        parser_properties_len_(0),
        hit_cnt_(0),
        context_()
    {
      parser_name_[0] = '\0';
      parser_properties_[0] = '\0';
    }
  };

  static ObFTParserContextCache &instance()
  {
    static thread_local ObFTParserContextCache cache;
    return cache;
  }

  int get(const common::ObString &parser_name,
          const common::ObString &parser_properties,
          ObFTParser &parser,
          ObFTParserProperty &property,
          const ObIFTParserDesc *&parser_desc,
          ObAddWordFlag &add_word_flag)
  {
    int ret = OB_SUCCESS;
    int64_t idx = find(parser_name, parser_properties);
    if (OB_LIKELY(idx >= 0)) {
      entries_[idx].hit_cnt_++;
      fill(entries_[idx].context_, parser, property, parser_desc, add_word_flag);
    } else {
      ParserContext context;
      if (OB_FAIL(build(parser_name, parser_properties, context))) {
      } else if (OB_FAIL(store(parser_name, parser_properties, context))) {
      } else {
        fill(context, parser, property, parser_desc, add_word_flag);
      }
    }
    return ret;
  }

private:
  static void fill(const ParserContext &context,
                   ObFTParser &parser,
                   ObFTParserProperty &property,
                   const ObIFTParserDesc *&parser_desc,
                   ObAddWordFlag &add_word_flag)
  {
    parser = context.parser_;
    property = context.property_;
    parser_desc = context.parser_desc_;
    add_word_flag = context.add_word_flag_;
  }

  int64_t find(const common::ObString &parser_name, const common::ObString &parser_properties) const
  {
    for (int64_t i = 0; i < entry_cnt_; ++i) {
      const Entry &entry = entries_[i];
      if (entry.parser_name_len_ == parser_name.length()
          && entry.parser_properties_len_ == parser_properties.length()
          && 0 == MEMCMP(entry.parser_name_, parser_name.ptr(), parser_name.length())
          && (parser_properties.empty()
              || 0 == MEMCMP(entry.parser_properties_, parser_properties.ptr(), parser_properties.length()))) {
        return i;
      }
    }
    return -1;
  }

  int build(const common::ObString &parser_name,
            const common::ObString &parser_properties,
            ParserContext &context)
  {
    int ret = OB_SUCCESS;
    if (OB_FAIL(context.parser_.parse_from_str(parser_name.ptr(), parser_name.length()))) {
    } else if (OB_FAIL(context.property_.parse_for_parser_helper(context.parser_, parser_properties))) {
    } else if (OB_FAIL(context.parser_.get_desc(context.parser_desc_))) {
    } else if (OB_ISNULL(context.parser_desc_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(context.parser_desc_->get_add_word_flag(context.add_word_flag_))) {
    } else {
      // ok
    }
    return ret;
  }

  int store(const common::ObString &parser_name,
            const common::ObString &parser_properties,
            const ParserContext &context)
  {
    int ret = OB_SUCCESS;
    if (OB_UNLIKELY(parser_name.length() >= OB_FT_PARSER_NAME_LENGTH
                    || parser_properties.length() >= MAX_PROPERTIES_LENGTH)) {
      // too long to cache, keep the caller's result
      return OB_SUCCESS;
    }
    int64_t idx = entry_cnt_;
    if (entry_cnt_ < MAX_ENTRY_CNT) {
      entry_cnt_++;
    } else {
      // evict the least recently used entry
      idx = 0;
      for (int64_t i = 1; i < entry_cnt_; ++i) {
        if (entries_[i].hit_cnt_ < entries_[idx].hit_cnt_) {
          idx = i;
        }
      }
    }
    Entry &entry = entries_[idx];
    if (!parser_name.empty()) {
      MEMCPY(entry.parser_name_, parser_name.ptr(), parser_name.length());
    }
    entry.parser_name_[parser_name.length()] = '\0';
    entry.parser_name_len_ = parser_name.length();
    if (!parser_properties.empty()) {
      MEMCPY(entry.parser_properties_, parser_properties.ptr(), parser_properties.length());
    }
    entry.parser_properties_[parser_properties.length()] = '\0';
    entry.parser_properties_len_ = parser_properties.length();
    entry.hit_cnt_ = 0;
    entry.context_ = context;
    return ret;
  }

private:
  Entry entries_[MAX_ENTRY_CNT];
  int64_t entry_cnt_ = 0;
};

/**
 * Collector of the raw token stream of one document.
 *
 * The tokens are appended to one buffer as
 *   [token header][word bytes][token header][word bytes]...
 * so that the recorded stream can be replayed with a simple walk over the
 * buffer.  The collector only feeds the segment memo, a failure to collect
 * never fails the segmentation itself.
 */
class ObFTTokenCollector final
{
public:
  struct Token
  {
    int32_t word_len_;
    int32_t char_cnt_;
    int32_t word_freq_;
    int32_t reserved_;
  };

public:
  ObFTTokenCollector() : buf_(nullptr), cap_(0), len_(0), token_cnt_(0), armed_(false), is_collecting_(true) {}
  ~ObFTTokenCollector() { destroy(); }

  void arm()
  {
    len_ = 0;
    token_cnt_ = 0;
    armed_ = true;
    is_collecting_ = true;
  }

  void disarm() { armed_ = false; }

  void destroy()
  {
    if (nullptr != buf_) {
      ob_free(buf_);
      buf_ = nullptr;
    }
    cap_ = 0;
    len_ = 0;
    token_cnt_ = 0;
    armed_ = false;
  }

  int add(const char *word, const int64_t word_len, const int64_t char_cnt, const int64_t word_freq)
  {
    int ret = OB_SUCCESS;
    if (!armed_) {
      // not collecting
    } else if (!is_collecting_) {
      // the collection failed before, keep the segmentation going
    } else if (OB_ISNULL(word) || word_len <= 0 || char_cnt <= 0 || word_freq <= 0) {
      // never produced by the tokenizer
    } else if (OB_FAIL(ensure_capacity(len_ + sizeof(Token) + word_len))) {
      is_collecting_ = false;
    } else {
      Token token;
      token.word_len_ = static_cast<int32_t>(word_len);
      token.char_cnt_ = static_cast<int32_t>(char_cnt);
      token.word_freq_ = static_cast<int32_t>(word_freq);
      token.reserved_ = 0;
      MEMCPY(buf_ + len_, &token, sizeof(Token));
      len_ += sizeof(Token);
      MEMCPY(buf_ + len_, word, word_len);
      len_ += word_len;
      ++token_cnt_;
    }
    return ret;
  }

  bool is_valid() const { return is_collecting_ && token_cnt_ > 0; }
  int64_t size() const { return len_; }
  int64_t token_cnt() const { return token_cnt_; }
  const char *data() const { return buf_; }

private:
  int ensure_capacity(const int64_t need)
  {
    int ret = OB_SUCCESS;
    if (need > cap_) {
      int64_t new_cap = (cap_ > 0 ? cap_ * 2 : 1024);
      while (new_cap < need) {
        new_cap *= 2;
      }
      char *new_buf = static_cast<char *>(ob_malloc(new_cap, common::ObMemAttr("FTSegmentMemo")));
      if (OB_ISNULL(new_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (nullptr != buf_ && len_ > 0) {
          MEMCPY(new_buf, buf_, len_);
        }
        if (nullptr != buf_) {
          ob_free(buf_);
        }
        buf_ = new_buf;
        cap_ = new_cap;
      }
    }
    return ret;
  }

private:
  char *buf_;
  int64_t cap_;
  int64_t len_;
  int64_t token_cnt_;
  bool armed_;
  bool is_collecting_;
};

// The collector of the thread that is currently segmenting a document.
thread_local ObFTTokenCollector tl_segment_collector;

/**
 * Process wide memo of the raw token stream of the fulltext tokenizers.
 *
 * A fulltext index is built by two auxiliary tables, the postings table
 * (`__idx_xxx_fti_ik`) and the doc word table (`__idx_xxx_fti_ik_fts_doc_word`).
 * Both of them carry the `__word_segment` column and both of them have to
 * split the very same document text into words, so every document is
 * segmented twice while the index is built.  The two build tasks run
 * concurrently, therefore the duplicated work is done twice in parallel and
 * the build is bound by the tokenizer.
 *
 * The memo records the raw token stream of a document (the output of
 * ObITokenIterator::get_next_token) keyed by the document text, so that the
 * second - and any later - segmentation of the same text can replay the
 * recorded tokens instead of running the tokenizer again.
 *
 * The recorded tokens are replayed through ObAddWord::process_word(), exactly
 * like the original loop does, therefore the words, the word counts and the
 * document length are the same as without the memo.
 *
 * The token stream of a document only depends on
 *   - the document text,
 *   - the collation type of the column,
 *   - the parser (descriptor and version) and the parser properties.
 * The built in dictionaries (main / quantifier / stop word) are process wide
 * and immutable for the lifetime of the process (see ObFTDictHub), so no
 * further state has to be part of the key.
 *
 * The memo is only used for parsers that copy the word into the allocator of
 * the caller (the casedown flag), so that the replayed words never point
 * into the memo buffer.
 */
class ObFTSegmentMemo final
{
public:
  static constexpr int64_t SHARD_CNT = 32;
  static constexpr int64_t SLOT_CNT = 8192;
  static constexpr int64_t MAX_DOC_LEN = 16 * 1024;
  static constexpr int64_t MAX_ENTRY_SIZE = 256 * 1024;
  static constexpr int64_t MAX_TOTAL_SIZE = 64 * 1024 * 1024;

  struct Entry
  {
    uint64_t hash1_;
    uint64_t hash2_;
    int64_t doc_len_;
    int64_t token_cnt_;
    int64_t size_;
    char *data_;
  };

  struct Shard
  {
    Entry entries_[SLOT_CNT];
    lib::ObMutex mutex_;
  };

public:
  static ObFTSegmentMemo &instance()
  {
    static ObFTSegmentMemo memo;
    return memo;
  }

  /**
   * Replay the recorded token stream of the given document.
   * A corrupted entry is treated as a miss.
   */
  int replay(const uint64_t hash1,
             const uint64_t hash2,
             const int64_t doc_len,
             ObAddWord &add_word,
             bool &is_hit)
  {
    int ret = OB_SUCCESS;
    is_hit = false;
    Shard &shard = shards_[hash1 % SHARD_CNT];
    lib::ObMutexGuard guard(shard.mutex_);
    Entry &entry = shard.entries_[(hash1 / SHARD_CNT) % SLOT_CNT];
    if (OB_ISNULL(entry.data_)
        || entry.hash1_ != hash1
        || entry.hash2_ != hash2
        || entry.doc_len_ != doc_len) {
      return OB_SUCCESS;
    }
    const char *pos = entry.data_;
    const char *end = entry.data_ + entry.size_;
    // validate the recorded stream first, a corrupted entry is treated as a
    // miss and must not add a part of the words twice
    bool is_valid = true;
    for (int64_t i = 0; i < entry.token_cnt_ && is_valid; ++i) {
      ObFTTokenCollector::Token token;
      if (pos + sizeof(token) > end) {
        is_valid = false;
        break;
      }
      MEMCPY(&token, pos, sizeof(token));
      pos += sizeof(token);
      if (token.word_len_ <= 0 || token.char_cnt_ <= 0 || token.word_freq_ <= 0
          || pos + token.word_len_ > end) {
        is_valid = false;
        break;
      }
      pos += token.word_len_;
    }
    if (is_valid && pos != end) {
      is_valid = false;
    }
    if (is_valid) {
      pos = entry.data_;
      for (int64_t i = 0; i < entry.token_cnt_ && OB_SUCC(ret); ++i) {
        ObFTTokenCollector::Token token;
        MEMCPY(&token, pos, sizeof(token));
        pos += sizeof(token);
        if (OB_FAIL(add_word.process_word(pos, token.word_len_, token.char_cnt_, token.word_freq_))) {
          break;
        }
        pos += token.word_len_;
      }
      // a failed replay falls back to the tokenizer
      is_hit = OB_SUCC(ret);
    }
    return OB_SUCCESS;
  }

  /**
   * Record the token stream of a document.  A failure to record never fails
   * the caller, the memo is only an optimization.
   */
  void store(const uint64_t hash1,
             const uint64_t hash2,
             const int64_t doc_len,
             const int64_t token_cnt,
             const char *data,
             const int64_t size)
  {
    if (size <= 0 || size > MAX_ENTRY_SIZE || OB_ISNULL(data)) {
      return;
    }
    if (total_size_.load(std::memory_order_relaxed) + size > MAX_TOTAL_SIZE) {
      // the memo is full
      return;
    }
    char *buf = static_cast<char *>(ob_malloc(size, common::ObMemAttr("FTSegmentMemo")));
    if (OB_ISNULL(buf)) {
      return;
    }
    MEMCPY(buf, data, size);
    Shard &shard = shards_[hash1 % SHARD_CNT];
    lib::ObMutexGuard guard(shard.mutex_);
    Entry &entry = shard.entries_[(hash1 / SHARD_CNT) % SLOT_CNT];
    if (nullptr != entry.data_) {
      total_size_.fetch_sub(entry.size_, std::memory_order_relaxed);
      ob_free(entry.data_);
    }
    entry.hash1_ = hash1;
    entry.hash2_ = hash2;
    entry.doc_len_ = doc_len;
    entry.token_cnt_ = token_cnt;
    entry.size_ = size;
    entry.data_ = buf;
    total_size_.fetch_add(size, std::memory_order_relaxed);
  }

  static inline uint64_t mix64(uint64_t x)
  {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
  }

  /**
   * The key of a document: the text itself plus everything that can change
   * the tokens produced for it.
   */
  static void calc_hash(const char *text,
                        const int64_t text_len,
                        const uint64_t ctx_hash,
                        uint64_t &hash1,
                        uint64_t &hash2)
  {
    uint64_t h1 = 1469598103934665603ULL;
    uint64_t h2 = 0x9e3779b97f4a7c15ULL;
    for (int64_t i = 0; i < text_len; ++i) {
      const uint64_t c = static_cast<uint64_t>(static_cast<unsigned char>(text[i]));
      h1 = (h1 ^ c) * 1099511628211ULL;
      h2 = mix64(h2 ^ (c + 0x9e3779b97f4a7c15ULL));
    }
    hash1 = mix64(h1 ^ ctx_hash);
    hash2 = mix64(h2 ^ mix64(ctx_hash + 0x165667b19e3779f9ULL));
  }

  static uint64_t calc_ctx_hash(const common::ObCollationType type,
                                const int64_t parser_version,
                                const void *parser_desc,
                                const ObFTParserProperty &property)
  {
    uint64_t ctx = mix64(static_cast<uint64_t>(type));
    ctx = mix64(ctx ^ static_cast<uint64_t>(parser_version));
    ctx = mix64(ctx ^ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(parser_desc)));
    ctx = mix64(ctx ^ static_cast<uint64_t>(property.min_token_size_));
    ctx = mix64(ctx ^ static_cast<uint64_t>(property.max_token_size_));
    ctx = mix64(ctx ^ static_cast<uint64_t>(property.ngram_token_size_));
    ctx = mix64(ctx ^ static_cast<uint64_t>(property.min_ngram_token_size_));
    ctx = mix64(ctx ^ static_cast<uint64_t>(property.max_ngram_token_size_));
    ctx = mix64(ctx ^ (property.ik_mode_smart_ ? 1ULL : 2ULL));
    ctx = mix64(ctx ^ hash_string(property.stopword_table_));
    ctx = mix64(ctx ^ hash_string(property.dict_table_));
    ctx = mix64(ctx ^ hash_string(property.quantifier_table_));
    return ctx;
  }

  static uint64_t hash_string(const common::ObString &str)
  {
    uint64_t h = 1469598103934665603ULL;
    for (int64_t i = 0; i < str.length(); ++i) {
      h = (h ^ static_cast<uint64_t>(static_cast<unsigned char>(str.ptr()[i]))) * 1099511628211ULL;
    }
    return mix64(h);
  }

private:
  Shard shards_[SHARD_CNT];
  std::atomic<int64_t> total_size_{0};
};
} // namespace

const char *ObFTParser::NAME_STR[ObFTParser::ParserType::FTP_MAX] = {
#define FT_PARSER_TYPE(ftp_type, parser_name) #parser_name,
  FTS_BUILD_IN_PARSER_LIST
#undef FT_PARSER_TYPE
};

bool ObFTParser::is_builtin() const
{
  bool is_builtin = false;
#define FT_PARSER_TYPE(ftp_type, parser_name) is_builtin = is_builtin || is_##parser_name();
  FTS_BUILD_IN_PARSER_LIST
#undef FT_PARSER_TYPE
  return is_builtin;
}

int ObFTParser::init(const common::ObString &parser_name)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(parser_name_.set_name(parser_name))) {
  } else if (!is_builtin()) {
    ret = OB_FUNCTION_NOT_DEFINED;
    LOG_USER_ERROR(OB_FUNCTION_NOT_DEFINED, parser_name.length(), parser_name.ptr());
  } else {
    parser_version_ = BUILTIN_VERSION;
  }
  return ret;
}

int ObFTParser::parse_from_str(const char *parser_name, const int64_t buf_len)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(parser_name)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(buf_len >= OB_FT_PARSER_NAME_LENGTH)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    char name[OB_FT_PARSER_NAME_LENGTH];
    char *saveptr = nullptr;
    char *token = nullptr;
    char *end_ptr = nullptr;
    MEMCPY(name, parser_name, buf_len);
    name[buf_len] = '\0';
    if (OB_ISNULL(token = STRTOK_R(name, ".", &saveptr))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(parser_name_.set_name(token))) {
    } else if (OB_ISNULL(token = STRTOK_R(nullptr, ".", &saveptr))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ob_strtoll(token, end_ptr, parser_version_))) {
    } else if (OB_NOT_NULL(token = STRTOK_R(nullptr, ".", &saveptr))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (!is_builtin()) {
      ret = OB_FUNCTION_NOT_DEFINED;
      LOG_USER_ERROR(OB_FUNCTION_NOT_DEFINED, static_cast<int>(buf_len), parser_name);
    } else if (OB_UNLIKELY(!is_valid())) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

// The fulltext parser name consists of two parts: name and version, e.g. default_parser.1,
// separated by dot. This function is designed to serialize them into cstring.
int ObFTParser::serialize_to_str(char *buf, const int64_t buf_len)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  if (OB_ISNULL(buf) || OB_UNLIKELY(buf_len < OB_FT_PARSER_NAME_LENGTH)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(!is_valid())) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(common::databuff_printf(buf, buf_len, pos, "%.*s.%" PRId64,
          static_cast<int>(parser_name_.len()), parser_name_.str(),
          parser_version_))) {
  }
  return ret;
}

int ObFTParser::get_desc(const ObIFTParserDesc *&parser_desc) const
{
  int ret = OB_SUCCESS;
  parser_desc = nullptr;
  if (OB_UNLIKELY(!is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (is_space()) {
    static const ObWhiteSpaceFTParserDesc desc;
    parser_desc = &desc;
  } else if (is_ngram()) {
    static const ObNgramFTParserDesc desc;
    parser_desc = &desc;
  } else if (is_beng()) {
    static const ObBasicEnglishFTParserDesc desc;
    parser_desc = &desc;
  } else if (is_ik()) {
    static const ObIKFTParserDesc desc;
    parser_desc = &desc;
  } else if (is_ngram2()) {
    static const ObNgram2FTParserDesc desc;
    parser_desc = &desc;
  } else {
    ret = OB_FUNCTION_NOT_DEFINED;
  }
  return ret;
}
////////////////////////////////////////////////////////////////////////////////
// ObFTParseData

static ObFTParseData *g_ftparse_data = nullptr;
static constexpr const char *FTPARSE_DATA_MEMORY_LABEL = "FtParse";

int ObFTParseData::init_global()
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(g_ftparse_data)) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(g_ftparse_data = OB_NEW(ObFTParseData, FTPARSE_DATA_MEMORY_LABEL))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(g_ftparse_data->init())) {
  }
  return ret;
}

void ObFTParseData::deinit_global()
{
  if (OB_NOT_NULL(g_ftparse_data)) {
    OB_DELETE(ObFTParseData, FTPARSE_DATA_MEMORY_LABEL, g_ftparse_data);
    g_ftparse_data = nullptr;
  }
}

ObFTParseData &ObFTParseData::instance()
{
  return *g_ftparse_data;
}

ObFTParseData::~ObFTParseData()
{
  destroy();
}

int ObFTParseData::init()
{
  int ret = OB_SUCCESS;
  lib::ObMemAttr mem_attr;
  mem_attr.label_ = FTPARSE_DATA_MEMORY_LABEL;

  if (OB_FAIL(handler_allocator_.init(lib::ObMallocAllocator::get_instance(),
                                      OB_MALLOC_NORMAL_BLOCK_SIZE,
                                      mem_attr))) {
  } else if (OB_FAIL(init_and_set_stopword_list())) {
  } else if (OB_FAIL(init_dict_hub())) {
  } else {
    is_inited_ = true;
    FLOG_INFO("succeeded to initialize fulltext parser data", KP(this));
  }

  if (OB_UNLIKELY(!is_inited_)) {
    destroy();
  }
  return ret;
}

int ObFTParseData::init_and_set_stopword_list()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(stop_word_checker_ = OB_NEWx(ObStopWordChecker, &handler_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(stop_word_checker_->init())) {
  }

  if (OB_FAIL(ret)) {
    OB_DELETEx(ObStopWordChecker, &handler_allocator_, stop_word_checker_);
    stop_word_checker_ = nullptr;
  }

  return ret;
}

int ObFTParseData::init_dict_hub()
{
  // make dict
  int ret = OB_SUCCESS;
  if (OB_ISNULL(dict_hub_ = OB_NEWx(ObFTDictHub, &handler_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(dict_hub_->init())) {
  }
  return ret;
}

void ObFTParseData::destroy()
{
  if (OB_NOT_NULL(stop_word_checker_)) {
    stop_word_checker_->destroy();
    OB_DELETEx(ObStopWordChecker, &handler_allocator_, stop_word_checker_);
    stop_word_checker_ = nullptr;
  }

  if (!OB_ISNULL(dict_hub_)) {
    dict_hub_->destroy();
    dict_hub_->~ObFTDictHub();
    handler_allocator_.free(dict_hub_);
    dict_hub_ = nullptr;
  }

  handler_allocator_.reset();
  is_inited_ = false;
}

int ObFTParseData::get_dict_hub(ObFTDictHub *&hub)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(dict_hub_)) {
    ret = OB_NOT_INIT;
  } else {
    hub = dict_hub_;
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// ObFTParseHelper
int ObFTParseHelper::segment(
    const ObFTParserProperty &property,
    const int64_t parser_version,
    const ObIFTParserDesc *parser_desc,
    const ObCharsetInfo *cs,
    const char *ft,
    const int64_t ft_len,
    common::ObIAllocator &allocator,
    ObAddWord &add_word)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(parser_version < 0 || nullptr == parser_desc || nullptr == cs || nullptr == ft || 0 >= ft_len)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObFTParserParam param;
    ObITokenIterator *iter = nullptr;
    param.allocator_ = &allocator;
    param.cs_ = cs;
    param.fulltext_ = ft;
    param.ft_length_ = ft_len;
    param.parser_version_ = parser_version;
    param.ngram_token_size_ = property.ngram_token_size_;
    param.ik_param_.mode_
        = (property.ik_mode_smart_ ? ObFTIKParam::Mode::SMART : ObFTIKParam::Mode::MAX_WORD);
    param.min_ngram_size_ = property.min_ngram_token_size_;
    param.max_ngram_size_ = property.max_ngram_token_size_;

    if (OB_FAIL(parser_desc->segment(&param, iter))) {
    } else if (OB_ISNULL(iter)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const char *word = nullptr;
      int64_t word_len = 0;
      int64_t char_cnt = 0;
      int64_t word_freq = 0;
      while (OB_SUCC(ret)) {
        if (OB_FAIL(iter->get_next_token(word, word_len, char_cnt, word_freq))) {
          if (OB_ITER_END != ret) {
          }
        } else if (OB_FAIL(add_word.process_word(word, word_len, char_cnt, word_freq))) {
        } else {
          // record the token for the segment memo, the word is copied before
          // the iterator reuses its buffer
          tl_segment_collector.add(word, word_len, char_cnt, word_freq);
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
    if (OB_NOT_NULL(iter)) {
      parser_desc->free_token_iter(&param, iter);
      iter = nullptr;
    }
  }
  return ret;
}

ObFTParseHelper::ObFTParseHelper()
    : allocator_(nullptr),
    parser_desc_(nullptr),
    parser_name_(),
    add_word_flag_(),
    parser_property_(),
    is_inited_(false)
{
}

ObFTParseHelper::~ObFTParseHelper()
{
  reset();
}

int ObFTParseHelper::init(
    common::ObIAllocator *allocator,
    const common::ObString &parser_name,
    const common::ObString &parser_properties)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(allocator) || OB_UNLIKELY(parser_name.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObFTParserContextCache::instance().get(parser_name,
                                                           parser_properties,
                                                           parser_name_,
                                                           parser_property_,
                                                           parser_desc_,
                                                           add_word_flag_))) {
  } else {
    allocator_ = allocator;
    is_inited_ = true;
  }
  if (OB_FAIL(ret) && OB_UNLIKELY(!is_inited_)) {
    reset();
  }
  return ret;
}

void ObFTParseHelper::reset()
{
  parser_desc_ = nullptr;
  allocator_ = nullptr;
  add_word_flag_.clear();
  is_inited_ = false;
}

int ObFTParseHelper::segment(
    const ObObjMeta &meta,
    const char *fulltext,
    const int64_t fulltext_len,
    int64_t &doc_length,
    ObFTWordMap &words) const
{
  int ret = OB_SUCCESS;
  const ObCharsetInfo *cs = nullptr;
  ObCollationType type = meta.get_collation_type();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(allocator_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_UNLIKELY(CS_TYPE_INVALID == type || type >= CS_TYPE_PINYIN_BEGIN_MARK)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(cs = common::ObCharset::get_charset(type))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    words.reuse();
    ObAddWord add_word(parser_property_, meta, add_word_flag_, *allocator_, words);
    // The segment memo is only used for the parsers that copy the word into
    // the allocator of the caller (the casedown flag), so that a replayed
    // word never points into the memo buffer.
    ObFTSegmentMemo &memo = ObFTSegmentMemo::instance();
    uint64_t hash1 = 0;
    uint64_t hash2 = 0;
    const bool memo_enabled = add_word_flag_.casedown()
                               && fulltext_len > 0
                               && fulltext_len <= ObFTSegmentMemo::MAX_DOC_LEN;
    bool is_hit = false;
    if (memo_enabled) {
      const uint64_t ctx_hash = ObFTSegmentMemo::calc_ctx_hash(
          type,
          parser_name_.get_parser_version(),
          parser_desc_,
          parser_property_);
      ObFTSegmentMemo::calc_hash(fulltext, fulltext_len, ctx_hash, hash1, hash2);
      if (OB_SUCC(memo.replay(hash1, hash2, fulltext_len, add_word, is_hit)) && is_hit) {
        doc_length = add_word.get_add_word_count();
      } else {
        // miss, segment the document and record the tokens
        words.reuse();
        tl_segment_collector.arm();
        ret = segment(
                parser_property_,
                parser_name_.get_parser_version(),
                parser_desc_,
                cs,
                fulltext,
                fulltext_len,
                *allocator_,
                add_word);
        tl_segment_collector.disarm();
        if (OB_SUCC(ret)) {
          if (tl_segment_collector.is_valid()) {
            memo.store(hash1,
                       hash2,
                       fulltext_len,
                       tl_segment_collector.token_cnt(),
                       tl_segment_collector.data(),
                       tl_segment_collector.size());
          }
          doc_length = add_word.get_add_word_count();
        }
      }
    } else if (OB_FAIL(segment(
                           parser_property_,
                           parser_name_.get_parser_version(),
                           parser_desc_,
                           cs,
                           fulltext,
                           fulltext_len,
                           *allocator_,
                           add_word))) {
    } else {
      doc_length = add_word.get_add_word_count();
    }
  }
  LOG_DEBUG("ft parse segment", K(ret), K(type), K(add_word_flag_), K(parser_name_),
      K(ObString(fulltext_len, fulltext)), K(words.size()));
  return ret;
}

int ObFTParseHelper::check_is_the_same(
    const common::ObString &parser_name_str,
    const common::ObString &parser_properties,
    bool &is_same) const
{
  int ret = OB_SUCCESS;
  is_same = false;
  if (is_inited_) {
    storage::ObFTParser parser_name;
    ObFTParserProperty parser_property;
    if (OB_UNLIKELY(parser_name_str.empty())) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(parser_name.parse_from_str(parser_name_str.ptr(), parser_name_str.length()))) {
    } else if (OB_FAIL(parser_property.parse_for_parser_helper(parser_name, parser_properties))) {
    } else if (parser_name == parser_name_ && parser_property.is_equal(parser_property_)) {
      is_same = true;
    }
  }
  return ret;
}

int ObFTParseHelper::make_detail_json(
    const ObFTWordMap &words,
    const int64_t doc_length,
    common::ObIJsonBase *&json_root)
{
 int ret = OB_SUCCESS;

 ObJsonObject *root_obj = nullptr;

 ObJsonInt *cnt = nullptr;

 ObJsonArray *token_array = nullptr;

 if (OB_ISNULL(root_obj = OB_NEWx(ObJsonObject, allocator_, allocator_))) {
   ret = OB_ALLOCATE_MEMORY_FAILED;
 } else if (OB_ISNULL(cnt = OB_NEWx(ObJsonInt, allocator_, doc_length))) {
   ret = OB_ALLOCATE_MEMORY_FAILED;
 } else if (OB_ISNULL(token_array = OB_NEWx(ObJsonArray, allocator_, allocator_))) {
   ret = OB_ALLOCATE_MEMORY_FAILED;
 } else {
   for (ObFTWordMap::const_iterator it = words.begin(); OB_SUCC(ret) && it != words.end(); ++it) {
     ObString key = it->first.get_word().get_string();
     ObJsonObject *node = nullptr;
     ObJsonInt *token_cnt_node = nullptr;
     if (OB_ISNULL(node = OB_NEWx(ObJsonObject, allocator_, allocator_))) {
       ret = OB_ALLOCATE_MEMORY_FAILED;
     } else if (OB_ISNULL(token_cnt_node = OB_NEWx(ObJsonInt, allocator_, it->second))) {
       ret = OB_ALLOCATE_MEMORY_FAILED;
     } else if (OB_FAIL(node->add(key, token_cnt_node))) {
     } else if (OB_FAIL(token_array->append(node))) {
     } else {
       // pass
     }

     if (OB_FAIL(ret)) {
       OB_DELETEx(ObJsonObject, allocator_, node);
       OB_DELETEx(ObJsonInt, allocator_, token_cnt_node);
     }
   } // for

   if (OB_SUCC(ret)) {
     if (OB_FAIL(root_obj->add(ENTRY_NAME_TOKENS, token_array))) {
     } else if (OB_FAIL(root_obj->add(ENTRY_NAME_DOC_LEN, cnt))) {
     }
   }
 }
  if (OB_SUCC(ret)) {
    json_root = root_obj;
  } else {
    OB_DELETEx(ObJsonObject, allocator_, root_obj);
    OB_DELETEx(ObJsonInt, allocator_, cnt);
    OB_DELETEx(ObJsonArray, allocator_, token_array);
  }

  return ret;
}

int ObFTParseHelper::make_token_array_json(
    const ObFTWordMap &words,
    common::ObIJsonBase *&json_root)
{
  int ret = OB_SUCCESS;
  ObJsonArray *token_array = nullptr;
  if (OB_UNLIKELY(OB_ISNULL(token_array = OB_NEWx(ObJsonArray, allocator_, allocator_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    for (ObFTWordMap::const_iterator it = words.begin(); OB_SUCC(ret) && it != words.end(); ++it) {
      ObString key = it->first.get_word().get_string();
      ObJsonString *token = nullptr;
      if (OB_UNLIKELY(OB_ISNULL(token = OB_NEWx(ObJsonString, allocator_, key)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (OB_FAIL(token_array->append(token))) {
          OB_DELETEx(ObJsonString, allocator_, token);
        } else {
        }
      }
    } // for
  }
  if (OB_SUCC(ret)) {
    json_root = token_array;
  } else {
    OB_DELETEx(ObJsonArray, allocator_, token_array);
  }
  return ret;
}

int ObFTParseHelper::set_add_word_flag(const ObIFTParserDesc &ftparser_desc)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ftparser_desc.get_add_word_flag(add_word_flag_))) {
  }
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
