/********************************************************************
 * MSFS-SimConnect 电脑端 - PCP (Port Control Protocol, RFC 6887)
 * 向默认网关的 PCP 服务器(5351)发送 MAP 请求放行入站端口
 *
 * 2026-08-09 修复：
 *  - PCP Client's IP Address 字段必须等于请求的源 IP（RFC 6887 §8.1），
 *    否则路由器返回 ADDRESS_MISMATCH(12)；
 *  - 发送前 bind 固定源地址，避免系统选到链路本地或已废弃前缀地址；
 *  - 统一用 GetBestIPv6Address() 选择"默认路由接口上 Preferred 的全局
 *    IPv6 地址"（EUI-64 优先），二维码与 PCP 使用同一个地址；
 *  - 只放行 UDP（手机端用 UDP 36666，不需要 TCP）；
 *  - 续期失败（如 NO_RESOURCES）时更换 nonce 重试。
 ********************************************************************/

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
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
const uint32_t PCP_LIFETIME = 3600;   // 放行有效期（秒），客户端定期续期
const int PCP_REFRESH_MS = 300000;    // 每 5 分钟续期一次
const int PCP_PROTO_UDP = 17;         // 只放行 UDP（手机端用 UDP）

std::atomic<bool> g_pcpRunning{ false };
std::atomic<bool> g_pcpOk{ false };
std::mutex g_pcpMutex;
std::string g_pcpStatus;
int g_pcpPort = 36666;

// 每个地址族各自维护 nonce（映射身份，删除时必须一致）
struct NonceState {
    uint8_t data[12] = { 0 };
    bool set = false;
};
NonceState g_nonce6;
NonceState g_nonce4;

// PCP 映射身份 = "PCP MAP <nonce>"（miniupnpd 用 nonce 派生描述）。
// 程序重启后若旧映射仍在路由器上，必须沿用同一 nonce 才能续期/删除，
// 因此把 nonce 持久化到本地文件。
std::string NonceFilePath() {
    const char* la = getenv("LOCALAPPDATA");
    std::string dir = (la && la[0]) ? std::string(la) : std::string(".");
    dir += "\\MSFS-SimConnect";
    return dir + "\\pcp_state.dat";
}

void SaveNonceState(int port) {
    try {
        std::string path = NonceFilePath();
        size_t pos = path.find_last_of("\\/");
        if (pos != std::string::npos)
            CreateDirectoryA(path.substr(0, pos).c_str(), NULL);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write("PCP1", 4);
        uint32_t p = (uint32_t)port;
        f.write((const char*)&p, 4);
        f.write((const char*)g_nonce6.data, 12);
        f.write((const char*)g_nonce4.data, 12);
    } catch (...) {}
}

void LoadNonceState(int port) {
    try {
        std::ifstream f(NonceFilePath(), std::ios::binary);
        if (!f) return;
        char magic[4] = { 0 };
        f.read(magic, 4);
        if (std::memcmp(magic, "PCP1", 4) != 0) return;
        uint32_t p = 0;
        f.read((char*)&p, 4);
        if (p != (uint32_t)port) return;   // 端口变了则放弃旧 nonce
        f.read((char*)g_nonce6.data, 12);
        f.read((char*)g_nonce4.data, 12);
        g_nonce6.set = true;
        g_nonce4.set = true;
    } catch (...) {}
}

struct Gateways {
    sockaddr_storage gw6;
    int gw6len = 0;
    sockaddr_storage gw4;
    int gw4len = 0;
    sockaddr_storage src6;   // GetBestRoute2 给出的 IPv6 源地址
    sockaddr_storage src4;   // GetBestRoute2 给出的 IPv4 源地址
};

