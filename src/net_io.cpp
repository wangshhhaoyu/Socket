/*
 * net_io.cpp
 * ---------------------------------------------------------------------------
 * 网络 I/O 模块实现 (裸 Socket 函数 + 协议帧收发)。
 * ---------------------------------------------------------------------------
 */
#include "net_io.h"

#include <cstring>
#include <cstdio>

namespace chat {

long raw_recv(int fd, char *buf, size_t len) {
    int n = ::recv((SOCKET)fd, buf, (int)len, 0);
    if (n == 0) return 0;
    if (n < 0) return -1;
    return (long)n;
}

/*
 * 把 recv 的失败原因翻译成人话。
 * 客户端进程被直接杀掉时, Windows 会给本端发 RST,
 * recv 返回 SOCKET_ERROR 且错误码是 WSAECONNRESET —— 这是"异常断开",
 * 不该显示成笼统的"recv 失败"(看起来像程序 bug)。
 */
static const char *recv_error_text() {
    int e = last_socket_error();
    if (e == WSAECONNRESET) return "连接被对端重置(异常断开)";
    if (e == WSAECONNABORTED) return "连接被中止";
    return "recv 失败";
}

/* 从 rbuf 中取出第一行 (已去掉 '\n' 与 '\r'), 找不全返回 false */
static bool take_line(std::string &rbuf, std::string &line) {
    size_t pos = rbuf.find('\n');
    if (pos == std::string::npos) return false;
    line = rbuf.substr(0, pos);
    rbuf.erase(0, pos + 1);
    if (!line.empty() && line[line.size() - 1] == '\r')
        line.erase(line.size() - 1);
    return true;
}

bool conn_write_frame(Conn &c, const Message &m) {
    std::string frame, err;
    if (!encode_message(m, frame, err))
        return false;
    std::lock_guard<std::mutex> lock(c.send_mu);
    return send_all(c.fd, frame.data(), frame.size());
}

int conn_read_frame(Conn &c, Message &m, std::string &err) {
    char tmp[4096];

    /*
     * 第一步: 读出完整头部行。
     * 注意: take_line 在整段缓冲区中查找换行符, 因此当它失败时,
     *       缓冲区中必然不存在任何 '\n', 整段都是"尚未收全的头部行",
     *       此时才可以安全地用缓冲区长度做超长判断。
     */
    std::string header;
    for (;;) {
        if (take_line(c.rbuf, header)) break;    /* 取到一行, 头部完整 */

        if (c.rbuf.size() > MAX_HEADER_LEN) {
            err = "头部行过长, 协议错误";
            return -1;
        }
        long n = raw_recv(c.fd, tmp, sizeof(tmp));
        if (n == 0) { err = "对端关闭"; return 0; }
        if (n < 0)  { err = recv_error_text(); return -1; }
        c.rbuf.append(tmp, (size_t)n);
    }
    if (header.empty()) { err = "空头部"; return -1; }

    /* 第二步: 解析头部, 得到主体长度 */
    Message head_msg;
    size_t slen = 0, plen = 0;
    if (!parse_header_line(header, head_msg, slen, plen, err))
        return -1;

    size_t body_len = slen + plen + 4;            /* 昵称 + 负载 + CRC32 */

    /* 第三步: 读满 body_len 字节的主体 */
    while (c.rbuf.size() < body_len) {
        long n = raw_recv(c.fd, tmp, sizeof(tmp));
        if (n == 0) { err = "读取主体时对端关闭"; return -1; }
        if (n < 0)  { err = recv_error_text(); return -1; }
        c.rbuf.append(tmp, (size_t)n);
    }

    std::string body = c.rbuf.substr(0, body_len);
    c.rbuf.erase(0, body_len);

    /* 第四步: 解码 + CRC 校验 */
    if (!decode_message(header, body, m, err))
        return -1;
    return 1;
}

/* ---------------------------------------------------------------- 监听 */

int create_listen_socket(const char *host, int port, std::string &err) {
    int fd = (int)::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { err = "socket 创建失败"; return -1; }

    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (chat_inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        err = "非法监听地址";
        close_socket(fd);
        return -1;
    }

    if (::bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "bind 失败, 错误码=%d", last_socket_error());
        err = buf;
        close_socket(fd);
        return -1;
    }
    if (::listen(fd, 16) < 0) {
        err = "listen 失败";
        close_socket(fd);
        return -1;
    }
    return fd;
}

/* ---------------------------------------------------------------- 连接 */

int create_connect_socket(const char *host, int port, std::string &err) {
    int fd = (int)::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { err = "socket 创建失败"; return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (chat_inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        err = "非法服务器地址";
        close_socket(fd);
        return -1;
    }
    if (::connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "connect 失败, 错误码=%d", last_socket_error());
        err = buf;
        close_socket(fd);
        return -1;
    }

    /* 关闭 Nagle 算法, 减少小消息延迟 (聊天场景友好) */
    int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
    return fd;
}

void conn_close(Conn &c) {
    if (c.fd >= 0) {
        close_socket(c.fd);
        c.fd = -1;
    }
}

} // namespace chat
