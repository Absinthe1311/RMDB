/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "log_recovery.h"

#include <fcntl.h>
#include <unistd.h>

#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "record/rm_scan.h"

namespace {
std::vector<std::unique_ptr<LogRecord>> logs;
std::unordered_set<txn_id_t> finished_txns;
std::unordered_set<txn_id_t> committed_txns;
std::unordered_set<txn_id_t> active_txns;
std::unordered_set<std::string> touched_tables;

std::unique_ptr<LogRecord> parse_log_record(const char *data) {
    LogType type = *reinterpret_cast<const LogType *>(data + OFFSET_LOG_TYPE);
    std::unique_ptr<LogRecord> rec;
    if (type == LogType::begin) rec = std::make_unique<BeginLogRecord>();
    else if (type == LogType::commit) rec = std::make_unique<CommitLogRecord>();
    else if (type == LogType::ABORT) rec = std::make_unique<AbortLogRecord>();
    else rec = std::make_unique<RecordLogRecord>();
    rec->deserialize(data);
    return rec;
}

void apply_insert(SmManager *sm_manager, const RecordLogRecord &log) {
    std::cerr << "  Applying INSERT to table " << log.table_name_ << std::endl;
    
    // 检查索引数量
    if (sm_manager->db_.is_table(log.table_name_)) {
        auto &tab = sm_manager->db_.get_table(log.table_name_);
        std::cerr << "    Indexes BEFORE insert: " << tab.indexes.size() << std::endl;
    }
    
    auto fh = sm_manager->fhs_.at(log.table_name_).get();
    try {
        fh->insert_record(log.rid_, log.new_value_.data);
    } catch (RMDBError &) {
        fh->update_record(log.rid_, log.new_value_.data, nullptr);
    }
    
    // 再次检查索引数量
    if (sm_manager->db_.is_table(log.table_name_)) {
        auto &tab = sm_manager->db_.get_table(log.table_name_);
        std::cerr << "    Indexes AFTER insert: " << tab.indexes.size() << std::endl;
    }
}

void apply_delete(SmManager *sm_manager, const RecordLogRecord &log) {
    auto fh = sm_manager->fhs_.at(log.table_name_).get();
    try {
        fh->delete_record(log.rid_, nullptr);
    } catch (RMDBError &) {
    }
}

void apply_update(SmManager *sm_manager, const RecordLogRecord &log, bool use_new_value) {
    auto fh = sm_manager->fhs_.at(log.table_name_).get();
    auto &rec = use_new_value ? log.new_value_ : log.old_value_;
    try {
        fh->update_record(log.rid_, rec.data, nullptr);
    } catch (RMDBError &) {
        fh->insert_record(log.rid_, rec.data);
    }
}

std::vector<char> make_index_key(const IndexMeta &index, const RmRecord &rec) {
    std::vector<char> key(index.col_tot_len);
    int offset = 0;
    for (auto &col : index.cols) {
        memcpy(key.data() + offset, rec.data + col.offset, col.len);
        offset += col.len;
    }
    return key;
}

void rebuild_table_indexes(SmManager *sm_manager, const std::string &tab_name) {
    std::cerr << "\n========== REBUILD INDEXES for " << tab_name << " ==========" << std::endl;
    
    std::cerr << "Step 1: Check if table exists in metadata..." << std::endl;
    if (!sm_manager->db_.is_table(tab_name)) {
        std::cerr << "ERROR: Table not found in metadata!" << std::endl;
        return;
    }
    std::cerr << "Table exists." << std::endl;
    
    auto &tab = sm_manager->db_.get_table(tab_name);
    std::cerr << "Step 2: Check indexes count..." << std::endl;
    std::cerr << "Indexes in metadata: " << tab.indexes.size() << std::endl;
    
    // 关键修复：先保存索引信息，防止后续操作修改元数据
    std::vector<std::vector<ColMeta>> indexes_to_rebuild;
    for (auto &index : tab.indexes) {
        indexes_to_rebuild.push_back(index.cols);
    }
    
    std::cerr << "Saved " << indexes_to_rebuild.size() << " indexes to rebuild" << std::endl;
    
    if (indexes_to_rebuild.empty()) {
        std::cerr << "ERROR: No indexes to rebuild!" << std::endl;
        return;
    }
    
    auto fh = sm_manager->fhs_.at(tab_name).get();
    auto ix_manager = sm_manager->get_ix_manager();
    
    std::cerr << "Step 3: Rebuilding indexes..." << std::endl;
    for (auto &index_cols : indexes_to_rebuild) {
        std::string index_name = ix_manager->get_index_name(tab_name, index_cols);
        std::cerr << "Processing index: " << index_name << std::endl;
        
        auto ih_iter = sm_manager->ihs_.find(index_name);
        if (ih_iter != sm_manager->ihs_.end()) {
            ix_manager->close_index(ih_iter->second.get());
            sm_manager->ihs_.erase(ih_iter);
        }
        
        if (ix_manager->exists(tab_name, index_cols)) {
            ix_manager->destroy_index(tab_name, index_cols);
        }
        
        ix_manager->create_index(tab_name, index_cols);
        auto ih = ix_manager->open_index(tab_name, index_cols);
        
        std::cerr << "Step 4: Inserting index entries..." << std::endl;
        int count = 0;
        for (RmScan scan(fh); !scan.is_end(); scan.next()) {
            auto rec = fh->get_record(scan.rid(), nullptr);
            std::vector<char> key(index_cols[0].len);  // 简化：假设单列索引
            int offset = 0;
            for (auto &col : index_cols) {
                memcpy(key.data() + offset, rec->data + col.offset, col.len);
                offset += col.len;
            }
            ih->insert_entry(key.data(), scan.rid(), nullptr);
            count++;
        }
        std::cerr << "Inserted " << count << " index entries" << std::endl;
        
        sm_manager->ihs_.emplace(index_name, std::move(ih));
    }
    std::cerr << "========================================\n" << std::endl;
}
}

