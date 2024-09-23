#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rmt.h"
#include "led_strip.h"

// Định nghĩa GPIO mà WS2812 được gắn vào (GPIO 4)
#define RMT_TX_CHANNEL RMT_CHANNEL_0
#define LED_PIN GPIO_NUM_4
#define LED_NUMBER 1  // Số lượng đèn LED WS2812

// Biến cấu hình đèn LED strip
static led_strip_t *strip;

// Hàm khởi tạo WS2812 sử dụng RMT driver
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

// Hàm đặt màu cho dải đèn LED với độ sáng giảm
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

void app_main(void)
{
    // Khởi tạo dải đèn WS2812
    setup_ws2812();

    while (1) {
        // Thay đổi màu: đỏ (giảm độ sáng)
        set_color(255, 0, 0);  // Màu đỏ
        vTaskDelay(pdMS_TO_TICKS(1000));  // Đợi 1 giây

        // Thay đổi màu: xanh lá cây (giảm độ sáng)
        set_color(0, 255, 0);  // Màu xanh lá cây
        vTaskDelay(pdMS_TO_TICKS(1000));  // Đợi 1 giây

        // Thay đổi màu: xanh dương (giảm độ sáng)
        set_color(0, 0, 255);  // Màu xanh dương
        vTaskDelay(pdMS_TO_TICKS(1000));  // Đợi 1 giây
    }
}
