/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_index_handle.h"

#include "ix_scan.h"

/**
 * @brief 在当前node中查找第一个>=target的key_idx
 *
 * @return key_idx，范围为[0,num_key)，如果返回的key_idx=num_key，则表示target大于最后一个key
 * @note 返回key index（同时也是rid index），作为slot no
 */
int IxNodeHandle::lower_bound(const char *target) const {
    // Todo:
    // 查找当前节点中第一个大于等于target的key，并返回key的位置给上层
    // 提示: 可以采用多种查找方式，如顺序遍历、二分查找等；使用ix_compare()函数进行比较
    int left = 0;
    int right = page_hdr->num_key;
    
    while (left < right) {
        int mid = left + (right - left) / 2;
        char *mid_key = keys + mid * file_hdr->col_tot_len_;
        int cmp = ix_compare(mid_key, target, file_hdr->col_types_, file_hdr->col_lens_);
        if (cmp < 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;  // 返回范围 [0, num_key]，若等于 num_key 表示所有 key 都小于 target
}

/**
 * @brief 在当前node中查找第一个>target的key_idx
 *
 * @return key_idx，范围为[1,num_key)，如果返回的key_idx=num_key，则表示target大于等于最后一个key
 * @note 注意此处的范围从1开始
 */
int IxNodeHandle::upper_bound(const char *target) const {
    // Todo:
    // 查找当前节点中第一个大于target的key，并返回key的位置给上层
    // 提示: 可以采用多种查找方式：顺序遍历、二分查找等；使用ix_compare()函数进行比较
    int left = 0;
    int right = page_hdr->num_key;
    
    while (left < right) {
        int mid = left + (right - left) / 2;
        char *mid_key = keys + mid * file_hdr->col_tot_len_;
        int cmp = ix_compare(mid_key, target, file_hdr->col_types_, file_hdr->col_lens_);
        if (cmp <= 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;  // 返回范围 [0, num_key]
}

/**
 * @brief 用于叶子结点根据key来查找该结点中的键值对
 * 值value作为传出参数，函数返回是否查找成功
 *
 * @param key 目标key
 * @param[out] value 传出参数，目标key对应的Rid
 * @return 目标key是否存在
 */
bool IxNodeHandle::leaf_lookup(const char *key, Rid **value) {
    // Todo:
    // 1. 在叶子节点中获取目标key所在位置
    // 2. 判断目标key是否存在
    // 3. 如果存在，获取key对应的Rid，并赋值给传出参数value
    // 提示：可以调用lower_bound()和get_rid()函数。
    int pos = lower_bound(key);
    // 检查 pos 是否有效且 key 是否匹配
    if (pos < page_hdr->num_key) {
        char *found_key = keys + pos * file_hdr->col_tot_len_;
        if (ix_compare(found_key, key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
            *value = &rids[pos];
            return true;
        }
    }
    return false;
}

/**
 * 用于内部结点（非叶子节点）查找目标key所在的孩子结点（子树）
 * @param key 目标key
 * @return page_id_t 目标key所在的孩子节点（子树）的存储页面编号
 */
page_id_t IxNodeHandle::internal_lookup(const char *key) {
    // Todo:
    // 1. 查找当前非叶子节点中目标key所在孩子节点（子树）的位置
    // 2. 获取该孩子节点（子树）所在页面的编号
    // 3. 返回页面编号
    // upper_bound 返回第一个 > key 的位置
    // 对于内部节点：keys[0..num_key-1] 是分隔 key，rids[i] 指向 <= keys[i] 的子树
    // 如果 key <= keys[0]，应该在 rids[0] 中查找
    // 如果 key > keys[num_key-1]，应该在 rids[num_key] 中查找
    int pos = upper_bound(key);
    
    // 调试输出：前10条、169附近和2700附近，以及root节点
    int key_value = *reinterpret_cast<const int*>(key);
    bool is_root = (page_hdr->parent == IX_NO_PAGE);
    if(key_value <= 10 || (key_value >= 168 && key_value <= 171) || (key_value >= 2700 && key_value <= 2710) || is_root) {
        std::cerr << "DEBUG internal_lookup: key=" << key_value
                  << " pos=" << pos
                  << " num_key=" << page_hdr->num_key
                  << " is_root=" << is_root;
        
        // 如果是root节点，打印所有key
        if(is_root && page_hdr->num_key <= 20) {
            std::cerr << " keys=[";
            for(int i = 0; i < page_hdr->num_key; i++) {
                int k = *reinterpret_cast<int*>(get_key(i));
                std::cerr << k;
                if(i < page_hdr->num_key - 1) std::cerr << ",";
            }
            std::cerr << "]";
        }
        
        std::cerr << " child_page=" << rids[pos].page_no << std::endl;
    }
    
    // pos 范围 [0, num_key]
    // pos=0 表示 key <= 所有 key，走 rids[0]
    // pos=num_key 表示 key > 所有 key，走 rids[num_key]
    // 否则 key <= keys[pos] 但 > keys[pos-1]，走 rids[pos]
    // 实际上：对于 key <= keys[i]，走 rids[i]
    // upper_bound 返回第一个 > key 的位置，所以应该走 rids[pos]
    return rids[pos].page_no;
}

/**
 * @brief 在指定位置插入n个连续的键值对
 * 将key的前n位插入到原来keys中的pos位置；将rid的前n位插入到原来rids中的pos位置
 *
 * @param pos 要插入键值对的位置
 * @param (key, rid) 连续键值对的起始地址，也就是第一个键值对，可以通过(key, rid)来获取n个键值对
 * @param n 键值对数量
 * @note [0,pos)           [pos,num_key)
 *                            key_slot
 *                            /      \
 *                           /        \
 *       [0,pos)     [pos,pos+n)   [pos+n,num_key+n)
 *                      key           key_slot
 */
void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    // Todo:
    // 1. 判断pos的合法性
    // 2. 通过key获取n个连续键值对的key值，并把n个key值插入到pos位置
    // 3. 通过rid获取n个连续键值对的rid值，并把n个rid值插入到pos位置
    // 4. 更新当前节点的键数量
    
    // 对于叶子节点：pos的范围是 [0, num_key]
    // 对于内部节点：pos的范围是 [0, num_key+1]（因为内部节点有 num_key+1 个孩子）
    int max_pos = is_leaf_page() ? page_hdr->num_key : (page_hdr->num_key + 1);
    assert(pos >= 0 && pos <= max_pos);
    
    int num_key = page_hdr->num_key;
    
    // 移动 [pos, num_key) 的 key 到 [pos+n, num_key+n)
    int key_size = file_hdr->col_tot_len_;
    if (pos < num_key) {
        memmove(keys + (pos + n) * key_size, keys + pos * key_size, (num_key - pos) * key_size);
    }
    
    // 移动 [pos, num_key) 的 rid 到 [pos+n, num_key+n)
    // 注意内部节点比叶子节点多一个 rid（最右孩子）
    int rid_move_count = is_leaf_page() ? (num_key - pos) : (num_key - pos + 1);
    if (pos <= num_key) {
        memmove(&rids[pos + n], &rids[pos], rid_move_count * sizeof(Rid));
    }
    
    // 复制新的 key 和 rid
    for (int i = 0; i < n; i++) {
        memcpy(keys + (pos + i) * key_size, key + i * key_size, key_size);
        rids[pos + i] = rid[i];
    }
    
    page_hdr->num_key += n;
}

/**
 * @brief 用于在结点中插入单个键值对。
 * 函数返回插入后的键值对数量
 *
 * @param (key, value) 要插入的键值对
 * @return int 键值对数量
 */
int IxNodeHandle::insert(const char *key, const Rid &value) {
    // Todo:
    // 1. 查找要插入的键值对应该插入到当前节点的哪个位置
    // 2. 如果key重复则不插入
    // 3. 如果key不重复则插入键值对
    // 4. 返回完成插入操作之后的键值对数量
    int pos = lower_bound(key);
    
    // 检查是否重复
    if (pos < page_hdr->num_key) {
        char *found_key = keys + pos * file_hdr->col_tot_len_;
        if (ix_compare(found_key, key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
            // key 已存在，返回 -1 表示重复（唯一索引不允许重复）
            return -1;
        }
    }
    
    insert_pair(pos, key, value);
    return page_hdr->num_key;
}

/**
 * @brief 用于在结点中的指定位置删除单个键值对
 *
 * @param pos 要删除键值对的位置
 */
void IxNodeHandle::erase_pair(int pos) {
    // Todo:
    // 1. 删除该位置的key
    // 2. 删除该位置的rid
    // 3. 更新结点的键值对数量
    assert(pos >= 0 && pos < page_hdr->num_key);
    
    int num_key = page_hdr->num_key;
    int key_size = file_hdr->col_tot_len_;
    
    // 移动 [pos+1, num_key) 的 key 到 [pos, num_key-1)
    if (pos + 1 < num_key) {
        memmove(keys + pos * key_size, keys + (pos + 1) * key_size, (num_key - pos - 1) * key_size);
    }
    
    // 移动 [pos+1, num_key] 的 rid 到 [pos, num_key-1]（叶子多一个占位）
    int rid_move_count = is_leaf_page() ? (num_key - pos - 1) : (num_key - pos);
    if (pos + 1 <= num_key) {
        memmove(&rids[pos], &rids[pos + 1], rid_move_count * sizeof(Rid));
    }
    
    page_hdr->num_key--;
}

/**
 * @brief 用于在结点中删除指定key的键值对。函数返回删除后的键值对数量
 *
 * @param key 要删除的键值对key值
 * @return 完成删除操作后的键值对数量
 */
int IxNodeHandle::remove(const char *key) {
    // Todo:
    // 1. 查找要删除键值对的位置
    // 2. 如果要删除的键值对存在，删除键值对
    // 3. 返回完成删除操作后的键值对数量
    int pos = lower_bound(key);
    
    if (pos < page_hdr->num_key) {
        char *found_key = keys + pos * file_hdr->col_tot_len_;
        if (ix_compare(found_key, key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
            erase_pair(pos);
        }
    }
    
    return page_hdr->num_key;
}

IxIndexHandle::IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
    : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
    // init file_hdr_
    // disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));
    char* buf = new char[PAGE_SIZE];
    memset(buf, 0, PAGE_SIZE);
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, buf, PAGE_SIZE);
    file_hdr_ = new IxFileHdr();
    file_hdr_->deserialize(buf);
    
    // disk_manager管理的fd对应的文件中，设置从file_hdr_->num_pages开始分配page_no
    // int now_page_no = disk_manager_->get_fd2pageno(fd);
    // disk_manager_->set_fd2pageno(fd, now_page_no + 1);
    disk_manager_->set_fd2pageno(fd, file_hdr_->num_pages_);
}

/**
 * @brief 用于查找指定键所在的叶子结点
 * @param key 要查找的目标key值
 * @param operation 查找到目标键值对后要进行的操作类型
 * @param transaction 事务参数，如果不需要则默认传入nullptr
 * @return [leaf node] and [root_is_latched] 返回目标叶子结点以及根结点是否加锁
 * @note need to Unlatch and unpin the leaf node outside!
 * 注意：用了FindLeafPage之后一定要unlatch叶结点，否则下次latch该结点会堵塞！
 */
std::pair<IxNodeHandle *, bool> IxIndexHandle::find_leaf_page(const char *key, Operation operation,
                                                            Transaction *transaction, bool find_first) {
    // Todo:
    // 1. 获取根节点
    // 2. 从根节点开始不断向下查找目标key
    // 3. 找到包含该key值的叶子结点停止查找，并返回叶子节点
    // 1. 获取根节点
    page_id_t root_page_no = file_hdr_->root_page_;
    IxNodeHandle *node = fetch_node(root_page_no);
    
    // 2. 从根节点开始不断向下查找目标 key
    while (!node->is_leaf_page()) {
        page_id_t child_page_no = node->internal_lookup(key);
        IxNodeHandle *child = fetch_node(child_page_no);
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        node = child;
    }
    
    // 3. 找到包含该 key 值的叶子节点
    // root_is_latched 暂时设为 false，并发控制后续再加
    return std::make_pair(node, false);
}

/**
 * @brief 用于查找指定键在叶子结点中的对应的值result
 *
 * @param key 查找的目标key值
 * @param result 用于存放结果的容器
 * @param transaction 事务指针
 * @return bool 返回目标键值对是否存在
 */
bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction) {
    // Todo:
    // 1. 获取目标key值所在的叶子结点
    // 2. 在叶子节点中查找目标key值的位置，并读取key对应的rid
    // 3. 把rid存入result参数中
    // 提示：使用完buffer_pool提供的page之后，记得unpin page；记得处理并发的上锁
    // 1. 获取目标 key 值所在的叶子节点
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::FIND, transaction);
    
    // 调试输出：前10条、169附近和2700附近
    int key_value = *reinterpret_cast<const int*>(key);
    if (key_value <= 10 || (key_value >= 168 && key_value <= 171) || (key_value >= 2700 && key_value <= 2710)) {
        std::cerr << "DEBUG get_value: key=" << key_value 
                  << " leaf_page=" << leaf->get_page_no() 
                  << " leaf_size=" << leaf->get_size()
                  << " last_leaf=" << file_hdr_->last_leaf_ << std::endl;
    }
    
    // 2. 在叶子节点中查找目标 key 值的位置
    Rid *rid = nullptr;
    bool found = leaf->leaf_lookup(key, &rid);
    
    // 3. 如果找到，把 rid 存入 result 参数中
    if (found) {
        result->push_back(*rid);
    }
    
    // 4. unpin 叶子节点
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    
    return found;
}

/**
 * @brief  将传入的一个node拆分(Split)成两个结点，在node的右边生成一个新结点new node
 * @param node 需要拆分的结点
 * @return 拆分得到的new_node
 * @note need to unpin the new node outside
 * 注意：本函数执行完毕后，原node和new node都需要在函数外面进行unpin
 */
std::tuple<IxNodeHandle *, char *> IxIndexHandle::split(IxNodeHandle *node) {
    // Todo:
    // 1. 将原结点的键值对平均分配，右半部分分裂为新的右兄弟结点
    //    需要初始化新节点的page_hdr内容
    // 2. 如果新的右兄弟结点是叶子结点，更新新旧节点的prev_leaf和next_leaf指针
    //    为新节点分配键值对，更新旧节点的键值对数记录
    // 3. 如果新的右兄弟结点不是叶子结点，更新该结点的所有孩子结点的父节点信息(使用IxIndexHandle::maintain_child())
    
    // 用于保存提升到父节点的key（动态分配，避免static buffer被覆盖）
    char *split_key = new char[file_hdr_->col_tot_len_];
    
    // 1. 创建新节点
    IxNodeHandle *new_node = create_node();
    
    // 2. 初始化新节点的 page_hdr
    new_node->page_hdr->parent = node->get_parent_page_no();
    new_node->page_hdr->is_leaf = node->is_leaf_page();
    new_node->page_hdr->num_key = 0;
    
    // 3. 计算分裂点：将后半部分移到新节点
    int total_keys = node->get_size();
    int split_point = total_keys / 2;  // 左半部分保留在原节点
    
    int move_count = total_keys - split_point;
    
    // 4. 移动键值对到新节点
    if (node->is_leaf_page()) {
        // 叶子节点分裂：直接复制键值对
        // 叶子节点中，中间key被复制到父节点，但仍然保留在右节点中
        char *move_keys = node->get_key(split_point);
        Rid *move_rids = node->get_rid(split_point);
        new_node->insert_pairs(0, move_keys, move_rids, move_count);
        
        // 叶子节点的split_key是新节点的第一个key（复制）
        memcpy(split_key, new_node->get_key(0), file_hdr_->col_tot_len_);
    } else {
        // 内部节点分裂：中间key被提升到父节点，不保留在子节点中
        // 左节点保留 keys[0..split_point-1]，rids[0..split_point]
        // 中间key = keys[split_point]（将被insert_into_parent提升）
        // 右节点得到 keys[split_point+1..end]，rids[split_point+1..end+1]
        
        // split_key是中间key，将被提升到父节点（复制）
        memcpy(split_key, node->get_key(split_point), file_hdr_->col_tot_len_);
        
        // 复制 keys[split_point+1..end] 到新节点（跳过中间key）
        int new_node_key_count = move_count - 1;
        if (new_node_key_count > 0) {
            char *move_keys = node->get_key(split_point + 1);
            for (int i = 0; i < new_node_key_count; i++) {
                memcpy(new_node->get_key(i), move_keys + i * file_hdr_->col_tot_len_, file_hdr_->col_tot_len_);
            }
        }
        
        // 复制 rids[split_point+1..end+1] 到新节点
        Rid *move_rids = node->get_rid(split_point + 1);
        for (int i = 0; i <= new_node_key_count; i++) {
            new_node->set_rid(i, move_rids[i]);
        }
        
        new_node->set_size(new_node_key_count);
    }
    
    // 5. 更新原节点的 num_key（截断到 split_point）
    node->set_size(split_point);
    
    // 6. 处理叶子节点的链表指针
    if (node->is_leaf_page()) {
        new_node->set_next_leaf(node->get_next_leaf());
        new_node->set_prev_leaf(node->get_page_no());
        
        // 更新原节点的后继节点的前驱指针
        if (node->get_next_leaf() != IX_LEAF_HEADER_PAGE) {
            IxNodeHandle *next_leaf = fetch_node(node->get_next_leaf());
            next_leaf->set_prev_leaf(new_node->get_page_no());
            buffer_pool_manager_->unpin_page(next_leaf->get_page_id(), true);
        }
        
        node->set_next_leaf(new_node->get_page_no());
        
        // 更新 last_leaf（如果原节点是最右叶子）
        if (file_hdr_->last_leaf_ == node->get_page_no()) {
            file_hdr_->last_leaf_ = new_node->get_page_no();
        }
    } else {
        // 内部节点：更新新节点所有孩子的父指针
        for (int i = 0; i <= new_node->get_size(); i++) {
            maintain_child(new_node, i);
        }
    }
    
    return std::make_tuple(new_node, split_key);
}

/**
 * @brief Insert key & value pair into internal page after split
 * 拆分(Split)后，向上找到old_node的父结点
 * 将new_node的第一个key插入到父结点，其位置在 父结点指向old_node的孩子指针 之后
 * 如果插入后>=maxsize，则必须继续拆分父结点，然后在其父结点的父结点再插入，即需要递归
 * 直到找到的old_node为根结点时，结束递归（此时将会新建一个根R，关键字为key，old_node和new_node为其孩子）
 *
 * @param (old_node, new_node) 原结点为old_node，old_node被分裂之后产生了新的右兄弟结点new_node
 * @param key 要插入parent的key
 * @note 一个结点插入了键值对之后需要分裂，分裂后左半部分的键值对保留在原结点，在参数中称为old_node，
 * 右半部分的键值对分裂为新的右兄弟节点，在参数中称为new_node（参考Split函数来理解old_node和new_node）
 * @note 本函数执行完毕后，new node和old node都需要在函数外面进行unpin
 */
void IxIndexHandle::insert_into_parent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node,
                                     Transaction *transaction) {
    // Todo:
    // 1. 分裂前的结点（原结点, old_node）是否为根结点，如果为根结点需要分配新的root
    // 2. 获取原结点（old_node）的父亲结点
    // 3. 获取key对应的rid，并将(key, rid)插入到父亲结点
    // 4. 如果父亲结点仍需要继续分裂，则进行递归插入
    // 提示：记得unpin page
    // 1. 如果 old_node 是根节点，创建新根
    if (old_node->is_root_page()) {
        IxNodeHandle *new_root = create_node();
        
        // 初始化新根节点
        new_root->page_hdr->parent = IX_NO_PAGE;
        new_root->page_hdr->is_leaf = false;
        new_root->page_hdr->num_key = 1;  // 直接设置为1，避免使用insert_pair
        
        // 手动设置key和children，避免insert_pair的移位逻辑导致左右孩子交换
        // 对于内部节点：rids[0]指向<=key的子树，rids[1]指向>key的子树
        // old_node包含<=key的部分，new_node包含>key的部分
        memcpy(new_root->get_key(0), key, file_hdr_->col_tot_len_);
        new_root->set_rid(0, Rid{.page_no = old_node->get_page_no(), .slot_no = 0});
        new_root->set_rid(1, Rid{.page_no = new_node->get_page_no(), .slot_no = 0});
        
        // 更新 old_node 和 new_node 的父指针
        old_node->set_parent_page_no(new_root->get_page_no());
        new_node->set_parent_page_no(new_root->get_page_no());
        
        // 更新 root_page
        update_root_page_no(new_root->get_page_no());
        
        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
        return;
    }
    
    // 2. 获取父节点
    IxNodeHandle *parent = fetch_node(old_node->get_parent_page_no());
    
    // 3. 找到 old_node 在父节点中的位置
    int index = parent->find_child(old_node);
    
    // 4. 在父节点中插入 (key, new_node->page_no)
    // 对于内部节点，不能使用insert_pair，因为keys和rids的下标含义不同：
    // - keys[i] 分隔 rids[i] 和 rids[i+1]
    // - 新key应插入到keys[index]（分隔old_node和new_node）
    // - 新rid应插入到rids[index+1]
    int num_key = parent->get_size();
    int key_size = file_hdr_->col_tot_len_;
    
    // 移动keys[index..num_key)到keys[index+1..num_key+1)，腾出keys[index]
    if (index < num_key) {
        memmove(parent->get_key(index + 1), parent->get_key(index), (num_key - index) * key_size);
    }
    // 写入新key到keys[index]
    memcpy(parent->get_key(index), key, key_size);
    
    // 移动rids[index+1..num_key]到rids[index+2..num_key+1]，腾出rids[index+1]
    // 注意：内部节点有num_key+1个rid，所以要移动(num_key - index + 1)个
    memmove(parent->get_rid(index + 2), parent->get_rid(index + 1), (num_key - index + 1) * sizeof(Rid));
    // 写入新rid到rids[index+1]
    parent->set_rid(index + 1, Rid{.page_no = new_node->get_page_no(), .slot_no = 0});
    
    // 更新num_key
    parent->set_size(num_key + 1);
    
    // 5. 更新 new_node 的父指针
    new_node->set_parent_page_no(parent->get_page_no());
    
    // 6. 如果父节点满了，需要递归分裂
    if (parent->get_size() >= parent->get_max_size()) {
        auto [new_parent, split_key] = split(parent);
        // 将分隔key插入祖父节点
        insert_into_parent(parent, split_key, new_parent, transaction);
        buffer_pool_manager_->unpin_page(new_parent->get_page_id(), true);
        delete[] split_key;  // 释放split_key内存
    }
    
    buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
}

/**
 * @brief 将指定键值对插入到B+树中
 * @param (key, value) 要插入的键值对
 * @param transaction 事务指针
 * @return page_id_t 插入到的叶结点的page_no
 */
page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    // Todo:
    // 1. 查找key值应该插入到哪个叶子节点
    // 2. 在该叶子节点中插入键值对
    // 3. 如果结点已满，分裂结点，并把新结点的相关信息插入父节点
    // 提示：记得unpin page；若当前叶子节点是最右叶子节点，则需要更新file_hdr_.last_leaf；记得处理并发的上锁
    // 1. 查找应该插入的叶子节点
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::INSERT, transaction);
    
    // 调试输出
    int key_value = *reinterpret_cast<const int*>(key);
    if (key_value >= 995 || key_value == 10) {
        std::cerr << "DEBUG insert_entry: key=" << key_value 
                  << " leaf_page=" << leaf->get_page_no() 
                  << " leaf_size_before=" << leaf->get_size() << std::endl;
    }
    
    // 2. 在叶子节点中插入键值对
    int new_size = leaf->insert(key, value);
    
    // 如果返回 -1 表示 key 重复（唯一索引冲突）
    if (new_size == -1) {
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        throw IndexExistsError("", {});  // 或者返回 -1 让上层处理
    }
    
    page_id_t leaf_page_no = leaf->get_page_no();
    
    // 3. 如果叶子节点满了，分裂
    if (leaf->get_size() >= leaf->get_max_size()) {
        auto [new_leaf, split_key] = split(leaf);
        // 将分隔key插入父节点
        insert_into_parent(leaf, split_key, new_leaf, transaction);
        buffer_pool_manager_->unpin_page(new_leaf->get_page_id(), true);
        delete[] split_key;  // 释放split_key内存
    }
    
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    
    return leaf_page_no;
}

