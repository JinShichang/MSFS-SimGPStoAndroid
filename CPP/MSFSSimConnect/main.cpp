/********************************************************************
 * MSFS-SimGPStoAndroid 电脑端
 * 时间: 2025-12-05
 * 本软件遵循 CC BY-NC-SA 4.0 协议，不得用于商业用途！
 ********************************************************************/

#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <MSWSock.h>
#include <dwmapi.h>
#include <SimConnect.h>
#include <shellscalingapi.h>

#include <thread>
#include <string>
#include <cstring>
#include <sstream>
#include <random>
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>
#include <algorithm>
#include <iostream>
#include "./ThirdParty/qrcodegen/qrcodegen.hpp"
#include "upnp_mapping.h"
#include "pcp_mapping.h"
#include "resource.h"

#define IDC_EDIT_INTERVAL 1001
#define IDC_BTN_APPLY     1002
#define IDC_CHK_PUBLIC    1003
#define IDC_STATIC_INFO   1004
#define IDC_LABEL_INTERVAL 1005
#define IDC_LABEL_PORT    1006
#define IDC_EDIT_PORT     1007
#define IDC_BTN_APPLY_PORT 1008
#define WM_APP_PCP_UPDATE (WM_APP + 3)   // 与 pcp_mapping.cpp 中的定义保持一致

#pragma comment(lib, "SimConnect.lib")
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Shcore.lib")

#pragma warning(disable : 28251)

 // -------------------- 全局 --------------------
HANDLE hSimConnect = NULL;
HWND hMainWnd = NULL;
HICON hIconGlobal = NULL;
HFONT g_fontUi = NULL;
std::atomic<bool> running{ true };
std::mutex sockMutex;

// UDP 客户端信息（受 sockMutex 保护）
const int MAX_CLIENTS = 3;
struct ClientInfo {
    sockaddr_storage addr;
    int addrLen = 0;
    long long lastHelloMs = 0;
    long long lastPhoneTs = 0; // 手机端 HELLO 携带的时间戳，PONG 回显用于测延迟
};
ClientInfo g_clients[MAX_CLIENTS];
std::atomic<int> g_clientCount{ 0 };

SOCKET g_udpSock = INVALID_SOCKET;

// 数据刷新间隔（毫秒），默认 50，电脑端界面可调
std::atomic<int> g_refreshIntervalMs{ 50 };
// 监听端口（可自定义，默认 36666）
std::atomic<int> g_port{ 36666 };
// UDP 服务器代数，端口变更时用于让旧监听线程退出
std::atomic<int> g_serverGen{ 0 };
// 公网使用开关（IPv6 显示 + 二维码 IPv6 + UPnP 映射）
std::atomic<bool> g_publicMode{ false };

struct SIMDATA {
    double latitude;
    double longitude;
    double altitude;
    double heading;
    double pitch;
    double roll;
    double gpsGroundSpeed;
    double indicatedAirspeed;
};

SIMDATA g_data = { 0 };
bool g_darkMode = false;
bool g_simConnected = false;
std::vector<std::vector<bool>> qrMatrix;

// 心跳
const int HEARTBEAT_INTERVAL_MS = 2000;
const int CLIENT_TIMEOUT_MS = 15000;

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// -------------------- 工具 --------------------
void EnableHighDPI() {
    HMODULE hShcore = LoadLibraryA("Shcore.dll");
    if (hShcore) {
        typedef HRESULT(WINAPI* SetProcessDpiAwareness_t)(PROCESS_DPI_AWARENESS);
        SetProcessDpiAwareness_t func = (SetProcessDpiAwareness_t)GetProcAddress(hShcore, "SetProcessDpiAwareness");
        if (func) func(PROCESS_PER_MONITOR_DPI_AWARE);
        FreeLibrary(hShcore);
    }
    SetProcessDPIAware();
}

std::string GetLocalIP() {
    std::string ip = "127.0.0.1";
    char hostname[256] = { 0 };
    if (gethostname(hostname, sizeof(hostname)) != 0) return ip;

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(hostname, NULL, &hints, &res) == 0) {
        for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
            struct sockaddr_in* addr = (struct sockaddr_in*)p->ai_addr;
            char buf[INET_ADDRSTRLEN] = { 0 };
            inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf));
            std::string cand = buf;
            // 优先取 192 开头的内网 IP
            if (cand.rfind("192.", 0) == 0) { ip = cand; break; }
            if (ip == "127.0.0.1") ip = cand;
        }
        freeaddrinfo(res);
    }
    return ip;
}

// 获取公网（全局单播）IPv6 地址：默认路由接口上 Preferred 的地址，
// EUI-64（含 ff:fe）优先；与 PCP 放行使用同一个地址，避免二维码指向已废弃前缀。
std::string GetPublicIPv6() {
    return GetBestIPv6Address();
}

// 以下客户端操作均在 sockMutex 保护下调用
int FindClient(const sockaddr_storage& addr, int len) {
    int n = g_clientCount.load();
    for (int i = 0; i < n; i++) {
        if (g_clients[i].addrLen == len &&
            std::memcmp(&g_clients[i].addr, &addr, len) == 0) {
            return i;
        }
    }
    return -1;
}

