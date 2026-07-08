class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<ColMeta> cols_;
    size_t len_;
    std::vector<Condition> fed_conds_;

    std::vector<std::string> index_col_names_;
    IndexMeta index_meta_;

    Rid rid_;
    std::unique_ptr<RecScan> scan_;
    bool has_current_ = false;   // 标记当前是否有有效记录

    SmManager *sm_manager_;

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name,
                      std::vector<Condition> conds,
                      std::vector<std::string> index_col_names,
                      Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        index_col_names_ = index_col_names;
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;

        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT},
            {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };
        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    void beginTuple() override {
        // 构建扫描范围（与之前相同，但略去调试输出）
        std::string index_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols);
        auto ih = sm_manager_->ihs_.at(index_name).get();

        int col_tot_len = index_meta_.col_tot_len;
        char *lower_key = new char[col_tot_len];
        char *upper_key = new char[col_tot_len];
        memset(lower_key, 0, col_tot_len);
        memset(upper_key, 0xFF, col_tot_len);

        enum BoundType { NONE, INCLUSIVE, EXCLUSIVE };
        BoundType lower_type = NONE;
        BoundType upper_type = NONE;
        bool can_use_boundary = true;

        int offset = 0;
        for (auto &index_col : index_meta_.cols) {
            if (!can_use_boundary) break;  // 已遇到范围条件，后续列不参与索引扫描
            bool found_col = false;
            for (auto &cond : fed_conds_) {
                if (cond.lhs_col.col_name != index_col.name || !cond.is_rhs_val) continue;
                char *val_ptr = cond.rhs_val.raw->data;
                found_col = true;
                switch (cond.op) {
                    case OP_EQ:
                        if (can_use_boundary) {
                            memcpy(lower_key + offset, val_ptr, index_col.len);
                            memcpy(upper_key + offset, val_ptr, index_col.len);
                            lower_type = INCLUSIVE; upper_type = INCLUSIVE;
                        }
                        break;
                    case OP_GT:
                        if (can_use_boundary) {
                            memcpy(lower_key + offset, val_ptr, index_col.len);
                            lower_type = EXCLUSIVE; can_use_boundary = false;
                        }
                        break;
                    case OP_GE:
                        if (can_use_boundary) {
                            memcpy(lower_key + offset, val_ptr, index_col.len);
                            lower_type = INCLUSIVE; can_use_boundary = false;
                        }
                        break;
                    case OP_LT:
                        if (can_use_boundary) {
                            memcpy(upper_key + offset, val_ptr, index_col.len);
                            upper_type = EXCLUSIVE; can_use_boundary = false;
                        }
                        break;
                    case OP_LE:
                        if (can_use_boundary) {
                            memcpy(upper_key + offset, val_ptr, index_col.len);
                            upper_type = INCLUSIVE; can_use_boundary = false;
                        }
                        break;
                    default: break;
                }
                break;  // 找到该列的条件后立即跳出
            }
            if (!found_col) break;  // 该列没有条件，最左匹配中断
            offset += index_col.len;
        }

        Iid lower_iid, upper_iid;
        if (lower_type == NONE) lower_iid = ih->leaf_begin();
        else if (lower_type == INCLUSIVE) lower_iid = ih->lower_bound(lower_key);
        else lower_iid = ih->upper_bound(lower_key);

        if (upper_type == NONE) upper_iid = ih->leaf_end();
        else if (upper_type == INCLUSIVE) upper_iid = ih->upper_bound(upper_key);
        else upper_iid = ih->lower_bound(upper_key);

        scan_ = std::make_unique<IxScan>(ih, lower_iid, upper_iid, sm_manager_->get_bpm());
        delete[] lower_key;
        delete[] upper_key;

        // 定位到第一条满足条件的记录，但不消耗扫描位置（不调用 scan_->next()）
        has_current_ = false;
        while (!scan_->is_end()) {
            rid_ = scan_->rid();   // 获取当前位置的 Rid
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(cols_, fed_conds_, rec.get())) {
                has_current_ = true;
                return;            // 找到，保持 scan_ 不动
            }
            scan_->next();         // 不满足条件，移动到下一个
        }
        // 未找到有效记录，has_current_ 保持 false
    }

    void nextTuple() override {
        if (!has_current_) return;
        // 移动到下一个位置
        scan_->next();
        has_current_ = false;
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(cols_, fed_conds_, rec.get())) {
                has_current_ = true;
                return;
            }
            scan_->next();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (!has_current_) return nullptr;
        return fh_->get_record(rid_, context_);
    }

    bool is_end() const override {
        return !has_current_;
    }

    Rid &rid() override { return rid_; }
};