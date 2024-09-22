#include <stdlib.h>
#include <string.h>      
#include <assert.h>
#include <sys/stat.h>
#include <dirent.h>
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
#define MAX_DATA_LEN 100

// WiFi credentials
#define WIFI_SSID "HELLO"          // SSID của WiFi
#define WIFI_PASS "12345678"       // Mật khẩu của WiFi

// HTTP server handle
httpd_handle_t server = NULL;
static const char *TAG = "example";
#define MNT_PATH         "/usb"     // Path in the Virtual File System, where the USB flash drive is going to be mounted
#define APP_QUIT_PIN     GPIO_NUM_0 // BOOT button on most boards
#define BUFFER_SIZE      4096       // The read/write performance can be improved with larger buffer for the cost of RAM, 4kB is enough for most usecases

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
            background-color: #f0f4f8;  /* Màu nền sáng */
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
            color: #0071C8;  /* Màu xanh Bách Khoa */
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
            background-color: #0071C8;  /* Màu xanh Bách Khoa */
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
            background-color: #005999;  /* Màu xanh đậm hơn khi hover */
        }

        #message {
            color: green;
            margin-top: 15px;
            visibility: hidden;  /* Ẩn thông báo mặc định */
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
            <input type="submit" value="Send Data">  <!-- Nút gửi dữ liệu -->
        </form>

        <!-- Input chọn file và nút gửi file -->
        <div class="file-upload-section">
            <input type="file" id="fileInput">  <!-- Input chọn file -->
            <button class="file-button" id="sendFileButton">Send File</button>  <!-- Nút gửi file -->
        </div>

        <!-- Công tắc bật tắt USB và nút gửi trạng thái -->
        <div class="toggle-section">
            <label class="switch">
                <input type="checkbox" id="usbToggle">
                <span class="slider"></span>
            </label>
            <button class="status-button" id="sendStatusButton">Send USB Status</button>  <!-- Nút gửi trạng thái -->
        </div>
        
        <p id="message"></p> <!-- Thông báo hiển thị ở đây -->
        <footer>Powered by ESP32</footer>
    </div>

    <script>
        // Hàm để hiển thị thông báo "Sending..." và ẩn sau khi gửi xong
        function showMessage(text, isError = false) {
            const messageElement = document.getElementById('message');
            messageElement.style.visibility = 'visible';
            messageElement.textContent = text;
            messageElement.style.color = isError ? 'red' : 'green';

            // Tự động ẩn sau 3 giây
            setTimeout(() => {
                messageElement.style.visibility = 'hidden';
            }, 3000);
        }

        // Khi nhấn nút "Send Data" (gửi dữ liệu từ textarea)
        document.getElementById('dataForm').addEventListener('submit', function(event) {
            event.preventDefault();  // Ngăn trang tải lại
            showMessage('Sending...');  // Hiển thị "Sending..."

            var data = document.querySelector('textarea').value;  // Lấy giá trị của data
            var formData = new FormData();
            formData.append("data", data);  // Chỉ gửi trường "data"
            
            fetch('/send', {
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

        // Khi nhấn nút "Send USB Status" (gửi trạng thái của công tắc USB)
        document.getElementById('sendStatusButton').addEventListener('click', function() {
            showMessage('Sending...');  // Hiển thị "Sending..."
            var usbStatus = document.getElementById('usbToggle').checked ? "enabled" : "disabled";  // Lấy giá trị của công tắc USB
            
            var formData = new FormData();
            formData.append("usbStatus", usbStatus);  // Chỉ gửi trường "usbStatus"
            
            fetch('/send', {
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

        // Khi nhấn nút "Send File" (gửi file mà người dùng đã chọn)
        document.getElementById('sendFileButton').addEventListener('click', function() {
            showMessage('Sending...');  // Hiển thị "Sending..."
            var fileInput = document.getElementById('fileInput');
            var file = fileInput.files[0];  // Lấy file từ input
            
            if (file) {
                var formData = new FormData();
                formData.append("file", file);  // Gửi file

                fetch('/send', {
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

// Hàm phân tích dữ liệu multipart
static esp_err_t parse_multipart_data(const char *data, char *output, size_t output_len) {
    const char *boundary = "------WebKitFormBoundary";  // Cần thay thế biên giới tương ứng với client gửi lên
    const char *data_field = "Content-Disposition: form-data; name=\"data\"";
    
    const char *data_start = strstr(data, data_field);
    if (data_start) {
        data_start = strstr(data_start, "\r\n\r\n");  // Tìm phần thân dữ liệu
        if (data_start) {
            data_start += 4;  // Bỏ qua các ký tự \r\n\r\n
            const char *data_end = strstr(data_start, boundary);  // Tìm điểm kết thúc dữ liệu
            if (data_end) {
                size_t len = data_end - data_start - 4;  // Trừ đi khoảng cách biên giới
                if (len > output_len) {
                    len = output_len;  // Đảm bảo không vượt quá kích thước buffer
                }
                strncpy(output, data_start, len);
                output[len] = '\0';  // Kết thúc chuỗi
                return ESP_OK;
            }
        }
    }
    return ESP_FAIL;
}


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

// Hàm xử lý POST từ HTTP server để in dữ liệu lên console
// Hàm xử lý POST từ HTTP server để nhận dữ liệu từ form
esp_err_t handle_post_data(httpd_req_t *req) {
    char content[300];  // Bộ đệm để chứa dữ liệu
    int total_len = req->content_len;  // Tổng số byte từ yêu cầu POST
    int cur_len = 0;
    int received = 0;

    if (total_len >= sizeof(content)) {
        // Nếu dữ liệu vượt quá kích thước bộ đệm, ta sẽ xử lý lỗi
        ESP_LOGI(TAG, "Request content too long");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    while (cur_len < total_len) {
        // Nhận dữ liệu từ HTTP request
        received = httpd_req_recv(req, content + cur_len, total_len - cur_len);
        if (received <= 0) {
            // Nếu có lỗi khi nhận, trả về lỗi
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_408(req);
            }
            return ESP_FAIL;
        }
        cur_len += received;
    }

    content[cur_len] = '\0';  // Đảm bảo kết thúc chuỗi

    // In dữ liệu lên console (dữ liệu đầy đủ)
    ESP_LOGI(TAG, "Received data: %s", content);

    // Trả về phản hồi cho client
    httpd_resp_send(req, "Data received and logged", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


// Định nghĩa handler cho send_usb_status
esp_err_t handle_post_usb_status(httpd_req_t *req) {
    char content[100];
    int ret = httpd_req_recv(req, content, sizeof(content));
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    content[ret] = '\0';  // Đảm bảo chuỗi kết thúc

    // In dữ liệu trạng thái USB lên console
    ESP_LOGI(TAG, "Received USB status: %s", content);

    httpd_resp_send(req, "USB status received and logged", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
// Đăng ký URI handler cho đường dẫn /send_usb_status
httpd_uri_t uri_post_usb_status = {
    .uri      = "/send_usb_status",
    .method   = HTTP_POST,
    .handler  = handle_post_usb_status,
    .user_ctx = NULL
};

httpd_uri_t uri_post = {
    .uri      = "/send",
    .method   = HTTP_POST,
    .handler  = handle_post_data, // Gọi hàm handle_post_data
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

        // Đăng ký URI handler cho đường dẫn /send
        httpd_register_uri_handler(server, &uri_post); // Thêm dòng này

        // Đăng ký URI handler cho đường dẫn /send_usb_status
        httpd_register_uri_handler(server, &uri_post_usb_status);

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
        fprintf(f, "Hello World!\n");
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
            file_operations();
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
