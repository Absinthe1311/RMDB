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
#include "execution/executor_abstract.h"
#include "parser/ast.h"
#include "system/sm.h"
#include <iomanip>
#include <sstream>
#include <cfloat>

class AggregationExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<std::shared_ptr<ast::AggExpr>> agg_exprs_;
    std::vector<ColMeta> cols_;
    bool is_end_;
    std::vector<Value> agg_results_;
    Context *context_;
    SmManager *sm_manager_;

public:
    AggregationExecutor(std::unique_ptr<AbstractExecutor> prev,
                        std::vector<std::shared_ptr<ast::AggExpr>> agg_exprs,
                        Context *context,
                        SmManager *sm_manager)
        : prev_(std::move(prev)), agg_exprs_(std::move(agg_exprs)), is_end_(false), 
          context_(context), sm_manager_(sm_manager) {
        
        for (auto &agg : agg_exprs_) {
            ColMeta col;
            col.name = agg->alias;
            col.tab_name = "";
            
            if (agg->is_count_star || agg->agg_type == ast::AGG_COUNT) {
                col.type = TYPE_INT;
                col.len = sizeof(int);
            } else {
                TabMeta &tab = sm_manager_->db_.get_table(agg->tab_name);
                auto col_meta = tab.get_col(agg->col_name);
                col.type = col_meta->type;
                col.len = col_meta->len;
            }
            
            cols_.push_back(col);
        }
    }

    size_t tupleLen() const override { return 0; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    std::string getType() override { return "AggregationExecutor"; }

    void beginTuple() override {
        if (agg_exprs_.empty()) {
            is_end_ = true;
            return;
        }

        std::vector<Value> results;
        for (size_t i = 0; i < agg_exprs_.size(); i++) {
            Value val;
            auto &agg = agg_exprs_[i];
            
            if (agg->agg_type == ast::AGG_COUNT) {
                val.set_int(0);
            } else if (agg->agg_type == ast::AGG_SUM) {
                if (cols_[i].type == TYPE_INT) {
                    val.set_int(0);
                } else if (cols_[i].type == TYPE_FLOAT) {
                    val.set_float(0.0f);
                } else if (cols_[i].type == TYPE_BIGINT) {
                    val.set_bigint(0);
                }
            } else if (agg->agg_type == ast::AGG_MAX) {
                val.type = cols_[i].type;
                val.init_raw(cols_[i].len);
                memset(val.raw->data, 0, cols_[i].len);
                if (cols_[i].type == TYPE_INT) {
                    val.set_int(INT32_MIN);
                } else if (cols_[i].type == TYPE_FLOAT) {
                    val.set_float(-FLT_MAX);
                } else if (cols_[i].type == TYPE_BIGINT) {
                    val.set_bigint(INT64_MIN);
                } else if (cols_[i].type == TYPE_STRING) {
                    // 对于字符串，初始化为全0，这样第一个字符串就会替代它
                }
            } else if (agg->agg_type == ast::AGG_MIN) {
                val.type = cols_[i].type;
                val.init_raw(cols_[i].len);
                memset(val.raw->data, 0, cols_[i].len);
                if (cols_[i].type == TYPE_INT) {
                    val.set_int(INT32_MAX);
                } else if (cols_[i].type == TYPE_FLOAT) {
                    val.set_float(FLT_MAX);
                } else if (cols_[i].type == TYPE_BIGINT) {
                    val.set_bigint(INT64_MAX);
                } else if (cols_[i].type == TYPE_STRING) {
                    // 对于字符串，初始化为全255（最大值），这样第一个字符串就会替代它
                    memset(val.raw->data, 255, cols_[i].len);
                }
            }
            results.push_back(val);
        }

        bool has_record = false;
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            auto rec = prev_->Next();
            has_record = true;
            
            for (size_t i = 0; i < agg_exprs_.size(); i++) {
                auto &agg = agg_exprs_[i];
                char *col_data = nullptr;
                
                if (!agg->is_count_star) {
                    auto &prev_cols = prev_->cols();
                    TabCol target = {.tab_name = agg->tab_name, .col_name = agg->col_name};
                    auto col_it = std::find_if(prev_cols.begin(), prev_cols.end(),
                        [&](const ColMeta &col) {
                            return col.tab_name == target.tab_name && col.name == target.col_name;
                        });
                    col_data = rec->data + col_it->offset;
                }
                
                if (agg->agg_type == ast::AGG_COUNT) {
                    results[i].set_int(results[i].int_val + 1);
                } else if (agg->agg_type == ast::AGG_SUM) {
                    if (cols_[i].type == TYPE_INT) {
                        results[i].set_int(results[i].int_val + *(int *)col_data);
                    } else if (cols_[i].type == TYPE_FLOAT) {
                        results[i].set_float(results[i].float_val + *(float *)col_data);
                    } else if (cols_[i].type == TYPE_BIGINT) {
                        results[i].set_bigint(results[i].bigint_val + *(int64_t *)col_data);
                    }
                } else if (agg->agg_type == ast::AGG_MAX) {
                    if (cols_[i].type == TYPE_INT) {
                        int cur_val = *(int *)col_data;
                        if (cur_val > results[i].int_val) {
                            results[i].set_int(cur_val);
                        }
                    } else if (cols_[i].type == TYPE_FLOAT) {
                        float cur_val = *(float *)col_data;
                        if (cur_val > results[i].float_val) {
                            results[i].set_float(cur_val);
                        }
                    } else if (cols_[i].type == TYPE_BIGINT) {
                        int64_t cur_val = *(int64_t *)col_data;
                        if (cur_val > results[i].bigint_val) {
                            results[i].set_bigint(cur_val);
                        }
                    } else if (cols_[i].type == TYPE_STRING) {
                        // 字符串比较
                        int cmp = memcmp(col_data, results[i].raw->data, cols_[i].len);
                        if (cmp > 0) {
                            // 当前字符串更大，更新结果
                            memcpy(results[i].raw->data, col_data, cols_[i].len);
                            results[i].set_str(std::string(col_data, cols_[i].len));
                        }
                    }
                } else if (agg->agg_type == ast::AGG_MIN) {
                    if (cols_[i].type == TYPE_INT) {
                        int cur_val = *(int *)col_data;
                        if (cur_val < results[i].int_val) {
                            results[i].set_int(cur_val);
                        }
                    } else if (cols_[i].type == TYPE_FLOAT) {
                        float cur_val = *(float *)col_data;
                        if (cur_val < results[i].float_val) {
                            results[i].set_float(cur_val);
                        }
                    } else if (cols_[i].type == TYPE_BIGINT) {
                        int64_t cur_val = *(int64_t *)col_data;
                        if (cur_val < results[i].bigint_val) {
                            results[i].set_bigint(cur_val);
                        }
                    } else if (cols_[i].type == TYPE_STRING) {
                        // 字符串比较
                        int cmp = memcmp(col_data, results[i].raw->data, cols_[i].len);
                        if (cmp < 0) {
                            // 当前字符串更小，更新结果
                            memcpy(results[i].raw->data, col_data, cols_[i].len);
                            results[i].set_str(std::string(col_data, cols_[i].len));
                        }
                    }
                }
            }
        }

        agg_results_ = results;
        is_end_ = false;
    }

    void nextTuple() override {
        is_end_ = true;
    }

    bool is_end() const override {
        return is_end_;
    }

    Rid &rid() override {
        return _abstract_rid;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_) {
            return nullptr;
        }
        
        auto rec = std::make_unique<RmRecord>(0);
        return rec;
    }

    const std::vector<Value> &get_agg_results() const {
        return agg_results_;
    }
};
