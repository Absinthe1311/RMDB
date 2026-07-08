# 基于死锁预防的可串行化隔离级别实现方案

## 一、功能需求分析

### 1.1 题目要求

根据题目描述，需要实现以下功能：

1. **两阶段封锁协议（2PL）**
   - 保证可串行化隔离级别
   - 增长阶段：只能申请锁，不能释放锁
   - 收缩阶段：只能释放锁，不能申请锁

2. **死锁预防策略**
   - 实现no-wait算法
   - 检测到冲突时立即回滚其中一个事务
   - 避免死锁的形成

3. **防止五种数据异常**
   - 脏写（Dirty Write）
   - 脏读（Dirty Read）
   - 丢失更新（Lost Update）
   - 不可重复读（Non-repeatable Read）
   - 幻读（Phantom Read）

4. **多粒度锁协议**
   - 表级锁（S、X）
   - 行级锁（S、X）
   - 意向锁（IS、IX、SIX）

5. **幻读预防（可选）**
   - 表级锁方案（简单）
   - 间隙锁方案（性能更好）

### 1.2 当前代码框架分析

#### 已有实现：

1. **锁管理器框架（LockManager）**：已定义
   - LockRequest类：锁请求
   - LockRequestQueue类：锁请求队列
   - LockMode枚举：SHARED、EXLUCSIVE、INTENTION_SHARED、INTENTION_EXCLUSIVE、S_IX
   - GroupLockMode枚举：IS、IX、S、X、SIX
   - lock_table_：全局锁表

2. **锁对象标识（LockDataId）**：已实现
   - 支持表级锁和行级锁
   - 支持哈希函数

3. **事务对象（Transaction）**：已实现
   - lock_set_：维护事务持有的所有锁
   - start_ts_：事务时间戳（用于死锁预防）

4. **事务管理器（TransactionManager）**：已实现
   - begin()、commit()、abort()
   - next_timestamp_：时间戳生成器

5. **事务回滚异常（TransactionAbortException）**：已定义
   - AbortReason：LOCK_ON_SHIRINKING、UPGRADE_CONFLICT、DEADLOCK_PREVENTION

#### 当前问题：

1. **LockManager所有加锁方法未实现**：
   ```cpp
   bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
       return true;  // 未实现
   }
   ```

2. **锁兼容性检查未实现**：缺少锁兼容性矩阵

3. **锁升级机制未实现**：S锁升级为X锁

4. **死锁预防逻辑未实现**：no-wait策略未实现

5. **两阶段锁协议未强制执行**：未检查增长/收缩阶段

---

## 二、需要修改/新增的文件

### 2.1 核心修改文件

#### 文件1: `/home/absinthe/rmdb/src/transaction/concurrency/lock_manager.h`

**修改位置**：第19-67行

**需要添加的内容**：

1. **锁兼容性矩阵检查函数**
   ```cpp
   private:
       // 检查锁兼容性
       bool is_lock_compatible(LockMode lock_mode, GroupLockMode group_lock_mode);
       
       // 更新锁队列的组锁模式
       void update_group_lock_mode(LockRequestQueue& queue);
       
       // 检查是否可以锁升级
       bool can_upgrade(LockMode old_mode, LockMode new_mode);
       
       // 锁兼容性矩阵
       static const bool lock_compatibility_matrix_[5][5];
   ```

2. **辅助函数**
   ```cpp
   private:
       // 检查事务是否已持有该数据项的锁
       bool is_lock_hold(Transaction* txn, const LockDataId& lock_data_id, LockMode& current_mode);
       
       // 检查队列中是否有其他事务在等待
       bool has_waiting_transaction(LockRequestQueue& queue, txn_id_t exclude_txn_id);
   ```

**修改原因**：
- 需要锁兼容性检查机制
- 需要辅助函数简化主逻辑

---

#### 文件2: `/home/absinthe/rmdb/src/transaction/concurrency/lock_manager.cpp`

**修改位置**：第20-90行，所有加锁和解锁方法

**实现框架**：

