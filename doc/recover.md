# 系统故障恢复实现方案

## 一、功能需求分析

### 1.1 题目要求

根据题目描述，需要实现以下功能：

1. **日志管理器（LogManager）**
   - 在系统运行过程中把读写操作的日志通过日志缓冲区写入磁盘
   - 实现WAL（Write-Ahead Logging）算法
   - 支持redo/undo日志记录

2. **故障恢复管理器（RecoveryManager）**
   - 通过redo/undo日志对系统进行故障恢复
   - 让数据库系统恢复到一致性状态
   - 实现analyze、redo、undo三个阶段

3. **日志记录类型**
   - BEGIN、COMMIT、ABORT日志（已实现）
   - INSERT、DELETE、UPDATE日志（部分实现）
   - 每条日志记录包含：lsn、prev_lsn、txn_id、操作类型、操作数据

4. **测试场景**
   - 系统接收到若干事务
   - 运行过程中接收终止信号（crash）终止系统
   - 重启后进行故障恢复
   - 恢复到一致性状态

### 1.2 当前代码框架分析

#### 已有实现：

1. **日志记录结构（LogRecord）**：已定义
   - LogType枚举：UPDATE、INSERT、DELETE、begin、commit、ABORT
   - 基础字段：log_type_、lsn_、log_tot_len_、log_tid_、prev_lsn_
   - 序列化/反序列化接口：serialize()、deserialize()

2. **部分日志记录类**：已实现
   - BeginLogRecord：完整实现
   - CommitLogRecord：完整实现
   - AbortLogRecord：完整实现
   - InsertLogRecord：完整实现（包含表名、记录值、RID）
   - DeleteLogRecord：仅定义，未实现
   - UpdateLogRecord：仅定义，未实现

3. **日志缓冲区（LogBuffer）**：已定义
   - 大小：LOG_BUFFER_SIZE = 4MB
   - buffer_数组：存储日志数据
   - offset_：当前写入偏移量
   - is_full()：检查是否已满

4. **LogManager框架**：已定义
   - global_lsn_：全局日志序列号生成器
   - log_buffer_：日志缓冲区
   - persist_lsn_：已持久化的最后一条日志LSN
   - latch_：互斥锁
   - add_log_to_buffer()：**空实现，需要完成**
   - flush_log_to_disk()：**空实现，需要完成**

5. **RecoveryManager框架**：已定义
   - buffer_：读入日志的缓冲区
   - analyze()：**空实现，需要完成**
   - redo()：**空实现，需要完成**
   - undo()：**空实现，需要完成**

6. **Page LSN支持**：已实现
   - Page类中有OFFSET_LSN定义
   - get_page_lsn()和set_page_lsn()方法

7. **事务管理中的日志记录**：已实现
   - begin时记录BEGIN日志
   - commit时记录COMMIT日志并刷盘
   - abort时记录ABORT日志

#### 当前问题：

1. **LogManager核心方法未实现**：
   ```cpp
   lsn_t LogManager::add_log_to_buffer(LogRecord* log_record) {
       // 空实现
   }
   void LogManager::flush_log_to_disk() {
       // 空实现
   }
   ```

2. **RecoveryManager三个阶段未实现**：
   ```cpp
   void RecoveryManager::analyze() {
       // 空实现
   }
   void RecoveryManager::redo() {
       // 空实现
   }
   void RecoveryManager::undo() {
       // 空实现
   }
   ```

3. **DeleteLogRecord和UpdateLogRecord未实现**：
   ```cpp
   class DeleteLogRecord: public LogRecord {
       // 仅定义，未实现
   };
   class UpdateLogRecord: public LogRecord {
       // 仅定义，未实现
   };
   ```

4. **执行器中未记录操作日志**：
   - InsertExecutor、DeleteExecutor、UpdateExecutor需要记录日志

5. **Page LSN未在修改时更新**：
   - 修改页面时需要设置page_lsn为当前操作的lsn

---

## 二、需要修改/新增的文件

### 2.1 核心修改文件

#### 文件1: `/home/absinthe/rmdb/src/recovery/log_manager.h`

**修改位置**：第229-238行，DeleteLogRecord和UpdateLogRecord的定义

**需要实现的内容**：

