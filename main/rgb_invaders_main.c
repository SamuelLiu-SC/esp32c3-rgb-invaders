#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"
#include "driver/gpio.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/i2c_master.h"
#include "rom/ets_sys.h" 

#define SPEED_LIMIT 4

// Hardware Config
#define LED_STRIP_GPIO       3
#define NUM_LEDS             50
#define LED_STRIP_RMT_RES    (10 * 1000 * 1000)

// 3 Buttons mapped directly to R, G, B channels
#define BUTTON_RED           0  
#define BUTTON_GREEN         1  
#define BUTTON_BLUE          2  
#define BUTTON_PIN_MASK      ((1ULL << BUTTON_RED) | (1ULL << BUTTON_GREEN) | (1ULL << BUTTON_BLUE))

// static const char *TAG = "rgb_button_invaders";
static led_strip_handle_t led_strip;

// Color Archetypes
typedef enum {
    COLOR_RED = 0,
    COLOR_GREEN,
    COLOR_BLUE,
    COLOR_MAX
} rgb_color_t;

// Game State Variables
static int invader_pos = NUM_LEDS - 1;
static int invader_tick_counter = 0;
static int invader_speed_ticks = 20;

// Laser variables
static int laser_pos = -1; 
static rgb_color_t laser_color = COLOR_RED;

static rgb_color_t invader_color = COLOR_RED;
// static int score = 0;
// static bool game_over = false;
volatile int score = 0;
volatile bool game_over = false;
int score_max = 0;
#define I2C_MASTER_SDA_IO       8      
#define I2C_MASTER_SCL_IO       9      
#define I2C_MASTER_NUM          0      
#define OLED_ADDR               0x3C

// PCF8574 Pin Map
#define PIN_RS                 (1 << 0)
#define PIN_RW                 (1 << 1)
#define PIN_EN                 (1 << 2)
#define BACKLIGHT              (1 << 3)

static const char *TAG = "NVS";
static const char *TAG2 = "OLED";
static const char *TAG3 = "GAME";
static const char *TAG4 = "MAIN";
static i2c_master_dev_handle_t dev_handle;
static bool oled_ready = false;
static uint8_t oled_buffer[1024];
static uint8_t oled_cursor_x;
static uint8_t oled_cursor_y;

static void oled_send_cmd(uint8_t cmd);
static void oled_refresh(void);
static void oled_set_pixel(uint8_t x, uint8_t y, bool on);
static void oled_draw_scaled_text(uint8_t x, uint8_t y, uint8_t scale, const char *str);
static void oled_render_score(int value);

// Task function prototype
void nvs_storage_task(void *pvParameters);
void oled_task(void *pvParameters);
void game_task(void *pvParameters);

