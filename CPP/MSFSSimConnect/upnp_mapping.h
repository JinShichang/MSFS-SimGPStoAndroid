/********************************************************************
 * MSFS-SimGPStoAndroid 电脑端 - UPnP 端口映射封装
 * 基于 miniupnpc（BSD 协议，见 ThirdParty/miniupnpc/LICENSE）
 ********************************************************************/

#pragma once

#include <string>

// 启动 UPnP 端口映射（后台线程，失败时自动重试）
void UpnpStart(const std::string& localIpv4, int port);

// 停止并立即取消映射
void UpnpStop();

bool UpnpIsOk();
std::string UpnpStatusText();
