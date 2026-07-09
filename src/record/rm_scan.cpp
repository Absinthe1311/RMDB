/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_scan.h"
#include "rm_file_handle.h"

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    // Todo:
    // 初始化file_handle和rid（指向第一个存放了记录的位置）
    // 从第一个可能存放记录的页开始找
    rid_.page_no = RM_FIRST_RECORD_PAGE;
    rid_.slot_no = -1;
    next();  // 定位到第一个真正存在记录的位置
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
    // Todo:
    // 找到文件中下一个存放了记录的非空闲位置，用rid_来指向这个位置
    int num_pages = file_handle_->file_hdr_.num_pages;
    int num_slots = file_handle_->file_hdr_.num_records_per_page;

    int page_no = rid_.page_no;
    int slot_no = rid_.slot_no;

    while (page_no < num_pages) {
        RmPageHandle page_handle = file_handle_->fetch_page_handle(page_no);

        // 在当前页的bitmap中，从slot_no之后找下一个被置1(占用)的位
        int next_slot = Bitmap::next_bit(true, page_handle.bitmap, num_slots, slot_no);

        file_handle_->buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);

        if (next_slot < num_slots) {
            rid_.page_no = page_no;
            rid_.slot_no = next_slot;
            return;
        }

        // 当前页没有更多记录了，换下一页，从头开始找
        page_no++;
        slot_no = -1;
    }

    // 遍历完所有页都没找到，说明到达末尾
    rid_.page_no = num_pages;
    rid_.slot_no = -1;
}

/**
 * @brief ​ 判断是否到达文件末尾
 */
bool RmScan::is_end() const {
    // Todo: 修改返回值

    return rid_.page_no >= file_handle_->file_hdr_.num_pages;
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const {
    return rid_;
}