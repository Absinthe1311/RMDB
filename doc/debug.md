# 系统故障恢复测试调试指南

## 一、准备工作

### 1.1 环境清理

每次测试前，**清空数据库目录**，保证从干净状态开始：

```bash
# 清理旧数据库
rm -rf ~/rmdb_data/test_db*
rm -rf /home/absinthe/rmdb/test_db*

# 重新编译（如果修改了代码）
cd /home/absinthe/rmdb/build
make -j4
```

### 1.2 调试日志开关

当前实现已包含详细日志，关键位置输出：
- `[Recovery]` - 日志扫描阶段
- `[Redo]` - 重做阶段
- `[Undo]` - 撤销阶段
- `[LOG]` - 日志记录阶段（如需要可添加）

### 1.3 关键概念确认

**WAL协议要点**：
- 日志先于数据落盘
- Commit前必须flush日志
- Crash时缓冲区内容可能丢失

**恢复三阶段**：
1. **Analyze**：扫描日志，构建活跃事务表（ATT）
2. **Redo**：重放所有修改操作（无条件）
3. **Undo**：撤销未提交事务的操作（逆序）

## 二、如何模拟 Crash

### 2.1 正确的Crash方式

**方法1：使用kill -9（推荐）**
```bash
# 找到server进程
ps aux | grep rmdb
# 输出示例：absinthe 12345 ... ./bin/rmdb test_db

# 强制杀死（模拟系统崩溃）
kill -9 12345
```

**方法2：使用Ctrl+C**
```bash
# 在server终端直接按 Ctrl+C
# 这会触发sigint_handler，会先flush日志再退出
# ⚠️ 注意：这不是真正的崩溃场景！
```

**关键区别**：
- `kill -9`：硬中断，缓冲区内容丢失 ✅
- `Ctrl+C`：优雅退出，日志已flush ❌（不是真正的故障）

### 2.2 测试建议

**测试时务必使用 `kill -9`**，不要用Ctrl+C，否则：
- 日志缓冲区已flush
- 没有真正的数据丢失
- 恢复逻辑不会被真正测试

## 三、单线程无索引测试（crash_recovery_single_thread_test）

### 3.1 测试步骤

**步骤1：启动服务器**
```bash
cd /home/absinthe/rmdb
./bin/rmdb test_db1
```

**步骤2：客户端操作（另一终端）**
```bash
cd /home/absinthe/rmdb/rmdb_client/build
./rmdb_client
```

```sql
-- 创建表
create table t1 (id int, num int);

-- 事务1：插入并提交
begin;
insert into t1 values(1, 1);
commit;

-- 事务2：插入但不提交
begin;
insert into t1 values(2, 2);
-- ⚠️ 此时不要commit，保持客户端连接
```

**步骤3：模拟崩溃**
```bash
# 新终端：找到server进程并杀死
ps aux | grep "rmdb test_db1"
# 假设输出：absinthe 12345 ... ./bin/rmdb test_db1
kill -9 12345
```

**步骤4：重启服务器**
```bash
cd /home/absinthe/rmdb
./bin/rmdb test_db1
```

观察恢复日志，应该看到：
```
[Recovery] Log size: XXX bytes
[Recovery] Log 0: type=BEGIN, lsn=0, txn=0, prev_lsn=-1
[Recovery] Log 1: type=COMMIT, lsn=1, txn=0, prev_lsn=0
[Recovery] Log 2: type=BEGIN, lsn=2, txn=1, prev_lsn=-1
[Recovery] Log 3: type=INSERT, lsn=3, txn=1, prev_lsn=2
[Recovery] Log 4: type=COMMIT, lsn=4, txn=1, prev_lsn=3
[Recovery] Log 5: type=BEGIN, lsn=5, txn=2, prev_lsn=-1
[Recovery] Log 6: type=INSERT, lsn=6, txn=2, prev_lsn=5
[Redo] Redo INSERT: table=t1, rid=(X,Y)
[Redo] Redo INSERT: table=t1, rid=(X,Y)
[Undo] Active transactions: 1
[Undo]   Active txn 2, last_lsn=6
[Undo] Undo INSERT: table=t1, rid=(X,Y)
```

**步骤5：验证结果**
```bash
# 客户端重新连接
./rmdb_client
```

```sql
select * from t1;
```

**预期输出**：
```
+------------------+------------------+
|               id |              num |
+------------------+------------------+
|                1 |                1 |
+------------------+------------------+
Total record(s): 1
```