/**
 * @brief 用于删除B+树中含有指定key的键值对
 * @param key 要删除的key值
 * @param transaction 事务指针
 */
bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction) {
    // Todo:
    // 1. 获取该键值对所在的叶子结点
    // 2. 在该叶子结点中删除键值对
    // 3. 如果删除成功需要调用CoalesceOrRedistribute来进行合并或重分配操作，并根据函数返回结果判断是否有结点需要删除
    // 4. 如果需要并发，并且需要删除叶子结点，则需要在事务的delete_page_set中添加删除结点的对应页面；记得处理并发的上锁
    // 1. 查找叶子节点
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::DELETE, transaction);
    
    // 2. 删除键值对
    int old_size = leaf->get_size();
    leaf->remove(key);
    
    // 如果 key 不存在
    if (leaf->get_size() == old_size) {
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        return false;
    }
    
    // 3. 合并或重分配
    bool root_is_latched_local = false;
    coalesce_or_redistribute(leaf, transaction, &root_is_latched_local);
    
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    
    return true;
}

/**
 * @brief 用于处理合并和重分配的逻辑，用于删除键值对后调用
 *
 * @param node 执行完删除操作的结点
 * @param transaction 事务指针
 * @param root_is_latched 传出参数：根节点是否上锁，用于并发操作
 * @return 是否需要删除结点
 * @note User needs to first find the sibling of input page.
 * If sibling's size + input page's size >= 2 * page's minsize, then redistribute.
 * Otherwise, merge(Coalesce).
 */
bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle *node, Transaction *transaction, bool *root_is_latched) {
    // Todo:
    // 1. 判断node结点是否为根节点
    //    1.1 如果是根节点，需要调用AdjustRoot() 函数来进行处理，返回根节点是否需要被删除
    //    1.2 如果不是根节点，并且不需要执行合并或重分配操作，则直接返回false，否则执行2
    // 2. 获取node结点的父亲结点
    // 3. 寻找node结点的兄弟结点（优先选取前驱结点）
    // 4. 如果node结点和兄弟结点的键值对数量之和，能够支撑两个B+树结点（即node.size+neighbor.size >=
    // NodeMinSize*2)，则只需要重新分配键值对（调用Redistribute函数）
    // 5. 如果不满足上述条件，则需要合并两个结点，将右边的结点合并到左边的结点（调用Coalesce函数）
    // 1. 如果是根节点
    if (node->is_root_page()) {
        return adjust_root(node);
    }
    
    // 2. 如果节点大小 >= 最小大小，不需要调整
    if (node->get_size() >= node->get_min_size()) {
        // 但如果删除了第一个 key，需要更新父节点中的分隔 key
        maintain_parent(node);
        return false;
    }
    
    // 3. 获取父节点
    IxNodeHandle *parent = fetch_node(node->get_parent_page_no());
    
    // 4. 找到 node 在父节点中的位置
    int index = parent->find_child(node);
    
    // 5. 寻找兄弟节点（优先选前驱）
    int neighbor_index;
    IxNodeHandle *neighbor;
    
    if (index > 0) {
        // 有前驱，选前驱
        neighbor_index = index - 1;
        neighbor = fetch_node(parent->get_rid(neighbor_index)->page_no);
    } else {
        // 没有前驱，选后继
        neighbor_index = index + 1;
        neighbor = fetch_node(parent->get_rid(neighbor_index)->page_no);
    }
    
    // 6. 判断是合并还是重分配
    if (node->get_size() + neighbor->get_size() >= node->get_min_size() * 2) {
        // 重分配
        redistribute(neighbor, node, parent, index);
        buffer_pool_manager_->unpin_page(neighbor->get_page_id(), true);
        buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
        return false;
    } else {
        // 合并
        bool result = coalesce(&neighbor, &node, &parent, index, transaction, root_is_latched);
        buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
        return result;
    }
}

