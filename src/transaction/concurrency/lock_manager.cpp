/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"
#include "transaction/txn_defs.h"

// 锁兼容性矩阵定义
const bool LockManager::lock_compatibility_matrix_[5][5] = {
    //       IS    IX    S     X     SIX
    /*IS*/ {true,  true,  true,  false, false},
    /*IX*/ {true,  true,  false, false, false},
    /*S */ {true,  false, true,  false, false},
    /*X */ {false, false, false, false, false},
    /*SIX*/{false, false, false, false, false}
};

/**
 * @description: 申请行级共享锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID 记录所在的表的fd
 * @param {int} tab_fd
 */
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
        // 已持有锁，检查是否需要升级
        if(current_mode == LockMode::SHARED || current_mode == LockMode::EXLUCSIVE || 
           current_mode == LockMode::S_IX) {
            // 已持有S或更强的锁，无需重复申请
            return true;
        }
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

/**
 * @description: 申请行级排他锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID
 * @param {int} tab_fd 记录所在的表的fd
 */
bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
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
        // 已持有锁，检查是否需要升级
        if(current_mode == LockMode::EXLUCSIVE) {
            // 已持有X锁，无需重复申请
            return true;
        } else if(current_mode == LockMode::SHARED) {
            // 持有S锁，需要升级为X锁
            // 检查是否有其他事务持有不兼容的锁
            if(has_conflict_lock(lock_queue, txn->get_transaction_id(), LockMode::EXLUCSIVE)) {
                // 锁升级冲突，回滚
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::UPGRADE_CONFLICT);
            }
            
            // 执行锁升级：修改锁请求的模式
            for(auto& request : lock_queue.request_queue_) {
                if(request.txn_id_ == txn->get_transaction_id() && request.granted_) {
                    request.lock_mode_ = LockMode::EXLUCSIVE;
                    break;
                }
            }
            
            // 更新组锁模式
            update_group_lock_mode(lock_queue);
            return true;
        }
    }
    
    // 6. 检查锁兼容性
    if(!is_lock_compatible(LockMode::EXLUCSIVE, lock_queue.group_lock_mode_)) {
        // 不兼容，死锁预防：no-wait策略
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }
    
    // 7. 创建锁请求并授予锁
    LockRequest lock_request(txn->get_transaction_id(), LockMode::EXLUCSIVE);
    lock_request.granted_ = true;
    lock_queue.request_queue_.push_back(lock_request);
    
    // 8. 更新组锁模式
    update_group_lock_mode(lock_queue);
    
    // 9. 将锁加入事务的锁集
    txn->get_lock_set()->insert(lock_data_id);
    
    return true;
}