```cpp
// 锁兼容性矩阵定义
const bool LockManager::lock_compatibility_matrix_[5][5] = {
    //       IS    IX    S     X     SIX
    /*IS*/ {true,  true,  true,  false, false},
    /*IX*/ {true,  true,  false, false, false},
    /*S */ {true,  false, true,  false, false},
    /*X */ {false, false, false, false, false},
    /*SIX*/{false, false, false, false, false}
};

bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    // 1. 检查事务状态（两阶段锁协议）
    if(txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }
    
    // 2. 构造锁对象ID
    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    
    // 3. 加锁表的latch
    std::unique_lock<std::mutex> lock(latch_);
    
    // 4. 获取或创建锁请求队列
    auto& lock_queue = lock_table_[lock_data_id];
    
    // 5. 检查是否已持有锁
    LockMode current_mode;
    if(is_lock_hold(txn, lock_data_id, current_mode)) {
        // 已持有锁，可能需要锁升级
        if(current_mode == LockMode::SHARED) {
            // 持有S锁，不需要升级
            return true;
        }
        // 其他情况...
    }
    
    // 6. 检查锁兼容性
    if(!is_lock_compatible(LockMode::SHARED, lock_queue.group_lock_mode_)) {
        // 不兼容，死锁预防：no-wait策略
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }
    
    // 7. 创建锁请求并授予锁
    LockRequest lock_request(txn->get_transaction_id(), LockMode::SHARED);
    lock_request.granted_ = true;
    lock_queue.request_queue_.push_back(lock_request);
    
    // 8. 更新组锁模式
    update_group_lock_mode(lock_queue);
    
    // 9. 将锁加入事务的锁集
    txn->get_lock_set()->insert(lock_data_id);
    
    return true;
}

bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    // 类似lock_shared_on_record，但锁模式为EXLUCSIVE
    // ...
}

bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    // 表级共享锁
    // ...
}

bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    // 表级排他锁
    // ...
}

bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    // 表级意向共享锁
    // ...
}

bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    // 表级意向排他锁
    // ...
}

bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    // 1. 加锁表的latch
    std::unique_lock<std::mutex> lock(latch_);
    
    // 2. 查找锁请求队列
    auto it = lock_table_.find(lock_data_id);
    if(it == lock_table_.end()) {
        return false;
    }
    
    auto& lock_queue = it->second;
    
    // 3. 在队列中查找该事务的锁请求
    for(auto iter = lock_queue.request_queue_.begin(); 
        iter != lock_queue.request_queue_.end(); ++iter) {
        if(iter->txn_id_ == txn->get_transaction_id()) {
            // 4. 移除锁请求
            lock_queue.request_queue_.erase(iter);
            
            // 5. 更新组锁模式
            update_group_lock_mode(lock_queue);
            
            // 6. 从事务的锁集中移除
            txn->get_lock_set()->erase(lock_data_id);
            
            // 7. 如果队列为空，从锁表中移除
            if(lock_queue.request_queue_.empty()) {
                lock_table_.erase(it);
            }
            
            return true;
        }
    }
    
    return false;
}

// 辅助函数实现
bool LockManager::is_lock_compatible(LockMode lock_mode, GroupLockMode group_lock_mode) {
    if(group_lock_mode == GroupLockMode::NON_LOCK) {
        return true;
    }
    int mode_idx = static_cast<int>(lock_mode);
    int group_idx = static_cast<int>(group_lock_mode) - 1;  // NON_LOCK为0，其他从1开始
    return lock_compatibility_matrix_[mode_idx][group_idx];
}

void LockManager::update_group_lock_mode(LockRequestQueue& queue) {
    queue.group_lock_mode_ = GroupLockMode::NON_LOCK;
    for(auto& request : queue.request_queue_) {
        if(!request.granted_) continue;
        
        LockMode mode = request.lock_mode_;
        GroupLockMode group_mode;
        switch(mode) {
            case LockMode::INTENTION_SHARED:
                group_mode = GroupLockMode::IS;
                break;
            case LockMode::INTENTION_EXCLUSIVE:
                group_mode = GroupLockMode::IX;
                break;
            case LockMode::SHARED:
                group_mode = GroupLockMode::S;
                break;
            case LockMode::EXLUCSIVE:
                group_mode = GroupLockMode::X;
                break;
            case LockMode::S_IX:
                group_mode = GroupLockMode::SIX;
                break;
        }
        
        // 更新为排他性更强的锁模式
        if(static_cast<int>(group_mode) > static_cast<int>(queue.group_lock_mode_)) {
            queue.group_lock_mode_ = group_mode;
        }
    }
}

bool LockManager::is_lock_hold(Transaction* txn, const LockDataId& lock_data_id, LockMode& current_mode) {
    auto it = lock_table_.find(lock_data_id);
    if(it == lock_table_.end()) {
        return false;
    }
    
    auto& lock_queue = it->second;
    for(auto& request : lock_queue.request_queue_) {
        if(request.txn_id_ == txn->get_transaction_id() && request.granted_) {
            current_mode = request.lock_mode_;
            return true;
        }
    }
    
    return false;
}
```

