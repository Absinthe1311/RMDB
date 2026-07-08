# GDB调试脚本 - 追踪failure来源

# 设置断点
break src/rmdb.cpp:219
break src/execution/executor_insert.h:75

# 显示断点信息
info breakpoints

# 设置自动显示
display /i $pc

# 输出提示
printf "\n===== 断点设置完成 =====\n"
printf "断点1: rmdb.cpp:219 - 异常处理输出failure\n"
printf "断点2: executor_insert.h:75 - Insert唯一索引检查失败\n"
printf "\n等待程序运行到断点...\n"
printf "可用命令:\n"
printf "  continue (c) - 继续执行\n"
printf "  backtrace (bt) - 查看调用栈\n"
printf "  info locals - 查看局部变量\n"
printf "  print <变量名> - 打印变量\n"
printf "  quit - 退出gdb\n"
printf "===========================\n\n"