/**
 * @brief 用于当根结点被删除了一个键值对之后的处理
 * @param old_root_node 原根节点
 * @return bool 根结点是否需要被删除
 * @note size of root page can be less than min size and this method is only called within coalesce_or_redistribute()
 */
bool IxIndexHandle::adjust_root(IxNodeHandle *old_root_node) {
    // Todo:
    // 1. 如果old_root_node是内部结点，并且大小为1，则直接把它的孩子更新成新的根结点
    // 2. 如果old_root_node是叶结点，且大小为0，则直接更新root page
    // 3. 除了上述两种情况，不需要进行操作
    // 1. 如果根节点是内部节点且 size == 1，孩子变成新根
    if (!old_root_node->is_leaf_page() && old_root_node->get_size() == 1) {
        // 获取唯一的孩子
        page_id_t child_page_no = old_root_node->get_rid(0)->page_no;
        IxNodeHandle *child = fetch_node(child_page_no);
        
        // 孩子成为新根
        child->set_parent_page_no(IX_NO_PAGE);
        update_root_page_no(child_page_no);
        
        // 删除旧根
        release_node_handle(*old_root_node);
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
        buffer_pool_manager_->delete_page(old_root_node->get_page_id());
        
        return false;
    }
    
    // 2. 如果根节点是叶子节点且 size == 0
    if (old_root_node->is_leaf_page() && old_root_node->get_size() == 0) {
        update_root_page_no(IX_NO_PAGE);
        release_node_handle(*old_root_node);
        buffer_pool_manager_->delete_page(old_root_node->get_page_id());
        return false;
    }
    
    return false;
}