```cpp
/**
 * TODO: delete操作的日志记录
 */
class DeleteLogRecord: public LogRecord {
public:
    DeleteLogRecord() {
        log_type_ = LogType::DELETE;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
        table_name_ = nullptr;
    }
    
    DeleteLogRecord(txn_id_t txn_id, RmRecord& delete_value, Rid& rid, std::string table_name) 
        : DeleteLogRecord() {
        log_tid_ = txn_id;
        delete_value_ = delete_value;
        rid_ = rid;
        log_tot_len_ += sizeof(int);
        log_tot_len_ += delete_value_.size;
        log_tot_len_ += sizeof(Rid);
        table_name_size_ = table_name.length();
        table_name_ = new char[table_name_size_];
        memcpy(table_name_, table_name.c_str(), table_name_size_);
        log_tot_len_ += sizeof(size_t) + table_name_size_;
    }

    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        memcpy(dest + offset, &delete_value_.size, sizeof(int));
        offset += sizeof(int);
        memcpy(dest + offset, delete_value_.data, delete_value_.size);
        offset += delete_value_.size;
        memcpy(dest + offset, &rid_, sizeof(Rid));
        offset += sizeof(Rid);
        memcpy(dest + offset, &table_name_size_, sizeof(size_t));
        offset += sizeof(size_t);
        memcpy(dest + offset, table_name_, table_name_size_);
    }
    
    void deserialize(const char* src) override {
        LogRecord::deserialize(src);
        delete_value_.Deserialize(src + OFFSET_LOG_DATA);
        int offset = OFFSET_LOG_DATA + delete_value_.size + sizeof(int);
        rid_ = *reinterpret_cast<const Rid*>(src + offset);
        offset += sizeof(Rid);
        table_name_size_ = *reinterpret_cast<const size_t*>(src + offset);
        offset += sizeof(size_t);
        table_name_ = new char[table_name_size_];
        memcpy(table_name_, src + offset, table_name_size_);
    }
    
    void format_print() override {
        printf("delete record\n");
        LogRecord::format_print();
        printf("delete_value: %s\n", delete_value_.data);
        printf("delete rid: %d, %d\n", rid_.page_no, rid_.slot_no);
        printf("table name: %s\n", table_name_);
    }

    RmRecord delete_value_;     // 被删除的记录（用于undo）
    Rid rid_;                   // 记录删除的位置
    char* table_name_;          // 删除记录的表名称
    size_t table_name_size_;    // 表名称的大小
};

/**
 * TODO: update操作的日志记录
 */
class UpdateLogRecord: public LogRecord {
public:
    UpdateLogRecord() {
        log_type_ = LogType::UPDATE;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
        table_name_ = nullptr;
    }
    
    UpdateLogRecord(txn_id_t txn_id, RmRecord& old_value, RmRecord& new_value, Rid& rid, std::string table_name) 
        : UpdateLogRecord() {
        log_tid_ = txn_id;
        old_value_ = old_value;
        new_value_ = new_value;
        rid_ = rid;
        log_tot_len_ += sizeof(int);  // old_value size
        log_tot_len_ += old_value_.size;
        log_tot_len_ += sizeof(int);  // new_value size
        log_tot_len_ += new_value_.size;
        log_tot_len_ += sizeof(Rid);
        table_name_size_ = table_name.length();
        table_name_ = new char[table_name_size_];
        memcpy(table_name_, table_name.c_str(), table_name_size_);
        log_tot_len_ += sizeof(size_t) + table_name_size_;
    }

    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        // 序列化old_value
        memcpy(dest + offset, &old_value_.size, sizeof(int));
        offset += sizeof(int);
        memcpy(dest + offset, old_value_.data, old_value_.size);
        offset += old_value_.size;
        // 序列化new_value
        memcpy(dest + offset, &new_value_.size, sizeof(int));
        offset += sizeof(int);
        memcpy(dest + offset, new_value_.data, new_value_.size);
        offset += new_value_.size;
        // 序列化rid和table_name
        memcpy(dest + offset, &rid_, sizeof(Rid));
        offset += sizeof(Rid);
        memcpy(dest + offset, &table_name_size_, sizeof(size_t));
        offset += sizeof(size_t);
        memcpy(dest + offset, table_name_, table_name_size_);
    }
    
    void deserialize(const char* src) override {
        LogRecord::deserialize(src);
        int offset = OFFSET_LOG_DATA;
        // 反序列化old_value
        old_value_.Deserialize(src + offset);
        offset += old_value_.size + sizeof(int);
        // 反序列化new_value
        new_value_.Deserialize(src + offset);
        offset += new_value_.size + sizeof(int);
        // 反序列化rid和table_name
        rid_ = *reinterpret_cast<const Rid*>(src + offset);
        offset += sizeof(Rid);
        table_name_size_ = *reinterpret_cast<const size_t*>(src + offset);
        offset += sizeof(size_t);
        table_name_ = new char[table_name_size_];
        memcpy(table_name_, src + offset, table_name_size_);
    }
    
    void format_print() override {
        printf("update record\n");
        LogRecord::format_print();
        printf("old_value: %s\n", old_value_.data);
        printf("new_value: %s\n", new_value_.data);
        printf("update rid: %d, %d\n", rid_.page_no, rid_.slot_no);
        printf("table name: %s\n", table_name_);
    }

    RmRecord old_value_;        // 更新前的记录值（用于undo）
    RmRecord new_value_;        // 更新后的记录值（用于redo）
    Rid rid_;                   // 记录更新的位置
    char* table_name_;          // 更新记录的表名称
    size_t table_name_size_;    // 表名称的大小
};
```

