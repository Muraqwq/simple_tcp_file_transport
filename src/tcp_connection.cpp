#include "tcp_connection.h"

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "tcp_protocol.h"

TCPConnection::TCPConnection() : state(CLOSED), snd_una(0), snd_nxt(0), rcv_nxt(0) {
    srand(time(nullptr));
    socket.create();
    socket.set_non_blocking(true);
}

TCPConnection::~TCPConnection() { socket.close(); }

bool TCPConnection::bind(int port) {
    if (socket.bind(port)) {
        state = LISTEN;
        std::cout << "[TCP] State changed to LISTEN" << std::endl;
        return true;
    }
    return false;
}

bool TCPConnection::connect(const std::string& ip, int port) {
    peer_ip = ip;
    peer_port = port;

    // TODO: 实现第一次握手
    // 1. 设置标志位 SYN
    // 2. 发送包
    // 3. 状态变更为 SYN_SENT
    // 3. 状态变更为 SYN_SENT
    send_packet(FLAG_SYN);
    state = SYN_SENT;
    return true;
}

void TCPConnection::update() {
    char buffer[MAX_PACKET_SIZE];
    std::string src_ip;
    int src_port;

    // 循环收取所有到达的包 (Drain the socket)
    while (true) {
        int bytes = socket.recv_from(buffer, MAX_PACKET_SIZE, src_ip, src_port);
        if (bytes <= 0) break;  // 读完了 (EAGAIN)

        // 解析 Header
        if (bytes < sizeof(TCPHeader)) continue;

        TCPHeader* header = (TCPHeader*)buffer;

        // 0. 校验和检查
        if (calculate_checksum(buffer, bytes) != 0) {
            // std::cout << "[TCP] Checksum failed! Dropping packet." << std::endl;
            continue;
        }

        // 调用状态机
        process_packet(*header, buffer + sizeof(TCPHeader), bytes - sizeof(TCPHeader), src_ip, src_port);
    }

    // 检查重传
    check_timeout();
}

std::string stateToString(TCPState state) {
    switch (state) {
        case CLOSED:
            return "CLOSED";
        case LISTEN:
            return "LISTEN";
        case SYN_SENT:
            return "SYN_SENT";
        case SYN_RCVD:
            return "SYN_RCVD";
        case ESTABLISHED:
            return "ESTABLISHED";
        case FIN_WAIT_1:
            return "FIN_WAIT_1";
        case FIN_WAIT_2:
            return "FIN_WAIT_2";
        case TIME_WAIT:
            return "TIME_WAIT";
        case CLOSE_WAIT:
            return "CLOSE_WAIT";
        case LAST_ACK:
            return "LAST_ACK";
        default:
            return "UNKNOWN";
    }
}

std::string flagsToString(uint8_t flags) {
    std::string s = "";
    if (flags & FLAG_SYN) s += "SYN ";
    if (flags & FLAG_ACK) s += "ACK ";
    if (flags & FLAG_FIN) s += "FIN ";
    if (flags & FLAG_RST) s += "RST ";
    if (flags & FLAG_PSH) s += "PSH ";
    if (s.empty()) return "NONE";
    return s;
}

