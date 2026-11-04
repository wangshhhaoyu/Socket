/*
 * protocol.cpp
 * ---------------------------------------------------------------------------
 * CHT/1.0 协议的编解码实现。
 *
 * 帧格式:
 *   头部行: "CHT1 <TYPE> <SEQ> <TS> <SENDER_LEN> <PAYLOAD_LEN>\n"
 *   主体  : SENDER(SENDER_LEN) + PAYLOAD(PAYLOAD_LEN) + CRC32(4 字节, 大端)
 *
 * 所有文本字段均为 UTF-8 编码, 中文直接以多字节形式放入负载, 无需转义。
 * ---------------------------------------------------------------------------
 */
#include "protocol.h"
#include "socket_compat.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <time.h>      /* time_t / struct tm / strftime */
#include <sstream>

namespace chat {

const char *type_name(int type) {
    switch (type) {
        case MSG_LOGIN_REQ:   return "LOGIN_REQ";
        case MSG_LOGIN_ACK:   return "LOGIN_ACK";
        case MSG_LOGOUT:      return "LOGOUT";
        case MSG_CHAT:        return "CHAT";
        case MSG_SYSTEM:      return "SYSTEM";
        case MSG_USERLIST:    return "USERLIST";
        case MSG_PING:        return "PING";
        case MSG_PONG:        return "PONG";
        case MSG_ACK:         return "ACK";
        case MSG_LOSS_REPORT: return "LOSS_REPORT";
        default:              return "UNKNOWN";
    }
}

/* ---------------------------------------------------------------- CRC32 */
/* 经典的查表法 CRC32 (多项式 0xEDB88320), 一次构建表, 多次复用 */
static uint32_t g_crc_table[256];
static bool    g_crc_ready = false;

static void crc_init() {
    if (g_crc_ready) return;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_table[i] = c;
    }
    g_crc_ready = true;
}