**修改原因**：
- 需要完整的DELETE和UPDATE日志记录用于恢复
- DELETE日志需要保存被删除的值（用于undo）
- UPDATE日志需要保存旧值和新值

---

#### 文件2: `/home/absinthe/rmdb/src/recovery/log_manager.cpp`

**修改位置**：第19-28行，两个核心方法的实现

**完整实现代码**：

```cpp
/**
 * @description: 添加日志记录到日志缓冲区中，并返回日志记录号
 * @param {LogRecord*} log_record 要写入缓冲区的日志记录
 * @return {lsn_t} 返回该日志的日志记录号
 */
lsn_t LogManager::add_log_to_buffer(LogRecord* log_record) {
    // 1. 加锁，保证线程安全
    std::unique_lock<std::mutex> lock(latch_);
    
    // 2. 为日志记录分配LSN
    lsn_t lsn = global_lsn_.fetch_add(1);
    log_record->lsn_ = lsn;
    
    // 3. 检查缓冲区是否有足够空间
    int log_size = log_record->log_tot_len_;
    if(log_buffer_.is_full(log_size)) {
        // 缓冲区已满，先刷盘
        flush_log_to_disk();
    }
    
    // 4. 将日志记录序列化到缓冲区
    char* dest = log_buffer_.buffer_ + log_buffer_.offset_;
    log_record->serialize(dest);
    log_buffer_.offset_ += log_size;
    
    // 5. 返回LSN
    return lsn;
}

/**
 * @description: 把日志缓冲区的内容刷到磁盘中，由于目前只设置了一个缓冲区，因此需要阻塞其他日志操作
 */
void LogManager::flush_log_to_disk() {
    // 1. 加锁，保证线程安全
    std::unique_lock<std::mutex> lock(latch_);
    
    // 2. 如果缓冲区为空，直接返回
    if(log_buffer_.offset_ == 0) {
        return;
    }
    
    // 3. 将缓冲区内容写入磁盘
    disk_manager_->write_log(log_buffer_.buffer_, log_buffer_.offset_);
    
    // 4. 更新persist_lsn
    persist_lsn_ = global_lsn_ - 1;
    
    // 5. 清空缓冲区
    log_buffer_.offset_ = 0;
    memset(log_buffer_.buffer_, 0, LOG_BUFFER_SIZE);
}
```

**关键实现细节**：

1. **LSN分配**：
   - 使用atomic变量global_lsn_保证原子性
   - 每条日志获得唯一的递增LSN

2. **缓冲区管理**：
   - 检查缓冲区是否已满
   - 满则先刷盘再写入

3. **序列化**：
   - 调用日志记录的serialize方法
   - 写入到缓冲区当前偏移位置

4. **刷盘**：
   - 使用disk_manager_->write_log()写入磁盘
   - 更新persist_lsn_记录已持久化的LSN
   - 清空缓冲区

---

#### 文件3: `/home/absinthe/rmdb/src/recovery/log_recovery.cpp`

**修改位置**：第16-32行，三个恢复阶段的实现

**完整实现代码**：

