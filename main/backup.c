#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "usb/usb_host.h"
#include "usb/usb_host.h"
#include "usb/msc_host.h"   
#include "esp_vfs_fat.h "
#include "diskio.h"
#include "usb/msc_host.h"

/* Wi-Fi configuration */
#define WIFI_SSID      "HELLO"
#define WIFI_PASS      "12345678"
#define WIFI_MAXIMUM_RETRY  5

static EventGroupHandle_t s_wifi_event_group;
static const char *TAG = "wifi_sta";
static int s_retry_num = 0;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *HTML_PAGE = "<!DOCTYPE html>"
"<html lang=\"en\">"
"<head><meta charset=\"UTF-8\"><title>File Upload</title></head>"
"<body>"
"<h1>Upload File to USB Disk</h1>"
"<form method=\"POST\" enctype=\"multipart/form-data\" action=\"/upload\">"
"<input type=\"file\" name=\"file\"/>"
"<input type=\"submit\" value=\"Upload\"/>"
"</form>"
"</body></html>";

/* Event handler for Wi-Fi */
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* Initialize Wi-Fi */
void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
    vEventGroupDelete(s_wifi_event_group);
}

/* Save data to USB */
void save_data_to_usb(const char *filename, const char *data, size_t len) {
    FATFS fs; // Tạo biến FATFS cục bộ
    FIL file;
    FRESULT res;

    /* Mount USB drive */
    res = f_mount(&fs, "0:", 1);  // Truyền địa chỉ của biến fs
    if (res != FR_OK) {
        ESP_LOGE(TAG, "Failed to mount USB drive (Error: %d)", res);
        return;
    }

    /* Open file */
    res = f_open(&file, filename, FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) {
        ESP_LOGE(TAG, "Failed to open file (Error: %d)", res);
        return;
    }

    /* Write data */
    UINT written;
    res = f_write(&file, data, len, &written);
    if (res != FR_OK || written != len) {
        ESP_LOGE(TAG, "Failed to write data (Error: %d)", res);
    }

    /* Close file */
    f_close(&file);

    /* Unmount USB drive */
    f_mount(NULL, "0:", 1);
}


/* Handler for HTTP GET (serve HTML page) */
esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_send(req, HTML_PAGE, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Handler for HTTP POST (file upload) */
esp_err_t upload_post_handler(httpd_req_t *req) {
    char buf[512];
    int received;
    ESP_LOGI(TAG, "Receiving file...");

    /* Open file on USB */
    FIL file;
    f_open(&file, "/usb/uploaded_file.txt", FA_WRITE | FA_CREATE_ALWAYS);

    /* Read data from request and save to USB */
    while ((received = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
        UINT written;
        f_write(&file, buf, received, &written);
        if (written != received) {
            ESP_LOGE(TAG, "Failed to write data to USB");
        }
    }

    f_close(&file);
    ESP_LOGI(TAG, "File received and saved to USB");
    httpd_resp_send(req, "File uploaded successfully", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Start the HTTP server */
void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t index_uri = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = index_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &index_uri);

        httpd_uri_t upload_uri = {
            .uri      = "/upload",
            .method   = HTTP_POST,
            .handler  = upload_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &upload_uri);
    }
}

/* Main application */
void app_main(void) {
    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize Wi-Fi */
    wifi_init_sta();

    /* Initialize USB Host */
usb_host_config_t host_config = {
    .intr_flags = ESP_INTR_FLAG_LOWMED,
};
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    /* Start HTTP server */
    start_webserver();
}