/**
 * @brief 重新分配node和兄弟结点neighbor_node的键值对
 * Redistribute key & value pairs from one page to its sibling page. If index == 0, move sibling page's first key
 * & value pair into end of input "node", otherwise move sibling page's last key & value pair into head of input "node".
 *
 * @param neighbor_node sibling page of input "node"
 * @param node input from method coalesceOrRedistribute()
 * @param parent the parent of "node" and "neighbor_node"
 * @param index node在parent中的rid_idx
 * @note node是之前刚被删除过一个key的结点
 * index=0，则neighbor是node后继结点，表示：node(left)      neighbor(right)
 * index>0，则neighbor是node前驱结点，表示：neighbor(left)  node(right)
 * 注意更新parent结点的相关kv对
 */
void IxIndexHandle::redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index) {
    // Todo:
    // 1. 通过index判断neighbor_node是否为node的前驱结点
    // 2. 从neighbor_node中移动一个键值对到node结点中
    // 3. 更新父节点中的相关信息，并且修改移动键值对对应孩字结点的父结点信息（maintain_child函数）
    // 注意：neighbor_node的位置不同，需要移动的键值对不同，需要分类讨论

    // index 是 node 在 parent 中的位置
    // 如果 index == 0，neighbor_node 是后继（右兄弟），node 是左节点
    // 如果 index > 0，neighbor_node 是前驱（左兄弟），node 是右节点
    
    if (index > 0) {
        // neighbor_node 是前驱（左兄弟），从 neighbor_node 移最后一个键值对到 node 的头部
        int neighbor_size = neighbor_node->get_size();
        
        // 获取 neighbor_node 最后一个键值对
        char *last_key = neighbor_node->get_key(neighbor_size - 1);
        Rid *last_rid = neighbor_node->get_rid(neighbor_size - 1);
        
        // 插入到 node 的头部
        node->insert_pair(0, last_key, *last_rid);
        
        // 删除 neighbor_node 的最后一个键值对
        neighbor_node->erase_pair(neighbor_size - 1);
        
        // 更新父节点中指向 node 的 key（node 的第一个 key 变了）
        parent->set_key(index, node->get_key(0));
        
        // 如果是内部节点，更新移动过来的孩子节点的父指针
        if (!node->is_leaf_page()) {
            maintain_child(node, 0);
        }
    } else {
        // neighbor_node 是后继（右兄弟），从 neighbor_node 移第一个键值对到 node 的尾部
        char *first_key = neighbor_node->get_key(0);
        Rid *first_rid = neighbor_node->get_rid(0);
        
        // 插入到 node 的尾部
        int node_size = node->get_size();
        node->insert_pair(node_size, first_key, *first_rid);
        
        // 如果是内部节点，注意还要处理 neighbor_node 的第一个孩子指针
        // 内部节点：rids[0] 是最左孩子，插入到 node 尾部时，
        // node 的新 rids[node_size] 应该是 neighbor 的 rids[0]
        // 而 neighbor 删除第一个 key 后，rids[0] 变为原来的 rids[1]
        
        // 删除 neighbor_node 的第一个键值对
        neighbor_node->erase_pair(0);
        
        // 更新父节点中指向 neighbor_node 的 key（neighbor_node 的第一个 key 变了）
        parent->set_key(index + 1, neighbor_node->get_key(0));
        
        // 如果是内部节点，更新移动过来的孩子节点的父指针
        if (!node->is_leaf_page()) {
            maintain_child(node, node->get_size() - 1);
        }
    }
}