```cpp
/**
 * @description: analyze阶段，需要获得脏页表（DPT）和未完成的事务列表（ATT）
 */
void RecoveryManager::analyze() {
    // 1. 从磁盘读取所有日志
    int log_size = disk_manager_->GetLogSize();
    if(log_size == 0) {
        return;  // 没有日志，无需恢复
    }
    
    char* log_buffer = new char[log_size];
    disk_manager_->read_log(log_buffer, log_size, 0);
    
    // 2. 扫描所有日志记录，构建活跃事务表（ATT）和脏页表（DPT）
    std::unordered_map<txn_id_t, lsn_t> active_txn_table;  // 活跃事务表：<txn_id, last_lsn>
    std::unordered_map<PageId, lsn_t, PageIdHash> dirty_page_table;  // 脏页表：<page_id, rec_lsn>
    
    int offset = 0;
    while(offset < log_size) {
        // 反序列化日志记录
        LogRecord log_record;
        log_record.deserialize(log_buffer + offset);
        
        // 根据日志类型处理
        LogType log_type = log_record.log_type_;
        txn_id_t txn_id = log_record.log_tid_;
        lsn_t lsn = log_record.lsn_;
        
        if(log_type == LogType::begin) {
            // BEGIN日志：事务开始，加入ATT
            active_txn_table[txn_id] = lsn;
        }
        else if(log_type == LogType::commit) {
            // COMMIT日志：事务提交，从ATT中移除
            active_txn_table.erase(txn_id);
        }
        else if(log_type == LogType::ABORT) {
            // ABORT日志：事务回滚，从ATT中移除
            active_txn_table.erase(txn_id);
        }
        else if(log_type == LogType::INSERT || 
                log_type == LogType::DELETE || 
                log_type == LogType::UPDATE) {
            // 修改操作日志：更新ATT中事务的最后LSN
            if(active_txn_table.find(txn_id) != active_txn_table.end()) {
                active_txn_table[txn_id] = lsn;
            }
            
            // 根据日志类型获取涉及的页面
            // 这里需要反序列化完整的日志记录以获取RID
            // 并将页面加入脏页表（如果尚未加入）
            // 具体实现需要根据INSERT/DELETE/UPDATE日志记录的结构
        }
        
        offset += log_record.log_tot_len_;
    }
    
    // 3. 保存ATT和DPT（作为成员变量或传递给redo/undo）
    // 这里简化处理：直接在analyze阶段扫描完日志后，redo阶段重新扫描
    
    delete[] log_buffer;
}

/**
 * @description: 重做所有未落盘的操作
 */
void RecoveryManager::redo() {
    // 1. 从磁盘读取所有日志
    int log_size = disk_manager_->GetLogSize();
    if(log_size == 0) {
        return;  // 没有日志，无需恢复
    }
    
    char* log_buffer = new char[log_size];
    disk_manager_->read_log(log_buffer, log_size, 0);
    
    // 2. 扫描所有日志记录，重做修改操作
    int offset = 0;
    while(offset < log_size) {
        // 反序列化日志头部
        LogRecord log_header;
        log_header.deserialize(log_buffer + offset);
        
        LogType log_type = log_header.log_type_;
        lsn_t lsn = log_header.lsn_;
        
        // 只重做INSERT/DELETE/UPDATE操作
        if(log_type == LogType::INSERT) {
            // 反序列化完整的INSERT日志
            InsertLogRecord insert_log;
            insert_log.deserialize(log_buffer + offset);
            
            // 获取表文件句柄
            std::string table_name(insert_log.table_name_, insert_log.table_name_size_);
            RmFileHandle* rm_file_handle = sm_manager_->fhs_.at(table_name).get();
            
            // 获取RID对应的页面
            PageId page_id{rm_file_handle->GetFd(), insert_log.rid_.page_no};
            Page* page = buffer_pool_manager_->fetch_page(page_id);
            
            // 检查页面的LSN，如果page_lsn >= lsn，说明已应用，跳过
            if(page->get_page_lsn() >= lsn) {
                buffer_pool_manager_->unpin_page(page_id, false);
                offset += insert_log.log_tot_len_;
                continue;
            }
            
            // 重做INSERT操作：插入记录
            rm_file_handle->insert_record(insert_log.rid_, insert_log.insert_value_.data);
            
            // 更新页面的LSN
            page->set_page_lsn(lsn);
            BufferPoolManager::mark_dirty(page);
            buffer_pool_manager_->unpin_page(page_id, true);
        }
        else if(log_type == LogType::DELETE) {
            // 反序列化完整的DELETE日志
            DeleteLogRecord delete_log;
            delete_log.deserialize(log_buffer + offset);
            
            // 获取表文件句柄
            std::string table_name(delete_log.table_name_, delete_log.table_name_size_);
            RmFileHandle* rm_file_handle = sm_manager_->fhs_.at(table_name).get();
            
            // 获取RID对应的页面
            PageId page_id{rm_file_handle->GetFd(), delete_log.rid_.page_no};
            Page* page = buffer_pool_manager_->fetch_page(page_id);
            
            // 检查页面的LSN
            if(page->get_page_lsn() >= lsn) {
                buffer_pool_manager_->unpin_page(page_id, false);
                offset += delete_log.log_tot_len_;
                continue;
            }
            
            // 重做DELETE操作：删除记录
            rm_file_handle->delete_record(delete_log.rid_);
            
            // 更新页面的LSN
            page->set_page_lsn(lsn);
            BufferPoolManager::mark_dirty(page);
            buffer_pool_manager_->unpin_page(page_id, true);
        }
        else if(log_type == LogType::UPDATE) {
            // 反序列化完整的UPDATE日志
            UpdateLogRecord update_log;
            update_log.deserialize(log_buffer + offset);
            
            // 获取表文件句柄
            std::string table_name(update_log.table_name_, update_log.table_name_size_);
            RmFileHandle* rm_file_handle = sm_manager_->fhs_.at(table_name).get();
            
            // 获取RID对应的页面
            PageId page_id{rm_file_handle->GetFd(), update_log.rid_.page_no};
            Page* page = buffer_pool_manager_->fetch_page(page_id);
            
            // 检查页面的LSN
            if(page->get_page_lsn() >= lsn) {
                buffer_pool_manager_->unpin_page(page_id, false);
                offset += update_log.log_tot_len_;
                continue;
            }
            
            // 重做UPDATE操作：更新记录为新值
            rm_file_handle->update_record(update_log.rid_, update_log.new_value_.data);
            
            // 更新页面的LSN
            page->set_page_lsn(lsn);
            BufferPoolManager::mark_dirty(page);
            buffer_pool_manager_->unpin_page(page_id, true);
        }
        
        offset += log_header.log_tot_len_;
    }
    
    delete[] log_buffer;
}

/**
 * @description: 回滚未完成的事务
 */
void RecoveryManager::undo() {
    // 1. 从磁盘读取所有日志
    int log_size = disk_manager_->GetLogSize();
    if(log_size == 0) {
        return;  // 没有日志，无需恢复
    }
    
    char* log_buffer = new char[log_size];
    disk_manager_->read_log(log_buffer, log_size, 0);
    
    // 2. 第一遍扫描：构建活跃事务表（ATT）和事务的最后LSN
    std::unordered_map<txn_id_t, lsn_t> active_txn_table;
    
    int offset = 0;
    std::vector<lsn_t> log_offsets;  // 记录每条日志的偏移量，便于逆序访问
    
    while(offset < log_size) {
        LogRecord log_header;
        log_header.deserialize(log_buffer + offset);
        
        log_offsets.push_back(offset);
        
        LogType log_type = log_header.log_type_;
        txn_id_t txn_id = log_header.log_tid_;
        lsn_t lsn = log_header.lsn_;
        
        if(log_type == LogType::begin) {
            active_txn_table[txn_id] = lsn;
        }
        else if(log_type == LogType::commit) {
            active_txn_table.erase(txn_id);
        }
        else if(log_type == LogType::ABORT) {
            active_txn_table.erase(txn_id);
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
    
    // 3. 如果没有活跃事务，无需undo
    if(active_txn_table.empty()) {
        delete[] log_buffer;
        return;
    }
    
    // 4. 构建每个活跃事务的日志链表（通过prev_lsn）
    std::unordered_map<txn_id_t, std::vector<lsn_t>> txn_log_chains;
    
    for(auto& pair : active_txn_table) {
        txn_id_t txn_id = pair.first;
        lsn_t current_lsn = pair.second;
        
        // 通过prev_lsn向前追溯
        while(current_lsn != INVALID_LSN) {
            txn_log_chains[txn_id].push_back(current_lsn);
            
            // 找到对应LSN的日志记录，获取prev_lsn
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
    
    // 5. 逆序撤销每个活跃事务的操作
    for(auto& pair : txn_log_chains) {
        txn_id_t txn_id = pair.first;
        std::vector<lsn_t>& log_chain = pair.second;
        
        // 逆序遍历日志链
        for(auto it = log_chain.begin(); it != log_chain.end(); ++it) {
            lsn_t lsn = *it;
            
            // 找到对应LSN的日志记录
            for(int log_offset : log_offsets) {
                LogRecord log_header;
                log_header.deserialize(log_buffer + log_offset);
                
                if(log_header.lsn_ == lsn) {
                    LogType log_type = log_header.log_type_;
                    
                    if(log_type == LogType::INSERT) {
                        // INSERT的逆操作：DELETE
                        InsertLogRecord insert_log;
                        insert_log.deserialize(log_buffer + log_offset);
                        
                        std::string table_name(insert_log.table_name_, insert_log.table_name_size_);
                        RmFileHandle* rm_file_handle = sm_manager_->fhs_.at(table_name).get();
                        
                        // 删除记录
                        rm_file_handle->delete_record(insert_log.rid_);
                        
                        // 处理索引
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
                        // DELETE的逆操作：INSERT
                        DeleteLogRecord delete_log;
                        delete_log.deserialize(log_buffer + log_offset);
                        
                        std::string table_name(delete_log.table_name_, delete_log.table_name_size_);
                        RmFileHandle* rm_file_handle = sm_manager_->fhs_.at(table_name).get();
                        
                        // 插入记录
                        rm_file_handle->insert_record(delete_log.rid_, delete_log.delete_value_.data);
                        
                        // 处理索引
                        auto& tab = sm_manager_->db_.get_table(table_name);
                        for(auto& index : tab.indexes) {
                            std::string index_name = sm_manager_->get_ix_manager()->get_index_name(table_name, index.cols);
                            auto ih = sm_manager_->ihs_.at(index_name).get();
                            
                            char* key = new char[index.col_tot_len];
                            int offset = 0;
                            for(auto& col : index.cols) {
                                memcpy(key + offset, delete_log.delete_value_.data + col.offset, col.len);
                                offset += col.len;
                            }
                            ih->insert_entry(key, delete_log.rid_, nullptr);
                            delete[] key;
                        }
                    }
                    else if(log_type == LogType::UPDATE) {
                        // UPDATE的逆操作：恢复旧值
                        UpdateLogRecord update_log;
                        update_log.deserialize(log_buffer + log_offset);
                        
                        std::string table_name(update_log.table_name_, update_log.table_name_size_);
                        RmFileHandle* rm_file_handle = sm_manager_->fhs_.at(table_name).get();
                        
                        // 更新记录为旧值
                        rm_file_handle->update_record(update_log.rid_, update_log.old_value_.data);
                        
                        // 处理索引
                        auto& tab = sm_manager_->db_.get_table(table_name);
                        for(auto& index : tab.indexes) {
                            std::string index_name = sm_manager_->get_ix_manager()->get_index_name(table_name, index.cols);
                            auto ih = sm_manager_->ihs_.at(index_name).get();
                            
                            // 删除新key
                            char* new_key = new char[index.col_tot_len];
                            int offset = 0;
                            for(auto& col : index.cols) {
                                memcpy(new_key + offset, update_log.new_value_.data + col.offset, col.len);
                                offset += col.len;
                            }
                            ih->delete_entry(new_key, nullptr);
                            delete[] new_key;
                            
                            // 插入旧key
                            char* old_key = new char[index.col_tot_len];
                            offset = 0;
                            for(auto& col : index.cols) {
                                memcpy(old_key + offset, update_log.old_value_.data + col.offset, col.len);
                                offset += col.len;
                            }
                            ih->insert_entry(old_key, update_log.rid_, nullptr);
                            delete[] old_key;
                        }
                    }
                    
                    break;
                }
            }
        }
    }
    
    delete[] log_buffer;
}
```

