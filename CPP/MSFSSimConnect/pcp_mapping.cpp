/********************************************************************
 * MSFS-SimConnect 电脑端 - PCP (Port Control Protocol, RFC 6887)
 * 向默认网关的 PCP 服务器(5351)发送 MAP 请求放行入站端口
 ********************************************************************/

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Iphlpapi.lib")

#include "pcp_mapping.h"

extern HWND hMainWnd;
#define WM_APP_PCP_UPDATE (WM_APP + 3)

namespace {

const int PCP_SERVER_PORT = 5351;
const uint32_t PCP_LIFETIME = 3600;   // 放行有效期（秒），到期自动关闭，客户端续期
const int PCP_REFRESH_MS = 300000;    // 每 5 分钟续期一次

std::atomic<bool> g_pcpRunning{ false };
std::atomic<bool> g_pcpOk{ false };
std::mutex g_pcpMutex;
std::string g_pcpStatus;
int g_pcpPort = 36666;

uint8_t g_nonceTcp[12] = { 0 };
uint8_t g_nonceUdp[12] = { 0 };
bool g_nonceTcpSet = false;
bool g_nonceUdpSet = false;

struct Gateways {
    sockaddr_storage gw6;
    int gw6len = 0;
    sockaddr_storage gw4;
    int gw4len = 0;
};

bool GetGateways(Gateways& out) {
    // 首选：按默认路由确定实际网关。
    // GetAdaptersAddresses 的 FirstGatewayAddress 字段在这类多网卡机器上经常为空，
    // 但路由表里一定有真实的默认网关（上网网卡上的网关）。
    SOCKADDR_INET dest4 = {};
    dest4.si_family = AF_INET;
    dest4.Ipv4.sin_addr.s_addr = htonl(INADDR_ANY);
    MIB_IPFORWARD_ROW2 r4 = {};
    SOCKADDR_INET src4 = {};
    if (GetBestRoute2(NULL, 0, NULL, &dest4, 0, &r4, &src4) == NO_ERROR &&
        r4.NextHop.si_family == AF_INET) {
        std::memset(&out.gw4, 0, sizeof(out.gw4));
        std::memcpy(&out.gw4, &r4.NextHop, sizeof(sockaddr_in));
        out.gw4len = sizeof(sockaddr_in);
    }
    SOCKADDR_INET dest6 = {};
    dest6.si_family = AF_INET6;
    dest6.Ipv6.sin6_addr = in6addr_any;
    MIB_IPFORWARD_ROW2 r6 = {};
    SOCKADDR_INET src6 = {};
    if (GetBestRoute2(NULL, 0, NULL, &dest6, 0, &r6, &src6) == NO_ERROR &&
        r6.NextHop.si_family == AF_INET6) {
        std::memset(&out.gw6, 0, sizeof(out.gw6));
        std::memcpy(&out.gw6, &r6.NextHop, sizeof(sockaddr_in6));
        ((sockaddr_in6*)&out.gw6)->sin6_scope_id = r6.InterfaceIndex;
        out.gw6len = sizeof(sockaddr_in6);
    }
    if (out.gw4len > 0 || out.gw6len > 0) return true;

    // 兜底：GetAdaptersAddresses
    ULONG size = 16384;
    std::vector<BYTE> buf(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    IP_ADAPTER_ADDRESSES* addrs = (IP_ADAPTER_ADDRESSES*)buf.data();
    ULONG r = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, addrs, &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        addrs = (IP_ADAPTER_ADDRESSES*)buf.data();
        r = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, addrs, &size);
    }
    if (r != NO_ERROR) return false;
    for (IP_ADAPTER_ADDRESSES* a = addrs; a != nullptr; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->FirstGatewayAddress == nullptr) continue;
        sockaddr* g = a->FirstGatewayAddress->Address.lpSockaddr;
        if (g->sa_family == AF_INET6 && out.gw6len == 0) {
            std::memset(&out.gw6, 0, sizeof(out.gw6));
            std::memcpy(&out.gw6, g, a->FirstGatewayAddress->Address.iSockaddrLength);
            out.gw6len = (int)a->FirstGatewayAddress->Address.iSockaddrLength;
        }
        else if (g->sa_family == AF_INET && out.gw4len == 0) {
            std::memset(&out.gw4, 0, sizeof(out.gw4));
            std::memcpy(&out.gw4, g, a->FirstGatewayAddress->Address.iSockaddrLength);
            out.gw4len = (int)a->FirstGatewayAddress->Address.iSockaddrLength;
        }
    }
    return out.gw6len > 0 || out.gw4len > 0;
}

// 构建 PCP MAP 请求（60 字节）
std::vector<uint8_t> BuildMapRequest(const uint8_t nonce[12], int port, int proto, uint32_t lifetime) {
    std::vector<uint8_t> msg(60, 0);
    msg[0] = 2;            // Version
    msg[1] = 1;            // R=0, Opcode=MAP(1)
    msg[4] = (uint8_t)((lifetime >> 24) & 0xff);
    msg[5] = (uint8_t)((lifetime >> 16) & 0xff);
    msg[6] = (uint8_t)((lifetime >> 8) & 0xff);
    msg[7] = (uint8_t)(lifetime & 0xff);
    // Client IP = ::（由服务器按源地址处理）
    std::memcpy(&msg[24], nonce, 12);
    msg[36] = (uint8_t)proto;
    msg[40] = (uint8_t)((port >> 8) & 0xff);
    msg[41] = (uint8_t)(port & 0xff);
    msg[42] = (uint8_t)((port >> 8) & 0xff);
    msg[43] = (uint8_t)(port & 0xff);
    return msg;
}

