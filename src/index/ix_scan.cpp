/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_scan.h"

/**
 * @brief 
 * @todo 加上读锁（需要使用缓冲池得到page）
 */
void IxScan::next() {
    assert(!is_end());
    IxNodeHandle *node = ih_->fetch_node(iid_.page_no);
    assert(node->is_leaf_page());
    assert(iid_.slot_no < node->get_size());
    
    // 调试输出：前10次和2700附近
    static int count = 0;
    count++;
    if(count <= 10 || (count >= 2700 && count <= 2710)) {
        std::cerr << "DEBUG IxScan::next: count=" << count
                  << " page_no=" << iid_.page_no
                  << " slot_no=" << iid_.slot_no
                  << " node_size=" << node->get_size()
                  << " last_leaf=" << ih_->file_hdr_->last_leaf_ << std::endl;
    }
    
    // increment slot no
    iid_.slot_no++;
    if (iid_.slot_no == node->get_size()) {
        // slot_no超出当前节点范围，需要处理
        if (iid_.page_no == ih_->file_hdr_->last_leaf_) {
            // 已经是最后一个叶子，到达末尾，设置为end_
            iid_ = end_;
        } else {
            // 不是最后一个叶子，跳转到下一个叶子
            iid_.slot_no = 0;
            iid_.page_no = node->get_next_leaf();
            
            // 调试输出：跳转到下一个叶子节点时
            if(count <= 10 || (count >= 2700 && count <= 2710)) {
                std::cerr << "DEBUG IxScan jump: count=" << count
                          << " jump to next_leaf=" << iid_.page_no << std::endl;
            }
            
            // 检查是否跳转到了叶子链表头节点（IX_LEAF_HEADER_PAGE）
            // 注意：不能使用page_no大小比较来判断是否越界，因为page_no只是页面分配顺序
            // 不代表叶子链表（按key排序）中的先后顺序
            if(iid_.page_no == IX_LEAF_HEADER_PAGE) {
                iid_ = end_;
            }
        }
    }

    ih_->buffer_pool_manager_->unpin_page(node->get_page_id(), false);
}

Rid IxScan::rid() const {
    return ih_->get_rid(iid_);
}