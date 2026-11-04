/*
 * client.cpp
 * ---------------------------------------------------------------------------
 * 聊天客户端 (多线程, 基于基本 Socket 函数)
 *
 * 模块划分:
 *   1. 连接模块   : create_connect_socket 建立 TCP 连接
 *   2. 接收线程   : 持续读取服务端帧, 分类型显示 (聊天/系统/用户列表/丢包报告)
 *   3. 发送模块   : 主线程读标准输入, 组装帧并发送
 *   4. 可靠性模块 : 序号连续性检查(检测服务端->本端丢包) + 测试报文统计
 *   5. 命令模块   : /nick /who /ping /test /stat /quit 等内建命令
 *
 * 命令行用法:
 *   chat_client [-h <host>] [-p <port>] [-n <nickname>]
 * ---------------------------------------------------------------------------
 */
#include "net_io.h"
#include "protocol.h"
#include "socket_compat.h"
#include "thread_compat.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <condition_variable>
#include <chrono>
#include <mutex>
#include <string>

#include <conio.h>
#include <windows.h>

using namespace chat;

/* ============================================================ 全局状态 */

/* 客户端本端序号 (发出的每一帧 +1) */
static unsigned g_seq_out = 0;

/* 发送互斥锁: 保证序号分配与写帧的原子性 */
static std::mutex g_send_mu;

/* 收到的服务端帧序号统计 (用于检测服务端->本端丢包) */
static unsigned g_last_seq_in  = 0;
static bool    g_has_seq_in    = false;
static unsigned g_gap_count     = 0;   /* 缺口个数 */
static unsigned g_gap_msgs      = 0;   /* 丢失的报文总数 */
static std::mutex g_stat_mu;

static Conn   g_conn;

/*
 * 本端昵称。默认为空 —— 表示"我没有指定名字", 由服务器按连接顺序分配
 * (pc1 / pc2 / pc3 ...)。收到登录应答后, 接收线程会把服务器分配的昵称回填进来。
 * 由于主线程与接收线程都会读写它, 用一把锁保护。
 */
static std::mutex   g_nick_mu;
static std::string  g_nick;

static std::string nick_get() {
    std::lock_guard<std::mutex> lock(g_nick_mu);
    return g_nick;
}

static void nick_set(const std::string &n) {
    std::lock_guard<std::mutex> lock(g_nick_mu);
    g_nick = n;
}

/* 退出标志: 主动退出时接收线程不再把 socket 错误视为异常 */
static bool    g_exiting = false;

/* 连接是否已被服务端断开 (由接收线程置位, 供主线程轮询) */
static volatile bool g_conn_closed = false;

/* 测试报文的 ACK 计数 (用于计算应用层丢包率) */
static unsigned g_ack_count = 0;      /* 已收到的 ACK 数 */
static unsigned g_test_sent = 0;      /* 已发出的测试报文数 */
static std::condition_variable g_ack_cv;
static std::mutex g_ack_mu;

/* 客户端侧人工丢包注入比例 (0~100): 用于验证"服务端也能检测到客户端丢包" */
static int g_drop_percent = 0;
static unsigned long g_rand_state = 987654321UL;

static unsigned long xorshift() {
    g_rand_state ^= g_rand_state << 13;
    g_rand_state ^= g_rand_state >> 7;
    g_rand_state ^= g_rand_state << 17;
    return g_rand_state % 100UL;
}

/* ============================================================ 工具 */

/*
 * 控制台输出互斥锁。
 *
 * 主线程(打印提示符、帮助、统计)和接收线程(打印时间戳消息)都要写 stdout,
 * 而 stdout 只有一份、光标只有一个。若不加锁, 两条输出会在控制台行中间交错:
 * 一条消息插进另一条的中间, 甚至落进某个中文字符的右半格 —— 控制台此时
 * 只能把那个格子渲染成一个替换字符(乱码方块)。
 *
 * 所以这里要求: 一次输出在控制台上必须是原子的, 整段加锁。
 */
