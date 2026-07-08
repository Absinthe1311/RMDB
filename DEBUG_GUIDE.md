# GDB调试步骤 - 追踪failure来源

## 问题分析

**failure的两个来源：**
1. **Server端异常处理** - `src/rmdb.cpp:219`：任何SQL执行抛出RMDBError异常都会输出failure
2. **Insert唯一索引检查** - `src/execution/executor_insert.h:75`：插入重复key时检查失败

**当前现象：**
- 测试点四执行3000条插入，创建索引，然后执行6000次查询（无索引3000次+有索引3000次）
- output.txt中出现16个failure，都是w_id=169倍数
- failure出现在查询结果之间，说明是**查询过程中抛出异常**

## 调试步骤

### 步骤1：启动GDB调试Server

```bash
# 终端1：启动gdb调试server
cd /home/absinthe/rmdb
gdb -x gdb_commands.gdb --args ./build/bin/rmdb test_db

# 在gdb中运行
(gdb) run
```

### 步骤2：运行Client测试

```bash
# 终端2：启动client执行测试（等待server启动后）
cd /home/absinthe/rmdb
./rmdb_client/build/rmdb_client < test/test_point4_single_index.sql > test_output.txt 2>&1
```

### 步骤3：触发断点时的调试命令

当gdb停在断点时，依次执行：

```gdb
# 1. 查看调用栈
(gdb) backtrace

# 2. 查看当前位置
(gdb) frame 0

# 3. 查看局部变量
(gdb) info locals

# 4. 查看参数
(gdb) info args

# 5. 如果是rmdb.cpp:219断点，查看异常信息
(gdb) print e.what()

# 6. 如果是executor_insert.h:75断点，查看key和result
(gdb) print key
(gdb) print result
(gdb) print *key@4    # 假设key是int类型，打印4字节

# 7. 查看当前SQL语句（如果可用）
(gdb) print str

# 8. 继续执行到下一个断点
(gdb) continue
```

### 步骤4：收集调试信息

每次触发断点时记录：
1. **调用栈** - 确认是哪个函数调用的
2. **异常信息** - e.what()返回的错误消息
3. **关键变量** - key值、result大小、当前操作的表名等

### 步骤5：退出调试

```gdb
# 调试完成后退出
(gdb) quit
```

## 预期结果

根据断点触发位置判断：

**如果断点1触发（rmdb.cpp:219）：**
- 说明是**查询或索引操作**时抛出异常
- 查看backtrace确定是哪个函数抛出异常
- 很可能是`get_value()`或其他索引查找函数的bug

**如果断点2触发（executor_insert.h:75）：**
- 说明是在执行INSERT操作
- 查看key值是否应该已存在
- 可能是`get_value()`错误返回true

## 快速测试命令（可选）

如果想快速测试w_id=169的场景：

```bash
# 创建简化测试
cat > test/test_169_debug.sql << 'EOF'
create table test(w_id int, name char(8));
insert into test values(168, 'a');
insert into test values(169, 'b');
insert into test values(170, 'c');
create index test(w_id);
select * from test where w_id = 168;
select * from test where w_id = 169;
select * from test where w_id = 170;
drop index test(w_id);
drop table test;
EOF

# 运行测试
./rmdb_client/build/rmdb_client < test/test_169_debug.sql
```

## 关键函数列表

如果需要额外断点：

```gdb
# B+树查找函数
break IxIndexHandle::get_value
break IxIndexHandle::internal_lookup
break IxNodeHandle::lower_bound

# 索引扫描
break IndexScanExecutor::beginTuple
break IndexScanExecutor::next
```

## 注意事项

1. **Debug版本**：已重新编译为Debug模式（-DCMAKE_BUILD_TYPE=Debug）
2. **多线程**：server是多线程，断点可能在任意线程触发
3. **输出位置**：failure会写入`test_db/output.txt`和stderr
4. **清理数据**：每次测试前可能需要删除`test_db`目录重新创建
