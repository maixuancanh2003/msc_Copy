#include <stdlib.h>
#include <string.h>      
#include <assert.h>
#include "esp_log.h"

#include <sys/stat.h>
#include "driver/gpio.h"
#include <dirent.h>
#include <ctype.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_err.h"     
#include "esp_log.h"     
#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "ffconf.h"
#include "errno.h"
#include "driver/gpio.h"
#include <stdio.h>
#include <esp_http_server.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <string.h>
#include "esp_system.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rmt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "led_strip.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_http_server.h"
#define CLEAN_BUFFER_SIZE 1024
#define RMT_TX_CHANNEL RMT_CHANNEL_0
#define LED_PIN GPIO_NUM_4
#define LED_NUMBER 1  // Số lượng đèn LED WS2812
#define MAX_DATA_LEN 100
#define GPIO_3 3
#define GPIO_4 4
// WiFi credentials
#define WIFI_SSID "HELLO"          // SSID của WiFi
#define WIFI_PASS "12345678"       // Mật khẩu của WiFi

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_RESET   "\x1b[0m"
// HTTP server handle
httpd_handle_t server = NULL;
static const char *TAG = "HTTP";
#define MNT_PATH         "/usb"     // Path in the Virtual File System, where the USB flash drive is going to be mounted
#define APP_QUIT_PIN     GPIO_NUM_0 // BOOT button on most boards
#define BUFFER_SIZE      4096       // The read/write performance can be improved with larger buffer for the cost of RAM, 4kB is enough for most usecases
static led_strip_t *strip;

/**
 * @brief Application Queue and its messages ID
 */
static QueueHandle_t app_queue;
typedef struct {
    enum {
        APP_QUIT,                // Signals request to exit the application
        APP_DEVICE_CONNECTED,    // USB device connect event
        APP_DEVICE_DISCONNECTED, // USB device disconnect event
    } id;
    union {
        uint8_t new_dev_address; // Address of new USB device for APP_DEVICE_CONNECTED event if
    } data;
} app_message_t;

/**
 * @brief BOOT button pressed callback
 *
 * Signal application to exit the main task
 *
 * @param[in] arg Unused
 */
// Hàm để làm sạch nội dung của file sau khi lưu


void setup_ws2812()
{
    // Cấu hình RMT
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(LED_PIN, RMT_TX_CHANNEL);
    config.clk_div = 2; // Chia clock để có tốc độ truyền phù hợp với WS2812
    rmt_config(&config);
    rmt_driver_install(config.channel, 0, 0);

    // Cấu hình dải đèn LED WS2812
    led_strip_config_t strip_config = LED_STRIP_DEFAULT_CONFIG(LED_NUMBER, RMT_TX_CHANNEL);
    strip = led_strip_new_rmt_ws2812(&strip_config);
    
    // Kiểm tra nếu khởi tạo không thành công
    if (!strip) {
        printf("Failed to initialize WS2812 LED strip\n");
        return;
    }

    // Xóa dữ liệu đèn, thiết lập mặc định
    strip->clear(strip, 100);
}
void set_color(uint8_t red, uint8_t green, uint8_t blue)
{
    // Giảm độ sáng (50% sáng so với giá trị tối đa)
    uint8_t dim_red = red / 6;
    uint8_t dim_green = green / 6;
    uint8_t dim_blue = blue / 6;

    // Đặt màu cho đèn WS2812
    strip->set_pixel(strip, 0, dim_red, dim_green, dim_blue);
    strip->refresh(strip, 100);  // Cập nhật màu
}