void TCPConnection::process_packet(const TCPHeader& header, const char* data, int len, const std::string& src_ip,
                                   int src_port) {
    // 转换网络字节序为主机字节序
    uint32_t seqNum = ntohl(header.seq_num);
    uint32_t ackNum = ntohl(header.ack_num);
    uint32_t codeFlags = header.flags;  // flags is uint8_t, no endian conversion needed

    // 打印收到的包作为调试 (高频日志严重影响性能，注释掉)
    // std::cout << "[TCP] Recv Flags: [" << flagsToString(codeFlags) << "] State: " << stateToString(state)
    //           << " Seq=" << seqNum << " Ack=" << ackNum << " Len=" << len << std::endl;

    switch (state) {
        case CLOSED:
            // 不处理
            break;

        case LISTEN:
            // TODO: Server 收到 SYN -> 发送 SYN+ACK -> 变为 SYN_RCVD
            // if (header.flags & FLAG_SYN) ...
            if (codeFlags & FLAG_SYN) {
                peer_ip = src_ip;
                peer_port = src_port;
                send_packet(FLAG_SYN | FLAG_ACK, data, len);
                state = SYN_RCVD;
            }
            break;

        case SYN_SENT:
            // TODO: Client 收到 SYN+ACK -> 发送 ACK -> 变为 ESTABLISHED
            if (codeFlags & (FLAG_SYN | FLAG_ACK)) {
                send_packet(FLAG_ACK, data, len);
                state = ESTABLISHED;
            }
            break;

        case SYN_RCVD:
            // TODO: Server 收到 ACK -> 变为 ESTABLISHED
            if (codeFlags & FLAG_ACK) {
                state = ESTABLISHED;
            }
            break;

        case ESTABLISHED: {
            // --- 1. 处理 ACK (推动发送窗口) ---
            uint32_t ack = ackNum;  // 使用已转换的本地变量
            if (ack > snd_una) {
                // 累积确认：清理掉所有 seq + len <= ack 的包
                while (!send_queue.empty()) {
                    auto& head = send_queue.front();
                    uint32_t endSeq = head.seq + head.len;
                    // 注意：序列号回绕 (Wrap Around) 先不考虑，假设足够大
                    if (endSeq <= ack) {
                        send_queue.pop_front();
                    } else {
                        break;
                    }
                }
                snd_una = ack;
                dup_ack_cnt = 0;
            }

            if (ack == snd_una) {
                if (len == 0 && ++dup_ack_cnt >= MAX_DUP_CNT) {
                    // std::cout << "[TCP] Fast Retransmit: seq=" << snd_una << std::endl;

                    if (!send_queue.empty()) {
                        auto& seg = send_queue.front();
                        if (seg.seq == snd_una) {
                            send_packet(seg.data.data(), seg.len, seg.seq);
                        }
                    }
                    dup_ack_cnt = 0;  // 为了简单，重传后可以清零
                }
            }
            // 关键修复：无论 ACK 是否推进，都要更新 rwnd (处理 Window Update 包)
            rwnd = ntohl(header.window_size);

            // --- 2. 处理接收数据 (写入接收缓冲) ---
            uint32_t seq = seqNum;  // 使用已转换的本地变量
            int32_t diff = (int32_t)(seq - rcv_nxt);

            if (len > 0) {
                if (diff == 0) {
                    // 正好是期望的包 (seq == rcv_nxt)
                    if (get_window_size() < len * sizeof(char)) {
                        // 接收窗口不足，丢弃包，但必须回复 ACK 告诉对方现在的窗口大小
                        // 接收窗口不足，丢弃包，但必须回复 ACK 告诉对方现在的窗口大小
                        send_packet(FLAG_ACK);
                        return;
                    }
                    const char* p = static_cast<const char*>(data);
                    in_buffer.insert(in_buffer.end(), p, p + len);
                    rcv_nxt += len;

                    // 检查乱序缓冲里有没有能接上的
                    auto it = out_of_order_buffer.begin();
                    while (it != out_of_order_buffer.end()) {
                        int32_t bufDiff = (int32_t)(it->first - rcv_nxt);

                        if (bufDiff == 0) {
                            if (get_window_size() < it->second.size() * sizeof(char)) break;
                            in_buffer.insert(in_buffer.end(), it->second.begin(), it->second.end());
                            rcv_nxt += it->second.size();

                            it = out_of_order_buffer.erase(it);
                        } else if (bufDiff < 0) {
                            // 这是一个已经处理过的包 (Partially overlapping or Duplicate)
                            // 简单起见，如果它完全被 rcv_nxt 覆盖，直接删除
                            uint32_t endOfPkt = it->first + it->second.size();
                            int32_t endDiff = (int32_t)(endOfPkt - rcv_nxt);

                            if (endDiff <= 0) {
                                it = out_of_order_buffer.erase(it);
                            } else {
                                // 部分重叠：裁剪头部
                                // 由于 bufDiff < 0, overlapStart = rcv_nxt
                                uint32_t overlap =
                                    rcv_nxt - it->first;  // 这是安全的因为 bufDiff < 0 implies rcv_nxt > it->first
                                if (overlap < it->second.size()) {
                                    std::vector<char> remainingData(it->second.begin() + overlap, it->second.end());
                                    // 插入 remaining
                                    in_buffer.insert(in_buffer.end(), remainingData.begin(), remainingData.end());
                                    rcv_nxt += remainingData.size();
                                    it = out_of_order_buffer.erase(it);
                                } else {
                                    it = out_of_order_buffer.erase(it);
                                }
                            }
                        } else {
                            break;  // 接不上了 (bufDiff > 0)
                        }
                    }
                    // 只有真的收到了数据才回复 ACK
                    send_packet(FLAG_ACK);
                } else if (diff > 0) {
                    // 未来的包（乱序），存起来
                    std::vector<char> data_vec(data, data + len);
                    out_of_order_buffer[seq] = data_vec;
                    // 回复我们期望的 seq (即 rcv_nxt)，触发对方快重传
                    send_packet(FLAG_ACK);
                }
                // diff < 0 的是重复包，直接丢弃，但也要回 ACK 确认
                else {
                    send_packet(FLAG_ACK);
                }
            } else if (codeFlags & FLAG_FIN) {
                // TODO: 处理对端发送的 FIN
                // 1. 回复 ACK
                // 2. 状态变更: ESTABLISHED -> CLOSE_WAIT
                // 3. (可选) 通知应用层
                send_packet(FLAG_ACK);
                state = CLOSE_WAIT;
            }
        } break;

        case FIN_WAIT_1: {
            if ((header.flags & FLAG_FIN) && (header.flags & FLAG_ACK)) {
                state = TIME_WAIT;
            } else if (header.flags & FLAG_FIN) {
                state = CLOSING;
                send_packet(FLAG_ACK);
            } else if (header.flags & FLAG_ACK) {
                state = FIN_WAIT_2;
            }
        } break;

        case FIN_WAIT_2: {
            if (header.flags & FLAG_FIN) {
                state = TIME_WAIT;
                send_packet(FLAG_ACK);
            }
        } break;

        case CLOSING: {
            if (header.flags & FLAG_ACK) {
                state = TIME_WAIT;
            }
        } break;

        case TIME_WAIT: {
            // MSL 听说是内核设置，这里我需要做吗。先不做了
            sleep(2000);
            reset();
            state = CLOSED;
        }

        case CLOSE_WAIT: {
            // 我觉得 主动断开连接后，两边都不应该发除了 fin or fin_ack ack 的数据包。
            // 所以在被动关闭的这方，不应该继续发包了，应该等待 in_buffer 接收完后，返回 fin。
            // 所以我使用 while 循环，直接将接收方的程序阻塞在这里。

        } break;

        case LAST_ACK: {
            if (header.flags & FLAG_ACK) {
                close();
            }
        } break;

        default:
            break;
    }
}

