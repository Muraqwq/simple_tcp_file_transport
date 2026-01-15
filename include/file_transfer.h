#ifndef FILE_TRANSFER_H
#define FILE_TRANSFER_H

#include <string>

#include "tcp_connection.h"

// Constants
const int SERVER_PORT = 8080;
const std::string SERVER_IP = "127.0.0.1";

// Entry points
void run_server(int port);
void run_client(const std::string& ip, int port);

// Core application logic exposed for potential reuse (optional)
void upload_file(TCPConnection& conn, const std::string& filepath);
void download_file(TCPConnection& conn, const std::string& filename);

#endif  // FILE_TRANSFER_H