bool GetGateways(Gateways& out) {
    // 首选：按默认路由确定实际网关。
    // GetAdaptersAddresses 的 FirstGatewayAddress 字段在多网卡机器上经常为空，
    // 但路由表里一定有真实的默认网关。
    SOCKADDR_INET dest4 = {};
    dest4.si_family = AF_INET;
    dest4.Ipv4.sin_addr.s_addr = htonl(INADDR_ANY);
    MIB_IPFORWARD_ROW2 r4 = {};
    SOCKADDR_INET src4 = {};
    if (GetBestRoute2(NULL, 0, NULL, &dest4, 0, &r4, &src4) == NO_ERROR &&
        r4.NextHop.si_family == AF_INET) {
        std::memset(&out.gw4, 0, sizeof(out.gw4));
        std::memcpy(&out.gw4, &r4.NextHop, sizeof(sockaddr_in));
        ((sockaddr_in*)&out.gw4)->sin_port = htons((u_short)PCP_SERVER_PORT);
        out.gw4len = sizeof(sockaddr_in);
        if (src4.si_family == AF_INET) {
            std::memset(&out.src4, 0, sizeof(out.src4));
            std::memcpy(&out.src4, &src4, sizeof(sockaddr_in));
        }
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
        ((sockaddr_in6*)&out.gw6)->sin6_port = htons((u_short)PCP_SERVER_PORT);
        ((sockaddr_in6*)&out.gw6)->sin6_scope_id = r6.InterfaceIndex;
        out.gw6len = sizeof(sockaddr_in6);
        if (src6.si_family == AF_INET6) {
            std::memset(&out.src6, 0, sizeof(out.src6));
            std::memcpy(&out.src6, &src6, sizeof(sockaddr_in6));
        }
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
            ((sockaddr_in6*)&out.gw6)->sin6_port = htons((u_short)PCP_SERVER_PORT);
            out.gw6len = (int)a->FirstGatewayAddress->Address.iSockaddrLength;
        }
        else if (g->sa_family == AF_INET && out.gw4len == 0) {
            std::memset(&out.gw4, 0, sizeof(out.gw4));
            std::memcpy(&out.gw4, g, a->FirstGatewayAddress->Address.iSockaddrLength);
            ((sockaddr_in*)&out.gw4)->sin_port = htons((u_short)PCP_SERVER_PORT);
            out.gw4len = (int)a->FirstGatewayAddress->Address.iSockaddrLength;
        }
    }
    return out.gw6len > 0 || out.gw4len > 0;
}

// 绑定 socket 到指定源地址（保证 PCP 请求源 IP 与 Client IP 字段一致）
bool BindToSource(SOCKET s, int family, const sockaddr_storage& src) {
    if (src.ss_family != family) return false;
    if (family == AF_INET6) {
        sockaddr_in6 local = *(const sockaddr_in6*)&src;
        local.sin6_port = 0;
        if (IN6_IS_ADDR_LINKLOCAL(&local.sin6_addr) && local.sin6_scope_id == 0)
            return false;   // 不把链路本地作为 PCP 源地址
        return bind(s, (sockaddr*)&local, sizeof(local)) == 0;
    }
    sockaddr_in local = *(const sockaddr_in*)&src;
    local.sin_port = 0;
    return bind(s, (sockaddr*)&local, sizeof(local)) == 0;
}

// 构建 PCP MAP 请求（60 字节）；lifetime=0 表示删除
std::vector<uint8_t> BuildMapRequest(const uint8_t nonce[12], int port, int proto,
                                     uint32_t lifetime, const sockaddr_storage& src) {
    std::vector<uint8_t> msg(60, 0);
    msg[0] = 2;            // Version
    msg[1] = 1;            // R=0, Opcode=MAP(1)
    msg[4] = (uint8_t)((lifetime >> 24) & 0xff);
    msg[5] = (uint8_t)((lifetime >> 16) & 0xff);
    msg[6] = (uint8_t)((lifetime >> 8) & 0xff);
    msg[7] = (uint8_t)(lifetime & 0xff);
    // PCP Client's IP Address（RFC 6887：必须等于请求的源 IP）
    if (src.ss_family == AF_INET6) {
        const sockaddr_in6* s6 = (const sockaddr_in6*)&src;
        std::memcpy(&msg[8], &s6->sin6_addr, 16);
    }
    else if (src.ss_family == AF_INET) {
        const sockaddr_in* s4 = (const sockaddr_in*)&src;
        std::memset(&msg[8], 0, 10);
        msg[18] = 0xFF;
        msg[19] = 0xFF;
        std::memcpy(&msg[20], &s4->sin_addr, 4);   // IPv4-mapped IPv6
    }
    std::memcpy(&msg[24], nonce, 12);
    msg[36] = (uint8_t)proto;
    msg[40] = (uint8_t)((port >> 8) & 0xff);
    msg[41] = (uint8_t)(port & 0xff);
    msg[42] = (uint8_t)((port >> 8) & 0xff);
    msg[43] = (uint8_t)(port & 0xff);
    return msg;
}

struct PcpResult {
    int result = -1;       // -1=网络/超时；>=0 为 PCP 错误码（0=SUCCESS）
    uint32_t lifetime = 0;
    int assignedPort = 0;
    std::string extIp;
};

