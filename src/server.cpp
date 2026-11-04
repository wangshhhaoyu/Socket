/*
 * server.cpp
 * ---------------------------------------------------------------------------
 * 聊天服务器 (多线程, 基于基本 Socket 函数)
 *
 * 模块划分:
 *   1. 监听模块     : create_listen_socket / accept 循环
 *   2. 连接线程模块 : 每接入一个客户端启动一个工作线程, 负责收帧与分发
 *   3. 会话管理模块 : 在线用户表 (昵称 -> 连接), 全局互斥保护
 *   4. 转发模块     : 群聊广播 / 在线列表 / 系统通知
 *   5. 可靠性模块   : 序号连续性检查(丢包检测) + 可控丢包注入(用于实验观察)
 *   6. 心跳模块     : 定时 PING, 探测并清理失联连接
 *
 * 命令行用法:
 *   chat_server [-a <addr>] [-p <port>] [--drop <percent>]
 *     -a <addr>    监听地址, 默认 0.0.0.0
 *     -p <port>    监听端口, 默认 8888
 *     --drop <pct> 人工注入丢包的百分比 (0~100), 用于验证丢包检测机制
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
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace chat;

/* ============================================================ 数据结构 */

/* 一个在线客户端的全部状态 */
struct Client {
    Conn         conn;         /* 套接字与读写缓冲 */
    int          id;           /* 服务端分配的用户编号 */
    std::string  nick;         /* 用户昵称 (UTF-8) */
    bool         logged_in;    /* 是否已完成登录握手 */
    bool         closed;       /* 是否已关闭, 用于避免重复关闭 */
    bool         quit_req;     /* 是否由客户端主动 /quit 退出(用于区分正常/异常断开) */

    unsigned     seq_out;      /* 本连接 服务端->客户端 的报文序号 */
    unsigned     last_seq_in;  /* 最近一次收到的客户端报文序号 */
    bool         has_seq_in;   /* 是否已经收到过带序号的报文 */

    int          missed;       /* 累计检测到的丢包(序号缺口)个数 */
    time_t       last_active;  /* 最近一次活动时间 */

    std::mutex   send_mu;      /* 保证"分配序号 + 写帧"原子化, 使序号单调无空洞 */
};

/* 服务器全局状态 */
static std::vector<std::shared_ptr<Client> > g_clients;
static std::mutex                            g_clients_mu;
static int                                   g_next_id = 1;

/* 可靠性统计 */
static unsigned long g_frames_out   = 0;   /* 服务端发出的帧数 */
static unsigned long g_frames_in    = 0;   /* 服务端收到的帧数 */
static unsigned long g_frames_drop  = 0;   /* 人为注入丢弃的帧数 */
static unsigned long g_gaps_found   = 0;   /* 检测到的序号缺口数 */
static std::mutex    g_stat_mu;

/* 丢包注入比例 (0 ~ 100) */
static int g_drop_percent = 0;
/* 伪随机数状态, 避免 <random> 在老编译器上的兼容问题 */
static unsigned long g_rand_state = 20240101UL;
static std::mutex    g_rand_mu;

static unsigned long xorshift() {
    std::lock_guard<std::mutex> lock(g_rand_mu);
    g_rand_state ^= g_rand_state << 13;
    g_rand_state ^= g_rand_state >> 7;
    g_rand_state ^= g_rand_state << 17;
    return g_rand_state % 100UL;
}

/* ============================================================ 工具函数 */

/*
 * 控制台输出互斥锁。
 * 服务器有多个连接线程 + 一个心跳线程, 它们都会写 stdout。
 * 若不加锁, 两条日志会在控制台行中间交错, 甚至落进某个中文字符的右半格,
 * 控制台只能把那个格子渲染成替换字符(乱码方块)。
 * 因此要求: 一次输出在控制台上必须是原子的, 整段加锁。
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
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    out_write("[" + format_time(now_ms()) + "] " + buf + "\n");
}

/* ============================================================ 发送模块 */

/*
 * 向单个客户端发送一帧。
 * 序号在 send_mu 保护下分配, 保证客户端看到的序号连续递增。
 * 若开启丢包注入, 则有概率直接丢弃该帧(模拟网络丢包)。
 */
