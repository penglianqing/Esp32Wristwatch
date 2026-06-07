# ESP32 AMOLED Dashboard

基于 ESP-IDF 和 LVGL 的圆形屏幕仪表盘，运行在微雪电子 `ESP32-S3-Touch-AMOLED-1.75C` 开发板上。当前版本以 `Photo` 页面为默认首页，并保留 `FnOS`、`Windows11` 两个数据看板页面。

开发板资料：
<https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-1.75C/>

## 主要功能

- `Photo` 默认首页
  - 开机先显示固件内置的 fallback 背景图
  - 支持点击屏幕手动刷新图片
  - 支持按周期从服务端拉取 `466x466 RGB565` 图片
  - 叠加显示时间、日期、电量、天气
- `FnOS` / `Windows11` 看板
  - 通过 MQTT 订阅 CPU / MEM / GPU / TX / RX 数据
  - 无数据时显示 `-`，不再使用随机模拟值
- 按键与触摸
  - `BOOT` 单击切换页面
  - `PWR` 单击回到 `Photo` 页
- 页面切换
  - 以黑屏过渡和串行重建 screen 的方式降低切屏残影和内存压力

## 开发环境

- ESP-IDF: `v5.5`
- 开发板: 微雪电子 `ESP32-S3-Touch-AMOLED-1.75C`
- 依赖显示框架: `LVGL v9`

## 仓库中的配置文件

- `sdkconfig.defaults`
  - 公开仓库默认配置
  - 可提交
- `sdkconfig.defaults.local.example`
  - 本地私有配置模板
  - 可提交
- `sdkconfig.defaults.local`
  - 你自己的 Wi-Fi / MQTT / 图片服务地址
  - 已加入 `.gitignore`
- `sdkconfig` / `sdkconfig.old`
  - 本地生成文件
  - 已加入 `.gitignore`

## 本地配置方法

1. 复制模板：

```powershell
Copy-Item sdkconfig.defaults.local.example sdkconfig.defaults.local
```

2. 修改 `sdkconfig.defaults.local`，至少填写这些配置：

- `CONFIG_DASHBOARD_WIFI_SSID`
- `CONFIG_DASHBOARD_WIFI_PASSWORD`
- `CONFIG_DASHBOARD_MQTT_URI`
- `CONFIG_DASHBOARD_MQTT_USERNAME`
- `CONFIG_DASHBOARD_MQTT_PASSWORD`
- `CONFIG_DASHBOARD_PHOTO_URL`

3. 构建时同时带上公共配置和本地私有配置：

```powershell
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.local" build
```

如果只是首次生成配置，也可以：

```powershell
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.local" menuconfig
```

## 图片服务要求

默认图片地址配置项为：

- `CONFIG_DASHBOARD_PHOTO_URL`

当前推荐服务端直接返回：

- HTTP `200`
- 原始 `RGB565`
- 分辨率 `466x466`

也就是单张图片大小应为：

- `466 x 466 x 2 = 434312 bytes`

## MQTT 主题

默认示例主题在 `sdkconfig.defaults` 中，主要包括：

- `esp32/fnos/cpu/state`
- `esp32/fnos/mem/state`
- `esp32/fnos/gpu/state`
- `esp32/fnos/tx/state`
- `esp32/fnos/rx/state`
- `esp32/windows11/cpu/state`
- `esp32/windows11/mem/state`
- `esp32/windows11/gpu/state`
- `esp32/windows11/tx/state`
- `esp32/windows11/rx/state`
- `esp32/dial/weather/state`

## 烧录与运行

```powershell
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.local" flash monitor
```

## 这次整理后的仓库约定

- 不提交本地 Wi-Fi / MQTT / 局域网地址
- 不提交 `sdkconfig`、`sdkconfig.old`
- 不保留本地 JPG 中间文件和临时转换脚本
- fallback 图片直接使用仓库中的 `main/assets/photo_fallback.rgb565`

