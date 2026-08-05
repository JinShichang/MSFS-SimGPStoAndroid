/********************************************************************
 * MSFS-SimGPStoAndroid 电脑端 - UPnP 端口映射封装
 * 基于 miniupnpc（BSD 协议，见 ThirdParty/miniupnpc/LICENSE）
 ********************************************************************/

#include "upnp_mapping.h"

#include <windows.h>

#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Iphlpapi.lib") // minissdpc 使用 GetAdaptersAddresses/GetBestInterfaceEx

extern HWND hMainWnd;
#define WM_APP_UPNP_UPDATE (WM_APP + 3)

extern std::string GetLocalIP();

namespace {

std::atomic<bool> g_running{ false };
std::atomic<bool> g_ok{ false };
std::mutex g_mutex;
std::string g_status;
std::string g_localIp;
int g_port = 36666;

UPNPUrls g_urls = {};
IGDdatas g_igdData = {};
bool g_mapped = false;
std::vector<std::string> g_mappedProtos;

bool UpnpAttempt() {
    std::lock_guard<std::mutex> lock(g_mutex);

    // 若已有映射，先删除旧映射再重建
    if (g_mapped) {
        for (const auto& proto : g_mappedProtos) {
            UPNP_DeletePortMapping(g_urls.controlURL, g_igdData.first.servicetype,
                std::to_string(g_port).c_str(), proto.c_str(), NULL);
        }
        g_mappedProtos.clear();
        g_mapped = false;
        FreeUPNPUrls(&g_urls);
        g_urls = UPNPUrls();
        g_igdData = IGDdatas();
    }

    UPNPUrls urls = {};
    IGDdatas data = {};
    // 用上网网卡的 IPv4 地址强制 SSDP 从正确网卡发出（多网卡机器默认发现会选错接口）
    std::string mcastIf = GetLocalIP();
    int err = 0;
    UPNPDev* devlist = upnpDiscover(2000,
        mcastIf.empty() ? NULL : mcastIf.c_str(), NULL, 0, 0, 2, &err);
    int index = UPNP_GetValidIGD(devlist, &urls, &data, NULL, 0, NULL, 0);
    if (devlist) freeUPNPDevlist(devlist);
    if (index != 1 && index != 2) {
        // 指定网卡发现失败时，兜底默认发现
        UPNPUrls urls2 = {};
        IGDdatas data2 = {};
        int idx2 = UPNP_GetValidIGD(NULL, &urls2, &data2, NULL, 0, NULL, 0);
        if (idx2 == 1 || idx2 == 2) {
            urls = urls2;
            data = data2;
            index = idx2;
        }
    }
    if (index == 1 || index == 2) {
        bool anyOk = false;
        std::string failInfo;
        for (const char* proto : { "TCP", "UDP" }) {
            int r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                std::to_string(g_port).c_str(),
                std::to_string(g_port).c_str(),
                g_localIp.c_str(),
                "MSFS-SimConnect",
                proto,
                NULL,
                NULL);
            if (r == UPNPCOMMAND_SUCCESS) {
                g_mappedProtos.push_back(proto);
                anyOk = true;
            } else if (failInfo.empty()) {
                failInfo = strupnperror(r);
            }
        }
        if (anyOk) {
            g_urls = urls;
            g_igdData = data;
            g_mapped = true;
            g_ok = true;
            g_status = "UPnP 已映射 " + std::to_string(g_port) + " (TCP/UDP) -> " + g_localIp;
            if (hMainWnd) PostMessageW(hMainWnd, WM_APP_UPNP_UPDATE, 0, 0);
            return true;
        }
        g_status = "UPnP 映射失败: " + failInfo;
        if (hMainWnd) PostMessageW(hMainWnd, WM_APP_UPNP_UPDATE, 0, 0);
        FreeUPNPUrls(&urls);
    }
    else {
        FreeUPNPUrls(&urls);
        g_status = "未发现支持 UPnP 的路由器";
        if (hMainWnd) PostMessageW(hMainWnd, WM_APP_UPNP_UPDATE, 0, 0);
    }
    g_ok = false;
    return false;
}

} // namespace

void UpnpStart(const std::string& localIpv4, int port) {
    if (g_running.exchange(true)) return;
    g_localIp = localIpv4;
    g_port = port;
    g_ok = false;

    std::thread([] {
        UpnpAttempt();
        // 失败时每 30 秒重试
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            if (!g_running.load()) break;
            if (!g_ok.load()) {
                UpnpAttempt();
            }
        }
    }).detach();
}

void UpnpStop() {
    g_running = false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_mapped) {
        for (const auto& proto : g_mappedProtos) {
            UPNP_DeletePortMapping(g_urls.controlURL, g_igdData.first.servicetype,
                std::to_string(g_port).c_str(), proto.c_str(), NULL);
        }
        g_mappedProtos.clear();
        g_mapped = false;
    }
    FreeUPNPUrls(&g_urls);
    g_urls = UPNPUrls();
    g_igdData = IGDdatas();
    g_ok = false;
    g_status.clear();
}

bool UpnpIsOk() {
    return g_ok.load();
}

std::string UpnpStatusText() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}