void TCPConnection::send_packet(uint8_t flags, const char* data, int len) {
    TCPHeader header;
    memset(&header, 0, sizeof(header));

    header.seq_num = htonl(snd_nxt);
    header.ack_num = htonl(rcv_nxt);
    header.flags = flags;
    header.length = len;
    header.window_size = htonl(get_window_size());  // TODO: 实现接收窗口通告

    std::vector<char> packet;
    packet.resize(sizeof(TCPHeader) + len);

    memcpy(packet.data(), &header, sizeof(header));
    if (data && len > 0) {
        memcpy(packet.data() + sizeof(TCPHeader), data, len);
    }

    header.checksum = calculate_checksum(packet.data(), packet.size());
    TCPHeader* h = (TCPHeader*)packet.data();
    h->checksum = header.checksum;

    socket.send_to(packet.data(), packet.size(), peer_ip, peer_port);
}

// 重载：指定 Seq 发送数据包 (用于重传/Sliding Window)
void TCPConnection::send_packet(const char* data, int len, uint32_t seq) {
    TCPHeader header;
    memset(&header, 0, sizeof(header));

    header.seq_num = htonl(seq);      // 指定 SEQ
    header.ack_num = htonl(rcv_nxt);  // 永远带上最新的 ACK
    header.flags = FLAG_ACK;          // 数据包通常带 ACK
    header.length = len;
    header.window_size = htonl(get_window_size());

    std::vector<char> packet;
    packet.resize(sizeof(TCPHeader) + len);
    memcpy(packet.data(), &header, sizeof(header));
    if (data && len > 0) {
        memcpy(packet.data() + sizeof(TCPHeader), data, len);
    }

    header.checksum = calculate_checksum(packet.data(), packet.size());
    TCPHeader* h = (TCPHeader*)packet.data();
    h->checksum = header.checksum;

    socket.send_to(packet.data(), packet.size(), peer_ip, peer_port);
}