uint32_t crc32(const void *data, size_t len) {
    crc_init();
    const unsigned char *p = (const unsigned char *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        c = g_crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---------------------------------------------------------------- 时间 */

/*
 * 高精度时钟基准: 一次性把"墙上时钟"和"QPC 单调计数器"对齐。
 * 这样 now_us() 既能给出正确的 Unix 时间, 又能分辨出微秒级的间隔。
 */
struct ClockBase {
    int64_t  unix_us;   /* 构造瞬间的 Unix 微秒时间 */
    int64_t  qpc;       /* 同一瞬间的 QPC 计数值 */
    int64_t  freq;      /* QPC 每秒的计数值 */

    ClockBase() {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        unsigned long long t =
            ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        /* 1601-01-01 到 1970-01-01 相差 11644473600 秒 */
        unix_us = (int64_t)(t / 10ULL) - 11644473600LL * 1000000LL;

        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        freq = f.QuadPart;
        qpc  = c.QuadPart;
    }
};

int64_t now_ms() {
    /* Windows 的 FILETIME 以 100ns 为单位, 起点 1601-01-01 */
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    /* 1601-01-01 到 1970-01-01 相差 11644473600 秒 */
    return (int64_t)(t / 10000ULL) - 11644473600LL * 1000LL;
}

int64_t now_us() {
    /*
     * 为什么不直接用 FILETIME 换算微秒?
     * FILETIME 的"100ns"只是数值单位, 它的**实际刷新粒度**由系统时钟节拍决定
     * (典型 15.6ms), 连续两次读取常常拿到同一个值。实测在本机回环上,
     * 用 FILETIME 测出的 RTT 恒为 0 —— 真实往返其实只有几十微秒。
     *
     * 所以这里用 QueryPerformanceCounter (QPC, 硬件高精度计时器):
     *   启动时用 FILETIME 取一次墙上时钟作为基准,
     *   之后用 QPC 的单调增量推进, 既有正确的 Unix 时间, 又有亚微秒分辨率。
     */
    static const ClockBase base;      /* 首次调用时构造, C++11 保证线程安全的一次性初始化 */
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    int64_t ticks = c.QuadPart - base.qpc;
    return base.unix_us + (int64_t)((double)ticks * 1000000.0 / (double)base.freq);
}

std::string format_time(int64_t ms) {
    time_t sec = (time_t)(ms / 1000);
    int msec = (int)(ms % 1000);
    struct tm tmv;
    localtime_s(&tmv, &sec);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    char out[80];
    snprintf(out, sizeof(out), "%s.%03d", buf, msec);
    return std::string(out);
}

/* ---------------------------------------------------------------- 编解码 */

bool encode_message(const Message &m, std::string &out, std::string &err) {
    if (m.sender.size() > MAX_SENDER_LEN) {
        err = "发送者名过长";
        return false;
    }
    if (m.payload.size() > MAX_PAYLOAD_LEN) {
        err = "负载过长";
        return false;
    }

    char head[MAX_HEADER_LEN];
    int hn = snprintf(head, sizeof(head),
                      "%s %d %u %lld %zu %zu\n",
                      PROTOCOL_MAGIC,
                      m.type,
                      (unsigned)m.seq,
                      (long long)m.ts_ms,
                      m.sender.size(),
                      m.payload.size());
    if (hn <= 0 || hn >= (int)sizeof(head)) {
        err = "头部构造失败";
        return false;
    }

    /* CRC 覆盖 sender + payload 的连续字节, 与解码端算法完全一致 */
    std::string tmp;
    tmp.reserve(m.sender.size() + m.payload.size());
    tmp.append(m.sender);
    tmp.append(m.payload);
    uint32_t crc = crc32(tmp.data(), tmp.size());

    out.append(head, (size_t)hn);
    out.append(m.sender);
    out.append(m.payload);

    unsigned char cbuf[4];
    cbuf[0] = (unsigned char)((crc >> 24) & 0xFF);
    cbuf[1] = (unsigned char)((crc >> 16) & 0xFF);
    cbuf[2] = (unsigned char)((crc >> 8) & 0xFF);
    cbuf[3] = (unsigned char)(crc & 0xFF);
    out.append((const char *)cbuf, 4);
    return true;
}

/*
 * 解析头部行, 并通过 out_slen / out_plen 返回声明的昵称长度与负载长度。
 * 头部行形如:
 *   CHT1 4 12 1761234567890 5 11
 */
static bool parse_header(const std::string &header, Message &m,
                         size_t &out_slen, size_t &out_plen, std::string &err) {
    /* 手工切分空格分隔的字段, 比 sscanf 更可控 */
    std::istringstream is(header);
    std::string magic, t_s, seq_s, ts_s, slen_s, plen_s;
    if (!(is >> magic >> t_s >> seq_s >> ts_s >> slen_s >> plen_s)) {
        err = "头部字段数量不足";
        return false;
    }
    if (magic != PROTOCOL_MAGIC) {
        err = "协议魔数不匹配";
        return false;
    }
    m.type = atoi(t_s.c_str());
    m.seq  = (uint32_t)strtoul(seq_s.c_str(), NULL, 10);
    m.ts_ms = (int64_t)strtoll(ts_s.c_str(), NULL, 10);
    size_t slen = (size_t)strtoul(slen_s.c_str(), NULL, 10);
    size_t plen = (size_t)strtoul(plen_s.c_str(), NULL, 10);

    if (slen > MAX_SENDER_LEN || plen > MAX_PAYLOAD_LEN) {
        err = "长度字段越界";
        return false;
    }
    out_slen = slen;
    out_plen = plen;
    return true;
}

bool parse_header_line(const std::string &header,
                       Message &m,
                       size_t &slen,
                       size_t &plen,
                       std::string &err) {
    return parse_header(header, m, slen, plen, err);
}

bool decode_message(const std::string &header,
                    const std::string &body,
                    Message &m,
                    std::string &err) {
    size_t slen = 0, plen = 0;
    if (!parse_header(header, m, slen, plen, err))
        return false;

    if (body.size() != slen + plen + 4) {
        err = "主体长度与头部声明不一致";
        return false;
    }

    m.sender.assign(body, 0, slen);
    m.payload.assign(body, slen, plen);

    const unsigned char *cbuf = (const unsigned char *)body.data() + slen + plen;
    uint32_t crc_recv = ((uint32_t)cbuf[0] << 24) | ((uint32_t)cbuf[1] << 16) |
                        ((uint32_t)cbuf[2] << 8)  | ((uint32_t)cbuf[3]);

    std::string tmp;
    tmp.reserve(slen + plen);
    tmp.append(m.sender);
    tmp.append(m.payload);
    uint32_t crc_calc = crc32(tmp.data(), tmp.size());

    if (crc_calc != crc_recv) {
        err = "CRC32 校验失败 (数据在传输中被破坏)";
        return false;
    }
    return true;
}

} // namespace chat
