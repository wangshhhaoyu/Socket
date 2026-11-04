# CHT/1.0 流式 Socket 多人聊天系统

基于 C++11 与**原生 Socket 函数**实现的多线程聊天系统，支持中英文混合聊天，
并内置**丢包检测与统计**机制。

**代码仓库**：<https://github.com/wangshhhaoyu/Socket.git> （分支 `main`）

```bash
git clone https://github.com/wangshhhaoyu/Socket.git
```

---

## 一、目录结构

```
socket/
├── build.bat             Windows 一键编译
├── demo.bat              线下演示一键启动（服务器 + 3 个客户端窗口）
├── README.md             本文件
├── docs/
│   ├── 协议设计.md        CHT/1.0 协议完整规范（语法/语义/时序/异常处理）
│   ├── 模块设计.md        模块划分说明与全部流程图
│   ├── 实验报告.md        实验报告正文
│   └── 演示指南.md        线下演示操作手册（含讲解词）
├── src/
│   ├── protocol.h/.cpp   协议模块：帧编解码、CRC32 校验、时间戳
│   ├── net_io.h/.cpp     网络 I/O 模块：流式切帧、阻塞收发
│   ├── socket_compat.h   Winsock2 初始化与接口适配
│   ├── thread_compat.h   线程启动封装
│   ├── server.cpp        服务器：监听/连接线程/会话/转发/可靠性/心跳
│   └── client.cpp        客户端：接收线程/发送/命令/可靠性
└── test/
    ├── run_all.sh            一键运行全部实验测试
    ├── multi_test.py         三客户端并发聊天自动校验
    ├── smoke_test.sh         基本收发功能测试
    ├── loss_test.sh          服务端→客户端 方向丢包检测
    └── loss_reverse_test.sh  客户端→服务端 方向丢包检测
```

---

## 二、编译（Windows）

环境要求：Windows 10/11 + MinGW-w64 或 TDM-GCC（提供 `g++`，并把其 `bin` 目录加入 `PATH`）。

**方式一：一键编译**

```bat
build.bat
```