/**
 * @brief 合并(Coalesce)函数是将node和其直接前驱进行合并，也就是和它左边的neighbor_node进行合并；
 * 假设node一定在右边。如果上层传入的index=0，说明node在左边，那么交换node和neighbor_node，保证node在右边；合并到左结点，实际上就是删除了右结点；
 * Move all the key & value pairs from one page to its sibling page, and notify buffer pool manager to delete this page.
 * Parent page must be adjusted to take info of deletion into account. Remember to deal with coalesce or redistribute
 * recursively if necessary.
 *
 * @param neighbor_node sibling page of input "node" (neighbor_node是node的前结点)
 * @param node input from method coalesceOrRedistribute() (node结点是需要被删除的)
 * @param parent parent page of input "node"
 * @param index node在parent中的rid_idx
 * @return true means parent node should be deleted, false means no deletion happend
 * @note Assume that *neighbor_node is the left sibling of *node (neighbor -> node)
 */
bool IxIndexHandle::coalesce(IxNodeHandle **neighbor_node, IxNodeHandle **node, IxNodeHandle **parent, int index,
                             Transaction *transaction, bool *root_is_latched) {
    // Todo:
    // 1. 用index判断neighbor_node是否为node的前驱结点，若不是则交换两个结点，让neighbor_node作为左结点，node作为右结点
    // 2. 把node结点的键值对移动到neighbor_node中，并更新node结点孩子结点的父节点信息（调用maintain_child函数）
    // 3. 释放和删除node结点，并删除parent中node结点的信息，返回parent是否需要被删除
    // 提示：如果是叶子结点且为最右叶子结点，需要更新file_hdr_.last_leaf
    // 保证 neighbor_node 是左节点，node 是右节点
    if (index == 0) {
        // node 在左边（index=0），neighbor_node 在右边
        // 交换，让 neighbor_node 始终在左边
        std::swap(*neighbor_node, *node);
        index = 1;
    }
    
    int neighbor_size = (*neighbor_node)->get_size();
    int node_size = (*node)->get_size();
    
    // 1. 将 node 的所有键值对移到 neighbor_node 尾部
    char *node_keys = (*node)->get_key(0);
    Rid *node_rids = (*node)->get_rid(0);
    
    // 注意：内部节点的 rids 比 keys 多一个
    int n_pairs = node_size;
    if (!(*node)->is_leaf_page()) {
        // 内部节点：还要移动最后一个孩子指针 rids[node_size]
        // 先移动键值对，再移动最后一个孩子指针
    }
    
    (*neighbor_node)->insert_pairs(neighbor_size, node_keys, node_rids, n_pairs);
    
    // 如果是内部节点，还需要复制 node 的最后一个孩子指针
    if (!(*node)->is_leaf_page()) {
        Rid *last_child_rid = (*node)->get_rid(node_size);
        (*neighbor_node)->set_rid(neighbor_size + n_pairs, *last_child_rid);
        
        // 更新移过来的所有孩子的父指针
        for (int i = 0; i <= node_size; i++) {
            maintain_child(*neighbor_node, neighbor_size + i);
        }
    }
    
    // 2. 如果是叶子节点，更新链表指针
    if ((*node)->is_leaf_page()) {
        (*neighbor_node)->set_next_leaf((*node)->get_next_leaf());
        
        if ((*node)->get_next_leaf() != IX_LEAF_HEADER_PAGE) {
            IxNodeHandle *next_leaf = fetch_node((*node)->get_next_leaf());
            next_leaf->set_prev_leaf((*neighbor_node)->get_page_no());
            buffer_pool_manager_->unpin_page(next_leaf->get_page_id(), true);
        }
        
        // 更新 last_leaf
        if (file_hdr_->last_leaf_ == (*node)->get_page_no()) {
            file_hdr_->last_leaf_ = (*neighbor_node)->get_page_no();
        }
    }
    
    // 3. 从父节点中删除 node 对应的 key
    // key 在 index 位置（node 在右边，分隔 key 在 node 之前，即 keys[index-1]）
    int key_pos = index - 1;  // 分隔 key 的位置
    (*parent)->erase_pair(key_pos);
    
    // 4. 释放 node
    erase_leaf(*node);
    release_node_handle(**node);
    buffer_pool_manager_->unpin_page((*node)->get_page_id(), true);
    // 删除页面需要调用 delete_page
    buffer_pool_manager_->delete_page((*node)->get_page_id());
    
    // 5. 检查父节点是否需要继续合并
    if ((*parent)->get_size() < (*parent)->get_min_size()) {
        return true;  // 需要继续向上调整
    }
    
    return false;
}