### 3.2 检查要点

**检查Redo阶段**：
- ✅ 应该重放**两条**INSERT操作
- ✅ 因为崩溃前数据可能未落盘
- ✅ 不检查页面LSN，无条件重放

**检查Undo阶段**：
- ✅ ATT应该包含**事务2**（未提交）
- ✅ 撤销事务2的INSERT操作
- ✅ 最终只剩事务1的数据

**常见错误**：
| 现象 | 原因 | 解决方法 |
|------|------|----------|
| 表为空 | Redo失败 | 检查insert_record调用 |
| 有两条记录 | Undo失败 | 检查活跃事务识别 |
| 数据错乱 | RID计算错误 | 检查序列化/反序列化 |

## 四、单线程+索引测试（crash_recovery_single_thread_test_2）

### 4.1 测试步骤

**服务器启动**：
```bash
./bin/rmdb test_db2
```

**客户端操作**：
```sql
create table t1 (id int, num int);
create index t1(id);

begin;
insert into t1 values(1, 1);
commit;

begin;
insert into t1 values(2, 2);
-- kill -9
```

**崩溃后重启**：
```bash
./bin/rmdb test_db2
```

**验证**：
```sql
-- 全表扫描
select * from t1;
-- 预期：(1, 1)

-- 索引查询
select * from t1 where id=1;
-- 预期：(1, 1)

select * from t1 where id=2;
-- 预期：空结果，不报错
```

### 4.2 检查要点

**索引一致性**：
- ✅ 表数据：(1, 1)
- ✅ 索引数据：只有id=1的索引项
- ✅ 索引查询不报错

**常见错误**：
| 现象 | 原因 | 解决方法 |
|------|------|----------|
| where id=2报错 | 索引未回滚 | 检查undo中的索引删除 |
| where id=2有结果 | 索引残留 | 检查索引key构造 |
| where id=1无结果 | 索引被误删 | 检查undo条件判断 |

## 五、索引一致性专项测试（crash_recovery_index_test）

### 5.1 测试步骤

**客户端操作**：
```sql
create table t1 (id int, num int);
create index t1(id);

-- 插入初始数据
insert into t1 values(1, 10);
insert into t1 values(2, 20);

-- 未提交事务（混合操作）
begin;
update t1 set num=100 where id=1;
delete from t1 where id=2;
insert into t1 values(3, 30);
-- kill -9
```

**重启后验证**：
```sql
-- 全表查询
select * from t1 order by id;
-- 预期：(1, 10) 和 (2, 20)

-- 索引查询
select * from t1 where id=1;
-- 预期：(1, 10)，不是(1, 100)

select * from t1 where id=2;
-- 预期：(2, 20)

select * from t1 where id=3;
-- 预期：空结果
```

### 5.2 检查要点

**UPDATE回滚**：
- ✅ 表数据：num恢复为10
- ✅ 索引：id=1指向正确记录

**DELETE回滚**：
- ✅ 表数据：记录(2,20)存在
- ✅ 索引：id=2的索引项存在

**INSERT回滚**：
- ✅ 表数据：无id=3记录
- ✅ 索引：无id=3索引项

**常见错误**：
| 现象 | 原因 | 解决方法 |
|------|------|----------|
| where id=1返回(1,100) | UPDATE未完全回滚 | 检查UPDATE undo逻辑 |
| where id=2报错 | DELETE的索引未恢复 | 检查DELETE undo的索引插入 |
| 全表扫描和索引查询结果不一致 | 索引数据不一致 | 重点检查索引回滚逻辑 |

### 5.3 UPDATE的Undo逻辑

**正确的回滚顺序**：
```cpp
// UPDATE的undo：
1. 恢复表数据（写回old_value）
2. 删除新索引key
3. 插入旧索引key
```

**关键检查**：
```cpp
// 检查update_log的序列化是否正确
old_value_.data  // 更新前的值
new_value_.data  // 更新后的值
```

## 六、多线程测试（crash_recovery_multi_thread_test）

### 6.1 测试步骤

**启动服务器**：
```bash
./bin/rmdb test_db3
```

**客户端A**：
```sql
create table t1 (id int, num int);
begin;
insert into t1 values(1, 1);
insert into t1 values(3, 3);
-- 不要commit
```

**客户端B**（另一终端）：
```sql
begin;
insert into t1 values(2, 2);
commit;
```

