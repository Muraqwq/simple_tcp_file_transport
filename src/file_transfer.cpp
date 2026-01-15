#include "file_transfer.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

#include "tcp_protocol.h"

// Helper functions (internal to this compilation unit mostly, but good to keep together)

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
    }
}

// 辅助函数：处理接收到的应用层数据 (处理粘包/半包)
bool process_app_messages(TCPConnection& conn, std::vector<char>& appBuffer,
                          std::function<void(uint8_t, const std::string&)> handler) {
    conn.update();
    char tempBuf[MAX_PACKET_SIZE * 2];
    size_t n = conn.receive(tempBuf, sizeof(tempBuf));

    if (n == -1) {
        // 收到 EOF，且处理完了残余数据
        return false;  // 告诉上层循环，该断开了
    }

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
    return true;
}

// Helper: 打印简单进度条
void print_progress(long long current, long long total) {
    if (total <= 0) return;
    double progress = (double)current / total;
    int barWidth = 50;

    std::cout << "\r[";
    int pos = barWidth * progress;
    for (int i = 0; i < barWidth; ++i) {
        if (i < pos)
            std::cout << "=";
        else if (i == pos)
            std::cout << ">";
        else
            std::cout << " ";
    }
    std::cout << "] " << int(progress * 100.0) << " % (" << (current / 1024) << " KB / " << (total / 1024) << " KB)"
              << std::flush;
}

// 辅助函数：比较两个文件内容 (Deprecated for remote, kept for logical completeness if needed locally)
bool check_files_equal(const std::string& f1, const std::string& f2) {
    std::ifstream s1(f1, std::ios::binary);
    std::ifstream s2(f2, std::ios::binary);

    if (!s1 || !s2) return false;

    std::istreambuf_iterator<char> begin1(s1);
    std::istreambuf_iterator<char> begin2(s2);
    std::istreambuf_iterator<char> end;

    return std::equal(begin1, end, begin2);
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
    long long receivedBytes = 0;
    std::string currentFileName;
    long long totalExpectedBytes = 0;
    std::vector<char> appBuffer;

    while (true) {
        // 1. 等待连接 (可选: 打印一下 waiting)
        if (conn.get_state() == LISTEN) {
            conn.update();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 2. 已连接，处理消息
        bool ok = process_app_messages(conn, appBuffer, [&](uint8_t op, const std::string& data) {
            if (op == OP_UPLOAD_REQ) {
                // Format: filename|filesize
                std::string payload = data;
                std::string sizeStr = "0";
                size_t sep = payload.find('|');
                if (sep != std::string::npos) {
                    currentFileName = "received_" + payload.substr(0, sep);
                    currentFileName = "received_" + currentFileName.substr(currentFileName.find_last_of("/\\") + 1);
                    sizeStr = payload.substr(sep + 1);
                } else {
                    currentFileName = "received_" + payload.substr(payload.find_last_of("/\\") + 1);
                }

                try {
                    totalExpectedBytes = std::stoll(sizeStr);
                } catch (...) {
                    totalExpectedBytes = 0;
                }

                outFile.open(currentFileName, std::ios::binary);
                receivingFile = true;
                receivedBytes = 0;
                std::cout << "[Server] Start receiving file: " << currentFileName << " (Size: " << totalExpectedBytes
                          << " bytes)" << std::endl;
            } else if (op == OP_DOWNLOAD_REQ) {
                std::string filePath = data.substr(data.find_last_of("/\\") + 1);
                std::cout << "[Server] Start uploading file " << filePath << std::endl;
                std::ifstream file(filePath, std::ios::binary);
                if (!file) {
                    send_app_msg(conn, OP_ERROR, "File not found");
                    return;
                }

                file.seekg(0, std::ios::end);
                long long fileSize = file.tellg();
                file.seekg(0, std::ios::beg);

                send_app_msg(conn, OP_FILE_INFO, std::to_string(fileSize));

                auto startTime = std::chrono::steady_clock::now();
                char readBuf[1024];
                long long totalBytes = 0;

                while (file.read(readBuf, sizeof(readBuf)) || file.gcount() > 0) {
                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count() >= 180) {
                        std::cout << "\n[Server] Timeout!" << std::endl;
                        break;
                    }

                    std::string chunk(readBuf, file.gcount());
                    send_app_msg(conn, OP_DATA, chunk);
                    conn.update();
                    totalBytes += chunk.size();

                    if (totalBytes % (1024 * 10) == 0) print_progress(totalBytes, fileSize);
                }
                print_progress(totalBytes, fileSize);
                std::cout << std::endl;
                send_app_msg(conn, OP_END, "");

            } else if (op == OP_DATA) {
                if (receivingFile && outFile.is_open()) {
                    outFile.write(data.data(), data.size());
                    receivedBytes += data.size();
                    if (totalExpectedBytes > 0 && receivedBytes % (1024 * 10) == 0) {
                        print_progress(receivedBytes, totalExpectedBytes);
                    }
                }
            } else if (op == OP_END) {
                if (receivingFile) {
                    print_progress(receivedBytes, totalExpectedBytes > 0 ? totalExpectedBytes : receivedBytes);
                    std::cout << std::endl;
                    outFile.close();
                    receivingFile = false;
                    std::cout << "[Server] File received successfully! Size: " << receivedBytes << " bytes"
                              << std::endl;
                    send_app_msg(conn, OP_END, std::to_string(receivedBytes));
                }
            }
        });

        // 3. 检查连接是否断开 (ok == false means EOF or Error)
        if (!ok) {
            std::cout << "[Server] Connection closed. Resetting..." << std::endl;
            conn.reset();
            // 重置应用层状态
            receivingFile = false;
            if (outFile.is_open()) outFile.close();
            appBuffer.clear();
        }

        std::this_thread::yield();
    }
}

