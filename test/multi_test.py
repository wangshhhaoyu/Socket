#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
multi_test.py -- 多人并发聊天集成测试

三个客户端同时连接到服务器并保持在线, 交错发送中英文消息, 校验:
  1. 每个人都能收到其他两人的全部消息 (广播正确)
  2. 中文消息在客户端输出中完整无误 (UTF-8 无乱码/截断)
  3. 未注入丢包时收到的序号连续, 无缺口

用法: python test/multi_test.py
"""
import os
import re
import subprocess
import sys
import threading
import time

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(BASE, "build")
OUT = os.path.join(BASE, "test", "out")
PORT = os.environ.get("PORT", "8903")
CN = "中文消息"
EXT = ".exe" if os.name == "nt" else ""


def start_client(name):
    """启动客户端并返回 (进程, 写入stdin的函数)"""
    exe = os.path.join(BIN, "chat_client" + EXT)
    p = subprocess.Popen(
        [exe, "-h", "127.0.0.1", "-p", PORT, "-n", name],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace", bufsize=1,
    )

    def send(line):
        p.stdin.write(line + "\n")
        p.stdin.flush()

    return p, send


def collect(p, name, results):
    out = p.stdout.read()
    results[name] = out
    with open(os.path.join(OUT, "multi_%s.log" % name), "w", encoding="utf-8") as f:
        f.write(out)


def main():
    os.makedirs(OUT, exist_ok=True)
    exe = os.path.join(BIN, "chat_server" + EXT)
    srv = subprocess.Popen([exe, "-p", PORT], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                           errors="replace")
    time.sleep(1.0)

    ok = True
    try:
        # 每个客户端 5 条消息, 中英文混合
        spec = {
            "Alice": ["Hello from Alice, this is English.",
                      "Alice says " + CN + " number one.",
                      "A" + CN, "test line a3", "final line a4"],
            "Bob":   ["Hi, Bob here in English.",
                      "Bob says " + CN + " number two.",
                      "B" + CN, "test line b3", "final line b4"],
            "Carol": ["Greetings from Carol.",
                      "Carol says " + CN + " number three.",
                      "C" + CN, "test line c3", "final line c4"],
        }
        names = list(spec.keys())

        procs = {}
        senders = {}
        for n in names:
            procs[n], senders[n] = start_client(n)
        time.sleep(1.5)          # 等待三者全部登录上线

        # 交错发送: 轮流各发一条, 保证三人都在线时互相可见
        for i in range(len(spec["Alice"])):
            for n in names:
                senders[n](spec[n][i])
                time.sleep(0.35)
        time.sleep(1.5)

        for n in names:
            senders[n]("/stat")
            senders[n]("/who")
            time.sleep(0.3)
        time.sleep(1.0)
        for n in names:
            senders[n]("/quit")

        results = {}
        threads = [threading.Thread(target=collect, args=(procs[n], n, results))
                   for n in names]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        for n in names:
            try:
                procs[n].wait(timeout=10)
            except subprocess.TimeoutExpired:
                procs[n].kill()

        # ---- 校验 ----
        for name, out in results.items():
            for other in names:
                if other == name:
                    continue
                for msg in spec[other]:
                    if msg not in out:
                        print("[FAIL] %s did not receive from %s: %r" % (name, other, msg))
                        ok = False
            m_gap = re.search(r"发现的序号缺口数\s*:\s*(\d+)", out)
            if m_gap and int(m_gap.group(1)) != 0:
                print("[FAIL] %s seq gap = %s" % (name, m_gap.group(1)))
                ok = False
            # /who 应返回完整的在线用户列表
            # 注意: 日志中"加入/离开聊天室"时也会打印 [在线用户],
            #       因此只要求"三人都在线"的那条列表存在即可
            lists = re.findall(r"\[在线用户\]\s*(\S+)", out)
            expected = set(names)
            if not any(set(x.split("|")) == expected for x in lists):
                print("[FAIL] %s /who 未返回完整在线用户列表: %s (期望 %s)"
                      % (name, lists, sorted(expected)))
                ok = False

        # 中文完整性: 客户端日志里应能原样找到所有中文消息
        for name, out in results.items():
            for other in names:
                for msg in spec[other]:
                    if CN in msg and msg not in out:
                        print("[FAIL] %s chinese message corrupted: %r" % (name, msg))
                        ok = False

        if ok:
            print("[PASS] 3 clients concurrent chat: all messages delivered, "
                  "chinese intact, no seq gaps")
            return 0
        print("[FAIL] concurrent chat test failed")
        return 1
    finally:
        srv.terminate()
        try:
            srv.wait(timeout=5)
        except subprocess.TimeoutExpired:
            srv.kill()


if __name__ == "__main__":
    sys.exit(main())