void RemoveClient(int idx) {
    int n = g_clientCount.load();
    if (idx < 0 || idx >= n) return;
    for (int i = idx; i < n - 1; i++) g_clients[i] = g_clients[i + 1];
    g_clients[n - 1] = ClientInfo();
    g_clientCount--;
}

void UpdateWindowTitle() {
    int n = g_clientCount.load();
    std::string status = n > 0 ? ("已连接 " + std::to_string(n) + " 台手机") : "未连接";
    std::string title = "MSFS-SimConnect 电脑端 (" + status + ")";
    SetWindowTextA(hMainWnd, title.c_str());
}

void DetectSystemTheme() {
    DWORD value = 0, size = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD, NULL, &value, &size) == ERROR_SUCCESS) {
        g_darkMode = (value == 0);
    }
}

void GenerateQRCode(const std::string& text) {
    using namespace qrcodegen;
    QrCode qr = QrCode::encodeText(text.c_str(), QrCode::Ecc::MEDIUM);
    int size = qr.getSize();
    qrMatrix.clear();
    qrMatrix.resize(size, std::vector<bool>(size, false));
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            qrMatrix[y][x] = qr.getModule(x, y);
}

// 根据当前模式（局域网 IPv4 / 公网 IPv6）更新二维码内容
void UpdateQRCode() {
    std::string ipv6 = GetPublicIPv6();
    std::string qrText;
    if (g_publicMode.load() && !ipv6.empty()) {
        qrText = "[" + ipv6 + "]:" + std::to_string(g_port.load());
    } else {
        qrText = GetLocalIP() + ":" + std::to_string(g_port.load());
    }
    GenerateQRCode(qrText);
}

// 刷新公网状态栏：IPv6 地址 + UPnP + PCP 状态
void UpdatePublicInfo(HWND hwnd) {
    if (!hwnd) return;
    std::string ipv6 = GetPublicIPv6();
    std::string info = ipv6.empty() ? "未检测到公网 IPv6" : ("公网IPv6 " + ipv6);
    std::string upnp = UpnpStatusText();
    std::string pcp = PcpStatusText();
    if (!upnp.empty()) info += " · " + upnp;
    if (!pcp.empty()) info += " · " + pcp;
    SetDlgItemTextA(hwnd, IDC_STATIC_INFO, info.c_str());
}

// -------------------- SimConnect --------------------
void CALLBACK SimDispatch(SIMCONNECT_RECV* pData, DWORD cbData, void* pContext) {
    if (!pData) return;
    switch (pData->dwID) {
    case SIMCONNECT_RECV_ID_SIMOBJECT_DATA: {
        SIMCONNECT_RECV_SIMOBJECT_DATA* obj = (SIMCONNECT_RECV_SIMOBJECT_DATA*)pData;
        SIMDATA* d = (SIMDATA*)&obj->dwData;
        if (d) {
            g_data = *d;

            std::lock_guard<std::mutex> lock(sockMutex);
            static long long lastSendMs = 0; // 按电脑端设置的刷新间隔限流发送
            if (g_clientCount.load() > 0
                && (NowMs() - lastSendMs >= g_refreshIntervalMs.load())) {
                lastSendMs = NowMs();
                char msg[256];
                int written = sprintf_s(msg, sizeof(msg),
                    "%.6f,%.6f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
                    g_data.latitude, g_data.longitude, g_data.altitude,
                    g_data.heading, g_data.pitch, g_data.roll,
                    g_data.gpsGroundSpeed, g_data.indicatedAirspeed);
                if (written > 0) {
                    // 向所有已连接手机推送
                    int n = g_clientCount.load();
                    for (int i = 0; i < n; i++) {
                        sendto(g_udpSock, msg, (int)strlen(msg), 0,
                            (sockaddr*)&g_clients[i].addr, g_clients[i].addrLen);
                    }
                }
            }

            // 限频只重绘右侧数据面板，避免整窗刷新导致底部控件闪烁
            static long long lastPaintMs = 0;
            long long nowMs = NowMs();
            if (nowMs - lastPaintMs >= 30) {
                lastPaintMs = nowMs;
                RECT rc;
                GetClientRect(hMainWnd, &rc);
                RECT panel = { rc.right - 360, 40, rc.right - 10, rc.bottom - 70 };
                InvalidateRect(hMainWnd, &panel, FALSE);
            }
        }
        break;
    }
    default: break;
    }
}

