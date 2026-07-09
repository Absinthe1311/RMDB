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

#include "ix_defs.h"
#include "ix_index_handle.h"

// class IxIndexHandle;

// 用于遍历叶子结点
// 用于直接遍历叶子结点，而不用findleafpage来得到叶子结点
// TODO：对page遍历时，要加上读锁
class IxScan : public RecScan {
    const IxIndexHandle *ih_;
    Iid iid_;  // 初始为lower（用于遍历的指针）
    Iid end_;  // 初始为upper
    BufferPoolManager *bpm_;

   public:
    IxScan(const IxIndexHandle *ih, const Iid &lower, const Iid &upper, BufferPoolManager *bpm)
        : ih_(ih), iid_(lower), end_(upper), bpm_(bpm) {}

    void next() override;

    bool is_end() const override { 
        // 简单的相等比较，但需要考虑 iid_ 可能超出节点大小的情况
        // 如果 iid_ 超出当前节点大小，也应该视为到达末尾
        if(iid_ == end_) {
            // 调试输出
            static int count = 0;
            count++;
            if(count <= 20) {
                std::cerr << "DEBUG is_end: iid==end, count=" << count 
                          << " page=" << iid_.page_no << " slot=" << iid_.slot_no << std::endl;
            }
            return true;
        }
        // 如果 iid_ 的槽位超出节点大小（无效位置），也视为结束
        // 这个检查在实际访问前进行，避免访问无效位置
        return false;
    }

    Rid rid() const override;

    const Iid &iid() const { return iid_; }
};