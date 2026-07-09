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

class LimitExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    int limit_count_;
    int current_count_;

   public:
    LimitExecutor(std::unique_ptr<AbstractExecutor> prev, int limit_count) {
        prev_ = std::move(prev);
        limit_count_ = limit_count;
        current_count_ = 0;
    }

    void beginTuple() override {
        prev_->beginTuple();
        current_count_ = 0;
    }

    void nextTuple() override {
        prev_->nextTuple();
        current_count_++;
    }

    std::unique_ptr<RmRecord> Next() override {
        return prev_->Next();
    }

    bool is_end() const override {
        return prev_->is_end() || current_count_ >= limit_count_;
    }

    const std::vector<ColMeta> &cols() const override {
        return prev_->cols();
    }

    size_t tupleLen() const override {
        return prev_->tupleLen();
    }

    Rid &rid() override { return _abstract_rid; }
};
