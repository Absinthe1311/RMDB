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

/**
 * @description: analyze阶段，需要获得脏页表（DPT）和未完成的事务列表（ATT）
 */
void RecoveryManager::analyze() {
    try {
        int log_size = disk_manager_->GetLogSize();
        if(log_size == 0) {
            std::cerr << "[Recovery] No log file found, skip recovery" << std::endl;
            return;
        }
        
        std::cerr << "[Recovery] Log size: " << log_size << " bytes" << std::endl;
        
        char* log_buffer = new char[log_size];
        disk_manager_->read_log(log_buffer, log_size, 0);
        
        int offset = 0;
        int log_count = 0;
        while(offset < log_size) {
            LogRecord log_record;
            log_record.deserialize(log_buffer + offset);
            
            // 边界检查
            if(log_record.log_tot_len_ <= 0 || offset + log_record.log_tot_len_ > log_size) {
                std::cerr << "[Recovery] Invalid log_tot_len_=" << log_record.log_tot_len_ 
                          << " at offset=" << offset << ", stop scan" << std::endl;
                break;
            }
            
            std::cerr << "[Recovery] Log " << log_count++ 
                      << ": type=" << LogTypeStr[log_record.log_type_]
                      << ", lsn=" << log_record.lsn_
                      << ", txn=" << log_record.log_tid_
                      << ", prev_lsn=" << log_record.prev_lsn_ << std::endl;
            
            offset += log_record.log_tot_len_;
        }
        
        delete[] log_buffer;
    } catch(const std::exception& e) {
        std::cerr << "[Recovery] Exception in analyze: " << e.what() << std::endl;
    }
}

/**
 * @description: 重做所有未落盘的操作
 */
