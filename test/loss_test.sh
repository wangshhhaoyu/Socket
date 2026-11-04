#!/usr/bin/env bash
# loss_test.sh -- 验证丢包检测机制:
#   服务端以 --drop 30 人工丢包, 客户端 /test 应统计出非零丢包率,
#   且客户端与服务端两侧都应打印"丢包告警"。
set -u
cd "$(dirname "$0")/.."

PORT=${PORT:-8901}
DROP=${DROP:-30}
BIN=build
OUT=test/out
mkdir -p "$OUT"

# Windows 下可执行文件带 .exe 后缀
SERVER="$BIN/chat_server.exe"
CLIENT="$BIN/chat_client.exe"

echo "== 启动服务器 (port=$PORT, drop=${DROP}%) =="
$SERVER -p $PORT --drop $DROP > "$OUT/server_drop.log" 2>&1 &
SRV=$!
sleep 1

echo "== 启动客户端 Carol =="
( printf 'hello world\n'
  sleep 1
  printf '/test 100\n'
  sleep 3
  printf '/stat\n'
  sleep 1
  printf '/quit\n'
  sleep 1 ) | $CLIENT -h 127.0.0.1 -p $PORT -n Carol > "$OUT/carol.log" 2>&1

sleep 1
kill $SRV 2>/dev/null
wait $SRV 2>/dev/null

echo "=================== Carol 日志 ==================="
cat "$OUT/carol.log"
echo "=================== 服务器日志 (前40行) ==================="
head -40 "$OUT/server_drop.log"
