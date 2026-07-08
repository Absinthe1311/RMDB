/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sm_manager.h"

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>

#include "index/ix.h"
#include "record/rm.h"
#include "record_printer.h"

/**
 * @description: 判断是否为一个文件夹
 * @return {bool} 返回是否为一个文件夹
 * @param {string&} db_name 数据库文件名称，与文件夹同名
 */
bool SmManager::is_dir(const std::string& db_name) {
    struct stat st;
    return stat(db_name.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * @description: 创建数据库，所有的数据库相关文件都放在数据库同名文件夹下
 * @param {string&} db_name 数据库名称
 */
void SmManager::create_db(const std::string& db_name) {
    if (is_dir(db_name)) {
        throw DatabaseExistsError(db_name);
    }
    //为数据库创建一个子目录
    std::string cmd = "mkdir " + db_name;
    if (system(cmd.c_str()) < 0) {  // 创建一个名为db_name的目录
        throw UnixError();
    }
    if (chdir(db_name.c_str()) < 0) {  // 进入名为db_name的目录
        throw UnixError();
    }
    //创建系统目录
    DbMeta *new_db = new DbMeta();
    new_db->name_ = db_name;

    // 注意，此处ofstream会在当前目录创建(如果没有此文件先创建)和打开一个名为DB_META_NAME的文件
    std::ofstream ofs(DB_META_NAME);

    // 将new_db中的信息，按照定义好的operator<<操作符，写入到ofs打开的DB_META_NAME文件中
    ofs << *new_db;  // 注意：此处重载了操作符<<

    delete new_db;

    // 创建日志文件
    disk_manager_->create_file(LOG_FILE_NAME);

    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 删除数据库，同时需要清空相关文件以及数据库同名文件夹
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::drop_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    std::string cmd = "rm -r " + db_name;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 打开数据库，找到数据库对应的文件夹，并加载数据库元数据和相关文件
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::open_db(const std::string& db_name) {
    // 1. 数据库对应的文件夹必须存在
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }

    // 2. 进入该数据库对应的目录
    if (chdir(db_name.c_str()) < 0) {
        throw UnixError();
    }

    // 3. 读取并恢复数据库元数据
    std::ifstream ifs(DB_META_NAME);
    ifs >> db_;   // 依赖已经定义好的 operator>>，将文件内容反序列化到 db_ 中

    // 4. 为每张表打开对应的记录文件句柄
    for (auto &entry : db_.tabs_) {
        auto &tab_name = entry.first;
        fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));
    }

    // 5. 为每张表上的每个索引打开索引句柄
    for (auto &entry : db_.tabs_) {
        auto &tab = entry.second;
        for (auto &index_meta : tab.indexes) {
            std::string index_name = ix_manager_->get_index_name(tab.name, index_meta.cols);
            // 避免重复打开（理论上不会重复）
            if (ihs_.find(index_name) == ihs_.end()) {
                ihs_.emplace(index_name, ix_manager_->open_index(tab.name, index_meta.cols));
            }
        }
    }
}

/**
 * @description: 把数据库相关的元数据刷入磁盘中
 */
void SmManager::flush_meta() {
    // 默认清空文件
    std::ofstream ofs(DB_META_NAME);
    ofs << db_;
}

/**
 * @description: 关闭数据库并把数据落盘
 */
