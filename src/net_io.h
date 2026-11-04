/*
 * net_io.h
 * ---------------------------------------------------------------------------
 * 网络 I/O 模块: 基于基本 Socket 函数的流式帧收发。
 *
 * 本模块不引入任何面向对象 Socket 封装(如 Boost.Asio、C++ Socket 类库),
 * 仅在裸 socket 之上提供"按协议帧读写"的最小辅助函数。
 *
 * 关键点: TCP 是字节流, 一次 send 的数据可能被拆成多次 recv 到达,
 *        因此必须先读满"头部行 + 定长主体"再算作一帧。
 * ---------------------------------------------------------------------------
 */
#ifndef CHAT_NET_IO_H
#define CHAT_NET_IO_H

#include "protocol.h"
#include "socket_compat.h"

#include <mutex>
#include <string>

namespace chat {

/*
 * 一条连接的读写上下文。
 *   fd        : 套接字描述符
 *   send_mu   : 写互斥锁, 防止多个线程的帧交叉写坏 (TCP 无消息边界)
 *   rbuf      : 读缓冲区, 保存已到达但尚未解析的字节
 */
struct Conn {
    int         fd;
    std::mutex  send_mu;
    std::string rbuf;

    Conn() : fd(-1) {}
};

/* 从 fd 阻塞读取, 返回读到的字节数; 出错返回 -1, 对端关闭返回 0 */
long raw_recv(int fd, char *buf, size_t len);

/* 一次性把整帧写入连接 (自动加写锁) */
bool conn_write_frame(Conn &c, const Message &m);

/*
 * 从连接中读取一帧完整的报文。
 * 返回值: 1 = 成功, 0 = 对端正常关闭(EOF), -1 = 出错(协议错误或 socket 错误)
 */
int conn_read_frame(Conn &c, Message &m, std::string &err);

/* 创建监听套接字 (server 用) */
int create_listen_socket(const char *host, int port, std::string &err);

/* 建立连接到服务器的套接字 (client 用) */
int create_connect_socket(const char *host, int port, std::string &err);

/* 关闭并置 fd = -1 */
void conn_close(Conn &c);

} // namespace chat

#endif /* CHAT_NET_IO_H */