void RecoveryManager::redo() {
    try {
        int log_size = disk_manager_->GetLogSize();
        if(log_size == 0) {
            return;
        }
        
        std::cerr << "[Redo] Starting redo phase..." << std::endl;
        
        char* log_buffer = new char[log_size];
        disk_manager_->read_log(log_buffer, log_size, 0);
        
        int offset = 0;
        int redo_count = 0;
        while(offset < log_size) {
            LogRecord log_header;
            log_header.deserialize(log_buffer + offset);
            
            // 边界检查
            if(log_header.log_tot_len_ <= 0 || offset + log_header.log_tot_len_ > log_size) {
                std::cerr << "[Redo] Invalid log_tot_len_=" << log_header.log_tot_len_ 
                          << " at offset=" << offset << ", stop redo" << std::endl;
                break;
            }
            
            LogType log_type = log_header.log_type_;
            lsn_t lsn = log_header.lsn_;
        
        if(log_type == LogType::INSERT) {
            InsertLogRecord insert_log;
            insert_log.deserialize(log_buffer + offset);
            
            std::string table_name(insert_log.table_name_, insert_log.table_name_size_);
            
            // 检查表是否存在
            if(sm_manager_->fhs_.find(table_name) == sm_manager_->fhs_.end()) {
                std::cerr << "[Redo] Warning: table " << table_name << " not found, skip INSERT log" << std::endl;
                offset += insert_log.log_tot_len_;
                continue;
            }
            
            RmFileHandle* rm_file_handle = sm_manager_->fhs_.at(table_name).get();
            
            std::cerr << "[Redo] Redo INSERT: table=" << table_name 
                      << ", rid=(" << insert_log.rid_.page_no << "," << insert_log.rid_.slot_no << ")" 
                      << std::endl;
            
            // 尝试插入记录，如果页面不存在会自动处理
            try {
                rm_file_handle->insert_record(insert_log.rid_, insert_log.insert_value_.data);
                
                // ✅ 同时插入索引项
                auto& tab = sm_manager_->db_.get_table(table_name);
                for(auto& index : tab.indexes) {
                    std::string index_name = sm_manager_->get_ix_manager()->get_index_name(table_name, index.cols);
                    auto ih = sm_manager_->ihs_.at(index_name).get();
                    
                    char* key = new char[index.col_tot_len];
                    int key_offset = 0;
                    for(auto& col : index.cols) {
                        memcpy(key + key_offset, insert_log.insert_value_.data + col.offset, col.len);
                        key_offset += col.len;
                    }
                    ih->insert_entry(key, insert_log.rid_, nullptr);
                    delete[] key;
                }
                
                redo_count++;
            } catch(const PageNotExistError& e) {
                // 页面不存在，需要创建页面
                std::cerr << "[Redo] Page " << insert_log.rid_.page_no 
                          << " not exist, creating new page" << std::endl;
                
                // 创建新页面直到达到需要的页面号
                while(rm_file_handle->get_file_hdr().num_pages <= insert_log.rid_.page_no) {
                    rm_file_handle->create_new_page_handle();
                }
                
                // 再次尝试插入
                rm_file_handle->insert_record(insert_log.rid_, insert_log.insert_value_.data);
                
                // ✅ 插入索引项
                auto& tab = sm_manager_->db_.get_table(table_name);
                for(auto& index : tab.indexes) {
                    std::string index_name = sm_manager_->get_ix_manager()->get_index_name(table_name, index.cols);
                    auto ih = sm_manager_->ihs_.at(index_name).get();
                    
                    char* key = new char[index.col_tot_len];
                    int key_offset = 0;
                    for(auto& col : index.cols) {
                        memcpy(key + key_offset, insert_log.insert_value_.data + col.offset, col.len);
                        key_offset += col.len;
                    }
                    ih->insert_entry(key, insert_log.rid_, nullptr);
                    delete[] key;
                }
                
                redo_count++;
            }
        }
        else if(log_type == LogType::DELETE) {
            DeleteLogRecord delete_log;
            delete_log.deserialize(log_buffer + offset);
            
            std::string table_name(delete_log.table_name_, delete_log.table_name_size_);
            
            // 检查表是否存在
            if(sm_manager_->fhs_.find(table_name) == sm_manager_->fhs_.end()) {
                std::cerr << "[Redo] Warning: table " << table_name << " not found, skip DELETE log" << std::endl;
                offset += delete_log.log_tot_len_;
                continue;
            }
            
            RmFileHandle* rm_file_handle = sm_manager_->fhs_.at(table_name).get();
            
            std::cerr << "[Redo] Redo DELETE: table=" << table_name << std::endl;
            
            // 尝试删除记录
            try {
                rm_file_handle->delete_record(delete_log.rid_, nullptr);
                
                // ✅ 同时删除索引项
                auto& tab = sm_manager_->db_.get_table(table_name);
                for(auto& index : tab.indexes) {
                    std::string index_name = sm_manager_->get_ix_manager()->get_index_name(table_name, index.cols);
                    auto ih = sm_manager_->ihs_.at(index_name).get();
                    
                    char* key = new char[index.col_tot_len];
                    int key_offset = 0;
                    for(auto& col : index.cols) {
                        memcpy(key + key_offset, delete_log.delete_value_.data + col.offset, col.len);
                        key_offset += col.len;
                    }
                    ih->delete_entry(key, nullptr);
                    delete[] key;
                }
            } catch(const PageNotExistError& e) {
                // 页面不存在，创建页面后再删除（实际上空页面上删除会失败，但保持一致性）
                std::cerr << "[Redo] Page " << delete_log.rid_.page_no 
                          << " not exist, creating new page" << std::endl;
                while(rm_file_handle->get_file_hdr().num_pages <= delete_log.rid_.page_no) {
                    rm_file_handle->create_new_page_handle();
                }
                // DELETE的redo可能失败（因为记录不存在），这是正常的，跳过
            }
        }
        else if(log_type == LogType::UPDATE) {
            UpdateLogRecord update_log;
            update_log.deserialize(log_buffer + offset);
            
            std::string table_name(update_log.table_name_, update_log.table_name_size_);
            
            // 检查表是否存在
            if(sm_manager_->fhs_.find(table_name) == sm_manager_->fhs_.end()) {
                std::cerr << "[Redo] Warning: table " << table_name << " not found, skip UPDATE log" << std::endl;
                offset += update_log.log_tot_len_;
                continue;
            }
            
            RmFileHandle* rm_file_handle = sm_manager_->fhs_.at(table_name).get();
            
            std::cerr << "[Redo] Redo UPDATE: table=" << table_name << std::endl;
            
            // 尝试更新记录
            try {
                rm_file_handle->update_record(update_log.rid_, update_log.new_value_.data, nullptr);
                
                // ✅ 更新索引：检查索引key是否改变
                auto& tab = sm_manager_->db_.get_table(table_name);
                for(auto& index : tab.indexes) {
                    std::string index_name = sm_manager_->get_ix_manager()->get_index_name(table_name, index.cols);
                    auto ih = sm_manager_->ihs_.at(index_name).get();
                    
                    // 构造旧key和新key
                    char* old_key = new char[index.col_tot_len];
                    char* new_key = new char[index.col_tot_len];
                    int key_offset = 0;
                    for(auto& col : index.cols) {
                        memcpy(old_key + key_offset, update_log.old_value_.data + col.offset, col.len);
                        memcpy(new_key + key_offset, update_log.new_value_.data + col.offset, col.len);
                        key_offset += col.len;
                    }
                    
                    // 只有key改变时才更新索引
                    if(memcmp(old_key, new_key, index.col_tot_len) != 0) {
                        ih->delete_entry(old_key, nullptr);
                        ih->insert_entry(new_key, update_log.rid_, nullptr);
                    }
                    
                    delete[] old_key;
                    delete[] new_key;
                }
            } catch(const PageNotExistError& e) {
                // 页面不存在，创建页面后插入新值（UPDATE的redo实际上是写入新值）
                std::cerr << "[Redo] Page " << update_log.rid_.page_no 
                          << " not exist, creating new page and insert" << std::endl;
                while(rm_file_handle->get_file_hdr().num_pages <= update_log.rid_.page_no) {
                    rm_file_handle->create_new_page_handle();
                }
                // UPDATE的redo：直接插入新值
                rm_file_handle->insert_record(update_log.rid_, update_log.new_value_.data);
                
                // ✅ 插入新索引key（因为页面是新创建的，旧key不存在）
                auto& tab = sm_manager_->db_.get_table(table_name);
                for(auto& index : tab.indexes) {
                    std::string index_name = sm_manager_->get_ix_manager()->get_index_name(table_name, index.cols);
                    auto ih = sm_manager_->ihs_.at(index_name).get();
                    
                    char* new_key = new char[index.col_tot_len];
                    int key_offset = 0;
                    for(auto& col : index.cols) {
                        memcpy(new_key + key_offset, update_log.new_value_.data + col.offset, col.len);
                        key_offset += col.len;
                    }
                    ih->insert_entry(new_key, update_log.rid_, nullptr);
                    delete[] new_key;
                }
            }
        }
        
        offset += log_header.log_tot_len_;
    }
    
    std::cerr << "[Redo] Redo phase completed, " << redo_count << " operations redone" << std::endl;
    delete[] log_buffer;
    } catch(const std::exception& e) {
        std::cerr << "[Redo] Exception in redo: " << e.what() << std::endl;
    }
}

