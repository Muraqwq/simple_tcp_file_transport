#ifndef TCP_CONNECTION_H
#define TCP_CONNECTION_H

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "tcp_protocol.h"
#include "tcp_socket.h"

// 内部结构：发送段记录
struct SendSegment {
    uint32_t seq;
    uint32_t len;
    std::vector<char> data;
    std::chrono::steady_clock::time_point lastSendTime;
    int retries = 0;
};

// TCP 状态枚举
enum TCPState {
    CLOSED,
    LISTEN,
    SYN_SENT,
    SYN_RCVD,
    ESTABLISHED,
    FIN_WAIT_1,
    FIN_WAIT_2,
    TIME_WAIT,
    CLOSE_WAIT,
    LAST_ACK
};

class TCPConnection {
public:
    TCPConnection();
    ~TCPConnection();

    // 作为 Server 启动监听
    bool bind(int port);

    // 作为 Client 连接 Server
    bool connect(const std::string& ip, int port);

    // 发送数据 (ARQ Stop-and-Wait)
    // 成功放入缓冲区返回 true，如果正在等待 ACK 则返回 false 或阻塞 (当前简单实现为返回 false)
    bool send(const void* data, size_t len);

    // 核心：接收并处理数据包（驱动状态机）
    // 你需要在 main loop 中不断调用它
    void update();

    // 应用层读取数据 (从接收缓冲区取出)
    // 返回读取的字节数
    size_t receive(void* buffer, size_t maxLen);

    // 检查发送队列是否为空 (所有数据都已收到 ACK)
    bool isSendComplete() const { return sendQueue.empty(); }

    // 获取当前状态
    TCPState getState() const { return state; }

private:
    // 状态机处理函数
    void processPacket(const TCPHeader& header, const char* data, int len, const std::string& srcIp, int srcPort);

    // 检查是否超时重传
    void checkTimeout();

    // 发送包的辅助函数
    void sendPacket(uint8_t flags, const char* data = nullptr, int len = 0);
    // 重载：指定 Seq 发送数据包 (用于重传/Sliding Window)
    void sendPacket(const char* data, int len, uint32_t seq);

    uint16_t calculateChecksum(const void* data, size_t len);

    uint32_t get_window_size() noexcept { return MAX_RWND - inBuffer.size() * sizeof(char); }

private:
    TCPSocket socket;
    TCPState state;

    // 对端信息
    std::string peerIp;
    int peerPort;

    // Sliding Window 状态
    std::deque<SendSegment> sendQueue;                       // 发送队列 (SND.UNA -> SND.NXT)
    std::map<uint32_t, std::vector<char>> outOfOrderBuffer;  // 乱序接收缓冲

    // 简单流控 & 拥塞控制
    uint32_t MAX_RWND = INT32_MAX;
    uint32_t rwnd = MAX_RWND;    // 对方的接收窗口 (默认 64KB)
    uint32_t cwnd = 100 * 1400;  // 拥塞窗口 (加大到 100 MSS 以测试吞吐)

    std::deque<char> inBuffer;  // 接收缓冲区 (存放已确认但应用层未取走的数据)
    uint16_t dup_ack_cnt = 0;
    uint16_t MAX_DUP_CNT = 3;
    const int RTO = 200;  // 超时时间 (ms)

    // 序列号管理 (TCP Standard Naming)
    uint32_t snd_una;  // Open Left of Send Window (Oldest Unacknowledged)
    uint32_t snd_nxt;  // Open Right of Send Window (Next to Send)
    uint32_t rcv_nxt;  // Receive Window (Next Expected)

    // Helper to get initial seq
    uint32_t iss;  // Initial Send Sequence
    uint32_t irs;  // Initial Receive Sequence
};

#endif  // TCP_CONNECTION_H
