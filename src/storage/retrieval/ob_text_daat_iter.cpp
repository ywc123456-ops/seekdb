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

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (nullptr != param.base_param_->relevance_expr_
             && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
    fast_membership_ = nullptr == param.base_param_->relevance_expr_
        && nullptr == param.base_param_->filter_expr_
        && sql::NATURAL_LANGUAGE_MODE == mode_flag_ && !function_lookup_mode_
        && param.minimum_should_match_ >= 0 && param.minimum_should_match_ <= 1
        && dim_iters_->count() > 0 && dim_iters_->count() <= 4;
    if (fast_membership_) {
      const auto &meta = param.base_param_->id_proj_expr_->datum_meta_;
      membership_cmp_ = ObDatumFuncs::get_basic_func(meta.type_, meta.cs_type_)->null_first_cmp_;
      if (ObVarcharType == meta.type_ && CS_TYPE_BINARY == meta.cs_type_) {
        membership_cmp_ = compare_binary_retrieval_ids;
      }
    }
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  for (bool &ended : membership_ended_) ended = false;
  fast_membership_ = false;
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  for (bool &ended : membership_ended_) ended = false;
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (nullptr != iter_param_->relevance_expr_) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

int ObTextDaaTIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  if (!fast_membership_ || !iter_param_->eval_ctx_->is_vectorized() || capacity <= 0) {
    return ObSRDaaTIterImpl::get_next_rows(capacity, count);
  }
  int ret = OB_SUCCESS;
  count = 0;
  const int64_t cap = MIN(capacity, iter_param_->max_batch_size_);
  const int64_t dims = dim_iters_->count();
  const ObLimitParam &limit = *iter_param_->limit_param_;
  ObTextRetrievalDaaTTokenIter *streams[4] = {};
  for (int i = 0; i < dims; ++i) {
    streams[i] = static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i));
  }
  if (limit.is_valid() && output_row_cnt_ >= limit.limit_) {
    return OB_ITER_END;
  }
  while (OB_SUCC(ret) && count < cap) {
    const ObDocIdExt *ids[4] = {};
    int64_t available[4] = {};
    int64_t consumed[4] = {};
    for (int i = 0; OB_SUCC(ret) && i < dims; ++i) {
      if (!membership_ended_[i]) {
        ret = streams[i]->peek_ids(ids[i], available[i]);
        if (ret == OB_ITER_END) { membership_ended_[i] = true; ret = OB_SUCCESS; }
      }
    }
    int64_t total_available = 0;
    for (int i = 0; i < dims; ++i) total_available += available[i];
    if (OB_FAIL(ret)) {
    } else if (total_available == 0) {
      ret = OB_ITER_END;
    } else {
      // Merge contiguous posting batches directly. Equal IDs are consumed in
      // both streams, preserving the same sorted union as the general heap.
      while (OB_SUCC(ret) && count < cap) {
        int winner = -1;
        unsigned equal_mask = 0;
        bool refill = false;
        for (int i = 0; OB_SUCC(ret) && i < dims; ++i) {
          if (consumed[i] == available[i]) {
            if (!membership_ended_[i]) { refill = true; break; }
          } else if (winner < 0) {
            winner = i;
            equal_mask = 1U << i;
          } else {
            int cmp = 0;
            const ObDatum &left = ids[i][consumed[i]].get_datum();
            const ObDatum &right = ids[winner][consumed[winner]].get_datum();
            // Keep the binary comparator visible to the optimizer instead of
            // taking an indirect call for every pair of posting IDs.
            ret = membership_cmp_ == compare_binary_retrieval_ids
                ? compare_binary_retrieval_ids(left, right, cmp, nullptr)
                : membership_cmp_(left, right, cmp, nullptr);
            if (OB_FAIL(ret)) {
            } else if (cmp < 0) {
              winner = i;
              equal_mask = 1U << i;
            } else if (cmp == 0) {
              equal_mask |= 1U << i;
            }
          }
        }
        if (OB_FAIL(ret) || refill || winner < 0) break;
        const ObDocIdExt &id = ids[winner][consumed[winner]];
        ++input_row_cnt_;
        if (!limit.is_valid() || input_row_cnt_ > limit.offset_) {
          buffered_domain_ids_.get_data()[count] = id;
          buffered_relevances_.get_data()[count] = 1.0;
          ++count;
          ++output_row_cnt_;
        }
        for (int i = 0; i < dims; ++i) if (equal_mask & (1U << i)) ++consumed[i];
        if (limit.is_valid() && output_row_cnt_ >= limit.limit_) ret = OB_ITER_END;
      }
      for (int i = 0; i < dims; ++i) if (consumed[i] > 0) streams[i]->consume_ids(consumed[i]);
    }
  }
  if ((OB_SUCCESS == ret || OB_ITER_END == ret) && count > 0) {
    const int project_ret = project_results(count);
    if (OB_SUCCESS != project_ret) ret = project_ret;
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_(), small_topk_candidate_(false) {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  small_topk_candidate_ = false;
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (nullptr != param.base_param_->relevance_expr_
             && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    small_topk_candidate_ = sql::NATURAL_LANGUAGE_MODE == param.mode_flag_
        && !param.function_lookup_mode_
        && nullptr == param.base_param_->dim_weights_
        && nullptr != param.base_param_->relevance_expr_
        && param.dim_iters_->count() > 0 && param.dim_iters_->count() <= 3;
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  // Small collections favor sequential batch scoring over per-document WAND
  // maintenance. Larger collections retain block-max skipping.
  if (OB_SUCC(ret) && small_topk_candidate_ && BMWStatus::MAX_STATUS == status_
      && dim_iters_->count() > 0 && dim_iters_->count() <= 3
      && iter_param_->eval_ctx_->is_vectorized()
      && bm25_param_estimator_.get_total_doc_cnt() <= 64 * 1024) {
    ret = materialize_small_topk();
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::materialize_small_topk()
{
  int ret = OB_SUCCESS;
  const int64_t dims = dim_iters_->count();
  const auto &meta = iter_param_->id_proj_expr_->datum_meta_;
  const bool binary_ids = ObVarcharType == meta.type_ && CS_TYPE_BINARY == meta.cs_type_;
  ObTextRetrievalBlockMaxIter *streams[3] = {};
  bool ended[3] = {};
  for (int64_t i = 0; i < dims; ++i) {
    streams[i] = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
  }
  while (OB_SUCC(ret) && top_k_count_ > 0) {
    const ObDocIdExt *ids[3] = {};
    const double *scores[3] = {};
    int64_t available[3] = {}, consumed[3] = {};
    int64_t total_available = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < dims; ++i) {
      if (!ended[i]) {
        ret = streams[i]->peek_scored_ids(ids[i], scores[i], available[i]);
        if (OB_ITER_END == ret) { ended[i] = true; ret = OB_SUCCESS; }
        total_available += available[i];
      }
    }
    if (OB_FAIL(ret) || total_available == 0) break;
    while (OB_SUCC(ret)) {
      int64_t winner = -1;
      unsigned equal_mask = 0;
      bool refill = false;
      for (int64_t i = 0; OB_SUCC(ret) && i < dims; ++i) {
        if (consumed[i] == available[i]) {
          if (!ended[i]) { refill = true; break; }
        } else if (winner < 0) {
          winner = i;
          equal_mask = 1U << i;
        } else {
          int cmp = 0;
          const ObDatum &left = ids[i][consumed[i]].get_datum();
          const ObDatum &right = ids[winner][consumed[winner]].get_datum();
          if (binary_ids && !left.is_ext() && !right.is_ext()) {
            ret = compare_binary_retrieval_ids(left, right, cmp, nullptr);
          } else {
            ret = domain_id_cmp_.compare(left, right, cmp);
          }
          if (OB_FAIL(ret)) {
          } else if (cmp < 0) {
            winner = i;
            equal_mask = 1U << i;
          } else if (cmp == 0) {
            equal_mask |= 1U << i;
          }
        }
      }
      if (OB_FAIL(ret) || refill || winner < 0) break;
      // The legacy <=3-way simple merger resolves equal IDs in ascending
      // iterator order. Preserve its exact floating-point addition order.
      for (int64_t i = 0; OB_SUCC(ret) && i < dims; ++i) {
        if (equal_mask & (1U << i)) {
          ret = relevance_collector_->collect_one_dim(i, scores[i][consumed[i]]);
        }
      }
      double relevance = 0;
      bool valid = false;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(relevance_collector_->get_result(relevance, valid))) {
      } else if (valid && OB_FAIL(process_collected_row(
                     ids[winner][consumed[winner]].get_datum(), relevance))) {
      }
      for (int64_t i = 0; i < dims; ++i) if (equal_mask & (1U << i)) ++consumed[i];
    }
    for (int64_t i = 0; i < dims; ++i) {
      if (consumed[i] > 0) streams[i]->consume_ids(consumed[i]);
    }
  }
  if (OB_SUCC(ret)) status_ = BMWStatus::FINISHED;
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
