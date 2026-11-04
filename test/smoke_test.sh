#!/usr/bin/env bash
# smoke_test.sh -- 自动化测试: 启动服务器, 用两个客户端验证中英文聊天与丢包检测
set -u
cd "$(dirname "$0")/.."

PORT=${PORT:-8899}
DROP=${DROP:-0}
BIN=build
OUT=test/out
mkdir -p "$OUT"

# Windows 下可执行文件带 .exe 后缀
SERVER="$BIN/chat_server.exe"
CLIENT="$BIN/chat_client.exe"

echo "== 启动服务器 (port=$PORT, drop=${DROP}%) =="
if [ "$DROP" = "0" ]; then
    $SERVER -p $PORT > "$OUT/server.log" 2>&1 &
else
    $SERVER -p $PORT --drop $DROP > "$OUT/server.log" 2>&1 &
fi
SRV=$!
sleep 1

echo "== 启动客户端 Alice =="
( printf 'Hello everyone, this is Alice!\n'
  printf '大家好，我是Alice，中文测试！\n'
  sleep 1
  printf '/stat\n'
  sleep 1
  printf '/quit\n'
  sleep 1 ) | $CLIENT -h 127.0.0.1 -p $PORT -n Alice > "$OUT/alice.log" 2>&1 &
CA=$!

sleep 1
echo "== 启动客户端 Bob =="
( sleep 1
  printf 'Hi Alice! Nice to meet you.\n'
  printf 'Bob here. 你好呀！\n'
  sleep 1
  printf '/test 50\n'
  sleep 2
  printf '/stat\n'
  sleep 1
  printf '/quit\n'
  sleep 1 ) | $CLIENT -h 127.0.0.1 -p $PORT -n Bob > "$OUT/bob.log" 2>&1 &
CB=$!

wait $CA $CB
sleep 1
kill $SRV 2>/dev/null
wait $SRV 2>/dev/null

echo "=================== Alice 日志 ==================="
cat "$OUT/alice.log"
echo "=================== Bob 日志 ==================="
cat "$OUT/bob.log"
echo "=================== 服务器日志 ==================="
cat "$OUT/server.log"