void RecoveryManager::analyze() {
    logs.clear();
    finished_txns.clear();
    committed_txns.clear();
    active_txns.clear();
    touched_tables.clear();
    
    std::cerr << "\n========== ANALYZE START ==========" << std::endl;
    
    if (!disk_manager_->is_file(LOG_FILE_NAME)) {
        std::cerr << "No log file found" << std::endl;
        std::cerr << "==================================\n" << std::endl;
        return;
    }

    int offset = 0;
    int log_count = 0;
    while (true) {
        char header[LOG_HEADER_SIZE];
        int n = disk_manager_->read_log(header, LOG_HEADER_SIZE, offset);
        if (n <= 0) break;
        if (n < LOG_HEADER_SIZE) break;
        uint32_t len = *reinterpret_cast<uint32_t *>(header + OFFSET_LOG_TOT_LEN);
        if (len < LOG_HEADER_SIZE || len > LOG_BUFFER_SIZE) break;
        std::vector<char> data(len);
        n = disk_manager_->read_log(data.data(), len, offset);
        if (n != static_cast<int>(len)) break;
        auto rec = parse_log_record(data.data());
        
        std::cerr << "Log " << log_count++ << ": type=" << LogTypeStr[rec->log_type_]
                  << " txn=" << rec->log_tid_ << " lsn=" << rec->lsn_ << std::endl;
        
        if (rec->log_type_ == LogType::begin) {
            active_txns.insert(rec->log_tid_);
        } else if (rec->log_type_ == LogType::commit || rec->log_type_ == LogType::ABORT) {
            if (rec->log_type_ == LogType::commit) {
                committed_txns.insert(rec->log_tid_);
            }
            finished_txns.insert(rec->log_tid_);
            active_txns.erase(rec->log_tid_);
        } else {
            active_txns.insert(rec->log_tid_);
            auto *record_log = dynamic_cast<RecordLogRecord *>(rec.get());
            if (record_log != nullptr) {
                touched_tables.insert(record_log->table_name_);
                std::cerr << "  -> Touched table: " << record_log->table_name_ << std::endl;
            }
        }
        logs.push_back(std::move(rec));
        offset += len;
    }
    
    std::cerr << "Committed txns: " << committed_txns.size() << std::endl;
    std::cerr << "Active txns: " << active_txns.size() << std::endl;
    std::cerr << "Touched tables: " << touched_tables.size() << std::endl;
    for (auto &tab : touched_tables) {
        std::cerr << "  - " << tab << std::endl;
    }
    std::cerr << "==================================\n" << std::endl;
}

void RecoveryManager::redo() {
    std::cerr << "\n========== REDO START ==========" << std::endl;
    std::cerr << "Committed txns to redo: " << committed_txns.size() << std::endl;
    
    // 检查元数据状态
    for (auto &tab_name : touched_tables) {
        if (sm_manager_->db_.is_table(tab_name)) {
            auto &tab = sm_manager_->db_.get_table(tab_name);
            std::cerr << "Table " << tab_name << " has " << tab.indexes.size() << " indexes BEFORE redo" << std::endl;
        }
    }
    
    for (auto &rec : logs) {
        if (!committed_txns.count(rec->log_tid_)) continue;
        auto *record_log = dynamic_cast<RecordLogRecord *>(rec.get());
        if (record_log == nullptr) continue;
        if (rec->log_type_ == LogType::INSERT) apply_insert(sm_manager_, *record_log);
        else if (rec->log_type_ == LogType::DELETE) apply_delete(sm_manager_, *record_log);
        else if (rec->log_type_ == LogType::UPDATE) apply_update(sm_manager_, *record_log, true);
    }
    
    std::cerr << "AFTER redo:" << std::endl;
    for (auto &tab_name : touched_tables) {
        if (sm_manager_->db_.is_table(tab_name)) {
            auto &tab = sm_manager_->db_.get_table(tab_name);
            std::cerr << "Table " << tab_name << " has " << tab.indexes.size() << " indexes AFTER redo" << std::endl;
        }
    }
    std::cerr << "================================\n" << std::endl;
}

void RecoveryManager::undo() {
    std::cerr << "\n========== UNDO START ==========" << std::endl;
    std::cerr << "Active txns: " << active_txns.size() << std::endl;
    std::cerr << "Touched tables: " << touched_tables.size() << std::endl;
    for (auto &tab : touched_tables) {
        std::cerr << "  - " << tab << std::endl;
    }
    
    for (auto it = logs.rbegin(); it != logs.rend(); ++it) {
        auto &rec = *it;
        if (!active_txns.count(rec->log_tid_)) continue;
        auto *record_log = dynamic_cast<RecordLogRecord *>(rec.get());
        if (record_log == nullptr) continue;
        if (rec->log_type_ == LogType::INSERT) apply_delete(sm_manager_, *record_log);
        else if (rec->log_type_ == LogType::DELETE) apply_insert(sm_manager_, *record_log);
        else if (rec->log_type_ == LogType::UPDATE) apply_update(sm_manager_, *record_log, false);
    }
    std::cerr << "Calling rebuild for touched tables..." << std::endl;
    for (auto &tab_name : touched_tables) rebuild_table_indexes(sm_manager_, tab_name);
    std::cerr << "=================================\n" << std::endl;
    for (auto &entry : sm_manager_->fhs_) {
        sm_manager_->get_bpm()->flush_all_pages(entry.second->GetFd());
    }
    int fd = open(LOG_FILE_NAME.c_str(), O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
}