**崩溃**：
```bash
kill -9 <server_pid>
```

**重启并验证**：
```sql
select * from t1 order by id;
-- 预期：只有 (2, 2)
```

### 6.2 检查要点

**并发下的原子性**：
- ✅ LSN分配：`global_lsn_.fetch_add(1)`
- ✅ prev_lsn维护：`txn->set_prev_lsn(lsn)`
- ✅ 日志缓冲区：加锁保护

**检查代码**：
```cpp
// LogManager::add_log_to_buffer
std::unique_lock<std::mutex> lock(latch_);  // ✅ 加锁

lsn_t lsn = global_lsn_.fetch_add(1);  // ✅ 原子操作
log_record->lsn_ = lsn;

// 检查缓冲区空间
if(log_buffer_.is_full(log_size)) {
    flush_log_to_disk();
}

// 序列化（在锁保护内）
char* dest = log_buffer_.buffer_ + log_buffer_.offset_;
log_record->serialize(dest);
log_buffer_.offset_ += log_size;

return lsn;
```

**常见错误**：
| 现象 | 原因 | 解决方法 |
|------|------|----------|
| 日志内容错乱 | 缓冲区并发写冲突 | 确保序列化在锁内 |
| prev_lsn指向错误 | 多线程竞争 | 检查set_prev_lsn时机 |
| 恢复后数据不一致 | LSN分配问题 | 检查global_lsn原子性 |

### 6.3 独立测试：并发日志正确性

**测试目的**：验证并发下日志本身是否正确

**测试脚本**（保存为test_concurrent_log.sh）：
```bash
#!/bin/bash
./bin/rmdb test_concurrent &

# 两个客户端并发插入
for i in {1..10}; do
    (echo "insert into t1 values($i, $i);"; sleep 0.1) | nc localhost 8765 &
done

wait
sleep 2
kill -9 $!
```

**重启后检查日志**：
```bash
# 查看日志文件
od -A x -t x1z -v test_concurrent/db.log

# 或用自定义工具解析
./log_parser test_concurrent/db.log
```

**预期**：
- ✅ 所有日志记录完整（无截断/覆盖）
- ✅ 每个事务的日志链正确（通过prev_lsn连接）
- ✅ LSN严格递增，无重复

## 七、大数据量测试（crash_recovery_large_data_test）

### 7.1 测试策略

**数据量**：大量记录，多个并发事务

**测试脚本**（保存为test_large_data.sh）：
```bash
#!/bin/bash
DB_NAME="test_large"
RECORDS=1000

./bin/rmdb $DB_NAME &
SERVER_PID=$!
sleep 2

# 建表
echo "create table t1 (id int, num int);" | nc localhost 8765

# 插入大量数据（部分提交，部分未提交）
for i in $(seq 1 $RECORDS); do
    if [ $((i % 2)) -eq 0 ]; then
        # 提交事务
        (echo "begin; insert into t1 values($i, $i); commit;"; sleep 0.01) | nc localhost 8765 &
    else
        # 未提交事务
        (echo "begin; insert into t1 values($i, $i);"; sleep 0.01) | nc localhost 8765 &
    fi
done

wait
sleep 2
kill -9 $SERVER_PID
sleep 1

# 重启验证
./bin/rmdb $DB_NAME &
SERVER_PID=$!
sleep 3

echo "select count(*) from t1;" | nc localhost 8765
# 预期：RECORDS/2 条记录（只有偶数记录）

kill $SERVER_PID
```

### 7.2 检查要点

**性能要求**：
- ✅ 恢复时间可接受（不超时）
- ✅ 内存使用合理
- ✅ 无段错误/崩溃

**正确性要求**：
- ✅ 已提交事务全部恢复
- ✅ 未提交事务全部撤销

### 7.3 性能优化建议

**当前复杂度**：
```cpp
// undo阶段查找：O(n³)
for(txn)          // O(活跃事务数)
  for(lsn)        // O(日志链长度)
    for(offset)   // O(总日志数)  ← 瓶颈
```

**优化方案**：
```cpp
// 预建索引：O(n²)
std::unordered_map<lsn_t, int> lsn_to_offset;
for(offset : log_offsets) {
    LogRecord log;
    log.deserialize(log_buffer + offset);
    lsn_to_offset[log.lsn_] = offset;
}

// 查找：O(1)
int offset = lsn_to_offset[lsn];
```

## 八、常见问题排查清单

### 8.1 恢复流程检查