**关键实现细节**：

1. **两阶段锁协议检查**：
   - 在申请锁前，检查事务状态是否为SHRINKING
   - 如果是，抛出LOCK_ON_SHIRINKING异常

2. **No-wait死锁预防**：
   - 检查锁兼容性时，如果不兼容，立即抛出DEADLOCK_PREVENTION异常
   - 不等待，直接回滚当前事务

3. **锁兼容性矩阵**：
   ```
         IS    IX    S     X     SIX
   IS    √     √     √     ×     ×
   IX    √     √     ×     ×     ×
   S     √     ×     √     ×     ×
   X     ×     ×     ×     ×     ×
   SIX   ×     ×     ×     ×     ×
   ```

4. **锁升级**：
   - 如果事务已持有S锁，申请X锁时需要升级
   - 检查是否有其他事务持有不兼容的锁
   - 如果有冲突，回滚当前事务

5. **锁释放**：
   - 从锁请求队列中移除
   - 更新组锁模式
   - 从事务的锁集中移除

---

#### 文件3: `/home/absinthe/rmdb/src/transaction/transaction_manager.cpp`

**修改位置**：commit和abort函数，释放所有锁

**当前代码**：
```cpp
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    // ... 现有逻辑
    
    // 释放所有锁
    auto lock_set = txn->get_lock_set();
    for(auto& lock_data_id : *lock_set) {
        lock_manager_->unlock(txn, lock_data_id);
    }
    
    // ... 
}
```

**需要确保**：
- commit时正确释放所有锁
- abort时正确释放所有锁
- 设置事务状态为SHRINKING

---

### 2.2 执行器修改（添加加锁逻辑）

#### 文件4: `/home/absinthe/rmdb/src/execution/executor_seq_scan.h`

**修改位置**：Next()函数，读取记录前加S锁

**需要添加的逻辑**：
```cpp
std::unique_ptr<RmRecord> Next() override {
    if(is_end()) return nullptr;
    
    RmRecord* rec = fh_.get_record(rid_, context_->txn_);
    
    // 加行级共享锁
    if(context_->txn_ != nullptr) {
        lock_manager_->lock_shared_on_record(
            context_->txn_, 
            rid_, 
            fh_.GetFd()
        );
    }
    
    rid_ = scan_.rid();
    return std::make_unique<RmRecord>(*rec);
}
```

---

#### 文件5: `/home/absinthe/rmdb/src/execution/executor_insert.h`

**修改位置**：Next()函数，插入前加X锁

**需要添加的逻辑**：
```cpp
std::unique_ptr<RmRecord> Next() override {
    // 插入记录
    Rid rid = fh_.insert_record(rec_.data, context_->txn_);
    
    // 加行级排他锁
    if(context_->txn_ != nullptr) {
        lock_manager_->lock_exclusive_on_record(
            context_->txn_, 
            rid, 
            fh_.GetFd()
        );
    }
    
    // 记录写操作
    WriteRecord* wr = new WriteRecord(WType::INSERT_TUPLE, tab_name_, rid);
    context_->txn_->append_write_record(wr);
    
    return nullptr;
}
```

---

#### 文件6: `/home/absinthe/rmdb/src/execution/executor_delete.h`

**修改位置**：Next()函数，删除前加X锁

**需要添加的逻辑**：
```cpp
std::unique_ptr<RmRecord> Next() override {
    for(auto& rid : rids_) {
        // 加行级排他锁
        if(context_->txn_ != nullptr) {
            lock_manager_->lock_exclusive_on_record(
                context_->txn_, 
                rid, 
                fh_.GetFd()
            );
        }
        
        // 先获取记录值（用于回滚）
        RmRecord rec = fh_.get_record(rid, context_->txn_);
        
        // 删除记录
        fh_.delete_record(rid, context_->txn_);
        
        // 记录写操作
        WriteRecord* wr = new WriteRecord(WType::DELETE_TUPLE, tab_name_, rid, rec);
        context_->txn_->append_write_record(wr);
    }
    
    return nullptr;
}
```

---

#### 文件7: `/home/absinthe/rmdb/src/execution/executor_update.h`

**修改位置**：Next()函数，更新前加X锁

