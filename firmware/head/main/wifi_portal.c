#include "wifi_portal.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <net/if.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_random.h"

#include "network_runtime.h"

#define PORTAL_BODY_CAPACITY 512

static const char *TAG = "baby_tank_portal";
static httpd_handle_t s_server;
static char s_form_token[33];
static struct ifreq s_ap_interface;

static void set_private_headers(httpd_req_t *request)
{
    httpd_resp_set_hdr(request, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Content-Security-Policy",
                       "default-src 'self'; style-src 'unsafe-inline'; "
                       "script-src 'unsafe-inline'; form-action 'self'");
}

static esp_err_t page_handler(httpd_req_t *request)
{
    static const char page_before_token[] =
        "<!doctype html><html lang='zh-CN'><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Baby Tank 配网</title><style>body{font-family:system-ui,sans-serif;"
        "max-width:520px;margin:32px auto;padding:0 20px;color:#171717}"
        "h1{font-size:25px}p{line-height:1.5;color:#555}label{display:block;"
        "margin:18px 0 6px}input,button{box-sizing:border-box;width:100%;"
        "padding:13px;font-size:16px;border:1px solid #aaa;border-radius:10px}"
        "button{margin-top:20px;background:#111;color:white;border:0}"
        "#status{padding:14px;background:#eee;border-radius:10px}</style>"
        "<h1>Baby Tank 配网</h1><p>只在首次使用或更换网络时配置。"
        "机器人只支持 2.4 GHz Wi-Fi；校园网页登录密码不要填在这里。</p>"
        "<p id='status'>正在读取设备网络状态…</p>"
        "<form method='post' action='/configure' accept-charset='UTF-8'>"
        "<input type='hidden' name='token' value='";
    static const char page_after_token[] =
        "'><button type='button' onclick='loadNetworks()'>扫描附近 Wi-Fi</button>"
        "<div id='networks'></div><label for='ssid'>Wi-Fi 名称</label>"
        "<input id='ssid' name='ssid' maxlength='32' required autocomplete='off'>"
        "<label for='password'>Wi-Fi 密码（开放网络留空）</label>"
        "<input id='password' name='password' type='password' maxlength='63' autocomplete='off'>"
        "<button type='submit'>连接并保存</button></form>"
        "<button type='button' onclick='testInternet()'>测试机器人互联网连接</button>"
        "<p>校园网登录页属于机器人所连的网络。手机打开登录页不等于机器人已获授权；"
        "若检测到认证，请向网络管理员申请设备 MAC 登记。</p>"
        "<script>async function refresh(){try{const r=await fetch('/status',{cache:'no-store'});"
        "const s=await r.json();let t=s.connected?'已连接 Wi-Fi，IP：'+s.ip:"
        "(s.phase==='connecting'?'机器人正在连接目标 Wi-Fi…':"
        "s.phase==='failed'?(s.reason===0?'已关联热点，但 DHCP 未分配地址或连接超时。':"
        "'连接失败（Wi-Fi 原因码 '+s.reason+'），请检查名称、密码和信号。'):"
        "'尚未连接 Wi-Fi，或已保存的网络当前不可达。');"
        "if(s.connected)t+='；云端：'+(s.internet==='online'?'已验证，可正常联网':"
        "s.internet==='portal'?'疑似校园网认证页（HTTP '+s.http+'）':"
        "s.internet==='testing'?'正在测试…':"
        "s.internet==='offline'?'测试失败，请检查网络或校园网限制':'尚未测试');"
        "document.getElementById('status').textContent=t;}catch(e){"
        "document.getElementById('status').textContent='暂时无法读取状态，稍后重试。';}}"
        "async function testInternet(){const token=document.querySelector('[name=token]').value;"
        "await fetch('/internet-test',{method:'POST',headers:{'X-Setup-Token':token}});"
        "refresh()}async function loadNetworks(){const box=document.getElementById('networks');"
        "box.textContent='正在扫描…';try{const r=await fetch('/networks',{cache:'no-store'});"
        "if(!r.ok)throw new Error('设备忙，请稍后重试');const data=await r.json();"
        "box.replaceChildren();for(const net of data.networks){const b=document.createElement('button');"
        "b.type='button';b.textContent=net.ssid+(net.open?'（开放）':'（需密码）');"
        "b.onclick=()=>{document.getElementById('ssid').value=net.ssid;"
        "document.getElementById('password').focus()};box.appendChild(b)}"
        "if(!data.networks.length)box.textContent='未扫到 2.4 GHz 网络，可手动输入名称。'"
        "}catch(e){box.textContent=e.message}}refresh();setInterval(refresh,2000)</script></html>";
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    set_private_headers(request);
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, page_before_token,
                                               HTTPD_RESP_USE_STRLEN), TAG,
                        "setup page header failed");
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, s_form_token,
                                               HTTPD_RESP_USE_STRLEN), TAG,
                        "setup page token failed");
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, page_after_token,
                                               HTTPD_RESP_USE_STRLEN), TAG,
                        "setup page footer failed");
    return httpd_resp_send_chunk(request, NULL, 0);
}