// 发送请求并等待响应（timeoutMs 可调；删除用短超时避免界面卡顿）
PcpResult SendPcpRequest(int family, const sockaddr_storage& target, int targetLen,
                         const sockaddr_storage& src, const std::vector<uint8_t>& msg,
                         int timeoutMs = 2500) {
    PcpResult r;
    SOCKET s = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return r;
    if (src.ss_family != 0 && !BindToSource(s, family, src)) {
        closesocket(s);
        return r;
    }
    DWORD to = (DWORD)timeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    int sent = sendto(s, (const char*)msg.data(), (int)msg.size(), 0,
                      (const sockaddr*)&target, targetLen);
    if (sent == SOCKET_ERROR) {
        closesocket(s);
        return r;
    }
    uint8_t resp[128] = { 0 };
    sockaddr_storage from = {};
    int fromLen = sizeof(from);
    int n = recvfrom(s, (char*)resp, sizeof(resp), 0, (sockaddr*)&from, &fromLen);
    closesocket(s);
    if (n < 24) return r;
    if (resp[0] != 2) return r;          // 版本
    if (!(resp[1] & 0x80)) return r;     // R 位必须为 1（响应）
    r.result = ((int)resp[2] << 8) | resp[3];
    r.lifetime = ((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16)
        | ((uint32_t)resp[6] << 8) | resp[7];
    if (n >= 44)
        r.assignedPort = ((int)resp[42] << 8) | resp[43];
    if (r.result == 0 && n >= 60) {
        char buf[INET6_ADDRSTRLEN] = { 0 };
        if (inet_ntop(AF_INET6, resp + 44, buf, sizeof(buf)))
            r.extIp = buf;
    }
    return r;
}

// 删除映射（lifetime=0），等待响应，最多 2 次
PcpResult DeleteMapping(int family, const sockaddr_storage& target, int targetLen,
                        const sockaddr_storage& src, int port, int proto,
                        const uint8_t nonce[12]) {
    PcpResult last;
    for (int i = 0; i < 2; i++) {
        auto msg = BuildMapRequest(nonce, port, proto, 0, src);
        last = SendPcpRequest(family, target, targetLen, src, msg, 800);
        if (last.result == 0) break;
        Sleep(100);
    }
    return last;
}

const char* ResultText(int code) {
    switch (code) {
    case 0:  return "SUCCESS";
    case 1:  return "UNSUPP_VERSION";
    case 2:  return "NOT_AUTHORIZED";
    case 3:  return "MALFORMED_REQUEST";
    case 4:  return "UNSUPP_OPCODE";
    case 5:  return "UNSUPP_OPTION";
    case 6:  return "MALFORMED_OPTION";
    case 7:  return "NETWORK_FAILURE";
    case 8:  return "NO_RESOURCES";
    case 9:  return "UNSUPP_PROTOCOL";
    case 10: return "USER_EX_QUOTA";
    case 11: return "CANNOT_PROVIDE_EXTERNAL";
    case 12: return "ADDRESS_MISMATCH";
    case 13: return "EXCESSIVE_REMOTE_PEERS";
    default: return "网络超时/错误";
    }
}

// 对一个地址族执行 MAP 创建/续期；成功保留 nonce，失败换新 nonce 重试
bool MapTarget(const sockaddr_storage& target, int targetLen, int family,
               const sockaddr_storage& src, int port, NonceState& nonceState,
               PcpResult& lastResult) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!g_pcpRunning.load()) return false;   // 已停止则不再发送，避免删除后被重建
        if (!nonceState.set) {
            std::random_device rd;
            for (int i = 0; i < 12; i++) nonceState.data[i] = (uint8_t)(rd() & 0xff);
            nonceState.set = true;
        }
        auto msg = BuildMapRequest(nonceState.data, port, PCP_PROTO_UDP, PCP_LIFETIME, src);
        lastResult = SendPcpRequest(family, target, targetLen, src, msg);
        if (lastResult.result == 0) {
            SaveNonceState(port);
            return true;
        }
        nonceState.set = false;   // 失败则下次换新 nonce（实测同 nonce 续期可能 NO_RESOURCES）
    }
    return false;
}

void SetStatus(const std::string& text, bool ok) {
    std::lock_guard<std::mutex> lock(g_pcpMutex);
    g_pcpStatus = text;
    g_pcpOk = ok;
}

