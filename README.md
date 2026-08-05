# MSFS-SimGPStoAndroid

把微软模拟飞行（MSFS 2020 / 2024）中飞机的实时位置，模拟到安卓手机的系统定位上，让手机上的高德、谷歌等地图 App 可以直接显示飞机位置并进行导航。

## 功能

- **模拟定位**：通过 Android 模拟位置提供者（GPS + 网络双通道）注入位置，支持 3 台手机同时连接
- **实时数据**：位置、航向、地速/空速、高度，以及网络延迟（PONG 往返测量 + 平滑显示）
- **平滑补偿**：手机端按可调频率（5~1000Hz，默认 25Hz）在真实帧之间做预测插值，航向/速度平滑系数可调，减少地图上的卡顿
- **状态悬浮窗**：半透明小卡片，显示状态、坐标、地速/空速、航向、高度和延迟；透明度可调，通知栏可一键开关
- **电脑端可调项**：数据刷新间隔（默认 50ms）、监听端口（默认 36666）均可自定义
- **公网使用**：自动选择稳定的公网 IPv6 地址；UPnP（IPv4 NAT）与 PCP（RFC 6887，IPv4/IPv6 入站放行）自动尝试，路由器不支持时界面明确提示手动放行
- 电脑端等待游戏启动：游戏未启动时窗口照常显示二维码，手机可以先连接，游戏启动后自动推送数据

## 项目结构

```text
MSFS-SimGPStoAndroid
├─ Android   (安卓手机端，包名 com.msfs.simconnect.client)
└─ CPP
   └─ MSFSSimConnect   (电脑端 Windows 程序，C++ / Win32)
```

## 构建

### 安卓端

需要 Android SDK 36 + JDK 17，在 `Android` 目录执行：

```text
gradlew assembleDebug
```

### 电脑端

需要 Visual Studio（C++ 工具链）+ MSFS SimConnect SDK（头文件与导入库，项目默认指向 `D:\MSFS SDK\SimConnect SDK`）。打开 `CPP/MSFSSimConnect/MSFSSimConnect.slnx` 以 Release x64 构建。UPnP 依赖（miniupnpc）已内置在 `CPP/MSFSSimConnect/ThirdParty/miniupnpc`，无需额外下载。

## 使用

1. 手机安装 APK，在 系统设置 → 开发者选项 → 选择模拟位置信息应用 中选中本应用
2. 电脑端运行 MSFS-SimConnect（首次运行允许防火墙，放行 UDP 36666）
3. 手机 App 填写电脑 IP 与端口（默认 36666），或扫描电脑端二维码
4. 连接成功后自动开启模拟定位，打开高德/谷歌地图即可看到飞机位置
5. 悬浮窗默认开启，可随时开关并调节透明度

## 常见问题

- 局域网连接：手机与电脑需在同一网络；Windows 防火墙需放行 UDP 36666
- 公网连接：需电脑有公网 IPv6；开启“公网使用”后程序自动尝试 UPnP/PCP 放行，若路由器不支持，请按界面提示在路由器后台手动放行 36666 端口
- 位置卡顿：适当调低电脑端刷新间隔（如 50ms）或调高手机端注入频率

## 参考与致谢

- 模拟定位与悬浮窗思路参考 [ZCShou/GoGoGo（影梭）](https://github.com/ZCShou/GoGoGo)
- UPnP 使用 [miniupnpc](https://github.com/miniupnp/miniupnpc)（BSD 协议，见 `CPP/MSFSSimConnect/ThirdParty/miniupnpc/LICENSE`）
- 本项目基于 [sunmutian88/msfs_map](https://github.com/sunmutian88/msfs_map) 修改而来

## 许可证

[CC BY-NC-SA 4.0](LICENSE)，不得用于商业用途。
