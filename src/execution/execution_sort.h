/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> order_cols_;
    std::vector<bool> is_desc_;
    std::vector<std::unique_ptr<RmRecord>> records_;
    size_t current_pos_;

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, 
                 const std::vector<TabCol> &orderby_cols,
                 const std::vector<bool> &is_desc) {
        prev_ = std::move(prev);
        auto &prev_cols = prev_->cols();
        for (auto &col : orderby_cols) {
            auto pos = get_col(prev_cols, col);
            order_cols_.push_back(*pos);
        }
        is_desc_ = is_desc;
        current_pos_ = 0;
    }

    void beginTuple() override {
        records_.clear();
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            records_.push_back(prev_->Next());
        }

        std::stable_sort(records_.begin(), records_.end(),
            [this](const std::unique_ptr<RmRecord> &a, const std::unique_ptr<RmRecord> &b) {
                for (size_t i = 0; i < order_cols_.size(); i++) {
                    auto &col = order_cols_[i];
                    int cmp = compare(a->data + col.offset, b->data + col.offset, col.len, col.type);
                    if (cmp != 0) {
                        return is_desc_[i] ? (cmp > 0) : (cmp < 0);
                    }
                }
                return false;
            });

        current_pos_ = 0;
    }

    void nextTuple() override {
        current_pos_++;
    }

    std::unique_ptr<RmRecord> Next() override {
        auto rec = std::make_unique<RmRecord>(records_[current_pos_]->size);
        memcpy(rec->data, records_[current_pos_]->data, records_[current_pos_]->size);
        return rec;
    }

    bool is_end() const override {
        return current_pos_ >= records_.size();
    }

    const std::vector<ColMeta> &cols() const override {
        return prev_->cols();
    }

    size_t tupleLen() const override {
        return prev_->tupleLen();
    }

    Rid &rid() override { return _abstract_rid; }
};