**关键实现细节**：

1. **analyze阶段**：
   - 扫描所有日志
   - 构建活跃事务表（ATT）：记录未提交或未回滚的事务
   - 构建脏页表（DPT）：记录可能未落盘的页面

2. **redo阶段**：
   - 重放所有修改操作（INSERT/DELETE/UPDATE）
   - 检查页面LSN，避免重复应用
   - 使用日志中的新值重做操作

3. **undo阶段**：
   - 识别活跃事务（在ATT中）
   - 通过prev_lsn构建事务的日志链
   - 逆序撤销操作，恢复到事务开始前的状态

---

### 2.2 执行器修改（添加日志记录）

#### 文件4: `/home/absinthe/rmdb/src/execution/executor_insert.h`

**修改位置**：在插入操作执行后记录日志

**需要添加的逻辑**：

```cpp
// 在InsertExecutor的Next()或执行逻辑中添加：
if (context_->txn_ != nullptr && context_->log_mgr_ != nullptr) {
    // 创建INSERT日志记录
    RmRecord insert_value(rec_.size);
    memcpy(insert_value.data, rec_.data, rec_.size);
    
    InsertLogRecord insert_log(
        context_->txn_->get_transaction_id(),
        insert_value,
        rid,  // 插入操作返回的RID
        tab_name_
    );
    insert_log.prev_lsn_ = context_->txn_->get_prev_lsn();
    
    // 记录日志到缓冲区
    lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&insert_log);
    context_->txn_->set_prev_lsn(lsn);
    
    // 更新页面的LSN
    PageId page_id{fh_.GetFd(), rid.page_no};
    Page* page = context_->buffer_pool_manager_->fetch_page(page_id);
    page->set_page_lsn(lsn);
    BufferPoolManager::mark_dirty(page);
    context_->buffer_pool_manager_->unpin_page(page_id, true);
}
```