static int hex_digit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool read_form_field(const char *body, const char *key, char *value,
                            size_t capacity)
{
    size_t key_length = strlen(key);
    const char *field = body;
    while (*field != '\0') {
        const char *end = strchr(field, '&');
        if (end == NULL) end = field + strlen(field);
        if ((size_t)(end - field) > key_length &&
            memcmp(field, key, key_length) == 0 && field[key_length] == '=') {
            const char *input = field + key_length + 1;
            size_t used = 0;
            while (input < end) {
                unsigned char decoded;
                if (*input == '+') {
                    decoded = ' ';
                    ++input;
                } else if (*input == '%' && end - input >= 3) {
                    int high = hex_digit(input[1]);
                    int low = hex_digit(input[2]);
                    if (high < 0 || low < 0) return false;
                    decoded = (unsigned char)((high << 4) | low);
                    input += 3;
                } else {
                    decoded = (unsigned char)*input++;
                }
                if (decoded == 0 || used + 1 >= capacity) return false;
                value[used++] = (char)decoded;
            }
            value[used] = '\0';
            return true;
        }
        field = *end == '&' ? end + 1 : end;
    }
    return false;
}

static esp_err_t configure_handler(httpd_req_t *request)
{
    char body[PORTAL_BODY_CAPACITY] = {0};
    char ssid[33] = {0};
    char password[64] = {0};
    char token[sizeof(s_form_token)] = {0};
    if (request->content_len <= 0 ||
        request->content_len >= sizeof(body)) {
        return httpd_resp_send_err(request, HTTPD_413_CONTENT_TOO_LARGE,
                                   "invalid form length");
    }
    size_t received = 0;
    while (received < request->content_len) {
        int count = httpd_req_recv(request, body + received,
                                   request->content_len - received);
        if (count <= 0) return ESP_FAIL;
        received += (size_t)count;
    }
    bool valid = read_form_field(body, "token", token, sizeof(token)) &&
                 strcmp(token, s_form_token) == 0 &&
                 read_form_field(body, "ssid", ssid, sizeof(ssid)) &&
                 ssid[0] != '\0' &&
                 read_form_field(body, "password", password,
                                 sizeof(password));
    memset(body, 0, sizeof(body));
    if (!valid) {
        memset(password, 0, sizeof(password));
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "invalid or expired setup form");
    }
    esp_err_t result = baby_tank_network_configure_wifi_async(ssid, password);
    memset(password, 0, sizeof(password));
    memset(ssid, 0, sizeof(ssid));
    if (result != ESP_OK) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "Wi-Fi setup is busy or invalid");
    }
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/");
    set_private_headers(request);
    return httpd_resp_sendstr(request, "");
}

static esp_err_t status_handler(httpd_req_t *request)
{
    const char *phase = "idle";
    switch (baby_tank_network_web_wifi_phase()) {
    case BABY_TANK_WEB_WIFI_CONNECTING: phase = "connecting"; break;
    case BABY_TANK_WEB_WIFI_GOT_IP: phase = "got_ip"; break;
    case BABY_TANK_WEB_WIFI_FAILED: phase = "failed"; break;
    default: break;
    }
    bool connected = baby_tank_network_is_connected();
    esp_netif_ip_info_t info = {0};
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (connected && sta != NULL) esp_netif_get_ip_info(sta, &info);
    const char *internet = "untested";
    switch (baby_tank_network_internet_phase()) {
    case BABY_TANK_INTERNET_TESTING: internet = "testing"; break;
    case BABY_TANK_INTERNET_ONLINE: internet = "online"; break;
    case BABY_TANK_INTERNET_PORTAL: internet = "portal"; break;
    case BABY_TANK_INTERNET_OFFLINE: internet = "offline"; break;
    default: break;
    }
    char body[224];
    int length = snprintf(body, sizeof(body),
                          "{\"phase\":\"%s\",\"connected\":%s,\"ip\":\"" IPSTR
                          "\",\"reason\":%d,\"internet\":\"%s\",\"http\":%d}", phase,
                          connected ? "true" : "false", IP2STR(&info.ip),
                          baby_tank_network_last_wifi_reason(), internet,
                          baby_tank_network_internet_http_status());
    ESP_RETURN_ON_FALSE(length > 0 && (size_t)length < sizeof(body),
                        ESP_ERR_INVALID_SIZE, TAG, "status overflow");
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    set_private_headers(request);
    return httpd_resp_send(request, body, length);
}

static esp_err_t internet_test_handler(httpd_req_t *request)
{
    char token[sizeof(s_form_token)] = {0};
    if (httpd_req_get_hdr_value_str(request, "X-Setup-Token", token,
                                    sizeof(token)) != ESP_OK ||
        strcmp(token, s_form_token) != 0) {
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN,
                                   "invalid setup token");
    }
    if (baby_tank_network_test_internet_async() != ESP_OK) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(request, "connect Wi-Fi first or wait for current test");
    }
    set_private_headers(request);
    return httpd_resp_sendstr(request, "test started");
}