static std::mutex g_out_mu;

/* 整段加锁地把一段文本写到控制台 */
static void out_write(const std::string &text) {
    std::lock_guard<std::mutex> lock(g_out_mu);
    fputs(text.c_str(), stdout);
    fflush(stdout);
}

/* 整段加锁地格式化输出 */
static void out_printf(const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    out_write(std::string(buf));
}

static void log_line(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    out_write("[" + format_time(now_ms()) + "] " + buf + "\n");
}

/* ============================================================ 发送模块 */

/* 发送一帧, 序号在锁内分配 */
static bool send_msg(int type, const std::string &payload, int64_t ts,
                     const std::string &sender) {
    std::lock_guard<std::mutex> lock(g_send_mu);

    /* 先分配序号再决定是否丢弃, 使序号留下真实的空洞 */
    unsigned my_seq = ++g_seq_out;

    /*
     * 丢包注入: 模拟本端->服务端方向的网络丢包。
     * 只对聊天报文(MSG_CHAT)生效, 控制类报文(登录/登出/心跳)始终送达,
     * 以保证实验流程可以正常进行。
     */
    if (type == MSG_CHAT && g_drop_percent > 0 &&
        xorshift() < (unsigned long)g_drop_percent)
        return true;

    Message m;
    m.type    = type;
    m.seq     = my_seq;
    m.ts_ms   = ts;
    m.sender  = sender;
    m.payload = payload;
    return conn_write_frame(g_conn, m);
}

/* ============================================================ 可靠性 */

/* 检查服务端发来的报文序号是否连续 */
static void check_seq(unsigned seq) {
    std::lock_guard<std::mutex> lock(g_stat_mu);
    if (!g_has_seq_in) {
        g_has_seq_in = true;
        g_last_seq_in = seq;
        return;
    }
    if (seq > g_last_seq_in + 1) {
        unsigned lost = seq - g_last_seq_in - 1;
        g_gap_count++;
        g_gap_msgs += lost;
        out_printf("  ** 丢包告警: 服务端报文序号从 %u 跳到 %u, 中间丢失 %u 帧 **\n",
                   g_last_seq_in, seq, lost);
    }
    g_last_seq_in = seq;
}

/* ============================================================ 接收线程 */

/* 显示一条聊天消息: [时间] 昵称: 内容 */
static void show_chat(const Message &m) {
    out_write("[" + format_time(m.ts_ms) + "] " + m.sender + ": " + m.payload + "\n");
}

/*
 * 从登录应答里取出服务器分配的用户名。
 * 应答格式固定为: 登录成功, 你的用户编号是 <id>, 用户名是 <nick>
 * 服务器分配的默认昵称(pc1/pc2/...)就是通过这条回填到本端的。
 */
static std::string parse_assigned_nick(const std::string &payload) {
    const std::string key = "用户名是 ";
    size_t p = payload.find(key);
    if (p == std::string::npos) return std::string();
    return payload.substr(p + key.size());
}

