#include "wifi_web.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "photo_store.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";

#define WIFI_NVS_NS       "wificfg"
#define WIFI_TXT_PATH     "/sdcard/wifi.txt"
#define AP_SSID           "LennonCam-Setup"
#define PAGE_MAX_BYTES    (128 * 1024)
#define ALBUM_MAX_ITEMS   200

// main.c 提供的快门 / 取景接口
extern void camera_app_request_shoot(void);
extern bool camera_app_get_preview_jpeg(const uint8_t **out, size_t *len, int timeout_ms);
extern void camera_app_preview_release(void);

static bool s_connected = false;
static bool s_setup = false;
static char s_ip[16] = "0.0.0.0";
static httpd_handle_t s_server = NULL;

// v3.1 配网加固：STA 连败计数 + 热重启标记 + 周边 SSID 扫描结果。
// 教训（2026-10-29 真机）：手打 SSID 两次翻车——一次首部混入空格、一次
// 手打 SSID 容易打错且大小写敏感。配网页改成扫描点选，免手打。
#define STA_FAIL_MAX 15
static int s_sta_fail_count = 0;
// 「连败后下次开机进配网」的标记存 NVS（键 setuponce）。
// 教训（2026-10-29 v3.1 实测）：RTC_DATA_ATTR 的初值段会被 bootloader
// 每次开机从 flash 重载，esp_restart 后标记丢失——别再用它传跨重启状态。
static char s_scan_ssid[24][33];
static int s_scan_count = 0;

// --------------------------------------------------------------------------
// 公共查询
// --------------------------------------------------------------------------
bool wifi_web_connected(void)
{
    return s_connected;
}

bool wifi_web_in_setup(void)
{
    return s_setup;
}

const char *wifi_web_ip(void)
{
    return s_ip;
}

// --------------------------------------------------------------------------
// 凭据：NVS -> /sdcard/wifi.txt
// --------------------------------------------------------------------------
static char *trim_ws(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return s;
}

static bool load_nvs_cred(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = ssid_len;
    bool ok = (nvs_get_str(h, "ssid", ssid, &len) == ESP_OK) && ssid[0] != '\0';
    if (ok) {
        len = pass_len;
        if (nvs_get_str(h, "pass", pass, &len) != ESP_OK) {
            pass[0] = '\0';
        }
    }
    nvs_close(h);
    return ok;
}

static bool load_file_cred(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    FILE *fp = fopen(WIFI_TXT_PATH, "r");
    if (!fp) {
        return false;
    }
    char line[160];
    ssid[0] = '\0';
    pass[0] = '\0';
    if (fgets(line, sizeof(line), fp)) {
        snprintf(ssid, ssid_len, "%s", trim_ws(line));
    }
    if (fgets(line, sizeof(line), fp)) {
        snprintf(pass, pass_len, "%s", trim_ws(line));
    }
    fclose(fp);
    return ssid[0] != '\0';
}

// --------------------------------------------------------------------------
// WiFi 事件
// --------------------------------------------------------------------------
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
        int reason = ev ? (int)ev->reason : -1;
        s_sta_fail_count++;
        ESP_LOGW(TAG, "STA disconnected reason=%d fail=%d, reconnecting",
                 reason, s_sta_fail_count);
        if (s_sta_fail_count >= STA_FAIL_MAX) {
            // 凭据大概率是错的（名字/密码/频段），别无限重连：
            // 在 NVS 里立标记后重启，下次开机进配网让用户改（凭据不擦，保存覆盖）。
            ESP_LOGW(TAG, "too many STA failures, rebooting into setup AP");
            nvs_handle_t nh;
            if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &nh) == ESP_OK) {
                nvs_set_u8(nh, "setuponce", 1);
                nvs_commit(nh);
                nvs_close(nh);
            }
            esp_restart();
        }
        esp_wifi_connect();
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        s_sta_fail_count = 0;
        ESP_LOGI(TAG, "WEB READY http://%s/", s_ip);
    }
}

static esp_err_t wifi_hw_init(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi event register failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ip event register failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static esp_err_t wifi_start_sta(const char *ssid, const char *pass)
{
    esp_netif_create_default_wifi_sta();

    esp_err_t err = wifi_hw_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        wifi_config_t wc = {0};
        snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ssid);
        snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", pass);
        err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STA start failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "STA connecting, ssid=%s", ssid);   // 密码绝不打印
    return ESP_OK;
}