void trim(char *str) {
    char *end;
    
    // Loại bỏ khoảng trắng phía trước
    while (isspace((unsigned char)*str)) str++;
    
    // Nếu chuỗi chỉ toàn khoảng trắng
    if (*str == 0) return;

    // Loại bỏ khoảng trắng phía sau
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;

    // Kết thúc chuỗi
    *(end + 1) = '\0';
}
static void gpio_cb(void *arg)
{
    BaseType_t xTaskWoken = pdFALSE;
    app_message_t message = {
        .id = APP_QUIT,
    };

    if (app_queue) {
        xQueueSendFromISR(app_queue, &message, &xTaskWoken);
    }

    if (xTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}
// Cấu hình các chân GPIO
static void configure_gpio() {
    esp_rom_gpio_pad_select_gpio(GPIO_3);
    // esp_rom_gpio_pad_select_gpio(GPIO_4);

    // Đặt các chân GPIO làm ngõ ra
    gpio_set_direction(GPIO_3, GPIO_MODE_OUTPUT);
    // gpio_set_direction(GPIO_4, GPIO_MODE_OUTPUT);
    // Thiết lập giá trị mặc định khi khởi động
    gpio_set_level(GPIO_3, 1);  // ENABLE USB
    // gpio_set_level(GPIO_4, 1);  // GPIO 4 mặc định mức 0
}

const char *html_page = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Data</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            background-color: #f0f4f8;
            margin: 0;
            padding: 0;
            display: flex;
            justify-content: center;
            align-items: center;
            height: 100vh;
        }

        .container {
            background-color: #fff;
            padding: 30px;
            border-radius: 10px;
            box-shadow: 0 6px 12px rgba(0, 0, 0, 0.1);
            max-width: 500px;
            text-align: center;
        }

        h1 {
            color: #0071C8;
            margin-bottom: 20px;
        }

        form {
            display: flex;
            flex-direction: column;
            align-items: center;
        }

        textarea, input[type="file"] {
            padding: 10px;
            margin: 10px 0;
            border: 1px solid #ddd;
            border-radius: 5px;
            font-size: 16px;
            width: 100%;
        }

        input[type="submit"], button {
            background-color: #0071C8;
            color: white;
            border: none;
            padding: 10px 20px;
            border-radius: 5px;
            cursor: pointer;
            font-size: 16px;
            margin-top: 10px;
            width: 100%;
        }

        input[type="submit"]:hover, button:hover {
            background-color: #005999;
        }

        #message {
            color: green;
            margin-top: 15px;
            visibility: hidden;
        }

        footer {
            margin-top: 20px;
            font-size: 12px;
            color: #999;
        }

        .file-upload-section,
        .toggle-section {
            margin-top: 20px;
            display: flex;
            flex-direction: column;
            align-items: center;
            width: 100%;
        }

        .file-button,
        .status-button {
            background-color: #0071C8;
            color: white;
            padding: 10px 20px;
            border: none;
            border-radius: 5px;
            cursor: pointer;
            font-size: 16px;
            margin-top: 10px;
            width: 100%;
        }

        .file-button:hover, 
        .status-button:hover {
            background-color: #005999;
        }

        .switch {
            position: relative;
            display: inline-block;
            width: 50px;
            height: 24px;
            margin-right: 10px;
        }

        .switch input {
            opacity: 0;
            width: 0;
            height: 0;
        }

        .slider {
            position: absolute;
            cursor: pointer;
            top: 0;
            left: 0;
            right: 0;
            bottom: 0;
            background-color: #ccc;
            transition: .4s;
            border-radius: 24px;
        }

        .slider:before {
            position: absolute;
            content: "";
            height: 20px;
            width: 20px;
            left: 4px;
            bottom: 2px;
            background-color: white;
            transition: .4s;
            border-radius: 50%;
        }

        input:checked + .slider {
            background-color: #0071C8;
        }

        input:checked + .slider:before {
            transform: translateX(26px);
        }
        
        .toggle-section {
            display: flex;
            align-items: center;
            justify-content: space-between;
            width: 100%;
            margin-top: 20px;
        }

    </style>