static void handle_server_msg(const Message &m) {
    check_seq(m.seq);

    switch (m.type) {
    case MSG_LOGIN_ACK: {
        log_line("%s", m.payload.c_str());
        /* 若服务器给我们分配了默认昵称, 回填到本端, 后续发言署名用它 */
        std::string assigned = parse_assigned_nick(m.payload);
        if (!assigned.empty()) nick_set(assigned);
        break;
    }

    case MSG_CHAT:
        show_chat(m);
        break;

    case MSG_SYSTEM:
        log_line("[系统] %s", m.payload.c_str());
        break;

    case MSG_USERLIST:
        log_line("[在线用户] %s", m.payload.empty() ? "(无)" : m.payload.c_str());
        break;

    case MSG_PING:
        /* 服务端心跳: 立即回 PONG, 载荷回显用于计算 RTT */
        send_msg(MSG_PONG, m.payload, now_ms(), nick_get());
        break;

    case MSG_PONG: {
        /*
         * 载荷是发 PING 时的微秒时间戳, 计算往返时延。
         * 用微秒而不是毫秒: 本机回环上 RTT 通常只有几十到几百微秒,
         * 整数毫秒会恒显示 0, 看不出网络往返的真实耗时。
         */
        int64_t t = atoll(m.payload.c_str());
        if (t > 0) {
            int64_t us = now_us() - t;
            if (us < 0) us = 0;
            log_line("Pong: 往返时延 RTT = %lld us (%.3f ms)", (long long)us, us / 1000.0);
        }
        break;
    }

    case MSG_ACK: {
        /* 测试报文的确认帧: 累计计数用于计算应用层丢包率 */
        {
            std::lock_guard<std::mutex> lock(g_ack_mu);
            g_ack_count++;
        }
        g_ack_cv.notify_all();
        break;
    }

    case MSG_LOSS_REPORT:
        log_line("[可靠性] %s", m.payload.c_str());
        break;

    default:
        log_line("收到未知类型 %d: %s", m.type, m.payload.c_str());
        break;
    }
}

static void recv_thread() {
    Message m;
    std::string err;
    for (;;) {
        int r = conn_read_frame(g_conn, m, err);
        if (r == 0) {
            if (!g_exiting)
                log_line("与服务器连接已关闭");
            break;
        }
        if (r < 0) {
            /* 主动退出时 socket 被关闭导致的错误属于正常收尾, 不必告警 */
            if (!g_exiting)
                log_line("接收出错: %s", err.c_str());
            break;
        }
        handle_server_msg(m);
    }
    /* 通知主线程结束 (不直接改 g_conn.fd, 避免与主线程的关闭操作竞争) */
    g_conn_closed = true;
}

/* ============================================================ 命令模块 */

static void print_help() {
    out_write(
        "\n================= 命令帮助 =================\n"
        "  (直接输入文字)   发送聊天消息, 支持中文与英文\n"
        "  /nick <名字>     修改昵称\n"
        "  /who            查看在线用户列表\n"
        "  /ping           发送心跳并测量往返时延\n"
        "  /test [n]       发送 n 条测试报文并统计丢失情况 (默认 20)\n"
        "  /stat           显示本端收发统计与丢包情况\n"
        "  /quit           退出聊天\n"
        "=============================================\n\n");
}

/*
 * 发送 n 条测试报文, 并等待服务端逐条 ACK。
 * ACK 数 < 发送数 的部分即为应用层观测到的丢失, 由此计算丢包率。
 */
static void run_test(int count) {
    if (count < 1) count = 1;
    if (count > 5000) count = 5000;

    {
        std::lock_guard<std::mutex> lock(g_ack_mu);
        g_ack_count = 0;
        g_test_sent = 0;
    }

    log_line("开始发送 %d 条测试报文...", count);
    int sent = 0;
    for (int i = 1; i <= count; ++i) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s%d", TEST_TAG, i);
        if (!send_msg(MSG_CHAT, buf, now_ms(), nick_get())) {
            log_line("发送失败, 中断测试");
            break;
        }
        sent++;
    }
    {
        std::lock_guard<std::mutex> lock(g_ack_mu);
        g_test_sent = (unsigned)sent;
    }

    /* 等待 ACK 收齐, 最多等待 10 秒 */
    {
        std::unique_lock<std::mutex> lock(g_ack_mu);
        for (int i = 0; i < 100 && g_ack_count < g_test_sent; ++i)
            g_ack_cv.wait_for(lock, std::chrono::milliseconds(100));

        unsigned acked = g_ack_count;
        unsigned total = g_test_sent;
        unsigned lost = (total > acked) ? (total - acked) : 0;
        double rate  = total ? (lost * 100.0 / total) : 0.0;
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "\n================= 丢包测试结果 =================\n"
                 "  发送测试报文数   : %u\n"
                 "  收到 ACK 数      : %u\n"
                 "  丢失报文数       : %u\n"
                 "  应用层丢包率     : %.2f %%\n"
                 "==================================================\n\n",
                 total, acked, lost, rate);
        out_write(std::string(buf));
    }
}

