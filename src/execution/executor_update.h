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
#include "common/datetime_util.h"

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }

    std::unique_ptr<RmRecord> Next() override {
        // 简化方案：先尝试插入新索引检测冲突，如果成功再删除旧索引并更新记录
        std::vector<std::tuple<std::string, std::vector<char>, Rid>> old_keys_to_delete;
        std::vector<std::tuple<std::string, std::vector<char>, Rid>> new_keys_to_insert;
        std::vector<std::pair<Rid, std::vector<char>>> old_records;
        
        // 阶段1：预检查 - 尝试插入所有新索引，检测唯一性冲突
        
        // 先计算所有新索引key
        for (auto &rid : rids_) {
            auto rec = fh_->get_record(rid, context_);
            
            // 保存旧记录
            std::vector<char> old_rec_data(rec->data, rec->data + rec->size);
            old_records.push_back({rid, old_rec_data});
            
            // 计算新记录值
            auto new_rec = std::make_unique<RmRecord>(rec->size);
            memcpy(new_rec->data, rec->data, rec->size);
            
            for (auto &set_clause : set_clauses_) {
                auto col = tab_.get_col(set_clause.lhs.col_name);
                Value &val = set_clause.rhs;
                
                if (col->type != val.type) {
                    if (col->type == TYPE_BIGINT && val.type == TYPE_INT) {
                        val.set_bigint(static_cast<int64_t>(val.int_val));
                    } else if(col->type == TYPE_DATETIME && val.type == TYPE_STRING){
                        val.set_datetime(encode_datetime(val.str_val));
                    } else {
                        throw IncompatibleTypeError(coltype2str(col->type), coltype2str(val.type));
                    }
                    val.raw = nullptr;
                }
                
                if (val.raw == nullptr) {
                    val.init_raw(col->len);
                }
                
                memcpy(new_rec->data + col->offset, val.raw->data, col->len);
            }
            
            // 保存旧索引key和新索引key
            for (auto &index : tab_.indexes) {
                std::string index_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
                
                // 旧key
                std::vector<char> old_key(index.col_tot_len);
                int offset = 0;
                for (auto &col : index.cols) {
                    memcpy(old_key.data() + offset, rec->data + col.offset, col.len);
                    offset += col.len;
                }
                
                // 新key
                std::vector<char> new_key(index.col_tot_len);
                offset = 0;
                for (auto &col : index.cols) {
                    memcpy(new_key.data() + offset, new_rec->data + col.offset, col.len);
                    offset += col.len;
                }
                
                // 如果索引key没有变化，跳过该索引的更新
                bool key_changed = false;
                for (size_t i = 0; i < old_key.size(); i++) {
                    if (old_key[i] != new_key[i]) {
                        key_changed = true;
                        break;
                    }
                }
                
                if (key_changed) {
                    // 只有key发生变化时才需要更新索引
                    old_keys_to_delete.push_back({index_name, old_key, rid});
                    new_keys_to_insert.push_back({index_name, new_key, rid});
                }
            }
        }
        
        // 尝试插入所有新索引（检测唯一性）
        // 如果冲突，insert_entry会抛出IndexExistsError
        try {
            for (auto &[index_name, key, rid] : new_keys_to_insert) {
                auto ih = sm_manager_->ihs_.at(index_name).get();
                ih->insert_entry(key.data(), rid, context_->txn_);
            }
        } catch (const IndexExistsError &e) {
            // 阶段1失败，回滚已插入的新索引
            for (auto &[index_name, key, rid] : new_keys_to_insert) {
                try {
                    auto ih = sm_manager_->ihs_.at(index_name).get();
                    ih->delete_entry(key.data(), context_->txn_);
                } catch (...) {
                    // 忽略删除失败（key可能不存在）
                }
            }
            throw RMDBError("Unique index constraint violation");
        } catch (...) {
            // 其他异常，也需要回滚
            for (auto &[index_name, key, rid] : new_keys_to_insert) {
                try {
                    auto ih = sm_manager_->ihs_.at(index_name).get();
                    ih->delete_entry(key.data(), context_->txn_);
                } catch (...) {
                    // 忽略删除失败
                }
            }
            throw;
        }
        
        // 阶段2：执行更新（新索引已插入，现在删除旧索引并更新记录）
        
        try {
            // 删除所有旧索引
            for (auto &[index_name, key, rid] : old_keys_to_delete) {
                auto ih = sm_manager_->ihs_.at(index_name).get();
                ih->delete_entry(key.data(), context_->txn_);
            }
            
            // 更新记录
            for (auto &rid : rids_) {
                auto rec = fh_->get_record(rid, context_);
                
                for (auto &set_clause : set_clauses_) {
                    auto col = tab_.get_col(set_clause.lhs.col_name);
                    Value &val = set_clause.rhs;
                    
                    if (col->type != val.type) {
                        if (col->type == TYPE_BIGINT && val.type == TYPE_INT) {
                            val.set_bigint(static_cast<int64_t>(val.int_val));
                        } else if(col->type == TYPE_DATETIME && val.type == TYPE_STRING){
                            val.set_datetime(encode_datetime(val.str_val));
                        }
                        val.raw = nullptr;
                    }
                    
                    if (val.raw == nullptr) {
                        val.init_raw(col->len);
                    }
                    
                    memcpy(rec->data + col->offset, val.raw->data, col->len);
                }
                
                fh_->update_record(rid, rec->data, context_);
            }
            
            
        } catch (const std::exception &e) {
            // 阶段3：回滚（删除已插入的新索引，恢复旧索引和记录）
            
            try {
                // 删除已插入的新索引
                for (auto &[index_name, key, rid] : new_keys_to_insert) {
                    auto ih = sm_manager_->ihs_.at(index_name).get();
                    ih->delete_entry(key.data(), context_->txn_);
                }
                
                // 恢复旧记录
                for (auto &[rid, old_data] : old_records) {
                    fh_->update_record(rid, old_data.data(), context_);
                }
                
                // 重新插入旧索引
                for (auto &[index_name, key, rid] : old_keys_to_delete) {
                    auto ih = sm_manager_->ihs_.at(index_name).get();
                    ih->insert_entry(key.data(), rid, context_->txn_);
                }
                
            } catch (...) {
                std::cerr << "Rollback failed!" << std::endl;
            }
            
            throw;
        }
        
        return nullptr;
    }
    Rid &rid() override { return _abstract_rid; }
};