static bool send_frame(const std::shared_ptr<Client> &c, int type,
                       const std::string &payload, int64_t ts,
                       const std::string &sender) {
    std::lock_guard<std::mutex> lock(c->send_mu);
    if (c->closed) return false;

    /*
     * 先分配序号再决定是否丢弃:
     *   真实网络丢包时, 丢失的报文同样占用了一个序号,
     *   接收方才能通过序号缺口发现丢失。若丢弃时不消耗序号,
     *   接收方将完全看不到任何异常, 丢包检测机制就形同虚设。
     */
    unsigned my_seq = ++c->seq_out;

    /*
     * 丢包注入: 直接不发送, 客户端将依据序号缺口检测到丢失。
     * 只对"数据类"报文生效(聊天报文 MSG_CHAT 与应用层确认 MSG_ACK),
     * 而控制类报文(登录应答/用户列表/心跳)始终送达,
     * 以保证实验流程可以正常进行。
     */
    bool droppable = (type == MSG_CHAT || type == MSG_ACK);
    if (droppable && g_drop_percent > 0 &&
        xorshift() < (unsigned long)g_drop_percent) {
        std::lock_guard<std::mutex> sl(g_stat_mu);
        g_frames_drop++;
        return true;
    }

    Message m;
    m.type    = type;
    m.seq     = my_seq;
    m.ts_ms   = ts;
    m.sender  = sender.empty() ? std::string("SERVER") : sender;
    m.payload = payload;

    bool ok = conn_write_frame(c->conn, m);
    if (ok) {
        std::lock_guard<std::mutex> sl(g_stat_mu);
        g_frames_out++;
    }
    return ok;
}

/* ============================================================ 转发模块 */

/* 广播一帧给所有已登录客户端 (可选排除某个客户端) */
static void broadcast(int type, const std::string &payload, int64_t ts,
                      const std::string &sender,
                      const std::shared_ptr<Client> *exclude = NULL) {
    std::vector<std::shared_ptr<Client> > snapshot;
    {
        std::lock_guard<std::mutex> lock(g_clients_mu);
        snapshot = g_clients;
    }
    for (size_t i = 0; i < snapshot.size(); ++i) {
        const std::shared_ptr<Client> &c = snapshot[i];
        if (exclude && *exclude == c) continue;
        if (!c->logged_in) continue;
        if (!send_frame(c, type, payload, ts, sender))
            log_line("向用户 %s(%d) 发送失败", c->nick.c_str(), c->id);
    }
}

/* 生成在线用户列表, 以 '|' 分隔 */
static std::string build_user_list() {
    std::string list;
    std::vector<std::shared_ptr<Client> > snapshot;
    {
        std::lock_guard<std::mutex> lock(g_clients_mu);
        snapshot = g_clients;
    }
    for (size_t i = 0; i < snapshot.size(); ++i) {
        if (!snapshot[i]->logged_in) continue;
        if (!list.empty()) list += "|";
        list += snapshot[i]->nick;
    }
    return list;
}

/* 广播最新在线用户列表 */
static void broadcast_user_list() {
    broadcast(MSG_USERLIST, build_user_list(), now_ms(), "SERVER");
}

/* ============================================================ 丢包检测 */

/*
 * 检查客户端报文序号是否连续。
 * 客户端每发一帧序号 +1, 若收到 seq > last_seq_in + 1, 说明中间有帧丢失。
 */
static void check_seq(const std::shared_ptr<Client> &c, unsigned seq) {
    if (!c->has_seq_in) {
        c->has_seq_in = true;
        c->last_seq_in = seq;
        return;
    }
    if (seq > c->last_seq_in + 1) {
        unsigned lost = seq - c->last_seq_in - 1;
        c->missed += (int)lost;
        {
            std::lock_guard<std::mutex> sl(g_stat_mu);
            g_gaps_found += lost;
        }
        log_line("检测到用户 %s(%d) 丢失 %u 个报文 (期望序号 %u, 实际 %u)",
                 c->nick.c_str(), c->id, lost, c->last_seq_in + 1, seq);
        /* 主动把丢包情况告知该客户端 */
        char buf[128];
        snprintf(buf, sizeof(buf), "服务端检测到你有 %u 个报文丢失", lost);
        send_frame(c, MSG_LOSS_REPORT, buf, now_ms(), "SERVER");
    }
    c->last_seq_in = seq;
}

/* ============================================================ 连接线程 */