void PcpAttempt() {
    if (!g_pcpRunning.load()) return;
    Gateways gw;
    if (!GetGateways(gw)) {
        SetStatus("PCP: 未找到默认网关", false);
        return;
    }

    bool ok = false;
    std::string detail;
    PcpResult last6, last4;

    // IPv6：源地址与二维码统一（默认路由接口上 Preferred 的全局地址）
    if (gw.gw6len > 0) {
        sockaddr_storage src6 = gw.src6;
        std::string best = GetBestIPv6Address();
        if (!best.empty()) {
            sockaddr_in6 s6 = {};
            s6.sin6_family = AF_INET6;
            if (inet_pton(AF_INET6, best.c_str(), &s6.sin6_addr) == 1) {
                std::memset(&src6, 0, sizeof(src6));
                std::memcpy(&src6, &s6, sizeof(s6));
            }
        }
        if (src6.ss_family == AF_INET6) {
            if (MapTarget(gw.gw6, gw.gw6len, AF_INET6, src6, g_pcpPort, g_nonce6, last6)) {
                ok = true;
                detail = "PCP已放行 " + std::to_string(g_pcpPort) + "(UDP)";
            }
            else {
                detail = "PCP IPv6 映射失败(" + std::string(ResultText(last6.result)) + ")";
                if (last6.result == 2)
                    detail += "，路由器上已有同端口映射（可能由旧进程创建），将等待其过期后重试";
            }
        }
        else {
            detail = "PCP: 未找到可用公网 IPv6 地址";
        }
    }

    // IPv4：仅当存在默认 IPv4 路由时尝试（手机走 IPv6，IPv4 属附加能力）
    if (gw.gw4len > 0 && gw.src4.ss_family == AF_INET) {
        if (MapTarget(gw.gw4, gw.gw4len, AF_INET, gw.src4, g_pcpPort, g_nonce4, last4)) {
            ok = true;
            if (detail.find("已放行") == std::string::npos)
                detail = "PCP已放行 " + std::to_string(g_pcpPort) + "(UDP)";
        }
        else if (detail.empty()) {
            detail = "PCP IPv4 映射失败(" + std::string(ResultText(last4.result)) + ")";
            if (last4.result == 2)
                detail += "，路由器上已有同端口映射（可能由旧进程创建），将等待其过期后重试";
        }
    }

    g_pcpOk = ok;
    if (!ok && detail.empty())
        detail = "路由器不支持 PCP，请手动放行 " + std::to_string(g_pcpPort) + " 端口";
    if (!ok && detail.find("请手动放行") == std::string::npos)
        detail += "，请手动放行 " + std::to_string(g_pcpPort) + " 端口";
    if (!g_pcpRunning.load()) return;   // 已停止：不覆盖 PcpStop 清空的状态
    SetStatus(detail, ok);
    if (hMainWnd) PostMessageW(hMainWnd, WM_APP_PCP_UPDATE, 0, 0);
}

} // namespace

// 选择用于 PCP / 二维码的公网 IPv6 源地址
std::string GetBestIPv6Address() {
    // 默认路由接口索引（尽量与 PCP 网关所在接口一致）
    int defaultIfIndex = -1;
    {
        SOCKADDR_INET dest6 = {};
        dest6.si_family = AF_INET6;
        dest6.Ipv6.sin6_addr = in6addr_any;
        MIB_IPFORWARD_ROW2 r6 = {};
        SOCKADDR_INET src6 = {};
        if (GetBestRoute2(NULL, 0, NULL, &dest6, 0, &r6, &src6) == NO_ERROR &&
            r6.NextHop.si_family == AF_INET6)
            defaultIfIndex = (int)r6.InterfaceIndex;
    }

    ULONG size = 0;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    std::vector<BYTE> buf;
    ULONG r = GetAdaptersAddresses(AF_INET6, flags, NULL, NULL, &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_INET6, flags, NULL, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    }
    if (r != NO_ERROR) return "";

    struct Cand {
        std::string ip;
        bool preferred = false;   // 非 deprecated（还有 Preferred 剩余时间）
        bool onDefault = false;   // 位于默认路由接口
        bool eui64 = false;       // 含 ff:fe 的 EUI-64 稳定地址
    };
    std::vector<Cand> cands;
    for (IP_ADAPTER_ADDRESSES* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        bool onDefault = (defaultIfIndex >= 0 && (int)a->IfIndex == defaultIfIndex);
        for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET6) continue;
            sockaddr_in6* s6 = (sockaddr_in6*)u->Address.lpSockaddr;
            const unsigned char* b = s6->sin6_addr.s6_addr;
            if (IN6_IS_ADDR_LOOPBACK(&s6->sin6_addr)) continue;
            if (IN6_IS_ADDR_LINKLOCAL(&s6->sin6_addr)) continue;
            if (IN6_IS_ADDR_MULTICAST(&s6->sin6_addr)) continue;
            if ((b[0] & 0xfe) == 0xfc) continue;      // fc00::/7 ULA
            if ((b[0] & 0xe0) != 0x20) continue;      // 只保留 2000::/3
            ULONG valid = u->ValidLifetime;
            if (valid != 0xFFFFFFFF && valid == 0) continue;   // 已失效
            bool preferred = (u->PreferredLifetime == 0xFFFFFFFF || u->PreferredLifetime > 0);
            char buf2[INET6_ADDRSTRLEN] = { 0 };
            inet_ntop(AF_INET6, &s6->sin6_addr, buf2, sizeof(buf2));
            std::string ip(buf2);
            cands.push_back({ ip, preferred, onDefault,
                              ip.find("ff:fe") != std::string::npos });
        }
    }
    if (cands.empty()) return "";

    // 评分：Preferred > 默认路由接口 > EUI-64，避免选到已废弃前缀
    auto score = [](const Cand& c) {
        int s = 0;
        if (c.preferred) s += 4;
        if (c.onDefault) s += 2;
        if (c.eui64)     s += 1;
        return s;
    };
    std::stable_sort(cands.begin(), cands.end(),
                     [&](const Cand& x, const Cand& y) { return score(x) > score(y); });
    return cands.front().ip;
}