</head>
<body>
    <div class="container">
        <h1>ESP32 Data Interface</h1>
        <p><strong>Name:</strong> Mai Xuan Canh</p>
        <p><strong>Student ID:</strong> 2110834</p>
        <p><strong>University:</strong> Ho Chi Minh City University of Technology</p>

        <!-- Form gửi dữ liệu (textarea) -->
        <form id="dataForm">
            <textarea name="data" rows="5" placeholder="Enter your data here..."></textarea>
            <input type="submit" value="Send Data">
        </form>

        <!-- Input chọn file và nút gửi file -->
        <div class="file-upload-section">
            <input type="file" id="fileInput">
            <button class="file-button" id="sendFileButton">Send File</button>
        </div>

        <!-- Công tắc bật tắt USB và nút gửi trạng thái -->
        <div class="toggle-section">
            <label class="switch">
                <input type="checkbox" id="usbToggle">
                <span class="slider"></span>
            </label>
            <button class="status-button" id="sendStatusButton">Send USB Status</button>
        </div>
        
        <p id="message"></p>
        <footer>Powered by ESP32</footer>
    </div>

    <script>
        // Hiển thị thông báo sau khi gửi dữ liệu
        function showMessage(text, isError = false) {
            const messageElement = document.getElementById('message');
            messageElement.style.visibility = 'visible';
            messageElement.textContent = text;
            messageElement.style.color = isError ? 'red' : 'green';

            setTimeout(() => {
                messageElement.style.visibility = 'hidden';
            }, 3000);
        }

        // Gửi dữ liệu từ textarea đến /send_data
        document.getElementById('dataForm').addEventListener('submit', function(event) {
            event.preventDefault();
            showMessage('Sending data...');

            var data = document.querySelector('textarea').value;
            var formData = new FormData();
            formData.append("data", data);

            fetch('/send_data', {
                method: 'POST',
                body: formData
            })
            .then(response => response.text())
            .then(result => {
                showMessage('Data sent successfully!');
            })
            .catch(error => {
                showMessage('Error sending data!', true);
                console.error('Error:', error);
            });
        });

        // Gửi trạng thái USB đến /send_usb_status
        document.getElementById('sendStatusButton').addEventListener('click', function() {
            showMessage('Sending USB status...');
            var usbStatus = document.getElementById('usbToggle').checked ? "enabled" : "disabled";

            var formData = new FormData();
            formData.append("usbStatus", usbStatus);

            fetch('/send_usb_status', {
                method: 'POST',
                body: formData
            })
            .then(response => response.text())
            .then(result => {
                showMessage('USB Status sent successfully!');
            })
            .catch(error => {
                showMessage('Error sending USB status!', true);
                console.error('Error:', error);
            });
        });

        // Gửi file đến /upload_file
        document.getElementById('sendFileButton').addEventListener('click', function() {
            showMessage('Sending file...');
            var fileInput = document.getElementById('fileInput');
            var file = fileInput.files[0];

            if (file) {
                var formData = new FormData();
                formData.append("file", file);

                fetch('/upload_file', {
                    method: 'POST',
                    body: formData
                })
                .then(response => response.text())
                .then(result => {
                    showMessage('File sent successfully!');
                })
                .catch(error => {
                    showMessage('Error sending file!', true);
                    console.error('Error:', error);
                });
            } else {
                showMessage('No file selected!', true);
            }
        });
    </script>
</body>
</html>
)rawliteral";