自动在 `build\` 下生成 `chat_server.exe` 与 `chat_client.exe`；
`build.bat clean` 可清除产物。

**方式二：手工编译**

```bat
g++ -std=c++11 -Wall -Wextra -O2 -o build\chat_server.exe src\server.cpp src\net_io.cpp src\protocol.cpp -lws2_32
g++ -std=c++11 -Wall -Wextra -O2 -o build\chat_client.exe src\client.cpp src\net_io.cpp src\protocol.cpp -lws2_32
```

> **已实测**：Windows 11 + MinGW-w64 (TDM-GCC 5.1.0) 编译通过，
> `-Wall -Wextra` 零警告零错误；`bash test/run_all.sh` 四项测试全部通过。
>
> 程序启动时会自动调用 `SetConsoleOutputCP(CP_UTF8)`，中文在 Windows 控制台可正常显示与输入。

---

## 三、线下演示

### 3.1 一键演示（推荐）

```bat
demo.bat
```

自动打开 **4 个命令行窗口**：1 个服务器 + 3 个客户端（昵称由服务器自动分配为 `pc1` / `pc2` / `pc3`）。
在任意客户端窗口输入消息，其余窗口立刻收到，中英文均可。

演示丢包检测（服务器侧人工注入 30% 丢包）：

```bat
demo.bat 30
```

此时客户端执行 `/test 100` 即可看到非零丢包率与逐条丢包告警。

> 完整的分环节演示流程（含讲解词、每步预期输出、常见问题排查）见
> [`docs/演示指南.md`](docs/演示指南.md)。

### 3.2 手动启动服务器

```bat
build\chat_server.exe [-a <addr>] [-p <port>] [--drop <percent>]
```

| 参数 | 含义 | 默认值 |
|------|------|--------|
| `-a <addr>` | 监听地址 | `0.0.0.0` |
| `-p <port>` | 监听端口 | `8888` |
| `--drop <pct>` | 人工注入丢包的百分比 (0~100)，用于验证丢包检测机制 | `0` |

示例：

```bat
build\chat_server.exe                    REM 监听 0.0.0.0:8888
build\chat_server.exe -p 9000            REM 换端口
build\chat_server.exe --drop 30          REM 注入 30% 丢包，观察丢包检测效果
```

服务器启动后输出：

```
[2026-11-04 13:19:09.482] 服务器已启动, 监听 0.0.0.0:8888 (人工丢包率=0%)
```

### 3.3 手动启动客户端

```bat
build\chat_client.exe [-h <host>] [-p <port>] [-n <nickname>] [--drop <percent>]
```

| 参数 | 含义 | 默认值 |
|------|------|--------|
| `-h <host>` | 服务器地址 | `127.0.0.1` |
| `-p <port>` | 服务器端口 | `8888` |
| `-n <nickname>` | 昵称 | 不指定则由服务器按连接顺序分配 `pc1` / `pc2` / `pc3` … |
| `--drop <pct>` | 本端人工丢包百分比，用于验证**服务端**能否检测到客户端丢包 | `0` |

示例：

```bat
build\chat_client.exe -h 127.0.0.1 -p 8888 -n 张三
build\chat_client.exe -h 127.0.0.1 -p 8888 -n Alice --drop 30
```

### 3.4 多人聊天

打开**多个命令行窗口**，各启动一个客户端并使用不同昵称：

```bat
REM 窗口 1
build\chat_client.exe -n Alice
REM 窗口 2
build\chat_client.exe -n Bob
REM 窗口 3
build\chat_client.exe -n 张三
```

任一客户端输入消息后，**所有在线用户**（含发送者本人）立即收到：

```
[2026-11-04 13:19:02.076] Alice: Hello from Alice, this is English.
[2026-11-04 13:19:02.427] Bob: Hi, Bob here in English.
[2026-11-04 13:19:02.777] Carol: Greetings from Carol.
[2026-11-04 13:19:03.128] Alice: Alice says 中文消息 number one.
[2026-11-04 13:19:03.478] Bob: Bob says 中文消息 number two.
```

### 3.5 客户端内建命令

在客户端提示符 `>` 后输入：

| 命令 | 功能 | 示例输出 |
|------|------|----------|
| 直接输入文字 | 发送聊天消息（支持中英文） | `[时间] Bob: 你好！` |
| `/help` | 查看命令帮助 | — |
| `/nick <名字>` | 修改昵称（其他人会看到"用户 X(id=N) 将昵称改为 …"） | `昵称已改为 李四` |
| `/who` | 查看在线用户列表 | `[在线用户] Alice\|Bob\|张三` |
| `/ping` | 发送心跳并测量往返时延（**仅本人可见**） | `Pong: 往返时延 RTT = 494 us (0.494 ms)` |
| `/test [n]` | 发送 n 条测试报文并统计丢包率 | 见下方 |
| `/stat` | 显示本端收发统计与丢包情况 | 见下方 |
| `/quit` | 退出聊天 | — |

> **注**：内建命令是**本端行为**，只对使用者本人可见，不会广播给其他人。
> 漏写开头的 `/` 也能识别（直接输入 `ping` 等同 `/ping`）；
> 但要求命令形式完整合法，`ping 你好吗`、`test message one` 这类句子仍按聊天消息发送。

**`/test` 输出示例：**

```
================= 丢包测试结果 =================
  发送测试报文数   : 100
  收到 ACK 数      : 62
  丢失报文数       : 38
  应用层丢包率     : 38.00 %
==================================================
```

**`/stat` 输出示例：**

```
================= 本端统计 =================
  已发送报文数     : 104
  收到的最大序号   : 106
  发现的序号缺口数 : 25
  丢失报文总数     : 38
