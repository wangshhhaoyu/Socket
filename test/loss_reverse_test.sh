#!/usr/bin/env bash
# loss_reverse_test.sh -- 验证"服务端也能检测到客户端方向丢包":
#   客户端以 --drop 30 丢包, 服务端应打印"检测到用户 X 丢失 N 个报文",
#   同时客户端应收到 MSG_LOSS_REPORT 类型的可靠性提示。
set -u
cd "$(dirname "$0")/.."

PORT=${PORT:-8902}
DROP=${DROP:-30}
BIN=build
OUT=test/out
mkdir -p "$OUT"

# Windows 下可执行文件带 .exe 后缀
SERVER="$BIN/chat_server.exe"
CLIENT="$BIN/chat_client.exe"

echo "== 启动服务器 (port=$PORT, 服务端不注入丢包) =="
$SERVER -p $PORT > "$OUT/server_rev.log" 2>&1 &
SRV=$!
sleep 1

echo "== 启动客户端 Dave (本端丢包 ${DROP}%) =="
( printf 'test message one\n'
  sleep 1
  printf '测试消息二\n'
  sleep 2
  printf '/stat\n'
  sleep 1
  printf '/quit\n'
  sleep 1 ) | $CLIENT -h 127.0.0.1 -p $PORT -n Dave --drop $DROP > "$OUT/dave.log" 2>&1

sleep 1
kill $SRV 2>/dev/null
wait $SRV 2>/dev/null

echo "=================== Dave 日志 ==================="
cat "$OUT/dave.log"
echo "=================== 服务器日志 ==================="
cat "$OUT/server_rev.log"