// 发送请求并等待响应；返回结果码（0=成功，-1=超时/失败）
int SendPcpRequest(int family, const sockaddr_storage& target, int targetLen,
                   const std::vector<uint8_t>& msg, uint32_t& outLifetime) {
    SOCKET s = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return -1;
    DWORD to = 2500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    int sent = sendto(s, (const char*)msg.data(), (int)msg.size(), 0,
        (const sockaddr*)&target, targetLen);
    if (sent == SOCKET_ERROR) {
        closesocket(s);
        return -1;
    }
    uint8_t resp[128] = { 0 };
    sockaddr_storage from = {};
    int fromLen = sizeof(from);
    int n = recvfrom(s, (char*)resp, sizeof(resp), 0, (sockaddr*)&from, &fromLen);
    closesocket(s);
    if (n < 24) return -1;
    if (resp[0] != 2) return -1;
    int result = ((int)resp[2] << 8) | resp[3];
    outLifetime = ((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16)
        | ((uint32_t)resp[6] << 8) | resp[7];
    return result;
}

void SendPcpDeleteOnly(int family, const sockaddr_storage& target, int targetLen,
                       int port, int proto, const uint8_t nonce[12]) {
    SOCKET s = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    auto msg = BuildMapRequest(nonce, port, proto, 0); // lifetime=0 即删除
    sendto(s, (const char*)msg.data(), (int)msg.size(), 0,
        (const sockaddr*)&target, targetLen);
    closesocket(s);
}

bool MapTarget(const sockaddr_storage& target, int targetLen, int family,
               int port, uint32_t lifetime, bool isDelete) {
    bool anyOk = false;
    for (int proto : { 6, 17 }) {
        uint8_t nonce[12] = { 0 };
        if (isDelete) {
            const uint8_t* stored = proto == 6 ? g_nonceTcp : g_nonceUdp;
            bool set = proto == 6 ? g_nonceTcpSet : g_nonceUdpSet;
            if (set) std::memcpy(nonce, stored, 12);
            SendPcpDeleteOnly(family, target, targetLen, port, proto, nonce);
            continue;
        }
        std::random_device rd;
        for (int i = 0; i < 12; i++) nonce[i] = (uint8_t)(rd() & 0xff);
        if (proto == 6) {
            std::memcpy(g_nonceTcp, nonce, 12);
            g_nonceTcpSet = true;
        }
        else {
            std::memcpy(g_nonceUdp, nonce, 12);
            g_nonceUdpSet = true;
        }
        auto msg = BuildMapRequest(nonce, port, proto, lifetime);
        uint32_t lt = 0;
        int r = SendPcpRequest(family, target, targetLen, msg, lt);
        if (r == 0) anyOk = true;
    }
    return anyOk;
}

void PcpAttempt() {
    Gateways gw;
    if (!GetGateways(gw)) {
        std::lock_guard<std::mutex> lock(g_pcpMutex);
        g_pcpStatus = "PCP: 未找到默认网关";
        g_pcpOk = false;
        return;
    }
    bool ok = false;
    if (gw.gw6len > 0) {
        if (MapTarget(gw.gw6, gw.gw6len, AF_INET6, g_pcpPort, PCP_LIFETIME, false)) ok = true;
    }
    if (gw.gw4len > 0) {
        if (MapTarget(gw.gw4, gw.gw4len, AF_INET, g_pcpPort, PCP_LIFETIME, false)) ok = true;
    }
    g_pcpOk = ok;
    {
        std::lock_guard<std::mutex> lock(g_pcpMutex);
        g_pcpStatus = ok
            ? ("PCP 已放行 " + std::to_string(g_pcpPort) + " (TCP/UDP)")
            : ("路由器不支持 PCP，请手动放行 " + std::to_string(g_pcpPort) + " 端口");
    }
    if (hMainWnd) PostMessageW(hMainWnd, WM_APP_PCP_UPDATE, 0, 0);
}

} // namespace

void PcpStart(int port) {
    if (g_pcpRunning.exchange(true)) return;
    g_pcpPort = port;
    g_pcpOk = false;
    {
        std::lock_guard<std::mutex> lock(g_pcpMutex);
        g_pcpStatus = "PCP 探测中...";
    }
    std::thread([] {
        PcpAttempt();
        while (g_pcpRunning.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(PCP_REFRESH_MS));
            if (!g_pcpRunning.load()) break;
            PcpAttempt();
        }
    }).detach();
}

void PcpStop() {
    g_pcpRunning = false;
    // 尽力删除放行（不等待响应；即使失败，映射到期后也会自动失效）
    Gateways gw;
    if (GetGateways(gw)) {
        if (gw.gw6len > 0) MapTarget(gw.gw6, gw.gw6len, AF_INET6, g_pcpPort, 0, true);
        if (gw.gw4len > 0) MapTarget(gw.gw4, gw.gw4len, AF_INET, g_pcpPort, 0, true);
    }
    g_pcpOk = false;
    {
        std::lock_guard<std::mutex> lock(g_pcpMutex);
        g_pcpStatus.clear();
    }
}

std::string PcpStatusText() {
    std::lock_guard<std::mutex> lock(g_pcpMutex);
    return g_pcpStatus;
}
