/*
 * socket_compat.h
 * ---------------------------------------------------------------------------
 * 目的: 集中 Winsock2 的初始化与少量接口差异, 使上层代码完全使用
 *       "基本 Socket 函数"
 *       (socket / bind / listen / accept / send / recv / close ...)。
 *
 * 注意: 本文件不封装任何 Socket 类, 也不提供面向对象的封装接口,
 *       仅做平台差异适配, 符合实验"不允许使用封装后的 Socket 类"的要求。
 * ---------------------------------------------------------------------------
 */
#ifndef CHAT_SOCKET_COMPAT_H
#define CHAT_SOCKET_COMPAT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace chat {

/* accept() 的长度参数类型 */
typedef int socklen_type;

/* Winsock 初始化 (Windows 上必须显式调用 WSAStartup) */
inline bool platform_init(std::string &err) {
    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "WSAStartup 失败, 错误码=%d", rc);
        err = buf;
        return false;
    }
    return true;
}

inline void platform_cleanup() {
    WSACleanup();
}

/* 统一的关闭接口 */
inline void close_socket(int fd) {
    if (fd < 0) return;
    closesocket((SOCKET)fd);
}

/* 统一的可读错误码 */
inline int last_socket_error() {
    return WSAGetLastError();
}

inline bool would_block(int err) {
    return err == WSAEWOULDBLOCK;
}

inline void set_nonblocking(int fd, bool enable) {
    u_long flag = enable ? 1UL : 0UL;
    ioctlsocket((SOCKET)fd, FIONBIO, &flag);
}

/*
 * 阻塞发送全部数据 (处理短写)。返回 true 表示全部发送成功。
 * 这是保证"应用层数据不丢失"的第一道防线。
 */
inline bool send_all(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = ::send((SOCKET)fd, data + sent, (int)(len - sent), 0);
        if (n <= 0) {
            if (n < 0 && would_block(last_socket_error())) continue;
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

/*
 * 地址解析: IPv4 点分十进制 -> 二进制。
 * 用 inet_addr 而非 inet_pton, 因为部分 MinGW 版本缺少后者。
 */
inline int chat_inet_pton(int af, const char *src, void *dst) {
    (void)af;
    unsigned long addr = inet_addr(src);   /* 仅支持点分十进制 IPv4 */
    if (addr == INADDR_NONE || addr == 0xFFFFFFFFul) return 0;
    memcpy(dst, &addr, sizeof(addr));
    return 1;
}

/*
 * 取程序名并统一转成 UTF-8。
 *
 * Windows 下 argv 是 ANSI(GBK) 字节, 而控制台被设置为 UTF-8 代码页,
 * 直接打印 argv[0] 会出现乱码(尤其是路径中含中文时)。
 * 这里用宽字符 API 取到原始命令行, 再转成 UTF-8 输出。
 */
inline std::string program_name(const char *argv0) {
    int wlen = (int)wcslen(GetCommandLineW());
    if (wlen > 0) {
        std::wstring wbuf(GetCommandLineW(), (size_t)wlen);
        /* 截取第一个空格之前的部分, 即程序路径 */
        size_t sp = wbuf.find(L' ');
        if (sp != std::wstring::npos) wbuf.resize(sp);
        int n = WideCharToMultiByte(CP_UTF8, 0, wbuf.c_str(), (int)wbuf.size(),
                                    NULL, 0, NULL, NULL);
        if (n > 0) {
            std::string out((size_t)n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wbuf.c_str(), (int)wbuf.size(),
                                &out[0], n, NULL, NULL);
            return out;
        }
    }
    return std::string(argv0 ? argv0 : "chat");
}

/*
 * 解析命令行参数, 全部统一转成 UTF-8。
 *
 * 为什么不用 argc/argv:
 *   Windows 的 main() 收到的 argv 是按系统 ANSI 代码页(中文系统为 GBK, ACP=936)
 *   转换出来的字节, 而本程序全链路使用 UTF-8。
 *   因此 `-n 张三` 在 argv 里是 GBK 字节(D5 C5 C8 FD), 一旦按 UTF-8 解释就乱码。
 *   GetCommandLineW() 拿到的是未被 ANSI 转换的 Unicode 命令行,
 *   再用 CommandLineToArgvW 切分、逐个转 UTF-8, 才能得到正确的中文昵称。
 *
 * 返回的 vector 第 0 项是程序路径(UTF-8), 其后与 argv 一一对应。
 */
inline std::vector<std::string> utf8_command_line(int argc, char **argv) {
    std::vector<std::string> out;
    int n = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    if (wargv != NULL) {
        out.reserve((size_t)n);
        for (int i = 0; i < n; ++i) {
            int k = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
            if (k <= 0) { out.push_back(std::string()); continue; }
            std::string s((size_t)k, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], k, NULL, NULL);
            s.resize((size_t)k - 1);   /* 去掉结尾的 '\0' */
            out.push_back(s);
        }
        LocalFree(wargv);
        return out;
    }
    /* 兜底: 极端情况下退回 ANSI 的 argv */
    for (int i = 0; i < argc; ++i) out.push_back(std::string(argv[i] ? argv[i] : ""));
    return out;
}

/* 控制台 UTF-8 设置: 保证中文在 Windows 控制台正确显示/输入 */
inline void enable_utf8_console() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

/* 关闭连接的双向读写 (用于踢掉失联客户端) */
inline void sock_shutdown(int fd) {
    if (fd < 0) return;
    ::shutdown((SOCKET)fd, SD_BOTH);
}

/* 毫秒级休眠 */
inline void sleep_ms(unsigned ms) {
    ::Sleep(ms);
}

} // namespace chat

#endif /* CHAT_SOCKET_COMPAT_H */