static esp_err_t wifi_start_ap(void)
{
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();   // APSTA：STA 侧只用来扫描周边 SSID

    esp_err_t err = wifi_hw_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        wifi_config_t ac = {0};
        snprintf((char *)ac.ap.ssid, sizeof(ac.ap.ssid), "%s", AP_SSID);
        ac.ap.ssid_len = (uint8_t)strlen(AP_SSID);
        ac.ap.channel = 1;
        ac.ap.max_connection = 2;
        ac.ap.authmode = WIFI_AUTH_OPEN;
        err = esp_wifi_set_config(WIFI_IF_AP, &ac);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_setup = true;
    ESP_LOGI(TAG, "SETUP AP READY http://192.168.4.1/");
    return ESP_OK;
}

// 扫描周边 SSID 存起来，配网页点选（免手打）。在 APSTA 模式下调用。
static void scan_nearby(void)
{
    s_scan_count = 0;
    wifi_scan_config_t sc = {0};
    sc.show_hidden = false;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan failed");
        return;
    }
    uint16_t n = 24;
    wifi_ap_record_t rec[24];
    if (esp_wifi_scan_get_ap_records(&n, rec) != ESP_OK) {
        return;
    }
    for (int i = 0; i < (int)n && s_scan_count < 24; i++) {
        const char *s = (const char *)rec[i].ssid;
        if (s[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < s_scan_count; j++) {
            if (strcmp(s_scan_ssid[j], s) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        snprintf(s_scan_ssid[s_scan_count], sizeof(s_scan_ssid[0]), "%s", s);
        s_scan_count++;
    }
    ESP_LOGI(TAG, "wifi scan found %d SSIDs", s_scan_count);
}

// --------------------------------------------------------------------------
// HTTP：拼页
// --------------------------------------------------------------------------
typedef struct {
    char *buf;
    size_t cap;     // 含结尾 '\0'
    size_t len;
} page_t;

static void page_printf(page_t *p, const char *fmt, ...)
{
    if (p->len + 1 >= p->cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(p->buf + p->len, p->cap - p->len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= p->cap - p->len) {
        p->len = p->cap - 1;    // 截断，后续不再追加
        return;
    }
    p->len += (size_t)n;
}

static const char *PAGE_HEAD =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>相机相册</title><style>"
    "body{font-family:-apple-system,Helvetica,Arial,sans-serif;margin:0;padding:12px;background:#111;color:#eee}"
    "h1{font-size:20px;margin:0 0 10px}"
    ".card{background:#1c1c1c;border-radius:10px;padding:10px;margin-bottom:12px}"
    "img{display:block;border-radius:8px}"
    "button{background:#e8e8e8;color:#111;border:0;border-radius:8px;padding:8px 14px;font-size:15px}"
    "button.small{padding:4px 10px;font-size:12px;background:#c0392b;color:#fff}"
    "form{margin:8px 0 0}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px}"
    ".item{background:#1c1c1c;border-radius:10px;padding:8px;font-size:12px}"
    ".name{word-break:break-all;margin:6px 0 2px}"
    ".size{color:#999;margin-bottom:6px}"
    "a{color:#7ec8ff;margin-right:10px;text-decoration:none}"
    ".thumb{width:100%;cursor:pointer}"
    ".acts{display:flex;align-items:center;justify-content:space-between;gap:8px}"
    ".acts form{margin:0}"
    ".ov{display:none;position:fixed;inset:0;background:rgba(0,0,0,.93);z-index:9;"
    "overflow:auto;padding:10px}"
    ".ov.on{display:block}"
    ".ov img{width:100%;height:auto}"
    "label{display:block;margin:10px 0}"
    "input{box-sizing:border-box;width:100%;padding:8px;margin-top:4px;border-radius:8px;border:0;font-size:15px}"
    "</style></head><body>";

static bool send_page(httpd_req_t *req, const char *html, size_t len)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    // 手机浏览器会缓存旧页面：配网页/相册页一律不缓存
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html, (int)len) == ESP_OK;
}

// 把 SSID 转义后追加进页面（按钮文字用 textContent 回填，转义不影响回填值）
static void page_printf_escaped(page_t *p, const char *s)
{
    for (const char *c = s; *c; c++) {
        switch (*c) {
            case '<':  page_printf(p, "&lt;");   break;
            case '>':  page_printf(p, "&gt;");   break;
            case '&':  page_printf(p, "&amp;");  break;
            case '"':  page_printf(p, "&quot;"); break;
            case '\'': page_printf(p, "&#39;");  break;
            default:   page_printf(p, "%c", *c); break;
        }
    }
}

static bool send_setup_page(httpd_req_t *req)
{
    page_t p = {
        .buf = (char *)heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .cap = 16 * 1024,
        .len = 0,
    };
    if (!p.buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no psram");
        return false;
    }
    p.buf[0] = '\0';

    page_printf(&p, "%s",
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>相机配网</title><style>"
        "body{font-family:-apple-system,Helvetica,Arial,sans-serif;margin:0;padding:16px;background:#111;color:#eee}"
        "h1{font-size:20px}p{color:#aaa;font-size:14px}"
        "label{display:block;margin:14px 0;font-size:14px}"
        "input{box-sizing:border-box;width:100%;padding:10px;margin-top:6px;border-radius:8px;border:0;font-size:16px}"
        "button{width:100%;margin-top:16px;background:#e8e8e8;color:#111;border:0;border-radius:8px;padding:12px;font-size:16px}"
        "button.ssidbtn{width:auto;margin:0 8px 8px 0;padding:8px 12px;font-size:14px;background:#2a2a2a;color:#eee;border:1px solid #555}"
        "</style></head><body>"
        "<h1>相机配网</h1>"
        "<p>点下面扫到的 WiFi 名字自动填入（免手打出错），再输密码保存。</p>");

    if (s_scan_count > 0) {
        page_printf(&p, "<div>");
        for (int i = 0; i < s_scan_count; i++) {
            page_printf(&p, "<button type=\"button\" class=\"ssidbtn\" "
                            "onclick=\"document.getElementById('ssid').value=this.textContent\">");
            page_printf_escaped(&p, s_scan_ssid[i]);
            page_printf(&p, "</button>");
        }
        page_printf(&p, "</div>");
    } else {
        page_printf(&p, "<p>没扫到周边 WiFi，请在下面手动输入名称。</p>");
    }

    page_printf(&p, "%s",
        "<form method=\"post\" action=\"/save\">"
        "<label>WiFi 名称 (SSID)<input id=\"ssid\" name=\"ssid\" maxlength=\"31\" required></label>"
        "<label>密码<input name=\"pass\" type=\"password\" maxlength=\"63\"></label>"
        "<button type=\"submit\">保存并重启</button>"
        "</form></body></html>");

    bool ok = send_page(req, p.buf, p.len);
    free(p.buf);
    return ok;
}

static bool send_album_page(httpd_req_t *req)
{
    page_t p = {
        .buf = (char *)heap_caps_malloc(PAGE_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .cap = PAGE_MAX_BYTES,
        .len = 0,
    };
    if (!p.buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no psram");
        return false;
    }
    p.buf[0] = '\0';

    int count = photo_store_count();
    uint16_t idx[ALBUM_MAX_ITEMS];
    int found = photo_store_list(idx, ALBUM_MAX_ITEMS);

    page_printf(&p, "%s", PAGE_HEAD);
    page_printf(&p, "<h1>相机相册</h1>");
    page_printf(&p,
                "<div class=\"card\"><img src=\"/preview.jpg\" style=\"width:100%%\">"
                "<form method=\"post\" action=\"/shot\"><button type=\"submit\">遥控快门</button></form>"
                "</div><p>共 %d 张</p>", count);

    if (found == 0) {
        page_printf(&p, "<p>还没有照片</p>");
    } else {
        page_printf(&p, "<div class=\"grid\">");
        int listed = 0;
        for (int i = 0; i < found; i++) {
            char name[16];
            snprintf(name, sizeof(name), "IMG_%04d.jpg", (int)idx[i]);
            char path[48];
            snprintf(path, sizeof(path), PHOTO_DIR "/%s", name);
            struct stat st;
            long kb = 0;
            if (stat(path, &st) == 0) {
                kb = (long)(st.st_size / 1024);
            }
            // 给收尾（浮层/脚本/</body></html>）留足空间
            if (p.cap - p.len < 1536) {
                break;
            }
            // 缩略图点击在本页浮层展开原图，不跳页；下载进隐藏 iframe，页面纹丝不动。
            page_printf(&p,
                        "<div class=\"item\">"
                        "<img class=\"thumb\" src=\"/img/%s\" loading=\"lazy\" "
                        "onclick=\"showImg('/img/%s')\">"
                        "<div class=\"name\">%s</div><div class=\"size\">%ld KB</div>"
                        "<div class=\"acts\"><a href=\"/dl/%s\" target=\"dlframe\">下载</a>"
                        "<form method=\"post\" action=\"/delete\" "
                        "onsubmit=\"return confirm('确定删除 %s?')\">"
                        "<input type=\"hidden\" name=\"name\" value=\"%s\">"
                        "<button class=\"small\" type=\"submit\">删除</button></form></div></div>",
                        name, name, name, kb, name, name, name);
            listed++;
        }
        page_printf(&p, "</div>");
        if (listed < found) {
            page_printf(&p, "<p>仅显示 %d / %d 张</p>", listed, found);
        }
    }
    // 全宽浮层：再点一次（点图或点背景）收起。
    page_printf(&p,
                "<div id=\"ov\" class=\"ov\" onclick=\"hideImg()\"><img id=\"ovimg\" alt=\"\"></div>"
                "<iframe name=\"dlframe\" style=\"display:none\"></iframe>"
                "<script>"
                "function showImg(u){var o=document.getElementById('ov'),"
                "i=document.getElementById('ovimg');"
                "if(o.className.indexOf('on')>=0&&i.getAttribute('src')===u){hideImg();return}"
                "i.src=u;o.className='ov on'}"
                "function hideImg(){document.getElementById('ov').className='ov'}"
                "</script>"
                "</body></html>");

    bool ok = send_page(req, p.buf, p.len);
    free(p.buf);
    return ok;
}

// --------------------------------------------------------------------------
// HTTP：路由
// --------------------------------------------------------------------------
static esp_err_t root_handler(httpd_req_t *req)
{
    if (s_setup) {
        send_setup_page(req);
    } else {
        send_album_page(req);
    }
    return ESP_OK;
}

static void url_decode(char *s)
{
    char *d = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') {
            *d++ = ' ';
        } else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = {p[1], p[2], '\0'};
            *d++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            *d++ = *p;
        }
    }
    *d = '\0';
}