/* 打印统计信息 */
static void print_stat() {
    std::lock_guard<std::mutex> lock(g_stat_mu);
    char buf[512];
    snprintf(buf, sizeof(buf),
             "\n================= 本端统计 =================\n"
             "  已发送报文数     : %u\n"
             "  收到的最大序号   : %u\n"
             "  发现的序号缺口数 : %u\n"
             "  丢失报文总数     : %u\n"
             "=============================================\n\n",
             g_seq_out, g_last_seq_in, g_gap_count, g_gap_msgs);
    out_write(std::string(buf));
}

/* 处理一行输入; 返回 false 表示需要退出 */
static bool handle_input(const std::string &line) {
    if (line.empty()) return true;

    /* 内建命令以 '/' 开头 */
    if (line[0] == '/') {
        if (line == "/quit" || line == "/exit") {
            send_msg(MSG_LOGOUT, "", now_ms(), nick_get());
            return false;
        } else if (line == "/help") {
            print_help();
        } else if (line == "/who") {
            out_printf("(等待服务器返回在线用户列表...)\n");
            send_msg(MSG_PING, "who", now_ms(), nick_get());
        } else if (line == "/ping") {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)now_us());
            send_msg(MSG_PING, buf, now_ms(), nick_get());
        } else if (line.compare(0, 5, "/test") == 0 &&
                   (line.size() == 5 || line[5] == ' ' || line[5] == '\t' ||
                    line[5] == '[')) {
            /*
             * 报文的条数。容错地接受多种写法:
             *   /test        /test 100    /test 100条
             *   /test [100]  /test[100]
             * 用户常常直接照抄帮助里 "/test [n]" 的写法, 若用 atoi 直接解析,
             * 遇到 '[' 会返回 0, 导致只发出 1 条报文、样本量不足。
             */
            int n = 20;
            size_t p = 5;
            while (p < line.size() &&
                   (line[p] == ' ' || line[p] == '\t' || line[p] == '[')) ++p;
            long v = 0;
            bool digits = false;
            while (p < line.size() && line[p] >= '0' && line[p] <= '9') {
                v = v * 10 + (line[p] - '0');
                ++p;
                digits = true;
                if (v > 1000000) break;   /* 防止输入超长数字导致溢出 */
            }
            if (digits) n = (int)v;
            run_test(n);
        } else if (line.compare(0, 6, "/nick ") == 0) {
            std::string n = line.substr(6);
            /* 容错: 允许直接照抄 "/nick <名字>" 的写法, 去掉尖括号 */
            if (n.size() >= 2 && n[0] == '<' && n[n.size() - 1] == '>')
                n = n.substr(1, n.size() - 2);
            if (n.empty()) {
                out_printf("用法: /nick <名字>\n");
            } else {
                if (n.size() > MAX_SENDER_LEN) n.resize(MAX_SENDER_LEN);
                nick_set(n);
                out_printf("昵称已改为 %s\n", n.c_str());
                /* 重新登录以更新昵称 */
                send_msg(MSG_LOGIN_REQ, n, now_ms(), n);
            }
        } else if (line == "/stat") {
            print_stat();
        } else {
            out_printf("未知命令, 输入 /help 查看帮助\n");
        }
        return true;
    }

    /*
     * 少了 '/' 的命令: 例如把 "/test 30" 打成 "test 30"。
     * 这类输入仍按普通聊天消息处理(绝不吞掉用户输入), 但额外给一句提示,
     * 免得误以为程序卡住了。
     */
    {
        size_t sp = line.find_first_of(" \t");
        std::string head = (sp == std::string::npos) ? line : line.substr(0, sp);
        if (head == "help" || head == "who" || head == "ping" ||
            head == "test" || head == "stat" || head == "quit" ||
            head == "exit" || head == "nick") {
            out_printf("提示: 内建命令需以 '/' 开头, 例如 /%s ... (本行仍按聊天消息发送)\n",
                       head.c_str());
        }
    }

    /* 普通聊天消息 */
    if (!send_msg(MSG_CHAT, line, now_ms(), nick_get()))
        log_line("发送失败, 连接可能已断开");
    return true;
}

