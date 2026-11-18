#include <winsock2.h>
#include <ws2tcpip.h>
#include "fluxtop/network_collector.hpp"
#include "fluxtop/windows.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <thread>
#include <stdexcept>

namespace {
struct Socket {
    SOCKET value = INVALID_SOCKET;
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
};
void require(bool ok) { if (!ok) throw std::runtime_error("Loopback traffic fixture failed"); }
void traffic(int family, int type) {
    Socket server{socket(family, type, 0)}, client{socket(family, type, 0)};
    require(server.value != INVALID_SOCKET && client.value != INVALID_SOCKET);
    sockaddr_storage address{};
    int length{};
    if (family == AF_INET) {
        auto& a = reinterpret_cast<sockaddr_in&>(address);
        a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); length = sizeof(a);
    } else {
        auto& a = reinterpret_cast<sockaddr_in6&>(address);
        a.sin6_family = AF_INET6; a.sin6_addr = in6addr_loopback; length = sizeof(a);
    }
    require(bind(server.value, reinterpret_cast<sockaddr*>(&address), length) == 0);
    require(getsockname(server.value, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    if (type == SOCK_STREAM) require(listen(server.value, 1) == 0);
    require(connect(client.value, reinterpret_cast<sockaddr*>(&address), length) == 0);
    Socket accepted;
    if (type == SOCK_STREAM) { accepted.value = accept(server.value, nullptr, nullptr); require(accepted.value != INVALID_SOCKET); }
    const auto receiver = type == SOCK_STREAM ? accepted.value : server.value;
    DWORD timeout = 3000;
    setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::array<char, 4096> bytes{};
    for (int packet = 0; packet < 32; ++packet) {
        require(send(client.value, bytes.data(), static_cast<int>(bytes.size()), 0) == static_cast<int>(bytes.size()));
        int received = 0;
        while (received < static_cast<int>(bytes.size())) {
            const auto count = recv(receiver, bytes.data(), static_cast<int>(bytes.size()) - received, 0);
            require(count > 0); received += count;
        }
    }
}
}
int main() {
    try {
        fluxtop::NetworkCollector disabled(false);
        fluxtop::Snapshot off; disabled.collect(off);
        require(off.network_status == "disabled" && !off.total_rx_bytes_per_second);
        fluxtop::NetworkCollector collector(true);
        fluxtop::Snapshot snapshot;
        fluxtop::Process process; process.pid = GetCurrentProcessId(); process.has_baseline = true;
        FILETIME created{}, exited{}, kernel{}, user{};
        require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE);
        process.creation_time = fluxtop::filetime_ticks(created);
        snapshot.processes = {process}; collector.collect(snapshot);
        if (snapshot.network_status == "administrator-required") {
            std::cout << "SKIP: elevated token required for the real ETW integration test\n"; return 77;
        }
        require(snapshot.network_status == "ready");
        WSADATA data{}; require(WSAStartup(MAKEWORD(2,2), &data) == 0);
        struct Cleanup { ~Cleanup() { WSACleanup(); } } cleanup;
        for (const int family : {AF_INET, AF_INET6}) for (const int type : {SOCK_STREAM, SOCK_DGRAM}) {
            traffic(family, type);
            bool seen = false;
            for (int retry = 0; retry < 5 && !seen; ++retry) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1200));
                snapshot = {}; snapshot.processes = {process}; collector.collect(snapshot);
                require(snapshot.network_status == "ready");
                const auto& row = snapshot.processes[0];
                seen = row.rx_bytes_per_second.value_or(0) > 0 && row.tx_bytes_per_second.value_or(0) > 0;
                if (seen) require(snapshot.total_rx_bytes_per_second >= row.rx_bytes_per_second
                    && snapshot.total_tx_bytes_per_second >= row.tx_bytes_per_second);
            }
            require(seen);
        }
        std::cout << "ETW TCP/UDP IPv4/IPv6 process attribution and system totals passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