**需要添加的逻辑**：
```cpp
std::unique_ptr<RmRecord> Next() override {
    for(auto& rid : rids_) {
        // 加行级排他锁
        if(context_->txn_ != nullptr) {
            lock_manager_->lock_exclusive_on_record(
                context_->txn_, 
                rid, 
                fh_.GetFd()
            );
        }
        
        // 先获取旧值（用于回滚）
        RmRecord old_rec = fh_.get_record(rid, context_->txn_);
        
        // 更新记录
        fh_.update_record(rid, new_rec_.data, context_->txn_);
        
        // 记录写操作
        WriteRecord* wr = new WriteRecord(WType::UPDATE_TUPLE, tab_name_, rid, old_rec);
        context_->txn_->append_write_record(wr);
    }
    
    return nullptr;
}
```

---

### 2.3 索引加锁（防止幻读）

#### 文件8: `/home/absinthe/rmdb/src/index/ix_index_handle.h`

**修改位置**：insert_entry和delete_entry函数

**需要添加的逻辑**：
```cpp
// 插入索引项
bool insert_entry(char* key, const Rid& rid, Transaction* txn) {
    // 加表级意向排他锁（IX锁）
    if(txn != nullptr) {
        lock_manager_->lock_IX_on_table(txn, fd_);
    }
    
    // ... 插入逻辑
}

// 删除索引项
bool delete_entry(char* key, const Rid& rid, Transaction* txn) {
    // 加表级意向排他锁（IX锁）
    if(txn != nullptr) {
        lock_manager_->lock_IX_on_table(txn, fd_);
    }
    
    // ... 删除逻辑
}
```

**幻读预防策略（两种选择）**：

**方案A：表级锁（简单）**
- 在范围查询时，加表级S锁
- 在插入/删除时，加表级X锁
- 优点：实现简单
- 缺点：并发度低

**方案B：间隙锁（推荐）**
- 在B+树叶节点上加锁
- 锁住键值之间的间隙
- 防止其他事务在间隙中插入
- 优点：并发度高
- 缺点：实现复杂

**建议先实现方案A（表级锁），测试通过后再考虑方案B**

---

### 2.4 客户端输出处理

#### 文件9: `/home/absinthe/rmdb/src/rmdb.cpp`

**修改位置**：异常捕获部分

**需要添加**：
```cpp
try {
    // ... 执行SQL
} catch(TransactionAbortException& e) {
    if(e.GetAbortReason() == AbortReason::DEADLOCK_PREVENTION) {
        std::cout << "abort\n";
    }
    // ... 其他处理
}
```

**注意**：不要修改record_printer.h，输出格式保持不变

---

## 三、具体实现步骤

### 步骤1：实现锁兼容性矩阵

**目标**：在lock_manager.h中定义锁兼容性矩阵

**关键点**：
- IS、IX、S、X、SIX五种锁模式
- 依据多粒度锁协议规则

---

### 步骤2：实现辅助函数

**目标**：实现is_lock_compatible、update_group_lock_mode等辅助函数

**关键点**：
- 锁兼容性检查
- 组锁模式更新
- 锁持有检查

---

### 步骤3：实现行级锁

**目标**：实现lock_shared_on_record和lock_exclusive_on_record

**关键点**：
- 两阶段锁协议检查
- No-wait死锁预防
- 锁升级处理
- 锁请求队列维护

---

### 步骤4：实现表级锁

**目标**：实现lock_shared_on_table、lock_exclusive_on_table、lock_IS_on_table、lock_IX_on_table

**关键点**：
- 类似行级锁
- 注意与行级锁的兼容性

---

### 步骤5：实现锁释放

**目标**：实现unlock函数

**关键点**：
- 从锁请求队列移除
- 更新组锁模式
- 从事务锁集移除

---

### 步骤6：在执行器中添加加锁逻辑

**目标**：修改seq_scan、insert、delete、update执行器

**关键点**：
- 读操作加S锁
- 写操作加X锁
- 记录写操作到write_set

---

### 步骤7：实现幻读预防（表级锁方案）

**目标**：在索引操作和范围查询时加表级锁

**关键点**：
- 范围查询加表级S锁
- 插入/删除加表级X锁或IX锁

---

### 步骤8：测试验证

**目标**：测试五种数据异常的预防

**测试场景**：
1. 脏写测试
2. 脏读测试
3. 丢失更新测试
4. 不可重复读测试
5. 幻读测试

---

