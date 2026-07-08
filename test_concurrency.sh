#!/bin/bash

# 并发控制测试脚本
# 需要启动rmdb服务器后运行此脚本
# 使用nc（netcat）模拟两个客户端连接

echo "========== 并发控制测试 =========="
echo ""
echo "测试场景：脏读数据异常预防"
echo ""
echo "初始化数据..."

# 创建第一个客户端连接的FIFO管道
rm -f /tmp/client1_input /tmp/client1_output
mkfifo /tmp/client1_input

# 创建第二个客户端连接的FIFO管道
rm -f /tmp/client2_input /tmp/client2_output
mkfifo /tmp/client2_input

# 启动两个客户端连接（后台运行）
nc localhost 8765 < /tmp/client1_input > /tmp/client1_output &
CLIENT1_PID=$!
exec 3>/tmp/client1_input

nc localhost 8765 < /tmp/client2_input > /tmp/client2_output &
CLIENT2_PID=$!
exec 4>/tmp/client2_input

sleep 1

echo "创建表和初始化数据..."
echo "create table concurrency_test (id int, name char(8), score float);" >&3
sleep 0.5
echo "insert into concurrency_test values (1, 'xiaohong', 90.0);" >&3
sleep 0.5
echo "insert into concurrency_test values (2, 'xiaoming', 95.0);" >&3
sleep 0.5
echo "insert into concurrency_test values (3, 'zhanghua', 88.5);" >&3
sleep 1

echo ""
echo "========== 开始并发测试 =========="
echo "操作序列: t1a t2a t1b t2b t1c t1d"
echo ""

# t1a: 事务1 begin
echo "[t1a] 事务1: begin"
echo "begin;" >&3
sleep 0.5

# t2a: 事务2 begin
echo "[t2a] 事务2: begin"
echo "begin;" >&4
sleep 0.5

# t1b: 事务1 update
echo "[t1b] 事务1: update concurrency_test set score = 100.0 where id = 2"
echo "update concurrency_test set score = 100.0 where id = 2;" >&3
sleep 0.5

# t2b: 事务2 select
echo "[t2b] 事务2: select * from concurrency_test where id = 2"
echo "select * from concurrency_test where id = 2;" >&4
sleep 0.5

# t1c: 事务1 abort
echo "[t1c] 事务1: abort"
echo "abort;" >&3
sleep 0.5

# t1d: 事务1 select
echo "[t1d] 事务1: select * from concurrency_test where id = 2"
echo "select * from concurrency_test where id = 2;" >&3
sleep 0.5

echo ""
echo "========== 测试完成 =========="
echo ""

# 清理
exec 3>&-
exec 4>&-
kill $CLIENT1_PID $CLIENT2_PID 2>/dev/null
rm -f /tmp/client1_input /tmp/client1_output
rm -f /tmp/client2_input /tmp/client2_output

echo "测试结束"
echo ""
echo "预期结果："
echo "- 事务2在t2b时应该因锁冲突被abort（因为事务1持有了X锁）"
echo "- 或者事务2等待事务1释放锁"
echo "- 最终事务1在t1d时应该看到score=95.0（原值）"
