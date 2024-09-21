#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_err.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include "tinyusb.h"
#include "usb/usb_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

// Tag for logging
static const char *TAG = "usb_msc_http";

// WiFi credentials
#define WIFI_SSID "HELLO"
#define WIFI_PASS "12345678"

// HTTP server handle
httpd_handle_t server = NULL;

// Hàm để ghi dữ liệu xuống USB disk
esp_err_t write_data_to_usb(const char *data) {
    FILE *f = fopen("/usb/data.txt", "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing. Ensure that USB is mounted and accessible.");
        return ESP_FAIL;
    }
    fprintf(f, "%s\n", data);
    fclose(f);
    ESP_LOGI(TAG, "Data written to USB: %s", data);
    return ESP_OK;
}

// Hàm xử lý POST từ HTTP server
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
    if (write_data_to_usb(content) == ESP_OK) {
        httpd_resp_send(req, "Data written to USB", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "Failed to write data to USB", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

// Khởi tạo HTTP server
httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // Khởi tạo HTTP server
    if (httpd_start(&server, &config) == ESP_OK) {
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

void init_usb_host(void) {
    ESP_LOGI(TAG, "Initializing USB host");

    // Gỡ cài đặt chỉ khi USB host đã được khởi tạo
    esp_err_t err = usb_host_uninstall();
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "USB host not found, skipping uninstallation.");
    } else if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "USB host not in valid state for uninstallation. Continuing without uninstall.");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to uninstall existing USB host: %s", esp_err_to_name(err));
        return;
    } else {
        ESP_LOGI(TAG, "USB host uninstalled successfully.");
    }

    // Khởi tạo USB host client
    usb_host_client_handle_t client_hdl;
    usb_device_handle_t msc_device;

    usb_host_client_config_t client_config = {
        .is_synchronous = true,
        .max_num_event_msg = 5,
    };

    // Kiểm tra nếu USB host client đã được đăng ký
    err = usb_host_client_register(&client_config, &client_hdl);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "USB host client already registered. Skipping registration.");
        return;  // Nếu đã đăng ký, thoát hàm để tránh lỗi.
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB host client registration failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "USB host client registered successfully.");

    // Mở thiết bị USB
    ESP_ERROR_CHECK(usb_host_device_open(client_hdl, 1, &msc_device));

    // Đăng ký VFS cho hệ thống file FAT
    esp_err_t ret = esp_vfs_fat_register("/usb", "usb", 4, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register FAT filesystem on USB storage: %s", esp_err_to_name(ret));
        return;  // Nếu mount thất bại, dừng lại và không tiếp tục ghi dữ liệu
    } else {
        ESP_LOGI(TAG, "USB storage mounted successfully");
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
    // Khởi tạo NVS (lưu trữ không biến đổi)
    ESP_ERROR_CHECK(nvs_flash_init());

    // Khởi động WiFi
    init_wifi();

    // Khởi tạo USB host và MSC
    init_usb_host();

    // Khởi tạo HTTP server
    start_webserver();

    ESP_LOGI(TAG, "HTTP server started. You can post data now.");

    // Ghi dữ liệu trực tiếp vào USB (chạy thử nghiệm không qua HTTP)
    if (write_data_to_usb("Test data from USB write function") == ESP_OK) {
        ESP_LOGI(TAG, "Test data written to USB successfully.");
    } else {
        ESP_LOGE(TAG, "Failed to write test data to USB.");
    }
}