/**
 * @brief 这里把iid转换成了rid，即iid的slot_no作为node的rid_idx(key_idx)
 * node其实就是把slot_no作为键值对数组的下标
 * 换而言之，每个iid对应的索引槽存了一对(key,rid)，指向了(要建立索引的属性首地址,插入/删除记录的位置)
 *
 * @param iid
 * @return Rid
 * @note iid和rid存的不是一个东西，rid是上层传过来的记录位置，iid是索引内部生成的索引槽位置
 */
Rid IxIndexHandle::get_rid(const Iid &iid) const {
    IxNodeHandle *node = fetch_node(iid.page_no);
    
    // 添加调试输出
    int key_value = -1;  // 尝试获取key值用于调试
    if(iid.slot_no < node->get_size()) {
        key_value = node->key_at(iid.slot_no);
    }
    
    static int count = 0;
    count++;
    if(count <= 20 || (iid.slot_no >= node->get_size())) {
        std::cerr << "DEBUG get_rid: count=" << count
                  << " page_no=" << iid.page_no
                  << " slot_no=" << iid.slot_no
                  << " node_size=" << node->get_size()
                  << " key_value=" << key_value << std::endl;
    }
    
    if (iid.slot_no >= node->get_size()) {
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        throw IndexEntryNotFoundError();
    }
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);  // unpin it!
    return *node->get_rid(iid.slot_no);
}

/**
 * @brief FindLeafPage + lower_bound
 *
 * @param key
 * @return Iid
 * @note 上层传入的key本来是int类型，通过(const char *)&key进行了转换
 * 可用*(int *)key转换回去
 */
Iid IxIndexHandle::lower_bound(const char *key) {
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    int pos = leaf->lower_bound(key);
    
    // 调试输出：前10条和2700附近
    int key_value = *reinterpret_cast<const int*>(key);
    if(key_value <= 10 || (key_value >= 2700 && key_value <= 2710)) {
        std::cerr << "DEBUG lower_bound: key=" << key_value 
                  << " leaf_page=" << leaf->get_page_no()
                  << " pos=" << pos
                  << " leaf_size=" << leaf->get_size()
                  << " first_leaf=" << file_hdr_->first_leaf_
                  << " last_leaf=" << file_hdr_->last_leaf_ << std::endl;
    }
    
    Iid iid = {.page_no = leaf->get_page_no(), .slot_no = pos};
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return iid;
}

/**
 * @brief FindLeafPage + upper_bound
 *
 * @param key
 * @return Iid
 */
Iid IxIndexHandle::upper_bound(const char *key) {
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    int pos = leaf->upper_bound(key);
    Iid iid = {.page_no = leaf->get_page_no(), .slot_no = pos};
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return iid;
}

/**
 * @brief 指向最后一个叶子的最后一个结点的后一个
 * 用处在于可以作为IxScan的最后一个
 *
 * @return Iid
 */
Iid IxIndexHandle::leaf_end() const {
    IxNodeHandle *node = fetch_node(file_hdr_->last_leaf_);
    Iid iid = {.page_no = file_hdr_->last_leaf_, .slot_no = node->get_size()};
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);  // unpin it!
    return iid;
}

/**
 * @brief 指向第一个叶子的第一个结点
 * 用处在于可以作为IxScan的第一个
 *
 * @return Iid
 */
Iid IxIndexHandle::leaf_begin() const {
    Iid iid = {.page_no = file_hdr_->first_leaf_, .slot_no = 0};
    return iid;
}

/**
 * @brief 获取一个指定结点
 *
 * @param page_no
 * @return IxNodeHandle*
 * @note pin the page, remember to unpin it outside!
 */
IxNodeHandle *IxIndexHandle::fetch_node(int page_no) const {
    // 检查页号是否有效
    if(page_no < 0 || page_no >= file_hdr_->num_pages_) {
        std::cerr << "ERROR: fetch_node called with invalid page_no=" << page_no 
                  << " num_pages=" << file_hdr_->num_pages_ 
                  << " root=" << file_hdr_->root_page_
                  << " first_leaf=" << file_hdr_->first_leaf_
                  << " last_leaf=" << file_hdr_->last_leaf_ << std::endl;
        throw InternalError("fetch_node: invalid page_no");
    }
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    IxNodeHandle *node = new IxNodeHandle(file_hdr_, page);
    
    return node;
}

/**
 * @brief 创建一个新结点
 *
 * @return IxNodeHandle*
 * @note pin the page, remember to unpin it outside!
 * 注意：对于Index的处理是，删除某个页面后，认为该被删除的页面是free_page
 * 而first_free_page实际上就是最新被删除的页面，初始为IX_NO_PAGE
 * 在最开始插入时，一直是create node，那么first_page_no一直没变，一直是IX_NO_PAGE
 * 与Record的处理不同，Record将未插入满的记录页认为是free_page
 */
IxNodeHandle *IxIndexHandle::create_node() {
    IxNodeHandle *node;
    file_hdr_->num_pages_++;

    PageId new_page_id = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    // 从3开始分配page_no，第一次分配之后，new_page_id.page_no=3，file_hdr_.num_pages=4
    Page *page = buffer_pool_manager_->new_page(&new_page_id);
    node = new IxNodeHandle(file_hdr_, page);
    return node;
}

/**
 * @brief 从node开始更新其父节点的第一个key，一直向上更新直到根节点
 *
 * @param node
 */
