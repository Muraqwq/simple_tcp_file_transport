#include <chrono>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "tcp_connection.h"
#include "tcp_protocol.h"

// 辅助函数：发送应用层消息 (阻塞直到从 buffer 发出)
void send_app_msg(TCPConnection& conn, uint8_t op, const std::string& data) {
    std::vector<char> packet;
    packet.resize(sizeof(AppHeader) + data.size());

    AppHeader* hdr = (AppHeader*)packet.data();
    hdr->opCode = op;
    hdr->length = data.size();

    memcpy(packet.data() + sizeof(AppHeader), data.data(), data.size());

    while (!conn.send(packet.data(), packet.size())) {
        conn.update();
        std::this_thread::yield();
        // std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 移除人工延迟，全速发送
    }
}

// Constants
const int SERVER_PORT = 8080;
const std::string SERVER_IP = "127.0.0.1";

// 辅助函数：处理接收到的应用层数据 (处理粘包/半包)
// 返回值: true 表示继续循环，false 表示应当退出外层循环 (如连接断开)
bool process_app_messages(TCPConnection& conn, std::vector<char>& appBuffer,
                          std::function<void(uint8_t, const std::string&)> handler) {
    conn.update();
    char tempBuf[MAX_PACKET_SIZE * 2];
    size_t n = conn.receive(tempBuf, sizeof(tempBuf));

    if (n > 0) {
        appBuffer.insert(appBuffer.end(), tempBuf, tempBuf + n);

        char* ptr = appBuffer.data();
        size_t remaining = appBuffer.size();
        size_t consumed = 0;

        while (remaining >= sizeof(AppHeader)) {
            AppHeader* appHdr = (AppHeader*)ptr;
            size_t totalLen = sizeof(AppHeader) + appHdr->length;

            if (remaining < totalLen) break;  // 半包

            // 完整包，回调处理
            std::string payload(ptr + sizeof(AppHeader), appHdr->length);
            handler(appHdr->opCode, payload);

            ptr += totalLen;
            remaining -= totalLen;
            consumed += totalLen;
        }

        if (consumed > 0) {
            appBuffer.erase(appBuffer.begin(), appBuffer.begin() + consumed);
        }
        return true;
    }
    return true;  // No data received is also "continue"
}

void run_server(int port) {
    TCPConnection conn;
    if (!conn.bind(port)) {
        std::cerr << "[Server] Failed to bind to port " << port << std::endl;
        return;
    }

    std::cout << "[Server] Listening on port " << port << "..." << std::endl;

    std::ofstream outFile;
    bool receivingFile = false;
    std::string currentFileName;
    std::vector<char> appBuffer;

    while (true) {
        bool ok = process_app_messages(conn, appBuffer, [&](uint8_t op, const std::string& data) {
            if (op == OP_UPLOAD_REQ) {
                currentFileName = "received_" + data.substr(data.find_last_of("/\\") + 1);
                outFile.open(currentFileName, std::ios::binary);
                receivingFile = true;
                std::cout << "[Server] Start receiving file: " << currentFileName << std::endl;
            } else if (op == OP_DOWNLOAD_REQ) {
                std::string filePath = data.substr(data.find_last_of("/\\") + 1);
                std::cout << "[Server] Start uploading file " << filePath << std::endl;

                std::ifstream file(filePath, std::ios::binary);
                if (!file) {
                    send_app_msg(conn, OP_ERROR, "File not found");
                    return;
                }

                auto startTime = std::chrono::steady_clock::now();
                char readBuf[1024];
                long long totalBytes = 0;

                while (file.read(readBuf, sizeof(readBuf)) || file.gcount() > 0) {
                    // Check timeout
                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count() >= 180) {
                        std::cout << "\n[Server] Timeout!" << std::endl;
                        break;
                    }

                    std::string chunk(readBuf, file.gcount());
                    send_app_msg(conn, OP_DATA, chunk);
                    conn.update();
                    totalBytes += chunk.size();

                    if (totalBytes % (1024 * 50) == 0)  // log
                        std::cout << "\r[Server] Sending " << (totalBytes / 1024) << " KB..." << std::flush;
                }
                std::cout << std::endl;
                send_app_msg(conn, OP_END, "");

            } else if (op == OP_DATA) {
                if (receivingFile && outFile.is_open()) {
                    outFile.write(data.data(), data.size());
                }
            } else if (op == OP_END) {
                if (receivingFile) {
                    outFile.close();
                    receivingFile = false;
                    std::cout << "\n[Server] File received successfully!" << std::endl;
                    // Send Confirmation back
                    send_app_msg(conn, OP_END, "OK");
                }
            }
        });

        if (!ok) break;
        std::this_thread::yield();
    }
}

