#ifndef TCP_SOCKET_H
#define TCP_SOCKET_H

// #include <cstdint>
#include <string>

// 平台差异宏定义
#ifdef _WIN32
#define NOMINMAX  // Prevent Windows min/max macros from conflicting with std::min/max
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int socket_t;
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#endif

// Socket 封装框架
class TCPSocket {
public:
    TCPSocket();
    ~TCPSocket();

    // 初始化Socket
    bool create();

    // 绑定端口 (用于接收方/服务器)
    bool bind(int port);

    // 发送数据到指定地址
    // 返回发送的字节数，失败返回 -1
    int send_to(const void* data, int len, const std::string& target_ip, int target_port);

    // 接收数据
    // buffer: 接收缓冲区
    // max_len: 缓冲区大小
    // src_ip/src_port: 输出参数，记录发送者的信息
    // 返回接收的字节数，超时或无数据可能返回 0 或 -1 (取决于是否阻塞)
    int recv_from(void* buffer, int max_len, std::string& src_ip, int& src_port);

    // 关闭 Socket
    void close();

    // 设置非阻塞模式 (可选，建议实现)
    void set_non_blocking(bool nonBlocking);

private:
    socket_t sock_fd;
};

#endif  // TCP_SOCKET_H
