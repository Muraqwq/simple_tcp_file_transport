# MyTCP Project Roadmap

[x] **Phase 1: Foundation & Packet Definition**
    [x] Define `TCPHeader` struct in `tcp_protocol.h`.
    [x] Create `Socket` wrapper class (cross-platform: Windows/Mac).
        - [x] Initialize Winsock (Windows only).
        - [x] Create UDP socket.
        - [x] Bind/SendTo/RecvFrom methods.
    [x] Verify basic UDP communication between two programs.

[x] **Phase 2: Connection Management (Handshake)**
    [x] Define TCP States (LISTEN, SYN_SENT, ESTABLISHED, etc.).
    [x] Implement 3-way Handshake logic.
        - [x] Client: Send SYN.
        - [x] Server: Recv SYN, Send SYN-ACK.
        - [x] Client: Recv SYN-ACK, Send ACK.
    [x] Implement 4-way Wave (Teardown).

[x] **Phase 3: Reliability & Flow Control (Core)**
    [x] Implement "Stop-and-Wait" ARQ (initially).
        - [x] Sequence Numbers logic.
        - [x] Timer/Timeout mechanism.
        - [x] Retransmission logic.
    [x] Verify Checksum (Receive side).

[ ] **Phase 4: Performance & Optimization (Advanced TCP)**
    [ ] Implement "Sliding Window" (Pipeline).
        - [ ] Send Window (SND.UNA, SND.NXT, SND.WND).
        [ ] Receive Window (RCV.NXT, RCV.WND).
    [ ] Flow Control (Advertised Window).
    [ ] Congestion Control (Basic Slow Start/Congestion Avoidance).

[/] **Phase 5: Application Layer (File Transfer)**
    [x] Define Application Protocol (OpCode + Length + Content).
    [x] Implement `upload` command.
    [ ] Implement `download` command.
    [ ] Add start/end transfer signals.

[ ] **Phase 6: Windows Portability & Final Polish**
    [ ] Verify compilation on Windows (using MinGW or VS).
    [ ] Polish CLI output.