// 辅助函数：比较两个文件内容
bool check_files_equal(const std::string& f1, const std::string& f2) {
    std::ifstream s1(f1, std::ios::binary);
    std::ifstream s2(f2, std::ios::binary);
    if (!s1 || !s2) return false;

    // 先比大小
    s1.seekg(0, std::ios::end);
    s2.seekg(0, std::ios::end);
    if (s1.tellg() != s2.tellg()) return false;

    s1.seekg(0, std::ios::beg);
    s2.seekg(0, std::ios::beg);

    return std::equal(std::istreambuf_iterator<char>(s1.rdbuf()), std::istreambuf_iterator<char>(),
                      std::istreambuf_iterator<char>(s2.rdbuf()));
}

void upload_file(TCPConnection& conn, const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        std::cerr << "File not found: " << filepath << std::endl;
        return;
    }

    // 获取文件名
    std::string filename = filepath.substr(filepath.find_last_of("/\\") + 1);
    std::string recvFilename = "received_" + filename;

    // 1. 发送 Upload Request
    std::cout << "[Client] Uploading " << filepath << " (Max 180s)..." << std::endl;
    send_app_msg(conn, OP_UPLOAD_REQ, filepath);

    // 2. 发送 Data (Benchmarking)
    auto startTime = std::chrono::steady_clock::now();

    char readBuf[1024];
    long long totalBytes = 0;
    bool timeout = false;

    while (file.read(readBuf, sizeof(readBuf)) || file.gcount() > 0) {
        // 检查超时 (180s)
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count() >= 180) {
            std::cout << "\n[Client] Timeout reached (180s)!" << std::endl;
            timeout = true;
            break;
        }

        std::string chunk(readBuf, file.gcount());
        send_app_msg(conn, OP_DATA, chunk);
        conn.update();
        totalBytes += chunk.size();

        if (totalBytes % (1024 * 50) == 0)  // Reduce logging frequency
            std::cout << "\r[Client] Sent " << (totalBytes / 1024) << " KB..." << std::flush;
    }
    std::cout << std::endl;

    // 3. 发送 END
    send_app_msg(conn, OP_END, "");

    // 等待应用层确认 (Server 必须回复 OP_END 表示写盘完成)
    std::cout << "[Client] Waiting for Server Confirmation..." << std::endl;
    std::vector<char> rxBuffer;
    bool confirmed = false;
    auto waitStart = std::chrono::steady_clock::now();

    while (!confirmed) {
        // 检查超时 (30s)
        if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - waitStart).count() >
            30) {
            std::cout << "[Client] Check confirmation timeout!" << std::endl;
            timeout = true;
            break;
        }

        bool ok = process_app_messages(conn, rxBuffer, [&](uint8_t op, const std::string& msg) {
            if (op == OP_END) {
                confirmed = true;
                std::cout << "[Client] Server confirmed receipt." << std::endl;
            } else if (op == OP_ERROR) {
                std::cerr << "[Client] Server Error: " << msg << std::endl;
                timeout = true;  // Treat as failure
                confirmed = true;
            }
        });
        if (!ok) break;
        std::this_thread::yield();
    }

    auto endTime = std::chrono::steady_clock::now();
    double duration = std::chrono::duration<double>(endTime - startTime).count();
    double speed = (totalBytes / 1024.0) / duration;  // KB/s

    std::cout << "[Client] Upload finished." << std::endl;
    std::cout << "  - Duration: " << duration << " s" << std::endl;
    std::cout << "  - Sent: " << (totalBytes / 1024.0) << " KB" << std::endl;
    std::cout << "  - Speed: " << speed << " KB/s" << std::endl;

    // 4. 校验 (仅在未超时且都在本地时)
    std::string verifyResult = "Skipped";
    if (!timeout) {
        // 给他一点时间写盘
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));

        std::ifstream s1(filepath, std::ios::binary | std::ios::ate);
        std::ifstream s2(recvFilename, std::ios::binary | std::ios::ate);
        if (s1.tellg() != s2.tellg()) {
            std::cout << "  - Verification: FAIL (Size Mismatch: " << s1.tellg() << " vs " << s2.tellg() << ")"
                      << std::endl;
            verifyResult = "FAIL_SIZE";
        } else {
            // Size Match, check content
            if (check_files_equal(filepath, recvFilename)) {
                verifyResult = "PASS";
                std::cout << "  - Verification: PASS" << std::endl;
            } else {
                verifyResult = "FAIL_CONTENT";
                std::cout << "  - Verification: FAIL (Content Mismatch)" << std::endl;
            }
        }
    } else {
        verifyResult = "Timeout";
    }

    // 5. 记录日志 (benchmark.log)
    // Format: Timestamp, File, Bytes, Time, Speed(KB/s), Result
    std::ofstream log("benchmark.log", std::ios::app);
    std::time_t t = std::time(nullptr);
    char timeStr[100];
    std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", std::localtime(&t));

    log << timeStr << ", " << filename << ", " << totalBytes << ", " << duration << ", " << speed << ", "
        << verifyResult << "\n";
    log.close();

    // 6. 删除接收文件
    // std::remove(recvFilename.c_str()); // 暂时注释掉，以免误删，你可以手动开启
    // 6. 删除接收文件
    // std::remove(recvFilename.c_str()); // 暂时注释掉，以免误删，你可以手动开启
    // if (std::remove(recvFilename.c_str()) == 0) {
    //     std::cout << "  - Removed received file: " << recvFilename << std::endl;
    // }
}

