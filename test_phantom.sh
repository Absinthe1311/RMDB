#!/bin/bash

# 幻读预防测试
# 测试场景：事务1范围查询，事务2尝试插入

echo "========== 幻读预防测试 =========="
echo ""
echo "测试场景："
echo "事务1: select * from table where id > 1 and id < 10"
echo "事务2: insert into table values (5, ...)"
echo "预期：事务2应该因锁冲突被abort"
echo ""

# 创建两个客户端
rm -f /tmp/phantom_client1_input /tmp/phantom_client1_output
rm -f /tmp/phantom_client2_input /tmp/phantom_client2_output
mkfifo /tmp/phantom_client1_input
mkfifo /tmp/phantom_client2_input

nc localhost 8765 < /tmp/phantom_client1_input > /tmp/phantom_client1_output &
CLIENT1_PID=$!
exec 3>/tmp/phantom_client1_input

nc localhost 8765 < /tmp/phantom_client2_input > /tmp/phantom_client2_output &
CLIENT2_PID=$!
exec 4>/tmp/phantom_client2_input

sleep 1

echo "初始化数据..."
echo "create table phantom_test (id int, name char(8), score float);" >&3
sleep 0.5
echo "insert into phantom_test values (1, 'alice', 90.0);" >&3
sleep 0.5
echo "insert into phantom_test values (2, 'bob', 95.0);" >&3
sleep 0.5
echo "insert into phantom_test values (3, 'charlie', 88.0);" >&3
sleep 1

echo ""
echo "========== 开始幻读测试 =========="
echo ""

# 事务1：开始并执行范围查询（应该加IS锁）
echo "[T1] begin;"
echo "begin;" >&3
sleep 0.5

echo "[T1] select * from phantom_test where id >= 1 and id <= 10;"
echo "select * from phantom_test where id >= 1 and id <= 10;" >&3
sleep 0.5

# 事务2：尝试插入（需要加IX锁，应该与IS锁冲突）
echo "[T2] begin;"
echo "begin;" >&4
sleep 0.5

echo "[T2] insert into phantom_test values (5, 'new', 100.0);"
echo "insert into phantom_test values (5, 'new', 100.0);" >&4
sleep 0.5

# 事务1：再次查询，验证没有幻读
echo "[T1] select * from phantom_test where id >= 1 and id <= 10;"
echo "select * from phantom_test where id >= 1 and id <= 10;" >&3
sleep 0.5

echo "[T1] commit;"
echo "commit;" >&3
sleep 0.5

echo ""
echo "========== 测试完成 =========="

# 清理
exec 3>&-
exec 4>&-
kill $CLIENT1_PID $CLIENT2_PID 2>/dev/null
rm -f /tmp/phantom_client1_input /tmp/phantom_client1_output
rm -f /tmp/phantom_client2_input /tmp/phantom_client2_output

echo ""
echo "预期结果："
echo "- 事务2在插入时应该因IS锁与IX锁冲突被abort"
echo "- 事务1两次查询结果应该相同（没有幻读）"