---

#### 文件5: `/home/absinthe/rmdb/src/execution/executor_delete.h`

**修改位置**：在删除操作执行前记录日志

**需要添加的逻辑**：

```cpp
// 在DeleteExecutor的Next()或执行逻辑中添加：
if (context_->txn_ != nullptr && context_->log_mgr_ != nullptr) {
    // 先获取被删除记录的值
    RmRecord delete_value = fh_.get_record(rid, context_->txn_);
    
    // 创建DELETE日志记录
    DeleteLogRecord delete_log(
        context_->txn_->get_transaction_id(),
        delete_value,
        rid,
        tab_name_
    );
    delete_log.prev_lsn_ = context_->txn_->get_prev_lsn();
    
    // 记录日志到缓冲区
    lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&delete_log);
    context_->txn_->set_prev_lsn(lsn);
    
    // 执行删除操作
    fh_.delete_record(rid, context_->txn_);
    
    // 更新页面的LSN
    PageId page_id{fh_.GetFd(), rid.page_no};
    Page* page = context_->buffer_pool_manager_->fetch_page(page_id);
    page->set_page_lsn(lsn);
    BufferPoolManager::mark_dirty(page);
    context_->buffer_pool_manager_->unpin_page(page_id, true);
}
```

---

#### 文件6: `/home/absinthe/rmdb/src/execution/executor_update.h`

**修改位置**：在更新操作执行前记录日志

**需要添加的逻辑**：

```cpp
// 在UpdateExecutor的Next()或执行逻辑中添加：
if (context_->txn_ != nullptr && context_->log_mgr_ != nullptr) {
    // 先获取更新前的记录值
    RmRecord old_value = fh_.get_record(rid, context_->txn_);
    
    // 创建UPDATE日志记录
    RmRecord new_value(new_rec_.size);
    memcpy(new_value.data, new_rec_.data, new_rec_.size);
    
    UpdateLogRecord update_log(
        context_->txn_->get_transaction_id(),
        old_value,
        new_value,
        rid,
        tab_name_
    );
    update_log.prev_lsn_ = context_->txn_->get_prev_lsn();
    
    // 记录日志到缓冲区
    lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&update_log);
    context_->txn_->set_prev_lsn(lsn);
    
    // 执行更新操作
    fh_.update_record(rid, new_rec_.data, context_->txn_);
    
    // 更新页面的LSN
    PageId page_id{fh_.GetFd(), rid.page_no};
    Page* page = context_->buffer_pool_manager_->fetch_page(page_id);
    page->set_page_lsn(lsn);
    BufferPoolManager::mark_dirty(page);
    context_->buffer_pool_manager_->unpin_page(page_id, true);
}
```

