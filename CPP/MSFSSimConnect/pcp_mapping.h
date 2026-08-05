/********************************************************************
 * MSFS-SimConnect 电脑端 - PCP (Port Control Protocol, RFC 6887)
 * 用于向路由器请求放行 IPv6/IPv4 入站端口（UPnP 只支持 IPv4 NAT）
 ********************************************************************/

#pragma once

#include <string>

// 启动 PCP 端口放行（后台线程，定期续期）
void PcpStart(int port);

// 停止并尽力删除放行（映射到期会自动失效）
void PcpStop();

std::string PcpStatusText();
