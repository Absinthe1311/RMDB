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
    // std::unique_ptr<RmRecord> Next() override {
    //     for (auto &rid : rids_) {
    //         auto rec = fh_->get_record(rid, context_);

    //         // 应用SET子句，修改记录内容
    //         for (auto &set_clause : set_clauses_) {
    //             auto col = tab_.get_col(set_clause.lhs.col_name);
    //             memcpy(rec->data + col->offset, set_clause.rhs.raw->data, col->len);
    //         }

    //         // 写回记录文件
    //         fh_->update_record(rid, rec->data, context_);
    //     }
    //     return nullptr;
    // }

    std::unique_ptr<RmRecord> Next() override {
        for (auto &rid : rids_) {
            auto rec = fh_->get_record(rid, context_);

            // 应用SET子句，修改记录内容
            for (auto &set_clause : set_clauses_) {
                auto col = tab_.get_col(set_clause.lhs.col_name);
                Value &val = set_clause.rhs;

                // 类型检查与转换：仅允许int字面量赋给bigint列这一种宽度提升
                if (col->type != val.type) {
                    if (col->type == TYPE_BIGINT && val.type == TYPE_INT) {
                        val.set_bigint(static_cast<int64_t>(val.int_val));
                    }else if(col->type == TYPE_DATETIME && val.type == TYPE_STRING){
                        val.set_datetime(encode_datetime(val.str_val));
                    } else {
                        throw IncompatibleTypeError(coltype2str(col->type), coltype2str(val.type));
                    }
                    // 类型发生变化后，旧的raw是按旧类型/旧长度生成的，必须作废重新生成
                    val.raw = nullptr;
                }

                if (val.raw == nullptr) {
                    val.init_raw(col->len);
                }

                memcpy(rec->data + col->offset, val.raw->data, col->len);
            }

            // 写回记录文件
            fh_->update_record(rid, rec->data, context_);
        }
        return nullptr;
    }
    Rid &rid() override { return _abstract_rid; }
};