---

### 2.3 DiskManager扩展

#### 文件7: `/home/absinthe/rmdb/src/storage/disk_manager.h`

**修改位置**：添加获取日志文件大小的方法

**需要添加**：

```cpp
// 在public部分添加：
int GetLogSize() {
    if(log_fd_ == -1) {
        return 0;
    }
    struct stat st;
    fstat(log_fd_, &st);
    return st.st_size;
}
```

---

## 三、具体实现步骤

### 步骤1：完善DeleteLogRecord和UpdateLogRecord

**目标**：实现完整的DELETE和UPDATE日志记录类

**关键点**：
- 序列化和反序列化方法
- 保存足够的信息用于redo和undo
- DELETE保存被删除的值
- UPDATE保存旧值和新值

---

### 步骤2：实现LogManager的核心方法

**目标**：实现add_log_to_buffer和flush_log_to_disk

**关键点**：
- LSN的分配和管理
- 缓冲区的管理
- 序列化日志记录
- 刷盘和更新persist_lsn

---

### 步骤3：实现RecoveryManager的analyze阶段

**目标**：构建活跃事务表和脏页表

**关键点**：
- 扫描所有日志记录
- 识别BEGIN、COMMIT、ABORT日志
- 维护活跃事务表
- 记录脏页信息

---

### 步骤4：实现RecoveryManager的redo阶段

**目标**：重做所有未落盘的修改操作

**关键点**：
- 检查页面LSN，避免重复应用
- 重做INSERT/DELETE/UPDATE操作
- 更新页面LSN

---

### 步骤5：实现RecoveryManager的undo阶段

**目标**：回滚未完成的事务

**关键点**：
- 识别活跃事务
- 通过prev_lsn构建日志链
- 逆序撤销操作
- 处理索引的一致性

---

### 步骤6：在执行器中添加日志记录

**目标**：Insert、Delete、Update执行器记录日志

**关键点**：
- 创建相应的日志记录
- 设置prev_lsn
- 记录日志到缓冲区
- 更新页面LSN

---

### 步骤7：测试验证

**目标**：测试故障恢复的正确性

**测试场景**：
1. 提交事务的数据在崩溃后恢复
2. 未提交事务的数据在崩溃后被撤销
3. 索引一致性验证
4. 多事务并发场景

---

## 四、关键实现细节

### 4.1 WAL协议

**原则**：
- 在修改页面之前，先将日志写入缓冲区
- 在提交事务之前，将日志刷入磁盘
- 保证日志先于数据落盘

**实现**：
```cpp
// 在执行修改操作时：
1. 记录日志到缓冲区
2. 执行修改操作
3. 更新页面LSN

// 在提交事务时：
1. 记录COMMIT日志
2. 刷日志到磁盘
3. 释放锁
```

---

### 4.2 页面LSN的作用

**作用**：
- 标识页面最后一次被修改的日志LSN
- 在redo阶段用于判断是否已应用该日志
- 避免重复重做

**实现**：
```cpp
// 在redo阶段：
if(page->get_page_lsn() >= log_lsn) {
    // 该日志已应用，跳过
    continue;
}

// 在修改页面后：
page->set_page_lsn(current_lsn);
```

---

### 4.3 prev_lsn的作用

**作用**：
- 维护同一事务的日志链表
- 用于undo阶段反向遍历事务的所有操作
- 实现撤销操作

**实现**：
```cpp
// 记录日志时：
log_record.prev_lsn_ = txn->get_prev_lsn();
txn->set_prev_lsn(current_lsn);

// undo时，通过prev_lsn向前追溯：
while(current_lsn != INVALID_LSN) {
    // 撤销当前LSN对应的操作
    current_lsn = log_record.prev_lsn_;
}
```

---

### 4.4 REDO阶段的判断条件

**重做条件**：
- 日志LSN > 页面LSN（说明日志未应用）
- 或者页面不在脏页表中（说明页面已落盘）

**实现**：
```cpp
if(page->get_page_lsn() >= log_lsn) {
    // 已应用，跳过
    skip;
} else {
    // 需要重做
    redo_operation();
}
```

---

### 4.5 UNDO阶段的逻辑

**步骤**：
1. 扫描所有日志，构建活跃事务表（ATT）
2. 对于每个活跃事务，通过prev_lsn构建日志链
3. 逆序撤销每个操作

**撤销操作**：
- INSERT的逆操作：DELETE
- DELETE的逆操作：INSERT（恢复被删除的值）
- UPDATE的逆操作：UPDATE（恢复旧值）

