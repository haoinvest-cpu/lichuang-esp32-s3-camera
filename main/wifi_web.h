#pragma once

#include <stdbool.h>

// v3：WiFi + 网页相册（含 AP 配网）。
// 凭据优先级：NVS("wificfg") -> /sdcard/wifi.txt -> 开放热点配网。
// 失败只记日志、返回 false，绝不影响拍照。

// 启动 WiFi 与 HTTP 服务。已有凭据走 STA（不等拿 IP 就开网页），
// 没凭据进 AP 配网模式（SSID LennonCam-Setup，http://192.168.4.1/）。
bool wifi_web_start(void);

// 擦除 NVS "wificfg" 命名空间（调用方负责重启）。
void wifi_web_forget(void);

bool wifi_web_connected(void);
bool wifi_web_in_setup(void);

// STA 未连接时返回 "0.0.0.0"。
const char *wifi_web_ip(void);