void upload_file(TCPConnection& conn, const std::string& filepath) {
    std::string filename = filepath.substr(filepath.find_last_of("/\\") + 1);
    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        std::cerr << "File not found: " << filepath << std::endl;
        return;
    }

    std::string recvFilename = "received_" + filename;

    // 1. 发送 Upload Request
    file.seekg(0, std::ios::end);
    long long fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::cout << "[Client] Uploading " << filepath << " (Size: " << fileSize << " bytes)..." << std::endl;
    // Send "filename|filesize"
    send_app_msg(conn, OP_UPLOAD_REQ, filename + "|" + std::to_string(fileSize));

    // 2. 发送 Data (Benchmarking)
    auto startTime = std::chrono::steady_clock::now();
    long long totalBytes = 0;
    char buffer[1024];

    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        std::string chunk(buffer, file.gcount());
        send_app_msg(conn, OP_DATA, chunk);

        conn.update();
        totalBytes += chunk.size();

        if (totalBytes % (1024 * 10) == 0) print_progress(totalBytes, fileSize);
    }
    print_progress(totalBytes, fileSize);
    std::cout << std::endl;

    // 3. 发送 END
    send_app_msg(conn, OP_END, "");

    // 等待应用层确认 (Server 必须回复 OP_END 表示写盘完成)
    std::cout << "[Client] Waiting for Server Confirmation..." << std::endl;
    std::vector<char> rxBuffer;
    bool confirmed = false;
    long long serverReceivedBytes = -1;
    bool timeout = false;
    auto waitStart = std::chrono::steady_clock::now();

    while (!confirmed) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - waitStart).count() > 10) {
            std::cout << "[Client] Confirmation Timeout!" << std::endl;
            timeout = true;
            break;
        }

        while (!conn.is_send_complete()) {
            conn.update();
            std::this_thread::yield();
        }

        bool ok = process_app_messages(conn, rxBuffer, [&](uint8_t op, const std::string& msg) {
            if (op == OP_END) {
                confirmed = true;
                // 解析服务器返回的字节数
                try {
                    serverReceivedBytes = std::stoll(msg);
                    std::cout << "[Client] Server confirmed. Received size: " << serverReceivedBytes << " bytes."
                              << std::endl;
                } catch (...) {
                    serverReceivedBytes = -1;
                    std::cout << "[Client] Server confirmed (No size info)." << std::endl;
                }
            } else if (op == OP_ERROR) {
                std::cout << "[Client] Server Error: " << msg << std::endl;
                confirmed = true;  // Treated as confirmed but error
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

    // 4. 校验
    std::string verifyResult = "Skipped";
    if (!timeout) {
        std::cout << "  - Verification (Remote): ";
        if (serverReceivedBytes == totalBytes) {
            std::cout << "PASS (Size Match)" << std::endl;
            verifyResult = "PASS_REMOTE";
        } else {
            std::cout << "FAIL (Size Mismatch: Sent " << totalBytes << " vs Recv " << serverReceivedBytes << ")"
                      << std::endl;
            verifyResult = "FAIL_SIZE";
        }
    } else {
        verifyResult = "Timeout";
    }

    // 5. 记录日志 (benchmark.log)
    std::ofstream log("benchmark.log", std::ios::app);
    std::time_t t = std::time(nullptr);
    char timeStr[100];
    std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    log << timeStr << "," << filename << "," << totalBytes << "," << duration << "," << speed << "," << verifyResult
        << "\n";
}

void download_file(TCPConnection& conn, const std::string& filename) {
    std::cout << "[Client] Downloading " << filename << "..." << std::endl;
    send_app_msg(conn, OP_DOWNLOAD_REQ, filename);

    std::vector<char> appBuffer;
    std::ofstream outFile;
    bool receiving = false;
    long long totalBytesRecv = 0;
    long long totalExpectedSize = 0;
    bool done = false;

    // Wait for response
    while (!done) {
        bool ok = process_app_messages(conn, appBuffer, [&](uint8_t op, const std::string& data) {
            if (op == OP_FILE_INFO) {
                try {
                    totalExpectedSize = std::stoll(data);
                } catch (...) {
                    totalExpectedSize = 0;
                }
                std::cout << "[Client] File size: " << totalExpectedSize << " bytes" << std::endl;
                outFile.open("downloaded_" + filename, std::ios::binary);
                receiving = true;
            } else if (op == OP_DATA) {
                if (receiving && outFile.is_open()) {
                    outFile.write(data.data(), data.size());
                    totalBytesRecv += data.size();
                    if (totalExpectedSize > 0 && totalBytesRecv % (1024 * 10) == 0) {
                        print_progress(totalBytesRecv, totalExpectedSize);
                    }
                } else if (!receiving) {
                    // Fallback if FILE_INFO missed (unlikely) or legacy server
                    outFile.open("downloaded_" + filename, std::ios::binary);
                    receiving = true;
                    outFile.write(data.data(), data.size());
                    totalBytesRecv += data.size();
                }
            } else if (op == OP_END) {
                print_progress(totalBytesRecv, totalExpectedSize > 0 ? totalExpectedSize : totalBytesRecv);
                std::cout << std::endl;
                std::cout << "[Client] Download complete! Saved to downloaded_" << filename << std::endl;
                done = true;
            } else if (op == OP_ERROR) {
                std::cerr << "[Client] Error: " << data << std::endl;
                done = true;
            }
        });
        if (!ok) break;
        std::this_thread::yield();
    }
}

void run_client(const std::string& ip, int port) {
    TCPConnection conn;
    if (!conn.connect(ip, port)) {
        std::cerr << "[Client] Failed to connect to " << ip << ":" << port << std::endl;
        return;
    }
    std::cout << "[Client] Send SYN to Server";
    // Wait for ESTABLISHED
    while (conn.get_state() != ESTABLISHED) {
        conn.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cout << "[Client] Connected! Type 'upload <filename>' or 'download <filename>'" << std::endl;

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
            conn.close();
            std::cout << "[Client] Closing connection..." << std::endl;
            // 简单的等待，直到 socket 关闭 (Encapsulated wait)
            for (int i = 0; i < 50; ++i) {              // 5s timeout
                if (conn.get_state() == CLOSED) break;  // Keep this internal check for now or move to verify method
                conn.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            break;
        } else {
            std::cout << "Unknown command" << std::endl;
        }
    }
}