// 从 application/x-www-form-urlencoded 里取一个值（不做解码）。
static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    char pat[16];
    snprintf(pat, sizeof(pat), "%s=", key);

    const char *p = body;
    const char *hit = NULL;
    while ((p = strstr(p, pat)) != NULL) {
        if (p == body || p[-1] == '&') {
            hit = p;
            break;
        }
        p++;
    }
    if (!hit) {
        return false;
    }
    hit += strlen(pat);
    const char *end = strchr(hit, '&');
    size_t n = end ? (size_t)(end - hit) : strlen(hit);
    if (n >= out_len) {
        n = out_len - 1;
    }
    memcpy(out, hit, n);
    out[n] = '\0';
    return true;
}

// /img/NAME（inline 显示）与 /dl/NAME（附件下载）共用：先严格校验名字，再流式发文件。
static esp_err_t send_photo_file(httpd_req_t *req, const char *prefix, bool attachment)
{
    if (strncmp(req->uri, prefix, strlen(prefix)) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }
    const char *name = req->uri + strlen(prefix);
    // 与 photo_store_delete 共用同一条严格校验：只许 IMG_%04d.jpg
    if (!photo_store_name_valid(name)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }

    char path[48];
    snprintf(path, sizeof(path), PHOTO_DIR "/%s", name);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "image/jpeg");
    if (attachment) {
        // 浏览器点这个链接直接存文件，不跳页
        char disp[64];
        snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
        httpd_resp_set_hdr(req, "Content-Disposition", disp);
    }
    char *chunk = (char *)malloc(8 * 1024);
    if (!chunk) {
        fclose(fp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_OK;
    }

    esp_err_t err = ESP_OK;
    size_t n = 0;
    while ((n = fread(chunk, 1, 8 * 1024, fp)) > 0) {
        err = httpd_resp_send_chunk(req, chunk, n);
        if (err != ESP_OK) {
            break;
        }
    }
    free(chunk);
    fclose(fp);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t img_handler(httpd_req_t *req)
{
    return send_photo_file(req, "/img/", false);
}

static esp_err_t dl_handler(httpd_req_t *req)
{
    return send_photo_file(req, "/dl/", true);
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    char body[129];
    int got = 0;
    while (got < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + got, sizeof(body) - 1 - got);
        if (r <= 0) {
            break;
        }
        got += r;
    }
    body[got] = '\0';

    char name[32];
    if (got == 0 || !form_get(body, "name", name, sizeof(name))
        || photo_store_delete(name) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, "delete failed", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t shot_handler(httpd_req_t *req)
{
    camera_app_request_shoot();
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t preview_handler(httpd_req_t *req)
{
    const uint8_t *buf = NULL;
    size_t len = 0;
    if (!camera_app_get_preview_jpeg(&buf, &len, 3000)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, "preview busy", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, (const char *)buf, (int)len);
    camera_app_preview_release();       // 发送完才放 mailbox
    return err;
}

static esp_err_t save_handler(httpd_req_t *req)
{
    char body[512];
    int got = 0;
    while (got < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + got, sizeof(body) - 1 - got);
        if (r <= 0) {
            break;
        }
        got += r;
    }
    body[got] = '\0';

    char ssid[64];
    char pass[96];
    bool has_ssid = form_get(body, "ssid", ssid, sizeof(ssid));
    if (has_ssid) {
        url_decode(ssid);
        // 去首尾空白：手机输入法会在词尾塞空格，SSID 不含首尾空格（2026-10-29 教训）
        char *t = trim_ws(ssid);
        if (t != ssid) {
            memmove(ssid, t, strlen(t) + 1);
        }
    }
    if (form_get(body, "pass", pass, sizeof(pass))) {
        url_decode(pass);
    } else {
        pass[0] = '\0';
    }

    if (!has_ssid || ssid[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, "ssid required", HTTPD_RESP_USE_STRLEN);
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "ssid", ssid);
        if (err == ESP_OK) {
            err = nvs_set_str(h, "pass", pass);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save credentials failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, "save failed", HTTPD_RESP_USE_STRLEN);
    }

    ESP_LOGI(TAG, "wifi credentials saved, ssid=%s, restarting", ssid);   // 密码绝不打印
    static const char saved_page[] =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>已保存</title></head><body style=\"font-family:sans-serif;padding:24px\">"
        "<h2>已保存，正在重启…</h2>"
        "<p>设备重启后会自动连接家里的 WiFi，请把手机连回原来的网络。</p>"
        "</body></html>";
    send_page(req, saved_page, sizeof(saved_page) - 1);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t http_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 8;       // /、/preview.jpg、/img/、/dl/、/delete、/shot、/save 共 7 条
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/",          .method = HTTP_GET,  .handler = root_handler},
        {.uri = "/preview.jpg", .method = HTTP_GET, .handler = preview_handler},
        {.uri = "/img/*",     .method = HTTP_GET,  .handler = img_handler},
        {.uri = "/dl/*",      .method = HTTP_GET,  .handler = dl_handler},
        {.uri = "/delete",    .method = HTTP_POST, .handler = delete_handler},
        {.uri = "/shot",      .method = HTTP_POST, .handler = shot_handler},
        {.uri = "/save",      .method = HTTP_POST, .handler = save_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        err = httpd_register_uri_handler(s_server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", routes[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

// --------------------------------------------------------------------------
// 对外入口
// --------------------------------------------------------------------------
bool wifi_web_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs_flash_init: %s, erasing", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(err));
        return false;
    }

    char ssid[64] = {0};
    char pass[96] = {0};
    bool have = load_nvs_cred(ssid, sizeof(ssid), pass, sizeof(pass));
    if (have) {
        ESP_LOGI(TAG, "credentials from NVS, ssid=%s", ssid);
    } else if (load_file_cred(ssid, sizeof(ssid), pass, sizeof(pass))) {
        have = true;
        ESP_LOGI(TAG, "credentials from %s, ssid=%s", WIFI_TXT_PATH, ssid);
    }

    // 上轮 STA 连败 15 次立下的标记：本次强制进配网（凭据不擦，保存覆盖）
    {
        nvs_handle_t nh;
        if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &nh) == ESP_OK) {
            uint8_t v = 0;
            if (nvs_get_u8(nh, "setuponce", &v) == ESP_OK && v == 1) {
                nvs_erase_key(nh, "setuponce");
                nvs_commit(nh);
                ESP_LOGW(TAG, "forced setup mode (STA failed repeatedly)");
                have = false;
            }
            nvs_close(nh);
        }
    }

    if (have) {
        err = wifi_start_sta(ssid, pass);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi STA start failed, staying offline");
            return false;
        }
    } else {
        ESP_LOGI(TAG, "no stored credentials, starting setup AP");
        err = wifi_start_ap();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi AP start failed");
            return false;
        }
        scan_nearby();
    }

    err = http_server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http server failed");
        return false;
    }
    return true;
}

void wifi_web_forget(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "forget: nvs_open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "wifi credentials cleared from NVS");
    } else {
        ESP_LOGW(TAG, "forget failed: %s", esp_err_to_name(err));
    }
}
