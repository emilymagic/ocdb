#!/bin/bash

# 查找包含 'python3' 和 'pool' 的进程并获取PID
PIDS=$(ps aux | grep '[p]ython3.*pool' | awk '{print $2}')

if [ -z "$PIDS" ]; then
    echo "未找到运行的 pool 程序。"
else
    echo "找到进程 ID：$PIDS"
    kill -9 $PIDS
    echo "已终止进程。"
fi