void SmManager::close_db() {
    // 1. 把内存中的元数据写回磁盘
    flush_meta();

    // 2. 关闭所有已打开的记录文件句柄
    for (auto &entry : fhs_) {
        rm_manager_->close_file(entry.second.get());
    }
    fhs_.clear();

    // 3. 关闭所有已打开的索引句柄
    for (auto &entry : ihs_) {
        ix_manager_->close_index(entry.second.get());
    }
    ihs_.clear();

    // 4. 清空内存中的数据库元数据
    db_.name_.clear();
    db_.tabs_.clear();

    // 5. 回到上一级目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 显示所有的表,通过测试需要将其结果写入到output.txt,详情看题目文档
 * @param {Context*} context 
 */
void SmManager::show_tables(Context* context) {
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << "| Tables |\n";
    RecordPrinter printer(1);
    printer.print_separator(context);
    printer.print_record({"Tables"}, context);
    printer.print_separator(context);
    for (auto &entry : db_.tabs_) {
        auto &tab = entry.second;
        printer.print_record({tab.name}, context);
        outfile << "| " << tab.name << " |\n";
    }
    printer.print_separator(context);
    outfile.close();
}

/**
 * @description: 显示表的元数据
 * @param {string&} tab_name 表名称
 * @param {Context*} context 
 */
void SmManager::desc_table(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);

    std::vector<std::string> captions = {"Field", "Type", "Index"};
    RecordPrinter printer(captions.size());
    // Print header
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);
    // Print fields
    for (auto &col : tab.cols) {
        std::vector<std::string> field_info = {col.name, coltype2str(col.type), col.index ? "YES" : "NO"};
        printer.print_record(field_info, context);
    }
    // Print footer
    printer.print_separator(context);
}

/**
 * @description: 创建表
 * @param {string&} tab_name 表的名称
 * @param {vector<ColDef>&} col_defs 表的字段
 * @param {Context*} context 
 */
void SmManager::create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context) {
    if (db_.is_table(tab_name)) {
        throw TableExistsError(tab_name);
    }
    // Create table meta
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    for (auto &col_def : col_defs) {
        ColMeta col = {.tab_name = tab_name,
                       .name = col_def.name,
                       .type = col_def.type,
                       .len = col_def.len,
                       .offset = curr_offset,
                       .index = false};
        curr_offset += col_def.len;
        tab.cols.push_back(col);
    }
    // Create & open record file
    int record_size = curr_offset;  // record_size就是col meta所占的大小（表的元数据也是以记录的形式进行存储的）
    rm_manager_->create_file(tab_name, record_size);
    db_.tabs_[tab_name] = tab;
    // fhs_[tab_name] = rm_manager_->open_file(tab_name);
    fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));

    flush_meta();
}

/**
 * @description: 删除表
 * @param {string&} tab_name 表的名称
 * @param {Context*} context
 */
void SmManager::drop_table(const std::string& tab_name, Context* context) {
    // 1. 表必须存在
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }

    TabMeta &tab = db_.get_table(tab_name);

    // 2. 若表上存在索引，先逐一删除索引
    //    拷贝一份索引列表再遍历，因为drop_index内部会修改tab.indexes，直接遍历原容器会有迭代器失效风险
    auto indexes = tab.indexes;
    for (auto &index : indexes) {
        drop_index(tab_name, index.cols, context);
    }

    // 3. 关闭并销毁该表的记录文件
    rm_manager_->close_file(fhs_.at(tab_name).get());
    rm_manager_->destroy_file(tab_name);
    fhs_.erase(tab_name);

    // 4. 从数据库元数据中移除该表
    db_.tabs_.erase(tab_name);

    // 5. 元数据落盘
    flush_meta();
}

