# ✈️ MSFS-SimGPStoAndroid

> 将微软模拟飞行（MSFS 2020 / 2024）中飞机的实时位置，模拟到安卓手机的系统定位上，让你可以使用第三方地图进行导航，现实中的 EFB 软件理论上也兼容。

![预览图](IMG/1.jpg)

![截图一](IMG/2.jpg)

## ✨ 功能特色

- 📍 **模拟定位**：通过 Android 模拟位置提供者注入位置，支持最多 3 台手机同时连接
- 🖥️ **电脑端可调**：数据刷新间隔（默认 50ms）、监听端口（默认 36666）均可自定义
- 🎯 **平滑补偿**：手机端按可调频率（5~1000Hz，默认 25Hz）在真实帧之间做预测插值，航向/速度平滑系数可调，减少地图上的卡顿
- 🪟 **状态悬浮窗**：半透明小卡片，显示状态、坐标、地速/空速、航向、高度与网络延迟，透明度可调，通知栏可一键开关
- 🌐 **公网使用**：自动选择稳定的公网 IPv6；UPnP（IPv4 NAT）与 PCP（RFC 6887）自动尝试放行，路由器不支持时界面明确提示手动放行
- ⚡ **网络延迟**：PONG 往返测量 + 平滑显示，局域网延迟稳定在个位数毫秒级

## 📲 使用

1. 📥 手机安装 APK，在 系统设置 → 开发者选项 → 选择模拟位置信息应用 中选中本应用
2. 🚀 电脑端运行 MSFS-SimConnect（首次运行允许防火墙，放行 UDP 36666）
3. 🔗 手机 App 填写电脑 IP 与端口（默认 36666），或扫描电脑端二维码
4. 🗺️ 连接成功后自动开启模拟定位，打开高德/谷歌等第三方地图即可看到飞机位置
5. 🪟 悬浮窗默认开启，可随时开关并调节透明度

## ❓ 常见问题

- 📡 局域网连接：手机与电脑需在同一网络；Windows 防火墙需放行 UDP 36666
- 🌍 公网连接：需电脑有公网 IPv6；开启“公网使用”后程序自动尝试 UPnP/PCP 放行，若路由器不支持，请按界面提示在路由器后台手动放行 36666 端口
- 🐢 位置卡顿：适当调低电脑端刷新间隔（如 50ms）或调高手机端注入频率

## 📁 项目结构

```text
MSFS-SimGPStoAndroid
├─ Android   (安卓手机端，包名 com.msfs.simconnect.client)
└─ CPP
   └─ MSFSSimConnect   (电脑端 Windows 程序，C++ / Win32)
```

## 🔧 构建

### 📱 安卓端

需要 Android SDK 36 + JDK 17，在 `Android` 目录执行：

```text
gradlew assembleDebug
```

### 🖥️ 电脑端

需要 Visual Studio（C++ 工具链）+ MSFS SimConnect SDK（头文件与导入库，项目默认指向 `D:\MSFS SDK\SimConnect SDK`）。打开 `CPP/MSFSSimConnect/MSFSSimConnect.slnx` 以 Release x64 构建。UPnP 依赖（miniupnpc）已内置在 `CPP/MSFSSimConnect/ThirdParty/miniupnpc`，无需额外下载。



## 🙏 参考与致谢

- 模拟定位与悬浮窗思路参考 [ZCShou/GoGoGo（影梭）](https://github.com/ZCShou/GoGoGo)
- UPnP 使用 [miniupnpc](https://github.com/miniupnp/miniupnpc)（BSD 协议，见 `CPP/MSFSSimConnect/ThirdParty/miniupnpc/LICENSE`）
- 本项目基于 [sunmutian88/msfs_map](https://github.com/sunmutian88/msfs_map) 修改而来

## 🍋 支持

觉得好就请我喝杯柠檬水吧～

![支付宝赞助码](IMG/zfb.jpg)

## 📄 许可证

[CC BY-NC-SA 4.0](LICENSE)，不得用于商业用途。
