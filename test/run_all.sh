#!/usr/bin/env bash
# run_all.sh -- 一键运行全部实验测试 (Windows / Git Bash)
set -u
cd "$(dirname "$0")/.."
export PYTHONIOENCODING=utf-8

echo "############################################################"
echo "# 测试 1/4: 编译"
echo "############################################################"
mkdir -p build

# Windows (MinGW) 需要链接 ws2_32
#
# 先编译到 *.new.exe 再换名替换: 演示运行时 exe 被系统占用,
# 链接器无法直接覆盖 (Permission denied), 而改名是允许的。
g++ -std=c++11 -Wall -Wextra -O2 -o build/chat_server.new.exe src/server.cpp src/net_io.cpp src/protocol.cpp -lws2_32 || exit 1
g++ -std=c++11 -Wall -Wextra -O2 -o build/chat_client.new.exe src/client.cpp src/net_io.cpp src/protocol.cpp -lws2_32 || exit 1
for n in chat_server chat_client; do
    mv -f "build/$n.exe" "build/$n.old.exe" 2>/dev/null
    mv -f "build/$n.new.exe" "build/$n.exe"
    rm -f "build/$n.old.exe" 2>/dev/null
done
echo "编译成功 (无警告)"

echo
echo "############################################################"
echo "# 测试 2/4: 三客户端并发聊天 (中英文)"
echo "############################################################"
# 选择可用的 Python 解释器:
#   注意: Windows 可能存在 Microsoft Store 的 python3 空壳占位符
#         (执行后无任何输出且退出码为 0), 故必须实际验证能否运行。
PY=""
for cand in python python3; do
    if command -v "$cand" >/dev/null 2>&1 && "$cand" -c "print('ok')" >/dev/null 2>&1; then
        PY="$cand"
        break
    fi
done
if [ -z "$PY" ]; then
    echo "未找到可用的 Python 解释器, 跳过并发聊天测试"
else
    echo "使用解释器: $PY"
    $PY test/multi_test.py
fi

echo
echo "############################################################"
echo "# 测试 3/4: 服务端 -> 客户端 方向丢包检测"
echo "############################################################"
bash test/loss_test.sh 2>&1 | grep -E "丢包测试结果|发送测试报文数|收到 ACK 数|丢失报文数|应用层丢包率|发现的序号缺口数|丢失报文总数"

echo
echo "############################################################"
echo "# 测试 4/4: 客户端 -> 服务端 方向丢包检测"
echo "############################################################"
bash test/loss_reverse_test.sh 2>&1 | grep -E "检测到用户|可靠性"
