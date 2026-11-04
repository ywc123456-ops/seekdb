"""Compile isolated production IK headers with a minimal platform shim.

Uses the real UTF-8 decoder, Unicode predicates and complete old/new classifier
headers. Charset lookup/error/logging are shimmed; this cannot validate server
integration, TOKENIZE/MATCH, DDL or performance scoring. Use the native Bazel
test and the official FTS benchmark for those. No official tests are modified.
"""
from pathlib import Path
import argparse, re, subprocess, tempfile
parser = argparse.ArgumentParser(description='Isolated old/new IK classifier check; not a database benchmark')
parser.add_argument('--baseline', required=True, help='Unmodified commit to compare against')
parser.add_argument('--cxx', default='c++', help='C++ compiler executable')
parser.add_argument('--zig', action='store_true', help='Invoke the compiler with the Zig c++ subcommand')
args = parser.parse_args()
r=Path(__file__).resolve().parents[3]
tmp = tempfile.TemporaryDirectory(prefix='ik-component-')
o=Path(tmp.name)
def strip(s): return re.sub(r'^#include.*$', '', s, flags=re.M)
shim=r'''
#pragma once
#include <cstdint>
#include <cstring>
#include <type_traits>
using ob_wc_t = unsigned long;
using uint16 = uint16_t;
using uint32 = uint32_t;
#define ALWAYS_INLINE inline
#define OB_SUCCESS 0
#define OB_INVALID_ARGUMENT -1
#define OB_UNEXPECT_INTERNAL_ERROR -2
#define OB_NOT_SUPPORTED -3
#define OB_CS_TOOSMALL -101
#define OB_CS_TOOSMALL2 -102
#define OB_CS_TOOSMALL3 -103
#define OB_CS_TOOSMALL4 -104
#define OB_CS_TOOSMALLN(x) (-100-(x))
#define OB_CS_ILSEQ -105
#define OB_CS_ILUNI -106
#define OB_FAIL(x) ((ret=(x)) != OB_SUCCESS)
#define OB_ISNULL(x) ((x)==nullptr)
#define OB_LIKELY(x) (x)
#define OB_UNLIKELY(x) (x)
#define UNUSED(x) (void)(x)
#define STORAGE_FTS_LOG(...)
#define ARRAYSIZEOF(x) (sizeof(x)/sizeof(x[0]))
namespace oceanbase { namespace common {
enum ObCharsetType { CHARSET_UTF8MB4, CHARSET_UTF16, CHARSET_UTF16LE, CHARSET_BINARY, CHARSET_LATIN1 };
using ObCollationType = ObCharsetType;
struct ObCharset { static ObCharsetType charset_type_by_coll(ObCollationType c) { return c; } };
}}
using namespace oceanbase::common;
'''
mb=(r/'src/oblib/lib/charset/mb_wc.h').read_text(encoding='utf-8')
start=mb.index('template <bool RANGE_CHECK, bool SUPPORT_MB4>\nstatic ALWAYS_INLINE')
end=mb.index('extern "C"',start)
sh=(r/'src/oblib/lib/charset/ob_charset_string_helper.h').read_text(encoding='utf-8')
a=sh.index('template<ObCharsetType cs_type>')
b=sh.index('class ObFastStringScanner')
shim+=mb[start:end]+'\nnamespace oceanbase { namespace common {\n'+sh[a:b]+'\n}}\n'
shim+=strip((r/'src/storage/fts/utils/unicode_utils.h').read_text(encoding='utf-8'))
(o/'shim.h').write_text(shim, encoding='utf-8')
header=strip((r/'src/storage/fts/ik/ob_ik_char_util.h').read_text(encoding='utf-8'))
old=subprocess.check_output(['git','show',args.baseline + ':src/storage/fts/ik/ob_ik_char_util.h'],cwd=r,text=True,encoding='utf-8')
old=strip(old).replace('_OCEANBASE_STORAGE_FTS_IK_OB_IK_CHAR_UTIL_H_','LEGACY_CHAR_UTIL_H').replace('ObFTCharUtil','LegacyCharUtil')
(o/'new.h').write_text('#include "shim.h"\n'+header, encoding='utf-8')
(o/'old.h').write_text('#include "shim.h"\n'+old, encoding='utf-8')
(o/'unicode.cpp').write_text('#include "shim.h"\n'+strip((r/'src/storage/fts/utils/unicode_utils.cpp').read_text(encoding='utf-8')), encoding='utf-8')
def function(source, signature):
    start = source.index(signature)
    begin = source.index('{', start)
    depth = 1
    end = begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
