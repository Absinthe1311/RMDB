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

class BlockNestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段
    
    std::vector<Condition> fed_conds_;          // join条件
    bool isend;
    
    // 块嵌套循环连接相关成员
    std::vector<std::unique_ptr<RmRecord>> block_buffer_;  // 块缓冲区，存储右表的一个块
    size_t block_size_;                                     // 块大小（记录数）
    size_t current_block_index_;                            // 当前块内索引
    size_t processed_block_count_;                          // 已处理的块数量

   public:
    BlockNestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                                std::vector<Condition> conds, size_t block_size = 100000) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }
        
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);
        
        // 初始化块相关参数
        block_size_ = block_size;
        current_block_index_ = 0;
        processed_block_count_ = 0;
    }

    void beginTuple() override {
        left_->beginTuple();
        if (left_->is_end()) {
            isend = true;
            return;
        }
        
        right_->beginTuple();
        if (right_->is_end()) {
            isend = true;
            return;
        }
        
        // 加载第一个块
        load_next_block();
        current_block_index_ = 0;
        
        find_next_match();
    }

    void nextTuple() override {
        current_block_index_++;
        find_next_match();        
    }

    std::unique_ptr<RmRecord> Next() override {
        auto left_rec = left_->Next();
        auto right_rec = block_buffer_[current_block_index_].get();

        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, left_rec->data, left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
        return rec;
    }

    bool is_end() const override { return isend; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    size_t tupleLen() const override { return len_; }

    Rid &rid() override { return _abstract_rid; }

private:
    // 加载下一个块
    bool load_next_block() {
        block_buffer_.clear();
        
        // 跳过已处理的块
        if (processed_block_count_ > 0) {
            size_t skip_count = processed_block_count_ * block_size_;
            for (size_t i = 0; i < skip_count && !right_->is_end(); i++) {
                right_->nextTuple();
            }
        }
        
        // 加载当前块
        for (size_t i = 0; i < block_size_ && !right_->is_end(); i++) {
            auto rec = right_->Next();
            block_buffer_.push_back(std::move(rec));
            right_->nextTuple();
        }
        
        processed_block_count_++;
        return !block_buffer_.empty();
    }
    
    // 清空块缓冲区
    void clear_block_buffer() {
        block_buffer_.clear();
        block_buffer_.shrink_to_fit();
    }
    
    // 重置右表扫描，从头开始
    void reset_right_scan() {
        right_->beginTuple();
        processed_block_count_ = 0;
    }

    void find_next_match() {
        while (true) {
            // 当前块已处理完
            if (current_block_index_ >= block_buffer_.size()) {
                // 尝试加载下一个块
                bool has_more = load_next_block();
                
                if (!has_more) {
                    // 右表所有块处理完，处理左表下一条
                    left_->nextTuple();
                    if (left_->is_end()) {
                        isend = true;
                        return;
                    }
                    
                    // 重置右表扫描
                    reset_right_scan();
                    has_more = load_next_block();
                    
                    if (!has_more) {
                        // 右表为空
                        isend = true;
                        return;
                    }
                }
                
                current_block_index_ = 0;
                continue;
            }
            
            // 检查连接条件
            auto left_rec = left_->Next();
            auto right_rec = block_buffer_[current_block_index_].get();

            auto rec = std::make_unique<RmRecord>(len_);
            memcpy(rec->data, left_rec->data, left_->tupleLen());
            memcpy(rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());

            if (eval_conds(cols_, fed_conds_, rec.get())) {
                return;
            }
            current_block_index_++;
        }
    }
};
