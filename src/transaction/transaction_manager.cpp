/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    // Todo:
    // 1. 判断传入事务参数是否为空指针
    // 2. 如果为空指针，创建新事务
    // 3. 把开始事务加入到全局事务表中
    // 4. 返回当前事务指针
    // 1. 判断传入事务参数是否为空指针
    if (txn == nullptr) {
        // 2. 如果为空指针，创建新事务，分配一个新的事务号
        txn_id_t txn_id = next_txn_id_++;
        txn = new Transaction(txn_id);
    }

    // 3. 把开始事务加入到全局事务表中
    std::unique_lock<std::mutex> lock(latch_);
    txn_map[txn->get_transaction_id()] = txn;
    lock.unlock();

    // 4. 返回当前事务指针
    return txn;
    return nullptr;
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    // Todo:
    // 1. 如果存在未提交的写操作，提交所有的写操作
    // 2. 释放所有锁
    // 3. 释放事务相关资源，eg.锁集
    // 4. 把事务日志刷入磁盘中
    // 5. 更新事务状态
    // 1. 如果存在未提交的写操作，此处不需要额外提交（写操作在执行时已经直接写入了记录文件，
    //    commit阶段主要处理的是锁的释放和状态更新，写操作的“提交”体现在不回滚）

    // 2. 释放所有锁
    auto lock_set = txn->get_lock_set();
    for (auto &lock_data_id : *lock_set) {
        lock_manager_->unlock(txn, lock_data_id);
    }
    lock_set->clear();

    // 3. 释放事务相关资源，清空写集
    txn->get_write_set()->clear();

    // 4. 把事务日志刷入磁盘中
    if (log_manager != nullptr) {
        log_manager->flush_log_to_disk();
    }

    // 5. 更新事务状态
    txn->set_state(TransactionState::COMMITTED);
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    // Todo:
    // 1. 回滚所有写操作
    // 2. 释放所有锁
    // 3. 清空事务相关资源，eg.锁集
    // 4. 把事务日志刷入磁盘中
    // 5. 更新事务状态
    // 1. 回滚所有写操作：按照写集里记录的操作，逆序做相反的操作
    auto write_set = txn->get_write_set();
    while (!write_set->empty()) {
        auto write_record = write_set->back();
        auto tab_name = write_record->GetTableName();
        auto rm_file_handle = sm_manager_->fhs_.at(tab_name).get();

        switch (write_record->GetWriteType()) {
            case WType::INSERT_TUPLE:
                // 插入的逆操作是删除
                rm_file_handle->delete_record(write_record->GetRid(), nullptr);
                break;
            case WType::DELETE_TUPLE:
                // 删除的逆操作是重新插入原来的记录
                rm_file_handle->insert_record(write_record->GetRid(), write_record->GetRecord().data);
                break;
            case WType::UPDATE_TUPLE:
                // 更新的逆操作是把记录改回旧值
                rm_file_handle->update_record(write_record->GetRid(), write_record->GetRecord().data, nullptr);
                break;
        }
        write_set->pop_back();
    }

    // 2. 释放所有锁
    auto lock_set = txn->get_lock_set();
    for (auto &lock_data_id : *lock_set) {
        lock_manager_->unlock(txn, lock_data_id);
    }
    lock_set->clear();

    // 3. 清空事务相关资源
    write_set->clear();

    // 4. 把事务日志刷入磁盘中
    if (log_manager != nullptr) {
        log_manager->flush_log_to_disk();
    }

    // 5. 更新事务状态
    txn->set_state(TransactionState::ABORTED);
}