## 四、关键实现细节

### 4.1 两阶段锁协议

**增长阶段（GROWING）**：
- 只能申请锁
- 事务开始时状态为GROWING

**收缩阶段（SHRINKING）**：
- 只能释放锁
- commit/abort时进入SHRINKING
- 申请锁时抛出LOCK_ON_SHIRINKING异常

**实现**：
```cpp
if(txn->get_state() == TransactionState::SHRINKING) {
    throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
}
```

---

### 4.2 No-wait死锁预防

**原理**：
- 当锁请求与已持有的锁不兼容时
- 不等待，立即回滚当前事务
- 避免死锁的形成

**实现**：
```cpp
if(!is_lock_compatible(lock_mode, lock_queue.group_lock_mode_)) {
    throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
}
```

**优点**：
- 实现简单
- 避免死锁检测的开销

**缺点**：
- 可能导致不必要的事务回滚
- 降低并发度

---

### 4.3 锁兼容性矩阵

**多粒度锁协议规则**：

1. **意向锁兼容性**：
   - IS与IS、IX兼容（可以并发读不同记录）
   - IX与IX兼容（可以并发写不同记录）
   - IS与IX兼容

2. **共享锁兼容性**：
   - S与IS、S兼容（多个事务可并发读）
   - S与IX不兼容（防止写冲突）

3. **排他锁兼容性**：
   - X与任何锁都不兼容（独占访问）

4. **SIX锁兼容性**：
   - SIX与IS兼容
   - SIX与其他锁不兼容

**矩阵定义**：
```cpp
const bool LockManager::lock_compatibility_matrix_[5][5] = {
    //       IS    IX    S     X     SIX
    /*IS*/ {true,  true,  true,  false, false},
    /*IX*/ {true,  true,  false, false, false},
    /*S */ {true,  false, true,  false, false},
    /*X */ {false, false, false, false, false},
    /*SIX*/{false, false, false, false, false}
};
```

---

### 4.4 锁升级

**场景**：
- 事务已持有S锁
- 后续需要X锁（如读后写）

**实现**：
```cpp
LockMode current_mode;
if(is_lock_hold(txn, lock_data_id, current_mode)) {
    if(current_mode == LockMode::SHARED && lock_mode == LockMode::EXLUCSIVE) {
        // 检查是否有其他事务持有不兼容的锁
        if(has_conflict_lock(lock_queue, txn->get_transaction_id())) {
            // 锁升级冲突，回滚
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::UPGRADE_CONFLICT);
        }
        
        // 执行锁升级：S锁 -> X锁
        update_lock_mode(lock_queue, txn->get_transaction_id(), LockMode::EXLUCSIVE);
        return true;
    }
}
```

---

### 4.5 幻读预防

**方案A：表级锁**

**范围查询**：
```cpp
// select * from t where id > 10 and id < 100
// 加表级S锁
lock_manager_->lock_shared_on_table(txn, tab_fd);
```

**插入操作**：
```cpp
// insert into t values (...)
// 加表级IX锁（意向排他锁）
lock_manager_->lock_IX_on_table(txn, tab_fd);
```

**原理**：
- S锁与IX锁不兼容
- 范围查询时，阻止其他事务插入
- 防止幻读

---

**方案B：间隙锁（高级）**

**原理**：
- 在B+树叶节点上加锁
- 锁住(key1, key2)之间的间隙
- 防止其他事务在此间隙插入

**实现位置**：`ix_index_handle.h`

**间隙锁定义**：
```cpp
class GapLock {
    int fd_;           // 表文件描述符
    char* left_key_;   // 左边界
    char* right_key_;  // 右边界
    txn_id_t txn_id_;  // 持有锁的事务ID
};
```

**加锁时机**：
- 范围查询时，对扫描过的间隙加锁

---

## 五、测试场景设计

### 5.1 脏写测试

**场景**：
```
事务1: update t set score=100 where id=2
事务2: update t set score=80 where id=2
```

**期望**：
- 事务2会因锁冲突被回滚
- 或等待事务1提交后再执行

**验证**：
- 最终结果应该是事务1或事务2的更新结果
- 不会出现部分更新

---

### 5.2 脏读测试

**场景**：
```
事务1: update t set score=100 where id=2; abort
事务2: select * from t where id=2 (在事务1未提交时)
```

**期望**：
- 事务2读取到的是事务1提交前的值
- 或事务2等待事务1提交/回滚

