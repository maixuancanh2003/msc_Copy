#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_err.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include "usb/usb_host.h"
#include "esp_vfs_fat.h"

static const char *TAG = "usb_msc_http";

// WiFi credentials
#define WIFI_SSID "HELLO"
#define WIFI_PASS "12345678"

// HTTP server handle
httpd_handle_t server = NULL;

// USB device handle
usb_device_handle_t msc_device;

// HTML trang web
const char html_page[] = "<!DOCTYPE html>"
    "<html lang=\"en\">"
    "<head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "<title>ESP32 USB Data</title>"
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
    "<h1>Send Data to USB</h1>"
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

// Xử lý POST từ HTTP server để ghi dữ liệu xuống USB disk
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

    // Ghi dữ liệu xuống USB disk
    FILE *f = fopen("/usb/data.txt", "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing.");
        httpd_resp_send(req, "Failed to write data to USB", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    fprintf(f, "%s\n", content);
    fclose(f);
    ESP_LOGI(TAG, "Data written to USB: %s", content);
    httpd_resp_send(req, "Data written to USB", HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

// Khởi tạo HTTP server
// Khởi tạo HTTP server
httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    
    // Tăng giới hạn kích thước cho URI handlers và response headers
    config.max_uri_handlers = 16;   // Số lượng handler tối đa
    config.max_resp_headers = 20;   // Số lượng header tối đa trong phản hồi
    
    // Đặt timeout để tránh lỗi nhận yêu cầu lớn
    config.recv_wait_timeout = 10;  // Tăng thời gian chờ nhận request lên 10 giây

    config.lru_purge_enable = true; // Bật tính năng loại bỏ các handler ít được sử dụng nhất

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
    }
    return server;
}


// Hàm khởi tạo USB host
void init_usb_host(void) {
    ESP_LOGI(TAG, "Initializing USB host");

    // Cài đặt USB host
    usb_host_config_t host_config = {
        .intr_flags = ESP_INTR_FLAG_LEVEL1
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB host: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "USB host installed successfully.");

    // Cấu hình và đăng ký USB client
    usb_host_client_handle_t client_hdl;
    usb_host_client_config_t client_config = {
        .is_synchronous = true,
        .max_num_event_msg = 5
    };

    err = usb_host_client_register(&client_config, &client_hdl);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "USB host client already registered. Skipping registration.");
        return;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB host client registration failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "USB host client registered successfully.");

    // Mở thiết bị USB
    err = usb_host_device_open(client_hdl, 1, &msc_device);  // Mở thiết bị USB đầu tiên
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open USB device: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "USB device opened successfully.");

    // Mount hệ thống file FAT trên USB disk
    esp_err_t ret = esp_vfs_fat_register("/usb", "usb", 4, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register FAT filesystem on USB storage: %s", esp_err_to_name(ret));
        return;
    } else {
        ESP_LOGI(TAG, "USB storage mounted successfully.");
    }
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

    // Khởi tạo USB host và MSC
    init_usb_host();

    // Khởi tạo HTTP server
    start_webserver();

    ESP_LOGI(TAG, "HTTP server started. You can access the web interface now.");
}
