#include "tcp_socket.h"

#ifndef _WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <cstring>

TCPSocket::TCPSocket() {
    sock_fd = INVALID_SOCKET;
#ifdef _WIN32
    // Windows 必须初始化 Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKELONG(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed!" << std::endl;
    }
#endif
}

TCPSocket::~TCPSocket() {
    close();
#ifdef _WIN32
    WSACleanup();
#endif
}

bool TCPSocket::create() {
    // TODO: 实现创建 UDP socket 的逻辑
    // sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    // 记得检查是否 == INVALID_SOCKET
    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd == INVALID_SOCKET) return false;
    // int opt = 4 * 1024 * 1024;  // 4MB Buffer
    // setsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF, (const char *)&opt, sizeof(opt));
    return true;
}

bool TCPSocket::bind(int port) {
    if (sock_fd == INVALID_SOCKET) return false;

    // TODO: 填写 sockaddr_in 结构体并调用 bind
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (::bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        return false;
    }

    return true;
}

int TCPSocket::send_to(const void *data, int len, const std::string &target_ip, int target_port) {
    if (sock_fd == INVALID_SOCKET) return -1;

    // TODO: 填写目标地址 sockaddr_in 并调用 sendto
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(target_port);
    addr.sin_addr.s_addr = inet_addr(target_ip.c_str());
    if (sendto(sock_fd, (const char *)data, len, 0, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        return -1;
    }
    return len;
}

int TCPSocket::recv_from(void *buffer, int max_len, std::string &src_ip, int &src_port) {
    if (sock_fd == INVALID_SOCKET) return -1;

    // TODO: 调用 recvfrom 接收数据
    // 记得把网络字节序转换回主机字节序 (ntohs, inet_ntop 等)
    // 更新 src_ip 和 src_port
    sockaddr_in addr;
    socklen_t sock_len = sizeof(addr);

    int ret = recvfrom(sock_fd, buffer, max_len, 0, (struct sockaddr *)&addr, &sock_len);
    if (ret == SOCKET_ERROR) return -1;

    char src_ip_n[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, (const void *)&addr.sin_addr, (char *)&src_ip_n, INET_ADDRSTRLEN);

    src_ip = std::string(src_ip_n);
    src_port = ntohs(addr.sin_port);
    return ret;
}

void TCPSocket::close() {
    if (sock_fd != INVALID_SOCKET) {
#ifdef _WIN32
        closesocket(sock_fd);
#else
        ::close(sock_fd);
#endif
        sock_fd = INVALID_SOCKET;
    }
}

void TCPSocket::set_non_blocking(bool nonBlocking) {
    // TODO: 选做，设置 socket 为非阻塞模式
#ifdef _WIN32
    u_long mode = 1;  // 1: 非阻塞, 0: 阻塞
    ioctlsocket(sock_fd, FIONBIO, &mode);
#else
    int flags = fcntl(sock_fd, F_GETFL, 0);
    fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);
#endif
}