**问题：数据完全丢失**
```
可能原因：
1. Redo阶段异常，跳过所有操作
2. 日志未正确写入磁盘
3. 日志文件被误删

排查：
- 检查[Redo]日志输出
- 检查db.log文件是否存在
- 检查日志文件大小是否为0
```

**问题：未提交事务的数据存在**
```
可能原因：
1. Undo阶段未识别活跃事务
2. Undo操作未执行
3. Undo逻辑错误

排查：
- 检查[Undo] Active transactions数量
- 检查[Undo] Undoing transaction日志
- 检查具体的UNDO操作是否执行
```

**问题：已提交事务的数据丢失**
```
可能原因：
1. Redo未重放已提交事务
2. Undo误撤销已提交事务

排查：
- 检查COMMIT日志是否记录
- 检查ATT是否包含已提交事务（不应该）
- 检查Redo是否重放该操作
```

### 8.2 日志记录检查

**问题：日志内容错误**
```cpp
// 检查点1：序列化是否完整
void serialize(char* dest) const override {
    LogRecord::serialize(dest);  // 基类序列化
    int offset = OFFSET_LOG_DATA;
    // ... 检查每个字段的序列化
}

// 检查点2：反序列化是否对称
void deserialize(const char* src) override {
    // 必须与serialize对称
}

// 检查点3：log_tot_len_是否正确
log_tot_len_ = LOG_HEADER_SIZE + ...  // 必须等于实际序列化长度
```

**问题：prev_lsn链断裂**
```cpp
// 每条日志必须设置prev_lsn
InsertLogRecord insert_log(...);
insert_log.prev_lsn_ = txn->get_prev_lsn();  // ✅ 正确

lsn_t lsn = log_mgr->add_log_to_buffer(&insert_log);
txn->set_prev_lsn(lsn);  // ✅ 更新prev_lsn
```

### 8.3 索引一致性检查

**检查方法**：
```sql
-- 全表扫描
select * from t1 order by id;

-- 索引扫描（每条记录）
select * from t1 where id=X;

-- 对比结果是否一致
```

**如果发现不一致**：
```cpp
// 检查Undo中的索引处理
case LogType::INSERT:
    // 撤销INSERT = 删除记录 + 删除索引
    rm_file_handle->delete_record(...);
    for(index) {
        ih->delete_entry(key, ...);  // ← 检查这里
    }
    break;

case LogType::DELETE:
    // 撤销DELETE = 插入记录 + 插入索引
    rm_file_handle->insert_record(...);
    for(index) {
        ih->insert_entry(key, ...);  // ← 检查这里
    }
    break;

case LogType::UPDATE:
    // 撤销UPDATE = 恢复旧值 + 删除新索引 + 插入旧索引
    rm_file_handle->update_record(...);
    for(index) {
        ih->delete_entry(new_key, ...);  // ← 删除新key
        ih->insert_entry(old_key, ...);  // ← 插入旧key
    }
    break;
```

## 九、自动化测试脚本

### 9.1 完整测试脚本

保存为 `test_all_recovery.sh`：

```bash
#!/bin/bash

echo "========== 系统故障恢复完整测试 =========="

# 测试函数
run_test() {
    local test_name=$1
    local db_name=$2
    local sql_commands=$3
    local expected_result=$4
    
    echo ""
    echo "---------- $test_name ----------"
    
    # 清理
    rm -rf $db_name
    mkdir -p $db_name
    
    # 启动服务器
    ./bin/rmdb $db_name > ${db_name}/server.log 2>&1 &
    SERVER_PID=$!
    sleep 2
    
    # 执行SQL
    {
        sleep 1
        echo "$sql_commands"
        sleep 0.5
    } | nc localhost 8765
    
    # 崩溃
    kill -9 $SERVER_PID
    sleep 1
    
    # 重启
    ./bin/rmdb $db_name > ${db_name}/recovery.log 2>&1 &
    SERVER_PID=$!
    sleep 3
    
    # 验证
    {
        sleep 1
        echo "select * from t1;"
        sleep 0.5
        echo "exit"
    } | nc localhost 8765 > ${db_name}/result.txt
    
    kill $SERVER_PID 2>/dev/null
    
    # 对比结果
    if grep -q "$expected_result" ${db_name}/result.txt; then
        echo "✅ $test_name PASSED"
    else
        echo "❌ $test_name FAILED"
        echo "Expected: $expected_result"
        echo "Got:"
        cat ${db_name}/result.txt
    fi
}

# 测试1：单线程无索引
run_test "Test1: Single thread no index" \
    "test1" \
    "create table t1 (id int, num int); begin; insert into t1 values(1,1); commit; begin; insert into t1 values(2,2);" \
    "Total record(s): 1"

# 测试2：单线程有索引
run_test "Test2: Single thread with index" \
    "test2" \
    "create table t1 (id int, num int); create index t1(id); begin; insert into t1 values(1,1); commit; begin; insert into t1 values(2,2);" \
    "Total record(s): 1"

# 可以继续添加更多测试...

echo ""
echo "========== 测试完成 =========="
```