/**
 * @description: 申请表级读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    // 1. 检查事务状态（两阶段锁协议）
    if(txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }
    
    // 2. 构造锁对象ID（表级锁）
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    
    // 3. 加锁表的latch
    std::unique_lock<std::mutex> lock(latch_);
    
    // 4. 获取或创建锁请求队列
    auto& lock_queue = lock_table_[lock_data_id];
    
    // 5. 检查是否已持有锁
    LockMode current_mode;
    if(is_lock_hold(txn, lock_data_id, current_mode)) {
        // 已持有锁，检查是否需要升级
        if(current_mode == LockMode::SHARED || current_mode == LockMode::EXLUCSIVE || 
           current_mode == LockMode::S_IX) {
            // 已持有S或更强的锁，无需重复申请
            return true;
        }
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

/**
 * @description: 申请表级写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    // 1. 检查事务状态（两阶段锁协议）
    if(txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }
    
    // 2. 构造锁对象ID（表级锁）
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    
    // 3. 加锁表的latch
    std::unique_lock<std::mutex> lock(latch_);
    
    // 4. 获取或创建锁请求队列
    auto& lock_queue = lock_table_[lock_data_id];
    
    // 5. 检查是否已持有锁
    LockMode current_mode;
    if(is_lock_hold(txn, lock_data_id, current_mode)) {
        // 已持有锁，检查是否需要升级
        if(current_mode == LockMode::EXLUCSIVE) {
            // 已持有X锁，无需重复申请
            return true;
        } else if(current_mode == LockMode::SHARED || current_mode == LockMode::INTENTION_SHARED ||
                  current_mode == LockMode::INTENTION_EXCLUSIVE || current_mode == LockMode::S_IX) {
            // 持有较弱的锁，需要升级为X锁
            // 检查是否有其他事务持有不兼容的锁
            if(has_conflict_lock(lock_queue, txn->get_transaction_id(), LockMode::EXLUCSIVE)) {
                // 锁升级冲突，回滚
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::UPGRADE_CONFLICT);
            }
            
            // 执行锁升级：修改锁请求的模式
            for(auto& request : lock_queue.request_queue_) {
                if(request.txn_id_ == txn->get_transaction_id() && request.granted_) {
                    request.lock_mode_ = LockMode::EXLUCSIVE;
                    break;
                }
            }
            
            // 更新组锁模式
            update_group_lock_mode(lock_queue);
            return true;
        }
    }
    
    // 6. 检查锁兼容性
    if(!is_lock_compatible(LockMode::EXLUCSIVE, lock_queue.group_lock_mode_)) {
        // 不兼容，死锁预防：no-wait策略
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }
    
    // 7. 创建锁请求并授予锁
    LockRequest lock_request(txn->get_transaction_id(), LockMode::EXLUCSIVE);
    lock_request.granted_ = true;
    lock_queue.request_queue_.push_back(lock_request);
    
    // 8. 更新组锁模式
    update_group_lock_mode(lock_queue);
    
    // 9. 将锁加入事务的锁集
    txn->get_lock_set()->insert(lock_data_id);
    
    return true;
}

/**
 * @description: 申请表级意向读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    // 1. 检查事务状态（两阶段锁协议）
    if(txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }
    
    // 2. 构造锁对象ID（表级锁）
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    
    // 3. 加锁表的latch
    std::unique_lock<std::mutex> lock(latch_);
    
    // 4. 获取或创建锁请求队列
    auto& lock_queue = lock_table_[lock_data_id];
    
    // 5. 检查是否已持有锁
    LockMode current_mode;
    if(is_lock_hold(txn, lock_data_id, current_mode)) {
        // 已持有锁，IS锁是最弱的读锁，如果已持有任何锁都不需要再申请IS锁
        return true;
    }
    
    // 6. 检查锁兼容性
    if(!is_lock_compatible(LockMode::INTENTION_SHARED, lock_queue.group_lock_mode_)) {
        // 不兼容，死锁预防：no-wait策略
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }
    
    // 7. 创建锁请求并授予锁
    LockRequest lock_request(txn->get_transaction_id(), LockMode::INTENTION_SHARED);
    lock_request.granted_ = true;
    lock_queue.request_queue_.push_back(lock_request);
    
    // 8. 更新组锁模式
    update_group_lock_mode(lock_queue);
    
    // 9. 将锁加入事务的锁集
    txn->get_lock_set()->insert(lock_data_id);
    
    return true;
}

/**
 * @description: 申请表级意向写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    // 1. 检查事务状态（两阶段锁协议）
    if(txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }
    
    // 2. 构造锁对象ID（表级锁）
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    
    // 3. 加锁表的latch
    std::unique_lock<std::mutex> lock(latch_);
    
    // 4. 获取或创建锁请求队列
    auto& lock_queue = lock_table_[lock_data_id];
    
    // 5. 检查是否已持有锁
    LockMode current_mode;
    if(is_lock_hold(txn, lock_data_id, current_mode)) {
        // 已持有锁，检查是否需要升级
        if(current_mode == LockMode::INTENTION_EXCLUSIVE || current_mode == LockMode::EXLUCSIVE || 
           current_mode == LockMode::S_IX) {
            // 已持有IX或更强的锁，无需重复申请
            return true;
        } else if(current_mode == LockMode::INTENTION_SHARED) {
            // 持有IS锁，需要升级为IX锁
            // 检查是否有其他事务持有不兼容的锁
            if(has_conflict_lock(lock_queue, txn->get_transaction_id(), LockMode::INTENTION_EXCLUSIVE)) {
                // 锁升级冲突，回滚
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::UPGRADE_CONFLICT);
            }
            
            // 执行锁升级：修改锁请求的模式
            for(auto& request : lock_queue.request_queue_) {
                if(request.txn_id_ == txn->get_transaction_id() && request.granted_) {
                    request.lock_mode_ = LockMode::INTENTION_EXCLUSIVE;
                    break;
                }
            }
            
            // 更新组锁模式
            update_group_lock_mode(lock_queue);
            return true;
        }
    }
    
    // 6. 检查锁兼容性
    if(!is_lock_compatible(LockMode::INTENTION_EXCLUSIVE, lock_queue.group_lock_mode_)) {
        // 不兼容，死锁预防：no-wait策略
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }
    
    // 7. 创建锁请求并授予锁
    LockRequest lock_request(txn->get_transaction_id(), LockMode::INTENTION_EXCLUSIVE);
    lock_request.granted_ = true;
    lock_queue.request_queue_.push_back(lock_request);
    
    // 8. 更新组锁模式
    update_group_lock_mode(lock_queue);
    
    // 9. 将锁加入事务的锁集
    txn->get_lock_set()->insert(lock_data_id);
    
    return true;
}

/**
 * @description: 释放锁
 * @return {bool} 返回解锁是否成功
 * @param {Transaction*} txn 要释放锁的事务对象指针
 * @param {LockDataId} lock_data_id 要释放的锁ID
 */
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
            
            // 6. 如果队列为空，从锁表中移除
            if(lock_queue.request_queue_.empty()) {
                lock_table_.erase(it);
            }
            
            return true;
        }
    }
    
    return false;
}