void IxIndexHandle::maintain_parent(IxNodeHandle *node) {
    IxNodeHandle *curr = node;
    while (curr->get_parent_page_no() != IX_NO_PAGE) {
        // Load its parent
        IxNodeHandle *parent = fetch_node(curr->get_parent_page_no());
        int rank = parent->find_child(curr);
        char *parent_key = parent->get_key(rank);
        char *child_first_key = curr->get_key(0);
        if (memcmp(parent_key, child_first_key, file_hdr_->col_tot_len_) == 0) {
            assert(buffer_pool_manager_->unpin_page(parent->get_page_id(), true));
            break;
        }
        memcpy(parent_key, child_first_key, file_hdr_->col_tot_len_);  // 修改了parent node
        curr = parent;

        assert(buffer_pool_manager_->unpin_page(parent->get_page_id(), true));
    }
}

/**
 * @brief 要删除leaf之前调用此函数，更新leaf前驱结点的next指针和后继结点的prev指针
 *
 * @param leaf 要删除的leaf
 */
void IxIndexHandle::erase_leaf(IxNodeHandle *leaf) {
    assert(leaf->is_leaf_page());

    IxNodeHandle *prev = fetch_node(leaf->get_prev_leaf());
    prev->set_next_leaf(leaf->get_next_leaf());
    buffer_pool_manager_->unpin_page(prev->get_page_id(), true);

    IxNodeHandle *next = fetch_node(leaf->get_next_leaf());
    next->set_prev_leaf(leaf->get_prev_leaf());  // 注意此处是SetPrevLeaf()
    buffer_pool_manager_->unpin_page(next->get_page_id(), true);
}

/**
 * @brief 删除node时，更新file_hdr_.num_pages
 *
 * @param node
 */
void IxIndexHandle::release_node_handle(IxNodeHandle &node) {
    file_hdr_->num_pages_--;
}

/**
 * @brief 将node的第child_idx个孩子结点的父节点置为node
 */
void IxIndexHandle::maintain_child(IxNodeHandle *node, int child_idx) {
    if (!node->is_leaf_page()) {
        //  Current node is inner node, load its child and set its parent to current node
        int child_page_no = node->value_at(child_idx);
        IxNodeHandle *child = fetch_node(child_page_no);
        child->set_parent_page_no(node->get_page_no());
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
    }
}

/**
 * @brief 打印B+树结构（用于调试）
 */
void IxIndexHandle::print_btree_structure() {
    std::cerr << "\n========== B+ TREE STRUCTURE ==========" << std::endl;
    std::cerr << "Root page: " << file_hdr_->root_page_ << std::endl;
    std::cerr << "First leaf: " << file_hdr_->first_leaf_ << std::endl;
    std::cerr << "Last leaf: " << file_hdr_->last_leaf_ << std::endl;
    std::cerr << "Total pages: " << file_hdr_->num_pages_ << std::endl;
    std::cerr << "Max size per node: " << (file_hdr_->btree_order_ + 1) << std::endl;
    std::cerr << "=======================================" << std::endl;
    
    // 打印root节点
    IxNodeHandle *root = fetch_node(file_hdr_->root_page_);
    
    std::cerr << "\n[ROOT NODE] Page " << root->get_page_no() << std::endl;
    std::cerr << "  is_leaf: " << root->is_leaf_page() << std::endl;
    std::cerr << "  num_key: " << root->get_size() << std::endl;
    
    if (!root->is_leaf_page()) {
        // 内部节点：打印keys和children
        std::cerr << "  keys: [";
        for (int i = 0; i < root->get_size(); i++) {
            if (i > 0) std::cerr << ", ";
            std::cerr << root->key_at(i);
        }
        std::cerr << "]" << std::endl;
        
        std::cerr << "  children (page_no): [";
        for (int i = 0; i <= root->get_size(); i++) {
            if (i > 0) std::cerr << ", ";
            std::cerr << root->value_at(i);
        }
        std::cerr << "]" << std::endl;
        
        // 打印每个叶子节点的信息
        std::cerr << "\n[LEAF NODES]" << std::endl;
        page_id_t leaf_page = file_hdr_->first_leaf_;
        int leaf_count = 0;
        while (leaf_page != INVALID_PAGE_ID && leaf_count < 25) {  // 限制打印前25个叶子
            IxNodeHandle *leaf = fetch_node(leaf_page);
            
            std::cerr << "  Leaf " << leaf_count << " (Page " << leaf_page << "): ";
            std::cerr << "num_key=" << leaf->get_size();
            
            if (leaf->get_size() > 0) {
                std::cerr << ", range=[" << leaf->key_at(0) << ", " << leaf->key_at(leaf->get_size()-1) << "]";
            }
            std::cerr << std::endl;
            
            page_id_t next_leaf = leaf->get_next_leaf();
            buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
            
            if (next_leaf == leaf_page) break;  // 防止死循环
            leaf_page = next_leaf;
            leaf_count++;
        }
    } else {
        // root也是叶子节点
        std::cerr << "  keys: [";
        for (int i = 0; i < root->get_size(); i++) {
            if (i > 0) std::cerr << ", ";
            std::cerr << root->key_at(i);
        }
        std::cerr << "]" << std::endl;
    }
    
    buffer_pool_manager_->unpin_page(root->get_page_id(), false);
    std::cerr << "========================================\n" << std::endl;
}

void IxIndexHandle::clear_entries() {
    buffer_pool_manager_->flush_all_pages(fd_);
    
    file_hdr_->root_page_ = IX_INIT_ROOT_PAGE;
    file_hdr_->first_leaf_ = IX_INIT_ROOT_PAGE;
    file_hdr_->last_leaf_ = IX_INIT_ROOT_PAGE;
    file_hdr_->num_pages_ = IX_INIT_NUM_PAGES;
    
    IxNodeHandle root = IxNodeHandle(file_hdr_, buffer_pool_manager_->fetch_page(PageId{fd_, IX_INIT_ROOT_PAGE}));
    root.page_hdr->parent = INVALID_PAGE_ID;
    root.page_hdr->num_key = 0;
    root.page_hdr->is_leaf = true;
    root.page_hdr->next_leaf = INVALID_PAGE_ID;
    root.page_hdr->prev_leaf = INVALID_PAGE_ID;
    
    BufferPoolManager::mark_dirty(root.page);
    buffer_pool_manager_->unpin_page(root.page->get_page_id(), true);
    
    char *buf = new char[file_hdr_->tot_len_];
    file_hdr_->serialize(buf);
    disk_manager_->write_page(fd_, IX_FILE_HDR_PAGE, buf, file_hdr_->tot_len_);
    delete[] buf;
}