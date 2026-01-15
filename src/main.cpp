#include <iostream>
#include <string>

#include "file_transfer.h"

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