void download_file(TCPConnection& conn, const std::string& filepath) {
    // 获取文件名
    std::string filename = filepath.substr(filepath.find_last_of("/\\") + 1);
    std::string recvFilename = "received_" + filename;

    // 1. 发送 Upload Request
    std::cout << "[Client] Requesting download:" << filepath << " (Max 180s)..." << std::endl;
    send_app_msg(conn, OP_DOWNLOAD_REQ, filepath);

    std::ofstream outFile;
    std::string saveName = "download_" + filename;
    outFile.open(saveName, std::ios::binary);
    bool receiving = true;
    std::vector<char> appBuffer;

    while (receiving) {
        bool ok = process_app_messages(conn, appBuffer, [&](uint8_t op, const std::string& data) {
            if (op == OP_DATA) {
                if (outFile.is_open()) {
                    outFile.write(data.data(), data.size());
                }
            } else if (op == OP_END) {
                receiving = false;
                outFile.close();
                std::cout << "[Client] Download finished: " << saveName << std::endl;
            } else if (op == OP_ERROR) {
                receiving = false;
                outFile.close();
                std::remove(saveName.c_str());
                std::cout << "[Client] Server Error: " << data << std::endl;
            }
        });

        if (!ok) break;
        std::this_thread::yield();
    }
}

void run_client(const std::string& ip, int port) {
    TCPConnection conn;
    if (!conn.connect(ip, port)) {
        std::cerr << "[Client] Connection failed" << std::endl;
        return;
    }

    // 等握手完成
    while (conn.getState() != ESTABLISHED) {
        conn.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cout << "[Client] Connected! Type 'upload <filename>' to send file." << std::endl;

    while (true) {
        conn.update();

        std::cout << "> ";
        std::string cmd;
        if (!(std::cin >> cmd)) break;

        if (cmd == "upload") {
            std::string path;
            std::cin >> path;
            upload_file(conn, path);
        } else if (cmd == "download") {
            std::string path;
            std::cin >> path;
            download_file(conn, path);
        } else if (cmd == "exit") {
            break;
        } else {
            std::cout << "Unknown command" << std::endl;
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: ./tcp_app <mode> [args]\n"
                  << " Modes:\n"
                  << "   server [port]       (default: 8080)\n"
                  << "   client [ip] [port]  (default: 127.0.0.1 8080)\n";
        return 0;
    }

    std::string mode = argv[1];

    if (mode == "server") {
        int port = (argc >= 3) ? std::stoi(argv[2]) : SERVER_PORT;
        run_server(port);
    } else if (mode == "client") {
        std::string ip = (argc >= 3) ? argv[2] : SERVER_IP;
        int port = (argc >= 4) ? std::stoi(argv[3]) : SERVER_PORT;
        run_client(ip, port);
    } else {
        std::cerr << "Unknown mode: " << mode << std::endl;
        return 1;
    }

    return 0;
}