=============================================
```

### 3.6 退出

输入 `/quit`，客户端会发送 `LOGOUT` 报文后正常关闭连接；
其他在线用户会收到 `[系统] <昵称> 离开了聊天室` 的通知。

---

## 四、运行测试

在 **Git Bash**（或任意 Bash 环境）中执行：

```bash
bash test/run_all.sh
```

该脚本依次执行：编译 → 三人并发聊天校验 → 服务端方向丢包检测 → 客户端方向丢包检测。

单独运行：

```bash
python test/multi_test.py         # 三客户端并发聊天 + 中文完整性 + 序号连续性
bash test/smoke_test.sh           # 基本收发功能
bash test/loss_test.sh            # 服务端→客户端 方向丢包
bash test/loss_reverse_test.sh    # 客户端→服务端 方向丢包
```

> `multi_test.py` 需要 Python 解释器；其余脚本仅依赖 Bash。
> 测试日志输出在 `test/out/` 目录。

---

## 五、协议速览

一帧报文的物理结构：

```
┌──────────────────────────────────────────────┐
│ 头部行: "CHT1 <TYPE> <SEQ> <TS> <SENDER_LEN> <PAYLOAD_LEN>\n" │
├──────────────────────────────────────────────┤
│ SENDER  (SENDER_LEN 字节, UTF-8)              │
├──────────────────────────────────────────────┤
│ PAYLOAD (PAYLOAD_LEN 字节, UTF-8)             │
├──────────────────────────────────────────────┤
│ CRC32   (4 字节, 大端序)                      │
└──────────────────────────────────────────────┘
```

报文类型：

| 值 | 名称 | 方向 | 含义 |
|----|------|------|------|
| 1 | `LOGIN_REQ` | C→S | 登录请求，负载为昵称 |
| 2 | `LOGIN_ACK` | S→C | 登录确认 |
| 3 | `LOGOUT` | C→S | 主动下线 |
| 4 | `CHAT` | 双向 | 群聊消息 |
| 5 | `SYSTEM` | S→C | 系统通知 |
| 6 | `USERLIST` | S→C | 在线用户列表 |
| 7 | `PING` | 双向 | 心跳探测 |
| 8 | `PONG` | 双向 | 心跳应答 |
| 9 | `ACK` | S→C | 应用层确认 |
| 10 | `LOSS_REPORT` | S→C | 丢包报告 |
| 11 | `NICK_CHANGE` | C→S | 请求修改昵称 |
| 12 | `NICK_ACK` | S→C | 改名结果 |

完整规范见 [`docs/协议设计.md`](docs/协议设计.md)。

---

## 六、实现要点

- **只用原生 Socket 函数**：`socket/bind/listen/accept/connect/send/recv/setsockopt/shutdown/close`，
  未使用任何 Socket 封装类库或框架。
- **流式切帧**：接收缓冲区 `Conn::rbuf` 保存未成帧字节，先凑齐头部行再按长度取主体。
- **写帧原子化**：每连接一把写锁，"分配序号 + 全量发送"在同一临界区内完成。
- **用户动态增减**：连接即入表、断开即出表，每次变化都全量广播最新在线列表。
  用户表由互斥量保护，广播前先做 `shared_ptr` 快照（**持锁不做 I/O**），
  慢客户端不会拖住其他人；正常 `/quit` 与异常掉线两条路径收敛到同一段收尾逻辑。
- **中文支持**：全链路 UTF-8，长度按字节计算；Windows 控制台自动切换 UTF-8 代码页。
- **时延测量**：用 `QueryPerformanceCounter` 硬件计时器（分辨率约 1 µs）测量 RTT。
  同机回环实测 0.2~0.5 ms；不用它的话毫秒精度会恒显示 `0 ms`。
- **丢包检测**：序号单调递增，接收方比对连续性即可发现丢失；双向检测并相互告警。
- **点对点报文**：`PING`/`PONG`/`ACK`/`LOSS_REPORT`/`NICK_ACK` 只回送给发起方那一条连接，
  服务端不广播，因此 `/ping` 的探测与往返时延**只有使用者本人可见**；
  客户端的内建命令一律就地执行，不会作为聊天消息发出。
- **平台**：Windows 11 + MinGW-w64 (TDM-GCC 5.1.0)，`-Wall -Wextra` 零警告。
  `src/socket_compat.h` 集中处理 Winsock2 的初始化与接口差异，
  上层代码只用实验要求的「基本 Socket 函数」，不引入任何封装类库。
