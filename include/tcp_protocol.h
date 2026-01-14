#ifndef TCP_PROTOCOL_H
#define TCP_PROTOCOL_H

// 提示: 你可以使用位掩码来处理标志位
#define FLAG_SYN 0x01
#define FLAG_ACK 0x02
#define FLAG_FIN 0x04
#define FLAG_RST 0x08
#define FLAG_PSH 0x10
#include <cstdint>

// 任务 1: 定义你的协议头
// 提示: TCP 头部通常包含 序列号、确认号、标志位、窗口大小等
// 这是一个参考结构，你可以根据需要修改
struct TCPHeader {
    uint32_t seq_num;  // 序列号
    uint32_t ack_num;  // 确认号 (它是期待收到的下一个字节的序号)

    uint8_t flags;  // 标志位 (SYN, ACK, FIN, RST)

    uint8_t unused;     // 填充对齐 (Padding)
    uint16_t checksum;  // 校验和 (可选)

    uint32_t length;  // 数据长度 (Body Length)

    // 你可以在这里添加更多字段，比如 window_size
    uint32_t window_size;  // 窗口大小
};

// 应用层协议头
struct AppHeader {
    uint8_t opCode;   // 操作码
    uint32_t length;  // 数据长度 (仅 Payload, 不含 AppHeader)
};

// Operation Codes
#define OP_MSG 0         // 普通文本消息
#define OP_UPLOAD_REQ 1  // 上传请求 (Payload = 文件名)
#define OP_DATA 2        // 文件数据块 (Payload = 字节流)
#define OP_END 3         // 结束信号 (Payload = 空)
#define OP_ACK 4         // 应用层确认 (Payload = 状态信息，可选)
#define OP_DOWNLOAD_REQ 5
#define OP_ERROR 6

// 最大的数据包大小 (MTU 限制通常是 1500，减去 IP/UDP 头，安全值设为 1400 左右)
const int MAX_PACKET_SIZE = 1400;

#endif  // TCP_PROTOCOL_H