// -------------------- UDP 服务 --------------------
void UDPServerThread(int port) {
    int myGen = g_serverGen.load();
    SOCKET serverSock = INVALID_SOCKET;

    // 优先尝试 IPv6 双栈（同时收 IPv4 和 IPv6，供公网 IPv6 使用）
    serverSock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (serverSock != INVALID_SOCKET) {
        int v6only = 0;
        setsockopt(serverSock, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof(v6only));
        sockaddr_in6 serverAddr6 = {};
        serverAddr6.sin6_family = AF_INET6;
        serverAddr6.sin6_port = htons((u_short)port);
        serverAddr6.sin6_addr = in6addr_any;
        if (bind(serverSock, (sockaddr*)&serverAddr6, sizeof(serverAddr6)) == SOCKET_ERROR) {
            closesocket(serverSock);
            serverSock = INVALID_SOCKET;
        }
    }

    // IPv6 双栈不可用时回退到 IPv4
    if (serverSock == INVALID_SOCKET) {
        serverSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (serverSock == INVALID_SOCKET) return;
        sockaddr_in serverAddr = {};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons((u_short)port);
        serverAddr.sin_addr.s_addr = INADDR_ANY;
        if (bind(serverSock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            closesocket(serverSock);
            return;
        }
    }

    // 忽略 UDP 的 WSAECONNRESET（Windows 收到 ICMP 端口不可达时会给套接字置错误状态）
    DWORD bytesReturned = 0;
    BOOL newBehavior = FALSE;
    WSAIoctl(serverSock, SIO_UDP_CONNRESET, &newBehavior, sizeof(newBehavior),
        NULL, 0, &bytesReturned, NULL, NULL);

    {
        std::lock_guard<std::mutex> lock(sockMutex);
        if (g_serverGen.load() != myGen) {
            closesocket(serverSock);
            return;
        }
        g_udpSock = serverSock;
    }
    char buf[2048];

    while (running && g_serverGen.load() == myGen) {
        sockaddr_storage from = {};
        int fromLen = sizeof(from);
        int len = recvfrom(serverSock, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromLen);
        if (len == SOCKET_ERROR) {
            if (!running || g_serverGen.load() != myGen) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (len <= 0) continue;
        buf[len] = '\0';
        std::string payload(buf, len);
        // 手机端注册/保活包：HELLO:<时间戳>
        if (payload.rfind("HELLO", 0) != 0) continue;

        long long phoneTs = 0;
        size_t colon = payload.find(':');
        if (colon != std::string::npos) {
            try { phoneTs = std::stoll(payload.substr(colon + 1)); } catch (...) {}
        }
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(sockMutex);
            int idx = FindClient(from, fromLen);
            if (idx >= 0) {
                g_clients[idx].lastHelloMs = NowMs();
                g_clients[idx].lastPhoneTs = phoneTs;
                accepted = true;
            }
            else if (g_clientCount.load() < MAX_CLIENTS) {
                int n = g_clientCount.load();
                g_clients[n].addr = from;
                g_clients[n].addrLen = fromLen;
                g_clients[n].lastHelloMs = NowMs();
                g_clients[n].lastPhoneTs = phoneTs;
                g_clientCount++;
                accepted = true;
            }
        }

        if (accepted) {
            UpdateWindowTitle();
            InvalidateRect(hMainWnd, NULL, TRUE);
            // 立即回 PONG（游戏式延迟测量：收到即回，不等待心跳刻度）
            std::string pong = "PONG:" + std::to_string(phoneTs);
            sendto(serverSock, pong.c_str(), (int)pong.size(), 0, (sockaddr*)&from, fromLen);
        }
        else {
            // 客户端已满：拒绝并提示
            const char* full = "SERVER_FULL";
            sendto(serverSock, full, (int)strlen(full), 0, (sockaddr*)&from, fromLen);
        }
    }
    {
        std::lock_guard<std::mutex> lock(sockMutex);
        if (g_udpSock == serverSock) g_udpSock = INVALID_SOCKET;
    }
    closesocket(serverSock);
}

// 端口变更时重启 UDP 监听
void RestartUDPServer(int port) {
    g_serverGen++;
    {
        std::lock_guard<std::mutex> lock(sockMutex);
        if (g_udpSock != INVALID_SOCKET) {
            closesocket(g_udpSock);
            g_udpSock = INVALID_SOCKET;
        }
    }
    std::thread(UDPServerThread, port).detach();
}

// -------------------- 心跳 --------------------
void HeartbeatThread() {
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(HEARTBEAT_INTERVAL_MS));
        std::lock_guard<std::mutex> lock(sockMutex);
        if (g_clientCount.load() == 0) continue;

        long long now = NowMs();
        // 倒序遍历：长时间收不到 HELLO 的客户端移除；其余发保活心跳
        for (int i = g_clientCount.load() - 1; i >= 0; i--) {
            if (now - g_clients[i].lastHelloMs > CLIENT_TIMEOUT_MS) {
                RemoveClient(i);
                UpdateWindowTitle();
                InvalidateRect(hMainWnd, NULL, TRUE);
                continue;
            }
            std::string hb = "HEARTBEAT";
            sendto(g_udpSock, hb.c_str(), (int)hb.size(), 0,
                (sockaddr*)&g_clients[i].addr, g_clients[i].addrLen);
        }
    }
}