static esp_err_t oled_write_cmd(uint8_t cmd)
{
    uint8_t payload[2] = {0x00, cmd};
    esp_err_t err = i2c_master_transmit(dev_handle, payload, sizeof(payload), 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG2, "OLED cmd 0x%02X failed: %s", cmd, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t oled_write_data_bytes(const uint8_t *data, size_t len)
{
    uint8_t payload[129];
    if (len > sizeof(payload) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }

    payload[0] = 0x40;
    memcpy(&payload[1], data, len);
    esp_err_t err = i2c_master_transmit(dev_handle, payload, len + 1, 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG2, "OLED data write failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void oled_refresh(void)
{
    for (uint8_t page = 0; page < 8; ++page) {
        oled_send_cmd(0xB0 | page);
        oled_send_cmd(0x00);
        oled_send_cmd(0x10);
        (void)oled_write_data_bytes(&oled_buffer[page * 128], 128);
    }
}

static void oled_set_pixel(uint8_t x, uint8_t y, bool on)
{
    if (x >= 128 || y >= 64) {
        return;
    }

    size_t index = (size_t)x + ((size_t)y / 8U) * 128U;
    uint8_t mask = (uint8_t)(1U << (y % 8U));
    if (on) {
        oled_buffer[index] |= mask;
    } else {
        oled_buffer[index] &= (uint8_t)~mask;
    }
}

static void oled_draw_scaled_text(uint8_t x, uint8_t y, uint8_t scale, const char *str)
{
    (void)scale;

    // Seven-segment masks for digits 0-9: bits are a,b,c,d,e,f,g.
    static const uint8_t digit_masks[10] = {
        0x3F, 0x06, 0x5B, 0x4F, 0x66,
        0x6D, 0x7D, 0x07, 0x7F, 0x6F
    };

    size_t len = strlen(str);
    if (len == 0) {
        return;
    }

    const int digit_w = 20;
    const int digit_h = 44;
    const int thickness = 4;
    const int gap = 4;

    for (size_t i = 0; i < len; ++i) {
        char c = str[i];
        if (c < '0' || c > '9') {
            continue;
        }

        uint8_t mask = digit_masks[c - '0'];
        int dx0 = (int)x + (int)i * (digit_w + gap);
        int dy0 = (int)y;
        int mid_y = digit_h / 2;

        // Helper macro to fill a rectangle segment.
#define DRAW_RECT(x0, y0, w, h)                                  \
        do {                                                      \
            for (int rx = 0; rx < (w); ++rx) {                   \
                for (int ry = 0; ry < (h); ++ry) {               \
                    oled_set_pixel((uint8_t)((x0) + rx),         \
                                  (uint8_t)((y0) + ry), true);   \
                }                                                 \
            }                                                     \
        } while (0)

        // a
        if (mask & 0x01) {
            DRAW_RECT(dx0 + thickness, dy0, digit_w - 2 * thickness, thickness);
        }
        // b
        if (mask & 0x02) {
            DRAW_RECT(dx0 + digit_w - thickness, dy0, thickness, mid_y);
        }
        // c
        if (mask & 0x04) {
            DRAW_RECT(dx0 + digit_w - thickness, dy0 + mid_y, thickness, digit_h - mid_y);
        }
        // d
        if (mask & 0x08) {
            DRAW_RECT(dx0 + thickness, dy0 + digit_h - thickness, digit_w - 2 * thickness, thickness);
        }
        // e
        if (mask & 0x10) {
            DRAW_RECT(dx0, dy0 + mid_y, thickness, digit_h - mid_y);
        }
        // f
        if (mask & 0x20) {
            DRAW_RECT(dx0, dy0, thickness, mid_y);
        }
        // g
        if (mask & 0x40) {
            DRAW_RECT(dx0 + thickness,
                      dy0 + mid_y - (thickness / 2),
                      digit_w - 2 * thickness,
                      thickness);
        }

#undef DRAW_RECT
    }
}

static void oled_render_score(int value)
{
    char score_text[16];
    snprintf(score_text, sizeof(score_text), "%d", value);

    memset(oled_buffer, 0, sizeof(oled_buffer));

    const int digit_w = 20;
    const int gap = 4;
    const int digit_h = 44;
    const int count = (int)strlen(score_text);
    const int text_width = count * digit_w + (count > 0 ? (count - 1) * gap : 0);
    const int start_x = (128 - text_width) / 2;
    const int start_y = (64 - digit_h) / 2;

    oled_draw_scaled_text((uint8_t)start_x, (uint8_t)start_y, 1, score_text);
    oled_refresh();
}

void oled_send_cmd(uint8_t cmd) {
    (void)oled_write_cmd(cmd);
}

void oled_clear(void) {
    memset(oled_buffer, 0, sizeof(oled_buffer));
    oled_cursor_x = 0;
    oled_cursor_y = 0;
    oled_refresh();
}

void oled_init(void)
{
    vTaskDelay(pdMS_TO_TICKS(50));

    oled_write_cmd(0xAE); // display off
    oled_write_cmd(0xD5); oled_write_cmd(0x80); // clock divide
    oled_write_cmd(0xA8); oled_write_cmd(0x3F); // multiplex 1/64
    oled_write_cmd(0xD3); oled_write_cmd(0x00); // display offset
    oled_write_cmd(0x40); // start line
    oled_write_cmd(0x8D); oled_write_cmd(0x14); // charge pump on
    oled_write_cmd(0x20); oled_write_cmd(0x02); // page addressing mode
    oled_write_cmd(0xA1); // segment remap reverse
    oled_write_cmd(0xC8); // com scan reverse
    oled_write_cmd(0xDA); oled_write_cmd(0x12); // com pins
    oled_write_cmd(0x81); oled_write_cmd(0xCF); // contrast
    oled_write_cmd(0xD9); oled_write_cmd(0xF1); // pre-charge
    oled_write_cmd(0xDB); oled_write_cmd(0x40); // vcom detect
    oled_write_cmd(0xA4); // resume RAM
    oled_write_cmd(0xA6); // normal display
    oled_write_cmd(0xAF); // display on

    oled_clear();
}

static bool oled_attach_device(i2c_master_bus_handle_t bus_handle, uint8_t addr)
{
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 50000,
    };

    esp_err_t err = i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG2, "Failed to add OLED device at 0x%02X: %s", addr, esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG2, "OLED device ready at 0x%02X", addr);
    return true;
}

// Setup Input Buttons
static void init_buttons(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = BUTTON_PIN_MASK,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
}

// Setup LED Strip (v2.x / v3.x abstraction)
static void init_led_strip(void) {
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = NUM_LEDS,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    #if defined(LED_STRIP_COLOR_COMPONENT_FMT_GRB)
        strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    #else
        strip_config.led_pixel_format = LED_PIXEL_FORMAT_GRB;
    #endif

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_STRIP_RMT_RES,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);
}

static void spawn_new_invader(void) {
    invader_pos = NUM_LEDS - 1;
    invader_tick_counter = 0;
    invader_color = (rgb_color_t)(esp_random() % COLOR_MAX);
    
    // Scale speed ticks dynamically based on current performance
    invader_speed_ticks = 60 - (score * 3);
    if (invader_speed_ticks < SPEED_LIMIT) invader_speed_ticks = SPEED_LIMIT; // Speed cap limit

    if (invader_color == COLOR_RED)   ESP_LOGI(TAG, "🔴 RED Invader approaching! Match with RED laser!");
    if (invader_color == COLOR_GREEN) ESP_LOGI(TAG, "🟢 GREEN Invader approaching! Match with GREEN laser!");
    if (invader_color == COLOR_BLUE)  ESP_LOGI(TAG, "🔵 BLUE Invader approaching! Match with BLUE laser!");
}

static void trigger_flash(uint8_t r, uint8_t g, uint8_t b, int delay_ms) {
    for (int i = 0; i < NUM_LEDS; i++) {
        led_strip_set_pixel(led_strip, i, r, g, b);
    }
    led_strip_refresh(led_strip);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}


void update_score_display(int score) {
    if (!oled_ready) {
        return;
    }

    oled_render_score(score);
}


// 3. Task Implementation
void nvs_storage_task(void *pvParameters)
{
    nvs_handle_t my_storage_handle;

    ESP_LOGI(TAG, "Task started.");

    while (1) {
        esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_storage_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        if (score > score_max) {
            score_max = score;
            err = nvs_set_i32(my_storage_handle, "max_score", score_max);
            if (err == ESP_OK) {
                err = nvs_commit(my_storage_handle);
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save max score: %s", esp_err_to_name(err));
            }
        }

        nvs_close(my_storage_handle);

        ESP_LOGI(TAG, "Task execution complete. Sleeping for 5 seconds...");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void oled_task(void *pvParameters)
{
    int last_score = -1;

    while (1)
    {
        if (score != last_score)
        {
            last_score = score;
            update_score_display(score);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void game_task(void *pvParameters)
{
    ESP_LOGI(TAG3, "Task started.");
    static TickType_t last_press = 0;

    while (1) {
        if (game_over) {
            trigger_flash(100, 0, 0, 300);
            trigger_flash(0, 0, 0, 300);

            if (gpio_get_level(BUTTON_RED) == 0 || gpio_get_level(BUTTON_GREEN) == 0 || gpio_get_level(BUTTON_BLUE) == 0) {
                laser_pos = -1;
                score = 0;
                update_score_display(score);
                game_over = false;
                spawn_new_invader();
                vTaskDelay(pdMS_TO_TICKS(300));
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (laser_pos == -1) {
            if (gpio_get_level(BUTTON_RED) == 0) {
                if ((xTaskGetTickCount() - last_press) > pdMS_TO_TICKS(50)) {
                    last_press = xTaskGetTickCount();
                    laser_color = COLOR_RED;
                    laser_pos = 1;
                }
            } else if (gpio_get_level(BUTTON_GREEN) == 0) {
                if ((xTaskGetTickCount() - last_press) > pdMS_TO_TICKS(50)) {
                    last_press = xTaskGetTickCount();
                    laser_color = COLOR_GREEN;
                    laser_pos = 1;
                }
            } else if (gpio_get_level(BUTTON_BLUE) == 0) {
                if ((xTaskGetTickCount() - last_press) > pdMS_TO_TICKS(50)) {
                    last_press = xTaskGetTickCount();
                    laser_color = COLOR_BLUE;
                    laser_pos = 1;
                }
            }
        }

        if (laser_pos != -1) {
            laser_pos += 2;
            if (laser_pos >= NUM_LEDS) laser_pos = -1;
        }

        invader_tick_counter++;
        if (invader_tick_counter >= invader_speed_ticks) {
            invader_tick_counter = 0;
            invader_pos--;

            if (invader_pos <= 0) {
                game_over = true;
                ESP_LOGE(TAG3, "BASE INFILTRATED! Game Over. Total Score: %d", score);
            }
        }

        if (laser_pos != -1 && (laser_pos >= invader_pos)) {
            if (laser_color == invader_color) {
                score++;
                ESP_LOGI(TAG3, "DIRECT HIT! Score: %d", score);
                update_score_display(score);
                trigger_flash(255, 255, 255, 100);
                laser_pos = -1;
                spawn_new_invader();
            } else {
                ESP_LOGW(TAG3, "Wrong color: laser passed through");
            }
        }

        led_strip_clear(led_strip);

        led_strip_set_pixel(led_strip, 0, 150, 150, 150);

        if (laser_pos != -1) {
            if (laser_color == COLOR_RED)   led_strip_set_pixel(led_strip, laser_pos, 255, 0, 0);
            if (laser_color == COLOR_GREEN) led_strip_set_pixel(led_strip, laser_pos, 0, 255, 0);
            if (laser_color == COLOR_BLUE)  led_strip_set_pixel(led_strip, laser_pos, 0, 0, 255);
        }

        if (!game_over) {
            if (invader_color == COLOR_RED)   led_strip_set_pixel(led_strip, invader_pos, 255, 0, 0);
            if (invader_color == COLOR_GREEN) led_strip_set_pixel(led_strip, invader_pos, 0, 255, 0);
            if (invader_color == COLOR_BLUE)  led_strip_set_pixel(led_strip, invader_pos, 0, 0, 255);
        }

        led_strip_refresh(led_strip);
        vTaskDelay(pdMS_TO_TICKS(25));

    }
}



void app_main(void)
{
    init_buttons();
    init_led_strip();
    spawn_new_invader();

    ESP_LOGI(TAG, "Initializing I2C Master Bus...");
    
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &bus_handle));

    esp_err_t err = ESP_FAIL;

    err = i2c_master_probe(bus_handle, OLED_ADDR, pdMS_TO_TICKS(100));
    if (err == ESP_OK) {
        ESP_LOGI(TAG2, "Found OLED device at 0x%02X", OLED_ADDR);
        if (oled_attach_device(bus_handle, OLED_ADDR)) {
            oled_ready = true;
        }
    }

    if (!oled_ready) {
        ESP_LOGE(TAG2, "No OLED device found at 0x3C");
    } else {
        ESP_LOGI(TAG2, "Probe result: %s", esp_err_to_name(err));

        // Force hardware startup initialization
        oled_init();

        oled_clear();
        vTaskDelay(pdMS_TO_TICKS(100));

        update_score_display(score);
    }

    score = 0;


    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    xTaskCreate(
        nvs_storage_task,     // Function that implements the task
        "nvs_storage_task",   // Text name for the task
        4096,                 // Stack depth in words
        NULL,                 // Parameter passed into the task
        tskIDLE_PRIORITY + 2,                    // Task priority
        NULL                  // Task handle (not needed here)
    );

    
    ESP_LOGI(TAG, "nvs_storage_task task created successfully. app_main exiting loop control to RTOS scheduler.");

    if (oled_ready) {
        xTaskCreate(
            oled_task,            // Function that implements the task
            "oled_task",         // Text name for the task
            4096,                 // Stack depth in words
            NULL,                 // Parameter passed into the task
            tskIDLE_PRIORITY + 5,                    // Task priority
            NULL                  // Task handle (not needed here)
        );

        ESP_LOGI(TAG2, "oled_task task created successfully. app_main exiting loop control to RTOS scheduler.");
    } else {
        ESP_LOGW(TAG2, "Skipping OLED task because no OLED was detected.");
    }

    xTaskCreate(
        game_task,            // Function that implements the task
        "game_task",          // Text name for the task
        4096,                 // Stack depth in words
        NULL,                 // Parameter passed into the task
        tskIDLE_PRIORITY + 4,                    // Task priority
        NULL                  // Task handle (not needed here)
    );

    ESP_LOGI(TAG3, "game_task task created successfully. app_main exiting loop control to RTOS scheduler.");    

    while (1) {
        ESP_LOGI(TAG4, "main_task loop iteration.");

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