void PcpStart(int port) {
    if (g_pcpRunning.exchange(true)) return;
    g_pcpPort = port;
    LoadNonceState(port);   // 重启后沿用上次的 nonce，可继续续期/删除旧映射
    g_pcpOk = false;
    SetStatus("PCP 探测中...", false);
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
    Sleep(50);   // 给工作线程一点时间跳出循环，避免删除后又被重建
    uint8_t zeroNonce[12] = { 0 };
    std::string detail;
    bool allOk = true;
    Gateways gw;
    if (!GetGateways(gw)) {
        detail = "PCP: 未找到默认网关，无法删除映射";
        allOk = false;
    }
    else {
        // 删除结果：0=成功；8(NO_RESOURCES)=映射已不存在（可能已过期），视为成功
        auto delOk = [](const PcpResult& r) { return r.result == 0 || r.result == 8; };

        // IPv6：删除 UDP pinhole（本程序只创建 UDP）
        if (gw.gw6len > 0) {
            sockaddr_storage src6 = gw.src6;
            std::string best = GetBestIPv6Address();
            if (!best.empty()) {
                sockaddr_in6 s6 = {};
                s6.sin6_family = AF_INET6;
                if (inet_pton(AF_INET6, best.c_str(), &s6.sin6_addr) == 1) {
                    std::memset(&src6, 0, sizeof(src6));
                    std::memcpy(&src6, &s6, sizeof(s6));
                }
            }
            if (src6.ss_family == AF_INET6) {
                const uint8_t* nonce = g_nonce6.set ? g_nonce6.data : zeroNonce;
                PcpResult r = DeleteMapping(AF_INET6, gw.gw6, gw.gw6len, src6,
                                            g_pcpPort, PCP_PROTO_UDP, nonce);
                if (!delOk(r)) {
                    allOk = false;
                    detail = "IPv6 删除失败(" + std::string(ResultText(r.result)) + ")";
                }
            }
        }
        // IPv4：仅在本程序确实建过 IPv4 映射时删除 UDP 与 TCP
        if (g_nonce4.set && gw.gw4len > 0 && gw.src4.ss_family == AF_INET) {
            const uint8_t* nonce4 = g_nonce4.set ? g_nonce4.data : zeroNonce;
            for (int proto : { PCP_PROTO_UDP, 6 }) {   // 17=UDP, 6=TCP
                PcpResult r = DeleteMapping(AF_INET, gw.gw4, gw.gw4len, gw.src4,
                                            g_pcpPort, proto, nonce4);
                if (!delOk(r)) {
                    allOk = false;
                    if (detail.empty())
                        detail = "IPv4 删除失败(" + std::string(ResultText(r.result)) + ")";
                }
            }
        }
    }
    // main.cpp 在调用 PcpStop 前总是先调用 UpnpStop()，因此成功时统一提示两者已移除
    if (allOk)
        detail = "PCP和UPnP规则已移除";
    g_pcpOk = allOk;
    SetStatus(detail, allOk);
    if (hMainWnd) PostMessageW(hMainWnd, WM_APP_PCP_UPDATE, 0, 0);
}

std::string PcpStatusText() {
    std::lock_guard<std::mutex> lock(g_pcpMutex);
    return g_pcpStatus;
}