// -------------------- SimConnect 管理 --------------------
void SimConnectManagerThread() {
    while (running) {
        if (!hSimConnect) {
            HRESULT hr = SimConnect_Open(&hSimConnect, "MSFS Sender", NULL, 0, 0, 0);
            if (SUCCEEDED(hr)) {
                g_simConnected = true;
                SimConnect_AddToDataDefinition(hSimConnect, 0, "PLANE LATITUDE", "degrees");
                SimConnect_AddToDataDefinition(hSimConnect, 0, "PLANE LONGITUDE", "degrees");
                SimConnect_AddToDataDefinition(hSimConnect, 0, "PLANE ALTITUDE", "feet");
                SimConnect_AddToDataDefinition(hSimConnect, 0, "PLANE HEADING DEGREES TRUE", "degrees");
                SimConnect_AddToDataDefinition(hSimConnect, 0, "PLANE PITCH DEGREES", "degrees");
                SimConnect_AddToDataDefinition(hSimConnect, 0, "PLANE BANK DEGREES", "degrees");
                SimConnect_AddToDataDefinition(hSimConnect, 0, "GPS GROUND SPEED", "meters per second");
                SimConnect_AddToDataDefinition(hSimConnect, 0, "AIRSPEED INDICATED", "meters per second");

                SimConnect_RequestDataOnSimObject(hSimConnect, 0, 0, SIMCONNECT_OBJECT_ID_USER, SIMCONNECT_PERIOD_SIM_FRAME);
                PostMessage(hMainWnd, WM_APP + 1, 0, 0);
            }
            else g_simConnected = false;
        }

        if (hSimConnect) {
            HRESULT hr = SimConnect_CallDispatch(hSimConnect, SimDispatch, NULL);
            if (FAILED(hr)) {
                SimConnect_Close(hSimConnect);
                hSimConnect = NULL;
                g_simConnected = false;
                PostMessage(hMainWnd, WM_APP + 2, 0, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (hSimConnect) { SimConnect_Close(hSimConnect); hSimConnect = NULL; g_simConnected = false; }
}

// -------------------- 窗口 --------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static bool btnMinHover = false, btnCloseHover = false;
    static bool mouseTrackActive = false;

    switch (msg) {
    case WM_CREATE: {
        hMainWnd = hwnd;
        DetectSystemTheme();
        UpdateQRCode();

        // 底部设置栏
        RECT rc;
        GetClientRect(hwnd, &rc);
        int barY1 = rc.bottom - 62;
        int barY2 = rc.bottom - 34;
        HINSTANCE hInst = GetModuleHandleW(NULL);
        // 第一行：刷新间隔 + 应用、端口 + 应用、公网使用
        CreateWindowExW(0, L"STATIC", L"刷新间隔(ms):", WS_CHILD | WS_VISIBLE,
            10, barY1 + 5, 86, 20, hwnd, (HMENU)IDC_LABEL_INTERVAL, hInst, NULL);
        CreateWindowExW(0, L"EDIT", L"50", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
            100, barY1, 48, 24, hwnd, (HMENU)IDC_EDIT_INTERVAL, hInst, NULL);
        CreateWindowExW(0, L"BUTTON", L"应用", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            152, barY1, 48, 24, hwnd, (HMENU)IDC_BTN_APPLY, hInst, NULL);
        CreateWindowExW(0, L"STATIC", L"端口:", WS_CHILD | WS_VISIBLE,
            212, barY1 + 5, 34, 20, hwnd, (HMENU)IDC_LABEL_PORT, hInst, NULL);
        CreateWindowExW(0, L"EDIT", L"36666", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
            250, barY1, 48, 24, hwnd, (HMENU)IDC_EDIT_PORT, hInst, NULL);
        CreateWindowExW(0, L"BUTTON", L"应用", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            302, barY1, 48, 24, hwnd, (HMENU)IDC_BTN_APPLY_PORT, hInst, NULL);
        CreateWindowExW(0, L"BUTTON", L"公网使用", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            360, barY1 + 5, 130, 20, hwnd, (HMENU)IDC_CHK_PUBLIC, hInst, NULL);
        // 第二行：IPv6 / UPnP 状态整行显示，避免挤占空间
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            10, barY2 + 5, rc.right - 20, 20, hwnd, (HMENU)IDC_STATIC_INFO, hInst, NULL);

        // 底部控件统一使用微软雅黑
        g_fontUi = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"微软雅黑");
        HWND ctrls[] = {
            GetDlgItem(hwnd, IDC_LABEL_INTERVAL),
            GetDlgItem(hwnd, IDC_EDIT_INTERVAL),
            GetDlgItem(hwnd, IDC_LABEL_PORT),
            GetDlgItem(hwnd, IDC_EDIT_PORT),
            GetDlgItem(hwnd, IDC_BTN_APPLY),
            GetDlgItem(hwnd, IDC_BTN_APPLY_PORT),
            GetDlgItem(hwnd, IDC_CHK_PUBLIC),
            GetDlgItem(hwnd, IDC_STATIC_INFO)
        };
        for (HWND c : ctrls) {
            if (c) SendMessageW(c, WM_SETFONT, (WPARAM)g_fontUi, TRUE);
        }

        std::thread(UDPServerThread, g_port.load()).detach();
        std::thread(HeartbeatThread).detach();
        std::thread(SimConnectManagerThread).detach();
        break;
    }

    case WM_APP + 1: case WM_APP + 2:
        InvalidateRect(hwnd, NULL, TRUE);
        break;

    case WM_APP + 3:   // PCP 创建/续期/删除结果刷新（与 pcp_mapping.cpp 的 WM_APP_PCP_UPDATE 一致）
        if (g_publicMode.load())
            UpdatePublicInfo(hwnd);
        else
            SetDlgItemTextA(hwnd, IDC_STATIC_INFO, PcpStatusText().c_str());
        break;

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (HIWORD(wParam) == BN_CLICKED) {
            if (id == IDC_BTN_APPLY) {
                char buf[32] = { 0 };
                GetDlgItemTextA(hwnd, IDC_EDIT_INTERVAL, buf, sizeof(buf));
                int v = atoi(buf);
                if (v < 5) v = 5;
                if (v > 5000) v = 5000;
                g_refreshIntervalMs = v;
                InvalidateRect(hwnd, NULL, TRUE);
            }
            else if (id == IDC_BTN_APPLY_PORT) {
                char buf[32] = { 0 };
                GetDlgItemTextA(hwnd, IDC_EDIT_PORT, buf, sizeof(buf));
                int port = atoi(buf);
                if (port < 1 || port > 65535) port = g_port.load();

                // 端口变更：重启 UDP 监听、更新二维码、公网模式重新 UPnP 映射
                if (port != g_port.load()) {
                    g_port = port;
                    RestartUDPServer(port);
                    if (IsDlgButtonChecked(hwnd, IDC_CHK_PUBLIC) == BST_CHECKED) {
                        UpnpStop();
                        UpnpStart(GetLocalIP(), port);
                        PcpStop();
                        PcpStart(port);
                        UpdatePublicInfo(hwnd);
                    }
                    UpdateQRCode();
                }
                InvalidateRect(hwnd, NULL, TRUE);
            }
            else if (id == IDC_CHK_PUBLIC) {
                bool on = IsDlgButtonChecked(hwnd, IDC_CHK_PUBLIC) == BST_CHECKED;
                g_publicMode = on;
                if (on) {
                    UpnpStart(GetLocalIP(), g_port.load());
                    PcpStart(g_port.load());
                    UpdatePublicInfo(hwnd);
                }
                else {
                    UpnpStop();
                    SetDlgItemTextA(hwnd, IDC_STATIC_INFO, "正在移除 PCP 映射...");
                    UpdateWindow(hwnd);
                    PcpStop();   // 同步等待删除结果，结果通过 WM_APP_PCP_UPDATE 显示
                }
                UpdateQRCode();
                InvalidateRect(hwnd, NULL, TRUE);
            }
        }
        break;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rect;
        GetClientRect(hwnd, &rect);

        // -------------------- 双缓冲 --------------------
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rect.right, rect.bottom);
        HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

        // 背景颜色
        COLORREF bgColor = g_darkMode ? RGB(30, 30, 30) : RGB(255, 255, 255);
        HBRUSH hBrushBG = CreateSolidBrush(bgColor);
        FillRect(memDC, &rect, hBrushBG);
        DeleteObject(hBrushBG);

        // -------------------- 自绘标题栏 --------------------
        int titleBarHeight = 30;
        RECT titleRect = { 0, 0, rect.right, titleBarHeight };
        HBRUSH hBrushTitle = CreateSolidBrush(g_darkMode ? RGB(45, 45, 45) : RGB(180, 180, 180));
        FillRect(memDC, &titleRect, hBrushTitle);
        DeleteObject(hBrushTitle);

        // 绘制图标（小图标）
        HICON hIconSmall = (HICON)SendMessage(hwnd, WM_GETICON, ICON_SMALL, 0);
        if (!hIconSmall) hIconSmall = hIconGlobal;
        if (hIconSmall) {
            // 绘制 16x16 小图标，距左 6px，垂直居中
            DrawIconEx(memDC, 6, (titleBarHeight - 16) / 2, hIconSmall, 16, 16, 0, NULL, DI_NORMAL);
        }

        // 标题文字
        char wndTitle[256] = { 0 };
        GetWindowTextA(hwnd, wndTitle, sizeof(wndTitle));
        if (!hSimConnect) {
            strncpy_s(wndTitle, sizeof(wndTitle), "MSFS-SimConnect 电脑端 (无 MSFS SimConnect Api 连接)", _TRUNCATE);
            wndTitle[sizeof(wndTitle) - 1] = '\0';
        }
        // 使用印刷体字体
        HFONT hFontTitle = CreateFont(
            16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"微软雅黑"
        );
        HFONT oldFont = (HFONT)SelectObject(memDC, hFontTitle);

        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, g_darkMode ? RGB(220, 220, 220) : RGB(0, 0, 0));

        RECT textRect = { 26, 0, rect.right - 100, titleBarHeight }; // 留出图标和按钮空间
        DrawTextA(memDC, wndTitle, -1, &textRect, DT_VCENTER | DT_SINGLELINE | DT_LEFT);

        SelectObject(memDC, oldFont);
        DeleteObject(hFontTitle);

        // -------------------- 最小化和关闭按钮 --------------------
        RECT btnMin = { rect.right - 80, 0, rect.right - 40, titleBarHeight };
        RECT btnClose = { rect.right - 40, 0, rect.right, titleBarHeight };

        // 自动适配黑白模式
        COLORREF minColor = g_darkMode ? RGB(45, 45, 45) : RGB(180, 180, 180);
        COLORREF minHoverColor = g_darkMode ? RGB(90, 90, 90) : RGB(150, 150, 150);
        COLORREF closeColor = g_darkMode ? RGB(45, 45, 45) : RGB(180, 180, 180);
        COLORREF closeHoverColor = g_darkMode ? RGB(255, 0, 0) : RGB(255, 0, 0);

        HBRUSH hBtnMin = CreateSolidBrush(btnMinHover ? minHoverColor : minColor);
        HBRUSH hBtnClose = CreateSolidBrush(btnCloseHover ? closeHoverColor : closeColor);
        FillRect(memDC, &btnMin, hBtnMin);
        FillRect(memDC, &btnClose, hBtnClose);
        DeleteObject(hBtnMin);
        DeleteObject(hBtnClose);

        // 按钮文字
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, g_darkMode ? RGB(255, 255, 255) : RGB(0, 0, 0)); // 文字黑白切换
        DrawTextA(memDC, "–", -1, &btnMin, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextA(memDC, "X", -1, &btnClose, DT_CENTER | DT_VCENTER | DT_SINGLELINE);


        // -------------------- 内容区 --------------------
        int bottomBarHeight = 62; // 底部两行设置栏预留
        RECT contentRect = { 0, titleBarHeight, rect.right, rect.bottom - bottomBarHeight };

        if (!hSimConnect)
        {
            SetWindowTextA(hMainWnd, "MSFS-SimConnect 电脑端 (等待 MSFS 启动)");
        }
        {
            // -------------------- 二维码和右侧信息居中（游戏未启动时也显示二维码） --------------------
            int qrSize = (int)qrMatrix.size();
            // 二维码模块数随内容变长（公网 IPv6 二维码更大），按大小自适应缩放，避免超出窗口
            int scale = 8;
            if (qrSize > 40) scale = 5;
            else if (qrSize > 30) scale = 6;
            else if (qrSize > 25) scale = 7;
            int spacing = 20;
            int noteHeight = 50;
            int noteSpacing = 8;
            int rightWidth = 300;

            int margin = 2; // 二维码外围白边（模块数）
            int modulePx = qrSize * scale;              // 实际模块区像素
            int quietPx = margin * scale * 2;           // quiet zone 两侧总像素
            int qrWithQuietPx = modulePx + quietPx;    // 模块 + quiet zone 总像素

            // 在白色卡片上额外给一点 padding（使白卡片比二维码略大，便于圆角视觉）
            int whitePad = scale * 2; // you can tune this (pixels)

            int whiteBgWidth = qrWithQuietPx + whitePad * 2;
            int whiteBgHeight = qrWithQuietPx + whitePad * 2;

            int totalWidth = whiteBgWidth + spacing + rightWidth;
            int totalHeight = whiteBgHeight + noteHeight + noteSpacing;

            // 把内容区的起始 Y 从 titleBarHeight 开始，并在剩余区域内居中
            int contentAreaHeight = rect.bottom - titleBarHeight - bottomBarHeight;
            int leftMargin = (rect.right - totalWidth) / 2;
            int topMargin = titleBarHeight + (contentAreaHeight - totalHeight) / 2;
            if (topMargin < titleBarHeight) topMargin = titleBarHeight + 8;

            // -------------------- 白色圆角背景（卡片） --------------------
            int bgRadius = 20;
            HRGN qrBgRgn = CreateRoundRectRgn(
                leftMargin,
                topMargin,
                leftMargin + whiteBgWidth,
                topMargin + whiteBgHeight,
                bgRadius, bgRadius
            );
            HBRUSH hBrushWhite = CreateSolidBrush(g_darkMode ? RGB(245, 245, 245) : RGB(255, 255, 255));
            FillRgn(memDC, qrBgRgn, hBrushWhite);
            DeleteObject(qrBgRgn);

            // -------------------- 在白色区域内居中绘制二维码 --------------------
            int drawOriginX = leftMargin + whitePad + margin * scale;
            int drawOriginY = topMargin + whitePad + margin * scale;

            HBRUSH hBrushFG = CreateSolidBrush(RGB(0, 0, 0));     // 黑
            HBRUSH hBrushBGQR = CreateSolidBrush(RGB(255, 255, 255)); // 白 (模块背景)

            for (int y = 0; y < qrSize; y++)
            {
                for (int x = 0; x < qrSize; x++)
                {
                    RECT r = {
                        drawOriginX + x * scale,
                        drawOriginY + y * scale,
                        drawOriginX + (x + 1) * scale,
                        drawOriginY + (y + 1) * scale
                    };
                    FillRect(memDC, &r, qrMatrix[y][x] ? hBrushFG : hBrushBGQR);
                }
            }

            // 清理二维码用画刷
            DeleteObject(hBrushFG);
            DeleteObject(hBrushBGQR);
            DeleteObject(hBrushWhite);

            // -------------------- 二维码下方注释 --------------------
            std::stringstream note;
            if (g_publicMode.load()) {
                std::string ipv6 = GetPublicIPv6();
                if (!ipv6.empty()) {
                    note << "通过MSFS-SimConnect手机端连接\n"
                        << "(公网 IPv6，蜂窝网络也可用)\n"
                        << "IPv6: [" << ipv6 << "] 端口: " << g_port.load();
                } else {
                    note << "通过MSFS-SimConnect手机端连接\n"
                        << "(未检测到公网 IPv6，仍为局域网)\n"
                        << "IP: " << GetLocalIP() << " 端口: " << g_port.load();
                }
            } else {
                note << "通过MSFS-SimConnect手机端连接\n"
                    << "(需要在同一个局域网内)\n"
                    << "IP: " << GetLocalIP() << " 端口: " << g_port.load();
            }

            HFONT hFontNote = CreateFont(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"微软雅黑");
            HFONT oldFontNote = (HFONT)SelectObject(memDC, hFontNote);

            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, g_darkMode ? RGB(220, 220, 220) : RGB(0, 0, 0));

            RECT noteRect = {
                leftMargin,
                topMargin + whiteBgHeight + noteSpacing,
                leftMargin + whiteBgWidth,
                topMargin + whiteBgHeight + noteSpacing + noteHeight * 2
            };
            DrawTextA(memDC, note.str().c_str(), -1, &noteRect, DT_CENTER | DT_TOP | DT_WORDBREAK);

            SelectObject(memDC, oldFontNote);
            DeleteObject(hFontNote);

            // -------------------- 右侧信息栏 --------------------
            int dataLeft = leftMargin + whiteBgWidth + spacing;
            int dataRight = dataLeft + rightWidth;
            int dataTop = topMargin;
            int dataBottom = topMargin + whiteBgHeight + noteHeight + noteSpacing;

            COLORREF dataBgColor = g_darkMode ? RGB(50, 50, 50) : RGB(200, 200, 200);
            HBRUSH hBrushData = CreateSolidBrush(dataBgColor);
            HRGN hRgn = CreateRoundRectRgn(dataLeft, dataTop, dataRight, dataBottom, 40, 40);
            FillRgn(memDC, hRgn, hBrushData);
            DeleteObject(hBrushData);
            DeleteObject(hRgn);

            // -------------------- 右侧内容 --------------------
            HFONT hFontStatus = CreateFont(24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"微软雅黑");
            HFONT hFontAPI = CreateFont(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"微软雅黑");
            HFONT hFontData = CreateFont(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"微软雅黑");

            // 文本内容
            int nClients = g_clientCount.load();
            std::string statusText = nClients > 0
                ? ("已连接 " + std::to_string(nClients) + " 台手机")
                : "未连接手机";
            std::string statusAPI;
            std::string coordText;
            if (hSimConnect) {
                statusAPI = "MSFS SimConnect API: 已连接";
                std::stringstream coordSS;
                coordSS << "当前游戏数据\n";
                coordSS << "纬度(Lat): " << abs(g_data.latitude) << (g_data.latitude >= 0 ? " N" : " S") << "\n";
                coordSS << "经度(Lon): " << abs(g_data.longitude) << (g_data.longitude >= 0 ? " E" : " W") << "\n";
                coordSS << "高度(Alt): " << g_data.altitude << " 英尺 / " << g_data.altitude * 0.3048 << " 米\n";
                coordSS << "航向(Heading): " << g_data.heading << "°\n";
                coordSS << "俯仰(Pitch): " << g_data.pitch << "°\n";
                coordSS << "横滚(Roll): " << g_data.roll << "°\n";
                coordSS << "地速(GS): " << g_data.gpsGroundSpeed << " m/s\n";
                coordSS << "空速(IAS): " << g_data.indicatedAirspeed << " m/s\n";
                coordText = coordSS.str();
            } else {
                statusAPI = "MSFS SimConnect API: 未连接";
                coordText = "等待 MSFS 启动...\n游戏启动后自动开始发送数据";
            }

            // 可用宽度（去掉左右 padding）
            int paddingX = 16;
            int contentWidth = (dataRight - dataLeft) - paddingX * 2;

            // 计算每块高度
            RECT tmp = { 0,0, contentWidth, 0 };

            // 状态高度
            SelectObject(memDC, hFontStatus);
            tmp.left = 0; tmp.top = 0; tmp.right = contentWidth; tmp.bottom = 0;
            DrawTextA(memDC, statusText.c_str(), -1, &tmp, DT_CALCRECT | DT_CENTER | DT_SINGLELINE);
            int hStatus = tmp.bottom - tmp.top;

            // API 高度
            SelectObject(memDC, hFontAPI);
            tmp.left = 0; tmp.top = 0; tmp.right = contentWidth; tmp.bottom = 0;
            DrawTextA(memDC, statusAPI.c_str(), -1, &tmp, DT_CALCRECT | DT_CENTER | DT_SINGLELINE);
            int hAPI = tmp.bottom - tmp.top;

            // Data 高度 (多行，换行并以居中显示)
            SelectObject(memDC, hFontData);
            tmp.left = 0; tmp.top = 0; tmp.right = contentWidth; tmp.bottom = 0;
            DrawTextA(memDC, coordText.c_str(), -1, &tmp, DT_CALCRECT | DT_WORDBREAK | DT_CENTER);
            int hData = tmp.bottom - tmp.top;

            int gap = 8; // 各块之间间隔
            int totalContentHeight = hStatus + gap + hAPI + gap + hData;

            // 起始 Y，使内容垂直居中在卡片内
            int boxInnerTop = dataTop + 16; // 小外边距
            int boxInnerBottom = dataBottom - 16;
            int boxInnerHeight = boxInnerBottom - boxInnerTop;
            int startY = boxInnerTop + (boxInnerHeight - totalContentHeight) / 2;
            if (startY < boxInnerTop) startY = boxInnerTop;

            // 绘制状态（居中）
            RECT drawRect = { dataLeft + paddingX, startY, dataRight - paddingX, startY + hStatus };
            SelectObject(memDC, hFontStatus);
            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, g_clientCount.load() > 0 ? RGB(0, 200, 0) : RGB(200, 0, 0));
            DrawTextA(memDC, statusText.c_str(), -1, &drawRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // 绘制 API 状态
            int y2 = drawRect.bottom + gap;
            RECT drawRectAPI = { dataLeft + paddingX, y2, dataRight - paddingX, y2 + hAPI };
            SelectObject(memDC, hFontAPI);
            SetTextColor(memDC, RGB(0, 200, 0));
            DrawTextA(memDC, statusAPI.c_str(), -1, &drawRectAPI, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // 绘制数据（多行）
            int y3 = drawRectAPI.bottom + gap;
            RECT drawRectData = { dataLeft + paddingX, y3, dataRight - paddingX, y3 + hData };
            SelectObject(memDC, hFontData);
            SetTextColor(memDC, g_darkMode ? RGB(220, 220, 220) : RGB(0, 0, 0));
            DrawTextA(memDC, coordText.c_str(), -1, &drawRectData, DT_CENTER | DT_WORDBREAK);

            // 清理字体对象
            SelectObject(memDC, GetStockObject(SYSTEM_FONT));
            DeleteObject(hFontStatus);
            DeleteObject(hFontAPI);
            DeleteObject(hFontData);
        }

        // -------------------- 绘制边框 --------------------
        int borderRadius = 20;
        HPEN hPenBorder = CreatePen(PS_SOLID, 1, g_darkMode ? RGB(25, 25, 25) : RGB(250, 250, 250));
        HGDIOBJ oldPen = SelectObject(memDC, hPenBorder);
        HBRUSH hOldBrush = (HBRUSH)SelectObject(memDC, GetStockObject(NULL_BRUSH)); // 不填充

        // 使用 RoundRect 绘制圆角边框
        Rectangle(memDC, 0, 0, rect.right, rect.bottom);
        RoundRect(memDC, 0, 0, rect.right, rect.bottom, borderRadius, borderRadius);

        SelectObject(memDC, hOldBrush);
        SelectObject(memDC, oldPen);
        DeleteObject(hPenBorder);

        // -------------------- 显示到窗口 --------------------
        BitBlt(hdc, 0, 0, rect.right, rect.bottom, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hwnd, &ps);
        break;
    }

    case WM_LBUTTONDOWN:
    {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        RECT rect; GetClientRect(hwnd, &rect);
        int titleBarHeight = 30;

        RECT btnMin = { rect.right - 80, 0, rect.right - 40, titleBarHeight };
        RECT btnClose = { rect.right - 40, 0, rect.right, titleBarHeight };

        if (PtInRect(&btnClose, pt)) { PostMessage(hwnd, WM_CLOSE, 0, 0); }
        else if (PtInRect(&btnMin, pt)) { ShowWindow(hwnd, SW_MINIMIZE); }
        else if (pt.y <= titleBarHeight) {
            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        break;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        RECT rect; GetClientRect(hwnd, &rect);
        int titleBarHeight = 30;
        RECT rMin = { rect.right - 80, 0, rect.right - 40, titleBarHeight };
        RECT rClose = { rect.right - 40, 0, rect.right, titleBarHeight };

        bool prevMin = btnMinHover;
        bool prevClose = btnCloseHover;

        btnMinHover = PtInRect(&rMin, pt);
        btnCloseHover = PtInRect(&rClose, pt);

        if (!mouseTrackActive) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            mouseTrackActive = true;
        }

        if (btnMinHover != prevMin || btnCloseHover != prevClose) {
            RECT rcInvalidate = { rect.right - 80, 0, rect.right, titleBarHeight };
            InvalidateRect(hwnd, &rcInvalidate, FALSE);
        }
    }
                     break;

    case WM_MOUSELEAVE:
        btnMinHover = false;
        btnCloseHover = false;
        mouseTrackActive = false;
        {
            RECT rect; GetClientRect(hwnd, &rect);
            int titleBarHeight = 30;
            RECT rcInvalidate = { rect.right - 80, 0, rect.right, titleBarHeight };
            InvalidateRect(hwnd, &rcInvalidate, FALSE);
        }
        break;

    case WM_DESTROY:
        running = false;
        UpnpStop(); // 关闭软件时自动取消 UPnP 映射
        PcpStop();  // 关闭软件时尽力删除 PCP 放行
        {
            std::lock_guard<std::mutex> lock(sockMutex);
            if (g_udpSock != INVALID_SOCKET) { closesocket(g_udpSock); g_udpSock = INVALID_SOCKET; }
        }
        if (g_fontUi) { DeleteObject(g_fontUi); g_fontUi = NULL; }
        PostQuitMessage(0);
        break;

    default:
        break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// -------------------- 主入口 --------------------
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    EnableHighDPI();
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { MessageBoxA(NULL, "WSAStartup failed", "Error", MB_OK | MB_ICONERROR); return -1; }

    hIconGlobal = (HICON)LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));

    WNDCLASSW wc = {};
    wc.lpszClassName = L"MSFSSimConnectWin";
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = hIconGlobal;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0, L"MSFSSimConnectWin", L"MSFS-SimConnect 电脑端 (未连接)",
        WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        200, 120, 820, 610, NULL, NULL, hInstance, NULL
    );
    // 设置窗口圆角
    int radius = 20; // 圆角半径
    RECT rc;
    GetClientRect(hwnd, &rc);
    HRGN hRgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, radius, radius);
    SetWindowRgn(hwnd, hRgn, TRUE);
    if (!hwnd) { WSACleanup(); return -1; }

    // 确保小图标可用（WM_GETICON / WM_SETICON）
    if (hIconGlobal) {
        SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIconGlobal);
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconGlobal);
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }

    WSACleanup();
    if (hIconGlobal) DestroyIcon(hIconGlobal);
    return (int)msg.wParam;
}