// Xử lý yêu cầu HTTP GET cho trang web chính
esp_err_t handle_get_root(httpd_req_t *req) {
    // Trả về HTML trang web
    const char *resp_str = (const char *)html_page;
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
httpd_uri_t uri_get_root = {
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = handle_get_root,
    .user_ctx = NULL
};

esp_err_t handle_post_file(httpd_req_t *req) {
    char content[CLEAN_BUFFER_SIZE];  // Bộ đệm để chứa dữ liệu
    int total_len = req->content_len;  // Tổng độ dài dữ liệu từ request
    int cur_len = 0;
    int received = 0;

    ESP_LOGI(TAG, "Total request length: %d", total_len);

    const char *base_path = "/usb/esp";
    struct stat st;

    // Kiểm tra xem thư mục có tồn tại không, nếu không thì tạo
    if (stat(base_path, &st) == -1) {
        if (mkdir(base_path, 0775) != 0) {
            ESP_LOGE(TAG, "Failed to create directory: %s", strerror(errno));
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }

    // Đường dẫn đầy đủ của file cần lưu
    char file_path[256];
    FILE *file = NULL;
    bool file_opened = false;

    // Nhận dữ liệu từng phần
    while (cur_len < total_len) {
        received = httpd_req_recv(req, content, sizeof(content) - 1);
        if (received < 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_408(req);
            }
            return ESP_FAIL;
        }

        cur_len += received;
        content[received] = '\0';  // Đảm bảo chuỗi kết thúc

        // Lấy tên file từ dữ liệu nhận được (phân tích "filename=" từ multipart data)
        if (!file_opened) {
            char *filename_start = strstr(content, "filename=\"");
            if (filename_start != NULL) {
                filename_start += 10;  // Bỏ qua "filename=\""
                char *filename_end = strchr(filename_start, '\"');
                if (filename_end != NULL) {
                    *filename_end = '\0';  // Kết thúc chuỗi tên file

                    // Clean tên file bằng cách loại bỏ ký tự không hợp lệ
                    for (int i = 0; filename_start[i] != '\0'; ++i) {
                        if (!isalnum((unsigned char)filename_start[i]) && filename_start[i] != '.' && filename_start[i] != '_') {
                            filename_start[i] = '_';  // Thay thế ký tự không hợp lệ bằng dấu gạch dưới
                        }
                    }

                    // Tạo đường dẫn đầy đủ cho file
                    snprintf(file_path, sizeof(file_path), "%s/%s", base_path, filename_start);
                    ESP_LOGI(TAG, "Full file path: %s", file_path);

                    // Mở file để ghi vào USB flash drive
                    file = fopen(file_path, "w");
                    if (file == NULL) {
                        ESP_LOGE(TAG, "Failed to open file for writing: %s", strerror(errno));
                        httpd_resp_send_500(req);
                        return ESP_FAIL;
                    }
                    file_opened = true;
                    ESP_LOGI(TAG, "Saving file to USB: %s", file_path);
                }
            }
        }

        // Tìm phần bắt đầu của nội dung thực tế
        char *data_start = content;
        if (!file_opened) {
            // Chỉ tìm kiếm lần đầu để bỏ qua header
            data_start = strstr(content, "\r\n\r\n");
            if (data_start != NULL) {
                data_start += 4;  // Bỏ qua đoạn \r\n\r\n
            } else {
                data_start = content;  // Nếu không tìm thấy, vẫn ghi toàn bộ
            }
        }

        // Nếu file đã được mở, ghi dữ liệu vào file
        if (file_opened && file != NULL) {
            fwrite(data_start, 1, received - (data_start - content), file);
        }
    }

    // Đóng file sau khi hoàn tất ghi
    if (file_opened && file != NULL) {
        fclose(file);
        ESP_LOGI(TAG, "File saved to USB successfully.");
    }

    // Gửi phản hồi xác nhận
    httpd_resp_send(req, "File received and saved to USB", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


esp_err_t handle_post_usb_status(httpd_req_t *req) {
    char content[200];  // Tăng kích thước bộ đệm nếu cần thiết
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);  // Nhận dữ liệu với kích thước giới hạn -1 để dành chỗ cho '\0'
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    content[ret] = '\0';  // Đảm bảo chuỗi kết thúc

    // In dữ liệu thô ban đầu nhận được lên console với màu đỏ
    ESP_LOGI(TAG, ANSI_COLOR_RED "Raw received USB status: %s" ANSI_COLOR_RESET, content);

    // Tìm phần dữ liệu thực sự sau "Content-Disposition"
    char *data_start = strstr(content, "\r\n\r\n");
    if (data_start != NULL) {
        data_start += 4;  // Bỏ qua đoạn phân cách "\r\n\r\n" để đến phần dữ liệu thực sự

        // Loại bỏ khoảng trắng và ký tự xuống dòng thừa
        for (int i = 0; i < strlen(data_start); i++) {
            if (data_start[i] == '\r' || data_start[i] == '\n') {
                data_start[i] = '\0';  // Kết thúc chuỗi khi gặp ký tự xuống dòng hoặc khoảng trắng
                break;
            }
        }

        // In dữ liệu sau khi tách ra với màu xanh lá cây
        ESP_LOGI(TAG, ANSI_COLOR_GREEN "Cleaned USB status: %s" ANSI_COLOR_RESET, data_start);

        // So sánh chuỗi nhận được với "disabled" hoặc "enabled"
        if (strcmp(data_start, "disabled") == 0) {
            // Đèn sáng màu đỏ khi nhận "disabled"
            set_color(255, 0, 0);
            gpio_set_level(GPIO_3, 0);  
            printf("Đèn LED đã được đặt thành màu đỏ (disabled)\n");
        } else if (strcmp(data_start, "enabled") == 0) {
            // Đèn sáng màu xanh khi nhận "enabled"
            set_color(0, 255, 0);
            gpio_set_level(GPIO_3, 1);  
            printf("Đèn LED đã được đặt thành màu xanh (enabled)\n");
        } else {
            // Nếu dữ liệu không hợp lệ, gửi phản hồi lỗi 400 (Bad Request)
            ESP_LOGI(TAG, "Invalid USB status received: %s", data_start);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        // Gửi phản hồi xác nhận đã nhận dữ liệu và xử lý
        httpd_resp_send(req, "USB status received and logged", HTTPD_RESP_USE_STRLEN);
    } else {
        ESP_LOGI(TAG, "Could not find data");
        httpd_resp_send_500(req);  // Gửi phản hồi lỗi 500 (Internal Server Error)
    }

    return ESP_OK;
}

// Đăng ký URI handler cho đường dẫn /send_usb_status
httpd_uri_t uri_post_usb_status = {
    .uri      = "/send_usb_status",
    .method   = HTTP_POST,
    .handler  = handle_post_usb_status,
    .user_ctx = NULL
};

httpd_uri_t uri_post_file = {
    .uri      = "/upload_file",  // Thay đổi URI cho việc upload file
    .method   = HTTP_POST,
    .handler  = handle_post_file,
    .user_ctx = NULL
};


// Đăng ký URI handler trong server
// Khởi động webserver và đăng ký các handler
httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // Khởi động HTTP server
    if (httpd_start(&server, &config) == ESP_OK) {
        // Đăng ký URI handler cho đường dẫn /
        httpd_register_uri_handler(server, &uri_get_root);
        httpd_register_uri_handler(server, &uri_post_usb_status); // Thêm dòng này
        httpd_register_uri_handler(server, &uri_post_file); // Thêm dòng này


        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
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
/**
 * @brief MSC driver callback
 *
 * Signal device connection/disconnection to the main task
 *
 * @param[in] event MSC event
 * @param[in] arg   MSC event data
 */
static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    if (event->event == MSC_DEVICE_CONNECTED) {
        ESP_LOGI(TAG, "MSC device connected");
        app_message_t message = {
            .id = APP_DEVICE_CONNECTED,
            .data.new_dev_address = event->device.address,
        };
        xQueueSend(app_queue, &message, portMAX_DELAY);
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        ESP_LOGI(TAG, "MSC device disconnected");
        app_message_t message = {
            .id = APP_DEVICE_DISCONNECTED,
        };
        xQueueSend(app_queue, &message, portMAX_DELAY);
    }
}

static void print_device_info(msc_host_device_info_t *info)
{
    const size_t megabyte = 1024 * 1024;
    uint64_t capacity = ((uint64_t)info->sector_size * info->sector_count) / megabyte;

    printf("Device info:\n");
    printf("\t Capacity: %llu MB\n", capacity);
    printf("\t Sector size: %"PRIu32"\n", info->sector_size);
    printf("\t Sector count: %"PRIu32"\n", info->sector_count);
    printf("\t PID: 0x%04X \n", info->idProduct);
    printf("\t VID: 0x%04X \n", info->idVendor);
#ifndef CONFIG_NEWLIB_NANO_FORMAT
    wprintf(L"\t iProduct: %S \n", info->iProduct);
    wprintf(L"\t iManufacturer: %S \n", info->iManufacturer);
    wprintf(L"\t iSerialNumber: %S \n", info->iSerialNumber);
#endif
}

static void file_operations(void)
{
    const char *directory = "/usb/esp";
    const char *file_path = "/usb/esp/test.txt";

    // Create /usb/esp directory
    struct stat s = {0};
    bool directory_exists = stat(directory, &s) == 0;
    if (!directory_exists) {
        if (mkdir(directory, 0775) != 0) {
            ESP_LOGE(TAG, "mkdir failed with errno: %s", strerror(errno));
        }
    }

    // Create /usb/esp/test.txt file, if it doesn't exist
    if (stat(file_path, &s) != 0) {
        ESP_LOGI(TAG, "Creating file");
        FILE *f = fopen(file_path, "w");
        if (f == NULL) {
            ESP_LOGE(TAG, "Failed to open file for writing");
            return;
        }
        fprintf(f, "Hello XuanCanhMai!\n");
        fclose(f);
    }

    // Read back the file
    FILE *f;
    ESP_LOGI(TAG, "Reading file");
    f = fopen(file_path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        return;
    }
    char line[64];
    fgets(line, sizeof(line), f);
    fclose(f);
    // strip newline
    char *pos = strchr(line, '\n');
    if (pos) {
        *pos = '\0';
    }
    ESP_LOGI(TAG, "Read from file '%s': '%s'", file_path, line);
}

void speed_test(void)
{
#define TEST_FILE "/usb/esp/dummy"
#define ITERATIONS  256  // 256 * 4kb = 1MB
    int64_t test_start, test_end;

    FILE *f = fopen(TEST_FILE, "wb+");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return;
    }
    // Set larger buffer for this file. It results in larger and more effective USB transfers
    setvbuf(f, NULL, _IOFBF, BUFFER_SIZE);

    // Allocate application buffer used for read/write
    uint8_t *data = malloc(BUFFER_SIZE);
    assert(data);

    ESP_LOGI(TAG, "Writing to file %s", TEST_FILE);
    test_start = esp_timer_get_time();
    for (int i = 0; i < ITERATIONS; i++) {
        if (fwrite(data, BUFFER_SIZE, 1, f) == 0) {
            return;
        }
    }
    test_end = esp_timer_get_time();
    ESP_LOGI(TAG, "Write speed %1.2f MiB/s", (BUFFER_SIZE * ITERATIONS) / (float)(test_end - test_start));
    rewind(f);

    ESP_LOGI(TAG, "Reading from file %s", TEST_FILE);
    test_start = esp_timer_get_time();
    for (int i = 0; i < ITERATIONS; i++) {
        if (0 == fread(data, BUFFER_SIZE, 1, f)) {
            return;
        }
    }
    test_end = esp_timer_get_time();
    ESP_LOGI(TAG, "Read speed %1.2f MiB/s", (BUFFER_SIZE * ITERATIONS) / (float)(test_end - test_start));

    fclose(f);
    free(data);
}

/**
 * @brief USB task
 *
 * Install USB Host Library and MSC driver.
 * Handle USB Host Library events
 *
 * @param[in] args Unused
 */
static void usb_task(void *args)
{
    const usb_host_config_t host_config = { .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    const msc_host_driver_config_t msc_config = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .callback = msc_event_cb,
    };
    ESP_ERROR_CHECK(msc_host_install(&msc_config));

    bool has_clients = true;
    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        // Release devices once all clients has deregistered
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            has_clients = false;
            if (usb_host_device_free_all() == ESP_OK) {
                break;
            };
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE && !has_clients) {
            break;
        }
    }

    vTaskDelay(10); // Give clients some time to uninstall
    ESP_LOGI(TAG, "Deinitializing USB");
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}
void app_main(void)
{
    configure_gpio();
    setup_ws2812();
    set_color(255, 0, 0);
      // Khởi tạo NVS (Non-Volatile Storage)
    ESP_ERROR_CHECK(nvs_flash_init());

    // Khởi động WiFi
    init_wifi();

    // Khởi tạo HTTP server
    // Create FreeRTOS primitives
    app_queue = xQueueCreate(5, sizeof(app_message_t));
    assert(app_queue);

    BaseType_t task_created = xTaskCreate(usb_task, "usb_task", 4096, NULL, 2, NULL);
    assert(task_created);

    // Init BOOT button: Pressing the button simulates app request to exit
    // It will disconnect the USB device and uninstall the MSC driver and USB Host Lib
    const gpio_config_t input_pin = {
        .pin_bit_mask = BIT64(APP_QUIT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&input_pin));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1));
    ESP_ERROR_CHECK(gpio_isr_handler_add(APP_QUIT_PIN, gpio_cb, NULL));

    ESP_LOGI(TAG, "Waiting for USB flash drive to be connected");
    msc_host_device_handle_t msc_device = NULL;
    msc_host_vfs_handle_t vfs_handle = NULL;

    // Perform all example operations in a loop to allow USB reconnections
    set_color(255, 0, 0);
    while (1) {
        app_message_t msg;
        xQueueReceive(app_queue, &msg, portMAX_DELAY);

        if (msg.id == APP_DEVICE_CONNECTED) {
            // 1. MSC flash drive connected. Open it and map it to Virtual File System
            ESP_ERROR_CHECK(msc_host_install_device(msg.data.new_dev_address, &msc_device));
            const esp_vfs_fat_mount_config_t mount_config = {
                .format_if_mount_failed = false,
                .max_files = 3,
                .allocation_unit_size = 8192,
            };
            ESP_ERROR_CHECK(msc_host_vfs_register(msc_device, MNT_PATH, &mount_config, &vfs_handle));

            // 2. Print information about the connected disk
            msc_host_device_info_t info;
            ESP_ERROR_CHECK(msc_host_get_device_info(msc_device, &info));
            msc_host_print_descriptors(msc_device);
            print_device_info(&info);
            file_operations();

            // 3. List all the files in root directory
            ESP_LOGI(TAG, "ls command output:");
            struct dirent *d;
            DIR *dh = opendir(MNT_PATH);
            assert(dh);
            while ((d = readdir(dh)) != NULL) {
                printf("%s\n", d->d_name);
            }
            closedir(dh);

            // 4. The disk is mounted to Virtual File System, perform some basic demo file operation
            start_webserver();
            // 5. Perform speed test
            speed_test();
            // while (1) {
            // ESP_LOGI(TAG, "InitDone");
            // }

        }
        if ((msg.id == APP_DEVICE_DISCONNECTED) || (msg.id == APP_QUIT)) {
            if (vfs_handle) {
                ESP_ERROR_CHECK(msc_host_vfs_unregister(vfs_handle));
                vfs_handle = NULL;
            }
            if (msc_device) {
                ESP_ERROR_CHECK(msc_host_uninstall_device(msc_device));
                msc_device = NULL;
            }
            if (msg.id == APP_QUIT) {
                // This will cause the usb_task to exit
                ESP_ERROR_CHECK(msc_host_uninstall());
                break;
            }
        }
    }

    ESP_LOGI(TAG, "Done");
    gpio_isr_handler_remove(APP_QUIT_PIN);
    vQueueDelete(app_queue);
}