static esp_err_t send_json_ssid(httpd_req_t *request, const uint8_t *ssid)
{
    char escaped[32 * 6 + 3];
    size_t used = 0;
    escaped[used++] = '"';
    for (size_t index = 0; index < 32 && ssid[index] != 0; ++index) {
        unsigned char value = ssid[index];
        if (value == '"' || value == '\\') {
            escaped[used++] = '\\';
            escaped[used++] = (char)value;
        } else if (value < 0x20) {
            int length = snprintf(escaped + used, sizeof(escaped) - used,
                                  "\\u%04x", value);
            if (length != 6) return ESP_ERR_INVALID_SIZE;
            used += 6;
        } else {
            escaped[used++] = (char)value;
        }
    }
    escaped[used++] = '"';
    return httpd_resp_send_chunk(request, escaped, used);
}

static esp_err_t networks_handler(httpd_req_t *request)
{
    wifi_ap_record_t records[20];
    uint16_t count = 20;
    esp_err_t result = baby_tank_network_scan_wifi(records, &count);
    if (result != ESP_OK) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(request, "Wi-Fi scan is busy; retry in a moment");
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    set_private_headers(request);
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, "{\"networks\":[", 13),
                        TAG, "Wi-Fi scan response failed");
    bool first = true;
    for (uint16_t index = 0; index < count; ++index) {
        if (records[index].ssid[0] == 0 ||
            strncmp((const char *)records[index].ssid, "BabyTank-", 9) == 0) {
            continue;
        }
        bool duplicate = false;
        for (uint16_t previous = 0; previous < index; ++previous) {
            if (strncmp((const char *)records[index].ssid,
                        (const char *)records[previous].ssid,
                        sizeof(records[index].ssid)) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        if (!first) ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, ",", 1),
                                        TAG, "Wi-Fi scan separator failed");
        first = false;
        ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, "{\"ssid\":", 8),
                            TAG, "Wi-Fi scan entry failed");
        ESP_RETURN_ON_ERROR(send_json_ssid(request, records[index].ssid), TAG,
                            "Wi-Fi scan name failed");
        const char *tail = records[index].authmode == WIFI_AUTH_OPEN
                               ? ",\"open\":true}" : ",\"open\":false}";
        ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, tail,
                                                   HTTPD_RESP_USE_STRLEN), TAG,
                            "Wi-Fi scan security failed");
    }
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, "]}", 2), TAG,
                        "Wi-Fi scan footer failed");
    return httpd_resp_send_chunk(request, NULL, 0);
}

esp_err_t baby_tank_wifi_portal_start(void)
{
    ESP_RETURN_ON_FALSE(s_server == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "setup portal already started");
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    ESP_RETURN_ON_FALSE(ap != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "robot hotspot is unavailable");
    memset(&s_ap_interface, 0, sizeof(s_ap_interface));
    ESP_RETURN_ON_ERROR(esp_netif_get_netif_impl_name(ap, s_ap_interface.ifr_name),
                        TAG, "unable to bind setup page to robot hotspot");
    for (size_t index = 0; index < 4; ++index) {
        uint32_t value = esp_random();
        snprintf(s_form_token + index * 8, 9, "%08lx", (unsigned long)value);
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8;
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.stack_size = 6144;
    config.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    config.if_name = &s_ap_interface;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG,
                        "setup HTTP server failed");
    const httpd_uri_t page = {
        .uri = "/", .method = HTTP_GET, .handler = page_handler,
    };
    const httpd_uri_t configure = {
        .uri = "/configure", .method = HTTP_POST,
        .handler = configure_handler,
    };
    const httpd_uri_t status = {
        .uri = "/status", .method = HTTP_GET, .handler = status_handler,
    };
    const httpd_uri_t internet_test = {
        .uri = "/internet-test", .method = HTTP_POST,
        .handler = internet_test_handler,
    };
    const httpd_uri_t networks = {
        .uri = "/networks", .method = HTTP_GET,
        .handler = networks_handler,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &page), TAG,
                        "setup page route failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &configure), TAG,
                        "setup submit route failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &status), TAG,
                        "setup status route failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &internet_test),
                        TAG, "internet test route failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &networks), TAG,
                        "Wi-Fi scan route failed");
    ESP_LOGI(TAG, "AP-only Wi-Fi setup page ready at http://192.168.4.1/");
    return ESP_OK;
}

httpd_handle_t baby_tank_wifi_portal_server(void)
{
    return s_server;
}

esp_err_t baby_tank_wifi_portal_stop(void)
{
    if (s_server == NULL) return ESP_OK;
    ESP_RETURN_ON_ERROR(httpd_stop(s_server), TAG,
                        "unable to stop AP HTTP listener");
    s_server = NULL;
    return ESP_OK;
}

esp_err_t baby_tank_wifi_portal_restart(void)
{
    ESP_RETURN_ON_ERROR(baby_tank_wifi_portal_stop(), TAG,
                        "unable to stop stale AP HTTP listener");
    return baby_tank_wifi_portal_start();
}