/* 处理一帧来自客户端的报文 */
static void handle_message(const std::shared_ptr<Client> &c, Message &m) {
    c->last_active = time(NULL);

    /* 序号连续性检查 (登录帧也参与计数) */
    check_seq(c, m.seq);

    switch (m.type) {
    case MSG_LOGIN_REQ: {
        /* 载荷即昵称; 若为空(客户端未指定 -n)则按连接顺序分配默认名 pc1/pc2/pc3 */
        std::string nick = m.payload;
        if (nick.empty()) {
            char def[32];
            snprintf(def, sizeof(def), "pc%d", c->id);
            nick = def;
        }
        /* 昵称长度限制, 防止超长 */
        if (nick.size() > MAX_SENDER_LEN) nick.resize(MAX_SENDER_LEN);

        c->nick = nick;
        c->logged_in = true;

        log_line("用户 %s(id=%d) 登录成功", c->nick.c_str(), c->id);

        /*
         * 登录应答: 同时告知用户编号与用户名, 客户端据此回填自己的昵称
         * (格式固定, 客户端用 "用户名是 " 前缀解析)。
         */
        char ack[256];
        snprintf(ack, sizeof(ack), "登录成功, 你的用户编号是 %d, 用户名是 %s",
                 c->id, c->nick.c_str());
        send_frame(c, MSG_LOGIN_ACK, ack, now_ms(), "SERVER");

        /* 通知所有人 */
        broadcast(MSG_SYSTEM, c->nick + " 加入了聊天室", now_ms(), "SERVER");
        broadcast_user_list();
        break;
    }

    case MSG_CHAT: {
        /* 校验: 未登录不允许发言 */
        if (!c->logged_in) {
            send_frame(c, MSG_SYSTEM, "请先登录后再发言", now_ms(), "SERVER");
            return;
        }
        /* 测试报文: 带 TEST: 前缀, 只回 ACK 不广播 */
        if (m.payload.size() >= 5 && m.payload.compare(0, 5, TEST_TAG) == 0) {
            char ack[64];
            snprintf(ack, sizeof(ack), "%u", m.seq);
            send_frame(c, MSG_ACK, ack, now_ms(), "SERVER");
            return;
        }
        /* 普通聊天: 广播给所有人(含发送者, 便于单机多窗口观察) */
        broadcast(MSG_CHAT, m.payload, m.ts_ms, c->nick);
        break;
    }

    case MSG_PING:
        /*
         * 特殊约定: 载荷为 "who" 时表示客户端请求在线用户列表,
         * 此时回 USERLIST 而非 PONG。其余情况按心跳处理, 回显载荷。
         */
        if (m.payload == "who") {
            send_frame(c, MSG_USERLIST, build_user_list(), now_ms(), "SERVER");
        } else {
            send_frame(c, MSG_PONG, m.payload, now_ms(), "SERVER");
        }
        break;

    case MSG_PONG:
        /* 客户端对心跳的应答: 仅表示连通性正常, 无需额外处理 */
        break;

    case MSG_LOGOUT:
        log_line("用户 %s(id=%d) 主动退出", c->nick.c_str(), c->id);
        c->quit_req = true;   /* 标记为正常退出, 收尾时不再重复记录 */
        c->closed = true;
        sock_shutdown(c->conn.fd);
        break;

    default:
        log_line("收到未知报文类型 %d, 已忽略", m.type);
        break;
    }
}

/* 每个客户端对应一个线程, 循环读取并处理报文 */
static void client_thread(std::shared_ptr<Client> c) {
    log_line("连接线程启动: fd=%d", c->conn.fd);

    Message m;
    std::string err;
    for (;;) {
        int r = conn_read_frame(c->conn, m, err);
        if (r == 0) {
            log_line("用户 %s(id=%d) 断开连接", c->nick.c_str(), c->id);
            break;
        }
        if (r < 0) {
            log_line("读取报文失败: %s", err.c_str());
            break;
        }
        {
            std::lock_guard<std::mutex> sl(g_stat_mu);
            g_frames_in++;
        }
        handle_message(c, m);
        if (c->closed) break;
    }

    /* 清理: 从用户表移除并广播下线通知 */
    {
        std::lock_guard<std::mutex> lock(g_clients_mu);
        for (size_t i = 0; i < g_clients.size(); ++i) {
            if (g_clients[i] == c) {
                g_clients.erase(g_clients.begin() + i);
                break;
            }
        }
    }
    if (c->logged_in) {
        /*
         * 用户表同步收缩。服务器窗口也要留痕 —— 否则演示时只能看到
         * "谁登录了", 看不到"谁走了", 异常掉线更是毫无记录。
         * 主动 /quit 的情况上面已打印过"主动退出", 这里不再重复。
         */
        if (!c->quit_req)
            log_line("用户 %s(id=%d) 断开连接, 已从用户表移除", c->nick.c_str(), c->id);
        broadcast(MSG_SYSTEM, c->nick + " 离开了聊天室", now_ms(), "SERVER");
    }
    broadcast_user_list();

    c->closed = true;
    conn_close(c->conn);
}

