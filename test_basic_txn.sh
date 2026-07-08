#!/bin/bash

# 单客户端测试脚本 - 基本事务测试
# 使用方法：先启动rmdb服务器，然后运行此脚本

echo "========== 基本事务控制测试 =========="
echo ""

# 使用nc连接到rmdb服务器
# 每个echo命令发送一条SQL语句
{
    sleep 1
    
    echo "create table test_table (id int, name char(8), score float);"
    sleep 0.3
    
    echo "insert into test_table values (1, 'alice', 90.0);"
    sleep 0.3
    
    echo "insert into test_table values (2, 'bob', 95.0);"
    sleep 0.3
    
    echo "-- 测试1：基本事务提交"
    echo "begin;"
    sleep 0.3
    
    echo "update test_table set score = 100.0 where id = 2;"
    sleep 0.3
    
    echo "commit;"
    sleep 0.3
    
    echo "select * from test_table;"
    sleep 0.5
    
    echo "-- 测试2：基本事务回滚"
    echo "begin;"
    sleep 0.3
    
    echo "update test_table set score = 80.0 where id = 2;"
    sleep 0.3
    
    echo "abort;"
    sleep 0.3
    
    echo "select * from test_table;"
    sleep 0.5
    
    echo "-- 测试3：插入回滚"
    echo "begin;"
    sleep 0.3
    
    echo "insert into test_table values (3, 'charlie', 88.0);"
    sleep 0.3
    
    echo "abort;"
    sleep 0.3
    
    echo "select * from test_table;"
    sleep 0.5
    
    echo "exit"
    
} | nc localhost 8765

echo ""
echo "========== 测试完成 =========="