uint16_t TCPConnection::calculate_checksum(const void* data, size_t len) {
    const uint16_t* ptr = (const uint16_t*)data;
    uint32_t sum = 0;

    // 累加 16-bit 单词
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }

    // 如果长度是奇数，处理最后一个字节
    if (len > 0) {
        sum += *(const uint8_t*)ptr;
    }

    // 折叠 32-bit Sum 到 16-bit
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

bool TCPConnection::send(const void* data, size_t len) {
    // 1. 记录飞行中的数据量 (已发 - 已确认)
    uint32_t flight_size = snd_nxt - snd_una;

    // 2. 计算有效发送窗口
    uint32_t win = std::min(cwnd, rwnd);
    if (flight_size >= win) return false;  // 窗口满了

    uint32_t effective_window = win - flight_size;

    // 3. 判断该包是否可发
    if (effective_window < len) return false;

    // 4. 创建这个包的缓存，并且发出
    std::vector<char> data_vec;
    const char* p = static_cast<const char*>(data);
    data_vec.assign(p, p + len);

    // 构造段，使用当前的 snd_nxt
    SendSegment segment{snd_nxt, uint32_t(len), data_vec, std::chrono::steady_clock::now()};
    send_queue.emplace_back(segment);

    // 发送 (使用带 seq 的重载)
    send_packet(segment.data.data(), len, segment.seq);

    // 推进 snd_nxt
    snd_nxt += len;

    return true;
}

void TCPConnection::check_timeout() {
    auto current_time = std::chrono::steady_clock::now();

    // 必须用引用 auto&，否则修改无效！
    for (auto& seg : send_queue) {
        auto pass_time =
            std::chrono::duration_cast<std::chrono::milliseconds>(current_time - seg.last_send_time).count();
        if (pass_time >= RTO) {
            // std::cout << "[TCP] Timeout! Retransmit seq=" << seg.seq << " len=" << seg.len << std::endl;
            // 重传：必须使用当时原本的 SEQ
            send_packet(seg.data.data(), seg.len, seg.seq);

            seg.last_send_time = current_time;
            seg.retries++;
        }
    }
}

size_t TCPConnection::receive(void* buffer, size_t maxLen) {
    if (in_buffer.empty()) {
        // 如果buffer空了，而且处于 CLOSE_WAIT，说明对方发过 FIN 了，我们也读完了
        if (state == CLOSE_WAIT) {
            return -1;  // EOF 信号
        }
        return 0;
    }

    size_t copyLen = std::min(maxLen, in_buffer.size());
    std::copy(in_buffer.begin(), in_buffer.begin() + copyLen, (char*)buffer);

    // 移除已读取的数据
    auto old_window_size = get_window_size();
    in_buffer.erase(in_buffer.begin(), in_buffer.begin() + copyLen);
    auto new_window_size = get_window_size();

    // Clark算法简化版：或者从 0 变有，或者腾出了显著空间 (MSS)
    if (old_window_size == 0 && new_window_size > 0) {
        send_packet(FLAG_ACK);
    } else if (new_window_size - old_window_size >= 1400) {
        send_packet(FLAG_ACK);
    }

    return copyLen;
}

void TCPConnection::close() {
    // 1. 发送 FIN 包
    send_packet(FLAG_FIN | FLAG_ACK);

    // 2. 状态变更
    if (state == ESTABLISHED) {
        state = FIN_WAIT_1;  // 主动关闭
    } else if (state == CLOSE_WAIT) {
        state = LAST_ACK;  // 被动关闭
    }
}

void TCPConnection::reset() {
    // TODO: 实现状态重置逻辑 (用于 Server 重用 Connection)
    // 1. 清空发送/接收队列
    // 2. 重置 seq, ack, window 等变量
    // 3. state = LISTEN
    in_buffer.clear();
    send_queue.clear();
    out_of_order_buffer.clear();
    snd_una = 0;
    snd_nxt = 0;
    rcv_nxt = 0;
    dup_ack_cnt = 0;
    rwnd = MAX_RWND;

    peer_ip = {};
    peer_port = {};

    state = LISTEN;
}