/* ============================================================ 心跳线程 */

/* 定时向所有客户端发 PING; 若某客户端长时间无任何活动, 则踢掉 */
static void heartbeat_loop() {
    while (true) {
        sleep_ms(5000);
        std::vector<std::shared_ptr<Client> > snapshot;
        {
            std::lock_guard<std::mutex> lock(g_clients_mu);
            snapshot = g_clients;
        }
        time_t nowt = time(NULL);
        for (size_t i = 0; i < snapshot.size(); ++i) {
            std::shared_ptr<Client> c = snapshot[i];
            if (c->closed) continue;
            if (nowt - c->last_active > 120) {
                log_line("用户 %s(id=%d) 心跳超时, 断开", c->nick.c_str(), c->id);
                c->closed = true;
                sock_shutdown(c->conn.fd);
                continue;
            }
            if (!c->logged_in) continue;
            char ts[32];
            snprintf(ts, sizeof(ts), "%lld", (long long)now_us());
            send_frame(c, MSG_PING, ts, now_ms(), "SERVER");
        }
    }
}

/* ============================================================ 主流程 */

static void print_usage(const std::string &prog) {
    out_printf("用法: %s [-a <addr>] [-p <port>] [--drop <percent>]\n", prog.c_str());
    out_printf("  -a <addr>    监听地址, 默认 0.0.0.0\n");
    out_printf("  -p <port>    监听端口, 默认 8888\n");
    out_printf("  --drop <pct> 人工丢包百分比 (0~100), 用于实验观察丢包检测效果\n");
}

int main(int argc, char **argv) {
    /* 先设置控制台代码页, 保证后续中文提示正确显示 */
    enable_utf8_console();

    /* 命令行参数统一按 UTF-8 解析 (argv 在中文 Windows 上是 GBK 字节) */
    std::vector<std::string> args = utf8_command_line(argc, argv);
    const std::string prog = program_name(argc > 0 ? argv[0] : NULL);

    std::string host = "0.0.0.0";
    int         port = 8888;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "-a" && i + 1 < args.size()) {
            host = args[++i];
        } else if (a == "-p" && i + 1 < args.size()) {
            port = atoi(args[++i].c_str());
        } else if (a == "--drop" && i + 1 < args.size()) {
            g_drop_percent = atoi(args[++i].c_str());
            if (g_drop_percent < 0)   g_drop_percent = 0;
            if (g_drop_percent > 100) g_drop_percent = 100;
        } else if (a == "-h" || a == "--help") {
            print_usage(prog);
            return 0;
        } else {
            print_usage(prog);
            return 1;
        }
    }

    std::string err;
    if (!platform_init(err)) {
        fprintf(stderr, "初始化失败: %s\n", err.c_str());
        return 1;
    }
    enable_utf8_console();

    int lfd = create_listen_socket(host.c_str(), port, err);
    if (lfd < 0) {
        fprintf(stderr, "监听失败: %s\n", err.c_str());
        platform_cleanup();
        return 1;
    }
    log_line("服务器已启动, 监听 %s:%d (人工丢包率=%d%%)", host.c_str(), port, g_drop_percent);

    /* 启动心跳线程 */
    thread_start(heartbeat_loop);

    /* 主线程: 接受连接并为每个连接启动工作线程 */
    while (true) {
        struct sockaddr_in peer;
        socklen_type plen = sizeof(peer);
        int cfd = (int)::accept(lfd, (struct sockaddr *)&peer, &plen);
        if (cfd < 0) {
            log_line("accept 失败, 错误码=%d", last_socket_error());
            continue;
        }

        std::shared_ptr<Client> c(new Client());
        c->conn.fd  = cfd;
        c->id      = g_next_id++;
        c->seq_out = 0;
        c->last_active = time(NULL);

        int on = 1;
        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));

        {
            std::lock_guard<std::mutex> lock(g_clients_mu);
            g_clients.push_back(c);
        }
        log_line("新连接: fd=%d, 分配 id=%d", cfd, c->id);

        /* 每个连接启动一个独立线程处理, shared_ptr 保证连接对象生命周期安全 */
        thread_start(client_thread, c);
    }

    close_socket(lfd);
    platform_cleanup();
    return 0;
}