/**
 * @description: 创建索引
 * @param {string&} tab_name 表的名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::create_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    // 1. 表必须存在
    TabMeta &tab = db_.get_table(tab_name);

    // 2. 收集索引列的元数据
    std::vector<ColMeta> col_metas;
    for (auto &col_name : col_names) {
        col_metas.push_back(*tab.get_col(col_name));
    }

    // 3. 该索引不能重复创建（使用精确匹配）
    if (tab.is_index_exact(col_names)) {
        throw IndexExistsError(tab_name, col_names);
    }

    // 4. 创建索引文件
    ix_manager_->create_index(tab_name, col_metas);

    // 5. 打开索引句柄
    auto ih = ix_manager_->open_index(tab_name, col_metas);

    int total_len = 0;
    for (auto &col : col_metas) total_len += col.len;

    // 6. 扫描表中已有的记录，逐条插入索引
    RmFileHandle *fh = fhs_.at(tab_name).get();
    int insert_count = 0;
    for (RmScan scan(fh); !scan.is_end(); scan.next()) {
        Rid rid = scan.rid();
        auto record = fh->get_record(rid, context);

        // 从记录中抽取索引列对应的数据，拼接成索引key
        int total_len = 0;
        for (auto &col : col_metas) total_len += col.len;
        char *key = new char[total_len];
        int offset = 0;
        for (auto &col : col_metas) {
            memcpy(key + offset, record->data + col.offset, col.len);
            offset += col.len;
        }

        try {
            ih->insert_entry(key, rid, context->txn_);
            insert_count++;
        } catch (const std::exception &e) {
            std::cerr << "ERROR: Failed to insert index entry " << insert_count 
                      << " rid=(" << rid.page_no << "," << rid.slot_no << "): " 
                      << e.what() << std::endl;
            delete[] key;
            throw;
        }
        delete[] key;
    }
    
    std::cerr << "DEBUG: Successfully inserted " << insert_count << " index entries" << std::endl;
    
    // 打印B+树结构（调试）
    ih->print_btree_structure();

    // 7. 记录索引句柄，更新表元数据
    ihs_.emplace(ix_manager_->get_index_name(tab_name, col_metas), std::move(ih));  

    IndexMeta index_meta;
    index_meta.tab_name = tab_name;
    index_meta.col_num = static_cast<int>(col_names.size());
    index_meta.cols = col_metas;
    index_meta.col_tot_len = total_len; 
    tab.indexes.push_back(index_meta);

    for (auto &col_name : col_names) {
        tab.get_col(col_name)->index = true;
    }

    // 8. 元数据落盘
    flush_meta();    
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);

    std::vector<ColMeta> col_metas;
    for (auto &col_name : col_names) {
        col_metas.push_back(*tab.get_col(col_name));
    }

    drop_index(tab_name, col_metas, context);    
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<ColMeta>&} 索引包含的字段元数据
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);

    std::vector<std::string> col_names;
    for (auto &col : cols) {
        col_names.push_back(col.name);
    }

    // 1. 索引必须存在（元数据检查，使用精确匹配）
    if (!tab.is_index_exact(col_names)) {
        throw IndexNotFoundError(tab_name, col_names);
    }

    // 2. 关闭并销毁索引文件（强化：处理句柄缺失的情况）
    std::string index_name = ix_manager_->get_index_name(tab_name, cols);
    auto it = ihs_.find(index_name);
    if (it != ihs_.end()) {
        // 句柄已打开，正常关闭
        ix_manager_->close_index(it->second.get());
        ix_manager_->destroy_index(tab_name, cols);
        ihs_.erase(it);
    } else {
        // 句柄未打开（可能因重启后未恢复），直接销毁底层文件
        if (ix_manager_->exists(tab_name, cols)) {
            ix_manager_->destroy_index(tab_name, cols);
        }
    }

    // 3. 从表元数据中移除该索引（使用精确匹配）
    auto meta_it = tab.get_index_meta_exact(col_names);
    tab.indexes.erase(meta_it);

    // 4. 若某列不再被任何索引覆盖，重置其index标记为false
    for (auto &col_name : col_names) {
        bool still_indexed = false;
        for (auto &index : tab.indexes) {
            auto it = std::find_if(index.cols.begin(), index.cols.end(),
                                    [&](const ColMeta &col) {
                                        return col.tab_name == tab_name && col.name == col_name;
                                    });
            if (it != index.cols.end()) {
                still_indexed = true;
                break;
            }
        }
        if (!still_indexed) {
            tab.get_col(col_name)->index = false;
        }
    }

    // 5. 元数据落盘
    flush_meta();
}

void SmManager::show_index(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    
    // 准备输出：客户端缓冲区 + 文件
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    
    // 表头
    std::vector<std::string> captions = {"table_name", "unique", "columns"};
    RecordPrinter printer(3);
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);
    
    // 遍历索引
    for (auto &index : tab.indexes) {
        std::string cols_str = "(";
        for (size_t i = 0; i < index.cols.size(); i++) {
            if (i > 0) cols_str += ",";
            cols_str += index.cols[i].name;
        }
        cols_str += ")";
        
        std::vector<std::string> row = {tab_name, "unique", cols_str};
        
        // 输出到客户端缓冲区
        printer.print_record(row, context);
        
        // 输出到文件
        outfile << "| " << tab_name << " | unique | " << cols_str << " |\n";
    }
    
    // 表尾
    printer.print_separator(context);
    
    outfile.close();
}