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


#include "storage/fts/ik/ob_ik_char_util.h"
#include <gtest/gtest.h>
#include <string>

namespace oceanbase {
namespace storage {
using namespace common;
using Type = ObFTCharUtil::CharType;

// Independent reference preserves the original repeated-decode decision chain.
static int reference(const char *input, uint8_t len, Type &type)
{
  type = Type::USELESS;
  if (input == nullptr) return OB_INVALID_ARGUMENT;
  const auto *s = reinterpret_cast<const unsigned char *>(input);
  ob_wc_t wc = 0;
  auto decode = [&]() {
    return ob_charset_decode_unicode<CHARSET_UTF8MB4>(s, s + len, wc) < 0
        ? OB_UNEXPECT_INTERNAL_ERROR : OB_SUCCESS;
  };
  int ret = decode();
  if (ret != OB_SUCCESS) return ret;
  if (ObUnicodeBlockUtils::is_alpha(wc)) type = Type::ENGLISH_LETTER;
  else if ((ret = decode()) != OB_SUCCESS) return ret;
  else if (ObUnicodeBlockUtils::is_arabic(wc)) type = Type::ARABIC_LETTER;
  else if ((ret = decode()) != OB_SUCCESS) return ret;
  else if (ObUnicodeBlockUtils::is_chinese(wc)) type = Type::CHINESE;
  else if ((ret = decode()) != OB_SUCCESS) return ret;
  else if (ObUnicodeBlockUtils::is_other_cjk(wc)) type = Type::OTHER_CJK;
  return ret;
}

TEST(IKCharUtil, EveryUnicodeScalarMatchesReference)
{
  char buf[4];
  for (ob_wc_t wc = 0; wc <= 0x10ffff; ++wc) {
    if (wc >= 0xd800 && wc <= 0xdfff) continue;
    int len = ob_charset_encode_unicode<CHARSET_UTF8MB4>(
        wc, reinterpret_cast<unsigned char *>(buf),
        reinterpret_cast<unsigned char *>(buf + sizeof(buf)));
    ASSERT_GT(len, 0);
    Type expected, actual;
    ASSERT_EQ(reference(buf, len, expected), ObFTCharUtil::classify_first_char(
        CS_TYPE_UTF8MB4_BIN, buf, len, actual)) << wc;
    ASSERT_EQ(expected, actual) << wc;
  }
}

TEST(IKCharUtil, MalformedAndTruncatedInputMatchesReference)
{
  const std::string inputs[] = {
      "", std::string("\0", 1), "\x80", "\xc0\xaf", "\xc2", "\xe4\xb8",
      "\xed\xa0\x80", "\xf0\x80\x80\x80", "\xf4\x90\x80\x80", "\xff"};
  for (const auto &input : inputs) {
    Type expected, actual;
    EXPECT_EQ(reference(input.data(), input.size(), expected),
        ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN,
            input.data(), input.size(), actual));
    EXPECT_EQ(expected, actual);
  }
  Type type;
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObFTCharUtil::classify_first_char(
      CS_TYPE_UTF8MB4_BIN, nullptr, 1, type));
  EXPECT_EQ(OB_NOT_SUPPORTED, ObFTCharUtil::classify_first_char(
      CS_TYPE_LATIN1_BIN, "a", 1, type));
}

TEST(IKCharUtil, FullwidthClassificationPrecedence)
{
  Type type;
  EXPECT_EQ(OB_SUCCESS, ObFTCharUtil::classify_first_char(
      CS_TYPE_UTF8MB4_BIN, "\xef\xbc\xa1", 3, type));
  EXPECT_EQ(Type::ENGLISH_LETTER, type);
  EXPECT_EQ(OB_SUCCESS, ObFTCharUtil::classify_first_char(
      CS_TYPE_UTF8MB4_BIN, "\xef\xbc\x91", 3, type));
  EXPECT_EQ(Type::ARABIC_LETTER, type);
}
} // namespace storage
} // namespace oceanbase