**验证**：
- 事务1回滚后，事务2看到的应该是原值

---

### 5.3 丢失更新测试

**场景**：
```
事务1: read(x), x=x+1, write(x)
事务2: read(x), x=x+1, write(x)
```

**期望**：
- 一个事务成功，另一个因锁冲突回滚
- 或串行执行

**验证**：
- 最终x的值应该是原值+2

---

### 5.4 不可重复读测试

**场景**：
```
事务1: select * from t where id=2 (第一次读)
事务2: update t set score=100 where id=2; commit
事务1: select * from t where id=2 (第二次读)
```

**期望**：
- 事务1两次读到的值相同（可重复读）
- 或事务2等待事务1提交

**验证**：
- 事务1两次读到的score值相同

---

### 5.5 幻读测试

**场景**：
```
事务1: select * from t where id>1 and id<10 (第一次读，读到N条)
事务2: insert into t values (5, 'new', 90.0); commit
事务1: select * from t where id>1 and id<10 (第二次读)
```

**期望**：
- 事务1两次读到的记录数相同
- 或事务2等待事务1提交

**验证**：
- 事务1两次读到的记录数相同

---

## 六、常见问题与解决方案

### Q1: 死锁预防导致大量回滚？

**原因**：No-wait策略过于激进

**解决方案**：
- 可以改用Wound-Wait策略（时间戳小的伤害时间戳大的）
- 或Wait-Die策略（时间戳大的等待时间戳小的）

---

### Q2: 锁升级冲突频繁？

**原因**：读后写场景较多

**解决方案**：
- 优化事务逻辑，减少锁升级
- 或直接申请X锁（如果确定会写）

---

### Q3: 幻读仍然存在？

**原因**：间隙锁未实现或表级锁未正确加

**解决方案**：
- 确保范围查询加表级S锁
- 或实现间隙锁

---

### Q4: 并发度太低？

**原因**：加锁粒度太粗

**解决方案**：
- 使用意向锁提高并发度
- 使用间隙锁代替表级锁

---

### Q5: 性能下降明显？

**原因**：锁争用严重

**解决方案**：
- 优化事务逻辑，缩短持有锁的时间
- 减少长事务

---

## 七、开发顺序建议

1. **第一步**：实现锁兼容性矩阵和辅助函数
2. **第二步**：实现行级共享锁和排他锁
3. **第三步**：实现锁释放函数
4. **第四步**：在执行器中添加加锁逻辑
5. **第五步**：测试基本功能（脏写、脏读）
6. **第六步**：实现表级锁和意向锁
7. **第七步**：测试幻读预防
8. **第八步**：优化性能
9. **第九步**：（可选）实现间隙锁

---

## 八、注意事项

1. **线程安全**：锁表的并发访问需要加latch
2. **异常处理**：正确处理事务回滚异常
3. **资源释放**：commit/abort时释放所有锁
4. **死锁预防**：No-wait策略的实现
5. **锁升级**：正确处理锁升级冲突
6. **两阶段锁**：强制执行两阶段锁协议
7. **输出格式**：不要修改record_printer.h
8. **回滚信息**：死锁预防回滚返回"abort\n"
9. **代码风格**：遵循项目现有的代码规范
10. **版权声明**：在文件头部保留版权声明

---

## 九、性能优化建议

### 9.1 锁粒度优化
- 优先使用行级锁
- 必要时使用表级锁
- 使用意向锁提高并发度

### 9.2 锁持有时间优化
- 尽量缩短事务执行时间
- 减少锁持有时间

### 9.3 死锁预防策略优化
- No-wait -> Wound-Wait或Wait-Die
- 减少不必要的事务回滚

---

## 总结

本方案详细描述了基于死锁预防的可串行化隔离级别的实现方法，包括：

1. **需求分析**：明确题目要求和当前代码问题
2. **文件修改**：列出所有需要修改的文件和具体位置
3. **实现细节**：提供关键代码示例和逻辑说明
4. **测试方案**：设计完整的测试场景和验证方法
5. **问题解决**：列出常见问题和解决方案

**关键点**：

- 实现锁兼容性矩阵和辅助函数
- 实现行级锁和表级锁
- 实现No-wait死锁预防策略
- 在执行器中添加加锁逻辑
- 防止五种数据异常
- 实现幻读预防（表级锁或间隙锁）
- 强制执行两阶段锁协议

按照本方案实施，可以正确实现基于死锁预防的可串行化隔离级别，满足题目要求。