/* ============================================================ 主流程 */

static void print_usage(const std::string &prog) {
    out_printf("用法: %s [-h <host>] [-p <port>] [-n <nickname>] [--drop <percent>]\n", prog.c_str());
    out_printf("  -h <host>      服务器地址, 默认 127.0.0.1\n");
    out_printf("  -p <port>      服务器端口, 默认 8888\n");
    out_printf("  -n <nickname>  昵称, 不指定则由服务器分配 (pc1/pc2/...)\n");
    out_printf("  --drop <pct>   本端人工丢包百分比 (0~100), 用于验证服务端丢包检测\n");
    out_printf("\n进入后输入 /help 查看命令帮助\n");
}

int main(int argc, char **argv) {
    /* 先设置控制台代码页, 保证后续中文提示与消息正确显示 */
    enable_utf8_console();

    /* 命令行参数统一按 UTF-8 解析 (argv 在中文 Windows 上是 GBK 字节) */
    std::vector<std::string> args = utf8_command_line(argc, argv);
    const std::string prog = program_name(argc > 0 ? argv[0] : NULL);

    std::string host = "127.0.0.1";
    int         port = 8888;
    std::string nick;   /* 留空 -> 由服务器按连接顺序分配 pc1/pc2/... */

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "-h" && i + 1 < args.size()) {
            host = args[++i];
        } else if (a == "-p" && i + 1 < args.size()) {
            port = atoi(args[++i].c_str());
        } else if (a == "-n" && i + 1 < args.size()) {
            nick = args[++i];
        } else if (a == "--drop" && i + 1 < args.size()) {
            g_drop_percent = atoi(args[++i].c_str());
            if (g_drop_percent < 0)   g_drop_percent = 0;
            if (g_drop_percent > 100) g_drop_percent = 100;
        } else if (a == "--help") {
            print_usage(prog);
            return 0;
        } else {
            print_usage(prog);
            return 1;
        }
    }
    nick_set(nick);

    std::string err;
    if (!platform_init(err)) {
        fprintf(stderr, "初始化失败: %s\n", err.c_str());
        return 1;
    }
    enable_utf8_console();

    int fd = create_connect_socket(host.c_str(), port, err);
    if (fd < 0) {
        fprintf(stderr, "连接服务器失败: %s\n", err.c_str());
        platform_cleanup();
        return 1;
    }
    g_conn.fd = fd;
    log_line("已连接到 %s:%d", host.c_str(), port);

    /* 启动接收线程 */
    thread_start(recv_thread);

    /* 登录握手 */
    send_msg(MSG_LOGIN_REQ, nick_get(), now_ms(), nick_get());

    print_help();

    /* 主线程: 读取标准输入 */
    std::string line;
    while (!g_conn_closed) {
        char buf[4096];
        /* Windows 下 fgets 在 UTF-8 代码页下按字节读取, 中文可正常输入 */
        out_write("> ");
        if (!fgets(buf, sizeof(buf), stdin)) break;

        /* 去掉行尾 '\n' 与 '\r' */
        size_t n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
            buf[--n] = '\0';
        }
        line = buf;
        if (!handle_input(line)) break;
    }

    print_stat();

    /* 收尾: 先置退出标志再关闭套接字, 避免接收线程把正常关闭当作异常上报 */
    g_exiting = true;
    sock_shutdown(g_conn.fd);
    sleep_ms(100);              /* 给接收线程一点时间自行退出 */
    conn_close(g_conn);
    platform_cleanup();
    return 0;
}
