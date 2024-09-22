#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_err.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <esp_wifi.h>

// Khai báo các biến cần thiết
static const char *TAG = "http_server";

// WiFi credentials
#define WIFI_SSID "HELLO"          // SSID của WiFi
#define WIFI_PASS "12345678"       // Mật khẩu của WiFi

// HTTP server handle
httpd_handle_t server = NULL;

// HTML trang web
const char html_page[] = "<!DOCTYPE html>"
    "<html lang=\"en\">"
    "<head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "<title>ESP32 Data</title>"
    "<style>"
    "body {font-family: Arial, sans-serif;background-color: #f0f0f0;margin: 0;padding: 0;display: flex;justify-content: center;align-items: center;height: 100vh;}"
    ".container {background-color: #fff;padding: 20px;border-radius: 10px;box-shadow: 0 4px 8px rgba(0, 0, 0, 0.1);max-width: 400px;text-align: center;}"
    "h1 {color: #333;}"
    "form {display: flex;flex-direction: column;}"
    "textarea {padding: 10px;margin: 10px 0;border: 1px solid #ddd;border-radius: 5px;font-size: 16px;}"
    "input[type=\"submit\"] {background-color: #28a745;color: white;border: none;padding: 10px;border-radius: 5px;cursor: pointer;font-size: 16px;}"
    "input[type=\"submit\"]:hover {background-color: #218838;}"
    "footer {margin-top: 20px;font-size: 12px;color: #999;}"
    "</style>"
    "</head>"
    "<body>"
    "<div class=\"container\">"
    "<h1>Send Data</h1>"
    "<form action=\"/send\" method=\"POST\">"
    "<textarea name=\"data\" rows=\"5\" placeholder=\"Enter your data here...\"></textarea>"
    "<input type=\"submit\" value=\"Send Data\">"
    "</form>"
    "<footer>Powered by ESP32</footer>"
    "</div>"
    "</body>"
    "</html>";

// Xử lý yêu cầu HTTP GET cho trang web chính
esp_err_t handle_get_root(httpd_req_t *req) {
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Hàm xử lý POST từ HTTP server để in dữ liệu lên console
esp_err_t handle_post_data(httpd_req_t *req) {
    char content[100];
    int ret = httpd_req_recv(req, content, sizeof(content));
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    content[ret] = '\0';  // Đảm bảo chuỗi kết thúc

    // In dữ liệu lên console
    ESP_LOGI(TAG, "Received data: %s", content);

    httpd_resp_send(req, "Data received and logged", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Khởi tạo HTTP server
httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // Khởi tạo HTTP server
    if (httpd_start(&server, &config) == ESP_OK) {
        // Đăng ký xử lý GET cho đường dẫn "/"
        httpd_uri_t uri_get = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = handle_get_root,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_get);

        // Xử lý POST dữ liệu cho đường dẫn "/send"
        httpd_uri_t uri_post = {
            .uri = "/send",
            .method = HTTP_POST,
            .handler = handle_post_data,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_post);
        return server;  // Trả về server nếu khởi tạo thành công
    }

    return NULL;  // Trả về NULL nếu khởi tạo thất bại
}

// Sự kiện WiFi
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Retrying to connect to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        char ip_str[16];
        ESP_LOGI(TAG, "Got IP: %s", esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str)));
    }
}

// Khởi tạo WiFi
void init_wifi(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialization finished.");
}

void app_main(void) {
    // Khởi tạo NVS (Non-Volatile Storage)
    ESP_ERROR_CHECK(nvs_flash_init());

    // Khởi động WiFi
    init_wifi();

    // Khởi tạo HTTP server
    start_webserver();

    ESP_LOGI(TAG, "HTTP server started. You can access the web interface now.");
}