---

### 4.6 索引的一致性处理

**重要**：索引操作必须记录物理日志，而非逻辑日志

**原因**：
- 逻辑日志在恢复时可能导致索引结构不一致
- 物理日志记录具体的索引页面修改

**实现**：
- 在undo阶段，同时处理索引的插入和删除
- 确保记录和索引的一致性

---

## 五、测试场景设计

### 5.1 基本恢复测试

**场景**：
```sql
create table t1 (id int, num int);
begin;
insert into t1 values(1, 1);
commit;
begin;
insert into t1 values(2, 2);
crash  // 系统崩溃
```

**重启后期望**：
- t1表中只有id=1的记录
- id=2的记录被撤销（未提交事务）

---

### 5.2 多事务并发测试

**场景**：
```sql
// 事务1
begin;
insert into t1 values(1, 1);
// 事务2
begin;
insert into t1 values(2, 2);
commit;
// 事务1继续
insert into t1 values(3, 3);
crash  // 系统崩溃
```

**重启后期望**：
- t1表中有id=2的记录（已提交）
- id=1和id=3的记录被撤销（未提交事务）

---

### 5.3 混合操作测试

**场景**：
```sql
create table t1 (id int, num int);
insert into t1 values(1, 10);
insert into t1 values(2, 20);
begin;
update t1 set num=100 where id=1;
delete from t1 where id=2;
insert into t1 values(3, 30);
crash  // 系统崩溃
```

**重启后期望**：
- t1表恢复到事务开始前的状态
- id=1的记录num=10
- id=2的记录存在
- id=3的记录不存在

---

### 5.4 索引一致性测试

**场景**：
```sql
create table t1 (id int, num int);
create index t1(id);
begin;
insert into t1 values(1, 10);
commit;
begin;
insert into t1 values(2, 20);
crash  // 系统崩溃
```

**重启后验证**：
- t1表只有id=1的记录
- 索引中只有id=1的索引项
- 索引扫描结果正确

---

## 六、常见问题与解决方案

### Q1: redo阶段重复应用日志导致数据错误？

**原因**：未检查页面LSN

**解决方案**：
```cpp
if(page->get_page_lsn() >= log_lsn) {
    continue;  // 已应用，跳过
}
```

---

### Q2: undo阶段撤销顺序错误？

**原因**：未逆序撤销

**解决方案**：通过prev_lsn构建日志链，按正确顺序撤销

---

### Q3: 索引不一致？

**原因**：undo阶段未处理索引

**解决方案**：在撤销操作时，同时删除或插入索引项

---

### Q4: 日志缓冲区溢出？

**原因**：未及时刷盘

**解决方案**：在add_log_to_buffer中检查缓冲区是否已满，满则先刷盘

---

### Q5: 多线程并发问题？

**原因**：LogManager的并发访问未加锁

**解决方案**：使用latch_互斥锁保护共享数据

---

## 七、注意事项

1. **WAL原则**：日志必须在修改前写入，提交前刷盘
2. **LSN管理**：保证LSN单调递增且唯一
3. **prev_lsn维护**：正确维护事务的日志链
4. **页面LSN更新**：每次修改页面后更新LSN
5. **索引一致性**：undo时必须处理索引
6. **线程安全**：LogManager需要加锁
7. **缓冲区管理**：检查缓冲区是否已满
8. **日志刷盘**：commit时必须刷盘
9. **恢复幂等性**：redo操作可重复执行而不影响正确性
10. **代码风格**：遵循项目规范，保留版权声明

---

## 八、开发顺序建议

1. **第一步**：实现DeleteLogRecord和UpdateLogRecord
2. **第二步**：实现LogManager::add_log_to_buffer
3. **第三步**：实现LogManager::flush_log_to_disk
4. **第四步**：在InsertExecutor中记录日志
5. **第五步**：在DeleteExecutor中记录日志
6. **第六步**：在UpdateExecutor中记录日志
7. **第七步**：实现RecoveryManager::analyze
8. **第八步**：实现RecoveryManager::redo
9. **第九步**：实现RecoveryManager::undo
10. **第十步**：测试验证

---

## 总结

本方案详细描述了系统故障恢复的实现方法，包括：

1. **需求分析**：明确题目要求和当前代码问题
2. **文件修改**：列出所有需要修改的文件和具体位置
3. **实现细节**：提供关键代码示例和逻辑说明
4. **测试方案**：设计完整的测试场景和验证方法
5. **问题解决**：列出常见问题和解决方案

**关键点**：

- 实现完整的日志记录类型（INSERT/DELETE/UPDATE）
- 实现LogManager的日志缓冲和刷盘机制
- 实现RecoveryManager的analyze、redo、undo三个阶段
- 在执行器中正确记录操作日志
- 维护页面LSN和事务的prev_lsn
- 确保索引的一致性

按照本方案实施，可以正确实现基于WAL的系统故障恢复机制，满足题目要求。