// 辅助函数实现

/**
 * @description: 检查锁兼容性
 */
bool LockManager::is_lock_compatible(LockMode lock_mode, GroupLockMode group_lock_mode) {
    if(group_lock_mode == GroupLockMode::NON_LOCK) {
        return true;
    }
    int mode_idx = static_cast<int>(lock_mode);
    int group_idx = static_cast<int>(group_lock_mode) - 1;  // NON_LOCK为0，其他从1开始
    return lock_compatibility_matrix_[mode_idx][group_idx];
}

/**
 * @description: 更新锁队列的组锁模式
 */
void LockManager::update_group_lock_mode(LockRequestQueue& queue) {
    queue.group_lock_mode_ = GroupLockMode::NON_LOCK;
    for(auto& request : queue.request_queue_) {
        if(!request.granted_) continue;
        
        GroupLockMode group_mode = lock_mode_to_group_mode(request.lock_mode_);
        
        // 更新为排他性更强的锁模式
        if(static_cast<int>(group_mode) > static_cast<int>(queue.group_lock_mode_)) {
            queue.group_lock_mode_ = group_mode;
        }
    }
}

/**
 * @description: 检查事务是否已持有该数据项的锁
 */
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

/**
 * @description: 检查队列中是否有其他事务持有不兼容的锁
 */
bool LockManager::has_conflict_lock(LockRequestQueue& queue, txn_id_t txn_id, LockMode lock_mode) {
    for(auto& request : queue.request_queue_) {
        if(request.txn_id_ != txn_id && request.granted_) {
            // 检查锁是否兼容
            GroupLockMode other_group_mode = lock_mode_to_group_mode(request.lock_mode_);
            int mode_idx = static_cast<int>(lock_mode);
            int group_idx = static_cast<int>(other_group_mode) - 1;
            if(!lock_compatibility_matrix_[mode_idx][group_idx]) {
                return true;  // 存在冲突
            }
        }
    }
    return false;
}

/**
 * @description: 将LockMode转换为GroupLockMode
 */
LockManager::GroupLockMode LockManager::lock_mode_to_group_mode(LockMode lock_mode) {
    switch(lock_mode) {
        case LockMode::INTENTION_SHARED:
            return GroupLockMode::IS;
        case LockMode::INTENTION_EXCLUSIVE:
            return GroupLockMode::IX;
        case LockMode::SHARED:
            return GroupLockMode::S;
        case LockMode::EXLUCSIVE:
            return GroupLockMode::X;
        case LockMode::S_IX:
            return GroupLockMode::SIX;
        default:
            return GroupLockMode::NON_LOCK;
    }
}