stream_path = 'src/share/text_analysis/ob_token_stream.cpp'
new_stream = (r/stream_path).read_text(encoding='utf-8')
old_stream = subprocess.check_output(['git', 'show', args.baseline + ':' + stream_path], cwd=r, text=True, encoding='utf-8')
signature = 'int ObTokenStopWordNormalizer::filter_special_marks('
filters = r"""
struct ObDatum { const char *ptr_; unsigned len_; };
struct Table { unsigned char flags[256]; };
bool ob_ispunct(const Table *cs, char c) { return cs->flags[(unsigned char)c] & 1; }
bool ob_iscntrl(const Table *cs, char c) { return cs->flags[(unsigned char)c] & 2; }
struct NewFilter { Table *cs_; int filter_special_marks(const ObDatum &, bool &); };
struct OldFilter { Table *cs_; int filter_special_marks(const ObDatum &, bool &); };
"""
filters += function(new_stream, signature).replace('ObTokenStopWordNormalizer', 'NewFilter')
filters += function(old_stream, signature).replace('ObTokenStopWordNormalizer', 'OldFilter')
main=r'''
#include "new.h"
#include "old.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
using namespace oceanbase::storage;
static unsigned long long checks=0;
void check(const char *p, uint8_t n, ObCharsetType cs) {
  ObFTCharUtil::CharType a = ObFTCharUtil::CharType::USELESS;
  LegacyCharUtil::CharType b = LegacyCharUtil::CharType::USELESS;
  int x=ObFTCharUtil::classify_first_char(cs,p,n,a);
  int y=LegacyCharUtil::classify_first_char(cs,p,n,b);
  if(x!=y || int(a)!=int(b)) {std::printf("Mismatch n=%u cs=%d\n",n,int(cs));std::fflush(stdout);std::abort();}
  ++checks;
}
int main() {
  Table table{};
  NewFilter nf{&table}; OldFilter of{&table};
  bool valid_new, valid_old;
  char pair[2];
  for (int trial=0; trial<4; ++trial) {
    for(int c=0;c<256;++c) table.flags[c]=(c+trial)%4;
    for(int first=0;first<256;++first) for(int last=0;last<256;++last) {
      pair[0]=char(first);pair[1]=char(last);
      ObDatum datum{pair,2};
      if(nf.filter_special_marks(datum,valid_new)!=of.filter_special_marks(datum,valid_old)
          ||valid_new!=valid_old) {std::printf("Filter mismatch\n");std::fflush(stdout);std::abort();}
    }
  }
  ObDatum empty{nullptr,0};
  nf.filter_special_marks(empty,valid_new);of.filter_special_marks(empty,valid_old);
  if(valid_new!=valid_old) {std::printf("Empty filter mismatch\n");std::fflush(stdout);std::abort();}
  std::printf("PASS: 262145 old/new token-filter comparisons (synthetic charset tables)\n");
  char b[4];
  for(ob_wc_t wc=0;wc<=0x10ffff;++wc) {
    if(wc>=0xd800 && wc<=0xdfff) continue;
    int n=ob_charset_encode_unicode<CHARSET_UTF8MB4>(wc,(unsigned char*)b,(unsigned char*)b+4);
    for(int len=0;len<=n;++len) check(b,len,CHARSET_UTF8MB4);
  }
  std::mt19937 rng(4916);
  for(int i=0;i<200000;++i) {
    for(auto &c:b)c=char(rng());
    for(auto cs:{CHARSET_UTF8MB4,CHARSET_UTF16,CHARSET_UTF16LE,CHARSET_LATIN1})
      check(b,rng()%5,cs);
  }
  check(nullptr,1,CHARSET_UTF8MB4);
  std::printf("PASS: %llu old/new return-code and type comparisons\n",checks);
  std::vector<unsigned long> points={'A','7',0x4e2d,0x6587,0xff21,0xff11,0x3002,0x3042,0xac00,0x1f600};
  std::vector<std::vector<char>> inputs;
  for(auto wc:points) {int n=ob_charset_encode_unicode<CHARSET_UTF8MB4>(wc,(unsigned char*)b,(unsigned char*)b+4);inputs.emplace_back(b,b+n);}
  volatile unsigned long sum=0;
  for(int trial=0;trial<5;++trial) {
    for(int version=0;version<2;++version) {
      auto start=std::chrono::steady_clock::now();
      for(int i=0;i<1000000;++i)for(const auto &s:inputs) {
        if(version==0) {LegacyCharUtil::CharType t;LegacyCharUtil::classify_first_char(CHARSET_UTF8MB4,s.data(),s.size(),t);sum+=int(t);}
        else {ObFTCharUtil::CharType t;ObFTCharUtil::classify_first_char(CHARSET_UTF8MB4,s.data(),s.size(),t);sum+=int(t);}
      }
      auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
      std::printf("sample=%d version=%s classification_ms=%.3f\n",trial,version?"optimized":"baseline",ms);
    }
  }
  std::printf("checksum=%lu\n",sum);
}
'''
main = main.replace('int main() {', filters + '\nint main() {')
(o/'main.cpp').write_text(main, encoding='utf-8')
print('Generated component differential harness from actual old/new headers and production UTF-8 decoder/predicates.')


subprocess.run([args.cxx] + (['c++'] if args.zig else []) + ['-std=c++17', '-O2', str(o/'main.cpp'), str(o/'unicode.cpp'), '-o', str(o/'check')], check=True)
subprocess.run([str(o/'check')], check=True)
