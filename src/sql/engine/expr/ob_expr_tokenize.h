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

#ifndef _OCEANBASE_SQL_ENGINE_EXPR_OB_EXPR_TOKENIZE_H_
#define _OCEANBASE_SQL_ENGINE_EXPR_OB_EXPR_TOKENIZE_H_

#include "lib/allocator/ob_allocator.h"
#include "lib/allocator/page_arena.h"
#include "lib/string/ob_string.h"
#include "sql/engine/expr/ob_expr_operator.h"
#include "data_plane/fts/ob_fts_parser_property.h"

namespace oceanbase
{
namespace sql
{
class MultimodeAlloctor;
class TokenizeCache;
class ObExprTokenize : public ObStringExprOperator
{
public:
  explicit ObExprTokenize(common::ObIAllocator &alloc);
  ~ObExprTokenize() override;
  /**
   * @brief evaluate function
   * @param expr expression
   * @param ctx expression evaluation context
   * @param expr_datum expression result
   * @note see cg_expr REG_OP and g_expr_eval_functions
   */
  static int eval_tokenize(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum);

  int calc_result_typeN(ObExprResType &type,
                        ObExprResType *types,
                        int64_t param_num,
                        common::ObExprTypeCtx &type_ctx) const override;
  int cg_expr(ObExprCGCtx &op_cg_ctx, const ObRawExpr &raw_expr, ObExpr &rt_expr) const override;

private:
  struct TokenizeParam
  {
  public:
    static constexpr const char *CASE_INDICATOR_STR = "case";
    static constexpr const char *OUTPUT_MODE_STR = "output";
    static constexpr const char *STOPWORDS_LIST_STR = "stopwords";
    static constexpr const char *ADDITIONAL_ARGS_STR = "additional_args";

  public:
    TokenizeParam();

    int parse_json_param(const ObIJsonBase *obj);

    // check and reform parser properties to standard format
    int reform_parser_properties(const ObString &properties, ObString &formatted);
    int try_load_dictionary_for_ik();

  public:
    // for property and tmp json string
    mutable ObArenaAllocator allocator_;
    // the inner parser name, e.g. "ik.1"
    ObString parser_name_;
    // the parser name given by the user, e.g. "ik"
    ObString raw_parser_name_;
    // the parser properties given by the user
    ObString raw_properties_;
    // the parser properties in standard format
    ObString properties_;
    ObObjMeta meta_;
    ObString fulltext_;
    enum OUTPUT_MODE
    {
      DEFAULT,
      ALL,
    } output_mode_;
  };

private:
  static int parse_raw_param(const ObExpr &expr,
                             ObEvalCtx &ctx,
                             TokenizeParam &param);

  static int prepare_param(TokenizeParam &param, TokenizeCache &cache);

  static int tokenize_and_pack(const ObExpr &expr,
                               ObEvalCtx &ctx,
                               common::ObArenaAllocator &allocator,
                               TokenizeParam &param,
                               ObDatum &expr_datum);

  static int parse_fulltext(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param);
  static int parse_raw_parser_name(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param);
  static int parse_parser_properties(const ObExpr &expr,
                                     ObEvalCtx &ctx,
                                     TokenizeParam &param);

  static int tokenize_fulltext(const TokenizeParam &param,
                               TokenizeParam::OUTPUT_MODE mode,
                               common::ObIAllocator &allocator,
                               ObIJsonBase *&result);

  static int construct_ft_parser_inner_name(const ObString &input_str, TokenizeParam &param);

private:
  DISALLOW_COPY_AND_ASSIGN(ObExprTokenize);
};

} // namespace sql
} // namespace oceanbase

#endif // _OCEANBASE_SQL_ENGINE_EXPR_OB_EXPR_TOKENIZE_H_