### 9.2 使用方法

```bash
cd /home/absinthe/rmdb
chmod +x test_all_recovery.sh
./test_all_recovery.sh
```

## 十、调试技巧

### 10.1 查看二进制日志

```bash
# 使用od查看
od -A x -t x1z -v test_db/db.log

# 输出示例：
# 000000 00 00 00 00 00 00 00 00 01 00 00 00 ...
```

### 10.2 打印日志链

在RecoveryManager中添加：

```cpp
void print_log_chain(txn_id_t txn_id, const std::vector<lsn_t>& chain) {
    std::cerr << "Transaction " << txn_id << " log chain:" << std::endl;
    for(lsn_t lsn : chain) {
        std::cerr << "  LSN=" << lsn;
        // 找到对应日志的类型
        // ...
        std::cerr << " type=" << LogTypeStr[type] << std::endl;
    }
}
```

### 10.3 对比期望输出

创建 `expected.txt`：
```
1  1
Total record(s): 1
```

对比：
```bash
diff expected.txt test_db/result.txt
```

## 十一、测试通过标准

### 11.1 crash_recovery_single_thread_test

✅ 条件：
- 重启后表中有且仅有(1,1)这一条记录
- 恢复日志显示：
  - Redo重放了2条INSERT
  - Undo识别了1个活跃事务
  - Undo撤销了1条INSERT

### 11.2 crash_recovery_single_thread_test_2

✅ 条件：
- 重启后表中有且仅有(1,1)
- 索引查询`where id=2`返回空（不报错）
- 索引查询`where id=1`返回(1,1)

### 11.3 crash_recovery_index_test

✅ 条件：
- UPDATE回滚：`where id=1`返回(1,10)而非(1,100)
- DELETE回滚：`where id=2`返回(2,20)
- INSERT回滚：`where id=3`返回空

### 11.4 crash_recovery_multi_thread_test

✅ 条件：
- 已提交事务的数据存在
- 未提交事务的数据不存在
- 并发下日志无错乱

### 11.5 crash_recovery_large_data_test

✅ 条件：
- 恢复时间不超时
- 内存使用合理
- 结果正确性同上

## 十二、问题反馈模板

如果测试失败，请提供：

```
## 测试名称
crash_recovery_xxx_test

## 操作步骤
1. ...
2. ...

## 实际结果
- 表数据：...
- 索引查询：...

## 期望结果
- 表数据：...
- 索引查询：...

## 恢复日志
[粘贴Recovery/Redo/Undo日志]

## 环境信息
- 操作系统：
- 数据库版本：
- 测试数据量：
```

## 附录：快速参考卡片

### A. 常用命令

```bash
# 启动服务器
./bin/rmdb test_db

# 启动客户端
./rmdb_client

# 查找进程
ps aux | grep rmdb

# 强制杀死
kill -9 <pid>

# 查看日志
cat test_db/server.log

# 清理数据库
rm -rf test_db
```

### B. 恢复三阶段速查

| 阶段 | 作用 | 输入 | 输出 |
|------|------|------|------|
| Analyze | 扫描日志 | db.log | ATT, DPT |
| Redo | 重放操作 | 所有日志 | 数据落盘 |
| Undo | 撤销事务 | ATT | 恢复一致性 |

### C. 日志类型速查

| 类型 | 记录时机 | Undo操作 |
|------|----------|----------|
| BEGIN | 事务开始 | - |
| INSERT | 插入记录 | DELETE |
| DELETE | 删除记录 | INSERT |
| UPDATE | 更新记录 | UPDATE(旧值) |
| COMMIT | 事务提交 | - |
| ABORT | 事务回滚 | - |

---

**文档版本**：v1.0  
**最后更新**：2026-07-08  
**作者**：系统故障恢复实现团队