/**
 * @description: 回滚未完成的事务
 */
void RecoveryManager::undo() {
    try {
        int log_size = disk_manager_->GetLogSize();
        if(log_size == 0) {
            return;
        }
        
        char* log_buffer = new char[log_size];
        disk_manager_->read_log(log_buffer, log_size, 0);
        
        std::unordered_map<txn_id_t, lsn_t> active_txn_table;
        
        int offset = 0;
        std::vector<lsn_t> log_offsets;
        
        while(offset < log_size) {
            LogRecord log_header;
            log_header.deserialize(log_buffer + offset);
            
            // 边界检查
            if(log_header.log_tot_len_ <= 0 || offset + log_header.log_tot_len_ > log_size) {
                std::cerr << "[Undo] Invalid log_tot_len_=" << log_header.log_tot_len_ 
                          << " at offset=" << offset << ", stop scan" << std::endl;
                break;
            }
            
            log_offsets.push_back(offset);
        
        LogType log_type = log_header.log_type_;
        txn_id_t txn_id = log_header.log_tid_;
        lsn_t lsn = log_header.lsn_;
        
        if(log_type == LogType::begin) {
            active_txn_table[txn_id] = lsn;
            std::cerr << "[Undo] Transaction " << txn_id << " BEGIN" << std::endl;
        }
        else if(log_type == LogType::commit) {
            active_txn_table.erase(txn_id);
            std::cerr << "[Undo] Transaction " << txn_id << " COMMIT" << std::endl;
        }
        else if(log_type == LogType::ABORT) {
            active_txn_table.erase(txn_id);
            std::cerr << "[Undo] Transaction " << txn_id << " ABORT" << std::endl;
        }
        else if(log_type == LogType::INSERT || 
                log_type == LogType::DELETE || 
                log_type == LogType::UPDATE) {
            if(active_txn_table.find(txn_id) != active_txn_table.end()) {
                active_txn_table[txn_id] = lsn;
            }
        }
        
        offset += log_header.log_tot_len_;
    }
    
    std::cerr << "[Undo] Active transactions: " << active_txn_table.size() << std::endl;
    for(auto& p : active_txn_table) {
        std::cerr << "[Undo]   Active txn " << p.first << ", last_lsn=" << p.second << std::endl;
    }
    
    if(active_txn_table.empty()) {
        delete[] log_buffer;
        return;
    }
    
    std::unordered_map<txn_id_t, std::vector<lsn_t>> txn_log_chains;
    
    for(auto& pair : active_txn_table) {
        txn_id_t txn_id = pair.first;
        lsn_t current_lsn = pair.second;
        
        while(current_lsn != INVALID_LSN) {
            txn_log_chains[txn_id].push_back(current_lsn);
            
            for(int log_offset : log_offsets) {
                LogRecord log_header;
                log_header.deserialize(log_buffer + log_offset);
                
                if(log_header.lsn_ == current_lsn) {
                    current_lsn = log_header.prev_lsn_;
                    break;
                }
            }
        }
    }
    
    for(auto& pair : txn_log_chains) {
        txn_id_t txn_id = pair.first;
        std::vector<lsn_t>& log_chain = pair.second;
        
        std::cerr << "[Undo] Undoing transaction " << txn_id 
                  << ", log chain size: " << log_chain.size() << std::endl;
        
        // log_chain已经是"最新lsn在前"的顺序，正序遍历就是"从最新往最旧撤销"
        for(auto it = log_chain.begin(); it != log_chain.end(); ++it) {
            lsn_t lsn = *it;
            
            std::cerr << "[Undo] Processing LSN=" << lsn << std::endl;
            
            for(int log_offset : log_offsets) {
                LogRecord log_header;
                log_header.deserialize(log_buffer + log_offset);
                
                if(log_header.lsn_ == lsn) {
                    LogType log_type = log_header.log_type_;
                    
                    try {
                        if(log_type == LogType::INSERT) {
                            InsertLogRecord insert_log;
                            insert_log.deserialize(log_buffer + log_offset);
                            
                            std::string table_name(insert_log.table_name_, insert_log.table_name_size_);
                            
                            std::cerr << "[Undo] Undo INSERT: table=" << table_name
                                      << ", rid=(" << insert_log.rid_.page_no << "," << insert_log.rid_.slot_no << ")"
                                      << std::endl;
                            
                            if(sm_manager_->fhs_.find(table_name) == sm_manager_->fhs_.end()) {
                                std::cerr << "[Undo] Warning: table " << table_name << " not found, skip" << std::endl;
                                break;
                            }
                            
                            RmFileHandle* rm_file_handle = sm_manager_->fhs_.at(table_name).get();
                            
                            // 尝试删除记录
                            try {
                                rm_file_handle->delete_record(insert_log.rid_, nullptr);
                            } catch(const PageNotExistError& e) {
                                // 页面不存在，创建页面
                                std::cerr << "[Undo] Page " << insert_log.rid_.page_no 
                                          << " not exist, creating" << std::endl;
                                while(rm_file_handle->get_file_hdr().num_pages <= insert_log.rid_.page_no) {
                                    rm_file_handle->create_new_page_handle();
                                }
                                // 页面已创建，但记录不存在是正常的，跳过
                            }
                        
                            auto& tab = sm_manager_->db_.get_table(table_name);
                            for(auto& index : tab.indexes) {
                                std::string index_name = sm_manager_->get_ix_manager()->get_index_name(table_name, index.cols);
                                auto ih = sm_manager_->ihs_.at(index_name).get();
                                
                                char* key = new char[index.col_tot_len];
                                int offset = 0;
                                for(auto& col : index.cols) {
                                    memcpy(key + offset, insert_log.insert_value_.data + col.offset, col.len);
                                    offset += col.len;
                                }
                                ih->delete_entry(key, nullptr);
                                delete[] key;
                            }
                        }
        else if(log_type == LogType::DELETE) {
            DeleteLogRecord delete_log;
            delete_log.deserialize(log_buffer + log_offset);
            
            std::string table_name(delete_log.table_name_, delete_log.table_name_size_);
            
            std::cerr << "[Undo] Undo DELETE: table=" << table_name 
                      << ", rid=(" << delete_log.rid_.page_no << "," << delete_log.rid_.slot_no << ")"
                      << ", value_size=" << delete_log.delete_value_.size << std::endl;
            
            if(sm_manager_->fhs_.find(table_name) == sm_manager_->fhs_.end()) {
                std::cerr << "[Undo] Warning: table " << table_name << " not found, skip" << std::endl;
                break;
            }
            
            RmFileHandle* rm_file_handle = sm_manager_->fhs_.at(table_name).get();
            
            // 尝试插入记录
            try {
                rm_file_handle->insert_record(delete_log.rid_, delete_log.delete_value_.data);
                
                std::cerr << "[Undo] DELETE undo: record inserted" << std::endl;
            } catch(const PageNotExistError& e) {
                // 页面不存在，创建页面后插入
                std::cerr << "[Undo] Page " << delete_log.rid_.page_no 
                          << " not exist, creating and insert" << std::endl;
                while(rm_file_handle->get_file_hdr().num_pages <= delete_log.rid_.page_no) {
                    rm_file_handle->create_new_page_handle();
                }
                rm_file_handle->insert_record(delete_log.rid_, delete_log.delete_value_.data);
            }
        
            // 恢复索引
            auto& tab = sm_manager_->db_.get_table(table_name);
            std::cerr << "[Undo] DELETE undo: processing " << tab.indexes.size() << " indexes" << std::endl;
            
            for(auto& index : tab.indexes) {
                std::string index_name = sm_manager_->get_ix_manager()->get_index_name(table_name, index.cols);
                auto ih = sm_manager_->ihs_.at(index_name).get();
                
                char* key = new char[index.col_tot_len];
                int key_offset = 0;
                for(auto& col : index.cols) {
                    memcpy(key + key_offset, delete_log.delete_value_.data + col.offset, col.len);
                    key_offset += col.len;
                }
                
                std::cerr << "[Undo] DELETE undo: inserting index key" << std::endl;
                ih->insert_entry(key, delete_log.rid_, nullptr);
                delete[] key;
            }
        }
                        else if(log_type == LogType::UPDATE) {
                            UpdateLogRecord update_log;
                            update_log.deserialize(log_buffer + log_offset);
                            
                            std::string table_name(update_log.table_name_, update_log.table_name_size_);
                            
                            std::cerr << "[Undo] Undo UPDATE: table=" << table_name << std::endl;
                            
                            if(sm_manager_->fhs_.find(table_name) == sm_manager_->fhs_.end()) {
                                std::cerr << "[Undo] Warning: table " << table_name << " not found, skip" << std::endl;
                                break;
                            }
                            
                            RmFileHandle* rm_file_handle = sm_manager_->fhs_.at(table_name).get();
                            
                            // 尝试更新记录
                            try {
                                rm_file_handle->update_record(update_log.rid_, update_log.old_value_.data, nullptr);
                            } catch(const PageNotExistError& e) {
                                // 页面不存在，创建页面后插入旧值
                                std::cerr << "[Undo] Page " << update_log.rid_.page_no 
                                          << " not exist, creating and insert old value" << std::endl;
                                while(rm_file_handle->get_file_hdr().num_pages <= update_log.rid_.page_no) {
                                    rm_file_handle->create_new_page_handle();
                                }
                                rm_file_handle->insert_record(update_log.rid_, update_log.old_value_.data);
                            }
                        
                            auto& tab = sm_manager_->db_.get_table(table_name);
                            for(auto& index : tab.indexes) {
                                std::string index_name = sm_manager_->get_ix_manager()->get_index_name(table_name, index.cols);
                                auto ih = sm_manager_->ihs_.at(index_name).get();
                                
                                // 构造旧key和新key
                                char* old_key = new char[index.col_tot_len];
                                char* new_key = new char[index.col_tot_len];
                                int offset = 0;
                                for(auto& col : index.cols) {
                                    memcpy(old_key + offset, update_log.old_value_.data + col.offset, col.len);
                                    memcpy(new_key + offset, update_log.new_value_.data + col.offset, col.len);
                                    offset += col.len;
                                }
                                
                                // 只有key改变时才恢复索引
                                if(memcmp(old_key, new_key, index.col_tot_len) != 0) {
                                    ih->delete_entry(new_key, nullptr);
                                    ih->insert_entry(old_key, update_log.rid_, nullptr);
                                }
                                
                                delete[] old_key;
                                delete[] new_key;
                            }
                        }
                    } catch(const std::exception& e) {
                        std::cerr << "[Undo] Error during undo: " << e.what() << std::endl;
                        // 继续处理下一条日志
                    }
                    
                    break;
                }
            }
        }
    }
    
    std::cerr << "[Undo] Undo phase completed" << std::endl;
    
    delete[] log_buffer;
    } catch(const std::exception& e) {
        std::cerr << "[Undo] Exception in undo: " << e.what() << std::endl;
    }
}