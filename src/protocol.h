/*
 * protocol.h
 * ---------------------------------------------------------------------------
 * 聊天协议 CHT/1.0 的定义与编解码接口。
 *
 * 一帧报文的物理结构 (面向字节流的 TCP):
 *
 *      +--------+--------+--------+--------+--------+--------+--------+
 *      |  头部行 (ASCII, 以 '\n' 结束, 变长)                          |
 *      +--------+--------+--------+--------+--------+--------+--------+
 *      | 发送者名 SENDER (SENDER_LEN 字节, UTF-8)                      |
 *      +--------+--------+--------+--------+--------+--------+--------+
 *      | 负载 PAYLOAD (PAYLOAD_LEN 字节, UTF-8)                       |
 *      +--------+--------+--------+--------+--------+--------+--------+
 *      | CRC32 (4 字节, 网络字节序, 覆盖 SENDER + PAYLOAD)             |
 *      +--------+--------+--------+--------+--------+--------+--------+
 *
 * 头部行格式:
 *      "CHT1 <TYPE> <SEQ> <TS> <SENDER_LEN> <PAYLOAD_LEN>\n"
 *
 * 详见 docs/协议设计.md
 * ---------------------------------------------------------------------------
 */
#ifndef CHAT_PROTOCOL_H
#define CHAT_PROTOCOL_H

#include <string>
#include <cstdint>

namespace chat {

/* 协议魔数与版本 */
#define PROTOCOL_MAGIC    "CHT1"
#define PROTOCOL_VERSION  "1.0"

/* 长度上限 (防止恶意/错乱长度导致越界) */
#define MAX_SENDER_LEN  64
#define MAX_PAYLOAD_LEN 8192
#define MAX_HEADER_LEN  128

/* 报文类型 */
enum MessageType {
    MSG_LOGIN_REQ   = 1,   /* 客户端 -> 服务端 : 登录请求, payload = 昵称 */
    MSG_LOGIN_ACK   = 2,   /* 服务端 -> 客户端 : 登录应答, payload = 欢迎语/身份说明 */
    MSG_LOGOUT      = 3,   /* 双向             : 主动下线 */
    MSG_CHAT        = 4,   /* 双向             : 聊天消息, payload = 正文 (UTF-8) */
    MSG_SYSTEM      = 5,   /* 服务端 -> 客户端 : 系统通知 */
    MSG_USERLIST    = 6,   /* 服务端 -> 客户端 : 在线用户列表 */
    MSG_PING        = 7,   /* 双向             : 心跳探测 */
    MSG_PONG        = 8,   /* 双向             : 心跳应答 */
    MSG_ACK         = 9,   /* 服务端 -> 客户端 : 应用层确认 (含被确认的序号) */
    MSG_LOSS_REPORT = 10,  /* 服务端 -> 客户端 : 丢包/序号缺失报告 */
    MSG_NICK_CHANGE = 11,  /* 客户端 -> 服务端 : 请求修改昵称, payload = 新昵称 */
    MSG_NICK_ACK    = 12   /* 服务端 -> 客户端 : 昵称修改结果, payload = 生效的新昵称 */
};

/* 负载中用于标记"丢包测试报文"的前缀 */
#define TEST_TAG "TEST:"

struct Message {
    int         type;      /* MessageType */
    uint32_t    seq;       /* 发送方本连接内的报文序号, 从 1 开始, 逐条递增 */
    int64_t     ts_ms;     /* 发送时刻 (Unix 毫秒时间戳, UTC) */
    std::string sender;    /* 发送者昵称, UTF-8 */
    std::string payload;   /* 负载, UTF-8 */

    Message() : type(0), seq(0), ts_ms(0) {}
};

/* 类型 -> 可读名称, 用于日志与报告 */
const char *type_name(int type);

/*
 * 编码: 把 Message 序列化成一整帧字节串, 追加到 out。
 * 失败返回 false 并给出 err (例如昵称或负载超长)。
 */
bool encode_message(const Message &m, std::string &out, std::string &err);

/*
 * 解码: 输入为"已去掉行尾 '\n' 的头部行" 与 "头部行之后的一帧主体字节"。
 * 主体长度 = SENDER_LEN + PAYLOAD_LEN + 4。
 * 解析成功后校验 CRC32, 不一致则返回 false。
 */
bool decode_message(const std::string &header,
                    const std::string &body,
                    Message &m,
                    std::string &err);

/*
 * 仅解析头部行, 用于流式读取时先获知主体长度。
 * slen / plen 输出头部中声明的昵称长度与负载长度。
 */
bool parse_header_line(const std::string &header,
                       Message &m,
                       size_t &slen,
                       size_t &plen,
                       std::string &err);

/* 标准 CRC32 (IEEE 802.3), 用于检测传输损坏 */
uint32_t crc32(const void *data, size_t len);

/* 当前 Unix 毫秒时间戳 */
int64_t now_ms();

/*
 * 当前 Unix 微秒时间戳。
 * 用于往返时延(RTT)测量: 毫秒精度在本机回环上 RTT 通常 < 1ms,
 * 整数毫秒会恒显示为 0, 演示效果差; 微秒精度才能看清真实时延。
 */
int64_t now_us();

/* 时间戳 -> "YYYY-MM-DD HH:MM:SS.mmm" (本地时区) */
std::string format_time(int64_t ms);

} // namespace chat

#endif /* CHAT_PROTOCOL_H */
