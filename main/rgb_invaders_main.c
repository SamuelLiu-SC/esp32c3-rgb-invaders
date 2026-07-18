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
#define LCD_ADDR_27             0x27
#define LCD_ADDR_3F             0x3F

// PCF8574 Pin Map
#define PIN_RS                 (1 << 0)
#define PIN_RW                 (1 << 1)
#define PIN_EN                 (1 << 2)
#define BACKLIGHT              (1 << 3)

static const char *TAG = "NVS";
static const char *TAG2 = "LCD";
static const char *TAG3 = "GAME";
static const char *TAG4 = "MAIN";
static i2c_master_dev_handle_t dev_handle;

// Task function prototype
void nvs_storage_task(void *pvParameters);
void lcd_task(void *pvParameters);
void game_task(void *pvParameters);

static esp_err_t i2c_write_raw(uint8_t data)
{
    esp_err_t err =
        i2c_master_transmit(
            dev_handle,
            &data,
            1,
            100);

    if (err != ESP_OK) {
        ESP_LOGE(TAG2,
                 "Write 0x%02X failed: %s",
                 data,
                 esp_err_to_name(err));
    }

    return err;
}

// Sends a single 4-bit nibble and executes a precise hardware clock pulse
void lcd_send_nibble(uint8_t nibble, uint8_t mode) {
    uint8_t data = ((nibble & 0x0F) << 4) | (mode & PIN_RS) | BACKLIGHT;

    // Step 1: Set data pins and RS, keep Enable LOW
    if (i2c_write_raw(data) != ESP_OK) {
        ESP_LOGE(TAG2, "Failed to send nibble data");
        return;
    }
    ets_delay_us(50);

    // Step 2: Pull Enable HIGH to latch data setup
    if (i2c_write_raw(data | PIN_EN) != ESP_OK) {
        ESP_LOGE(TAG2, "Failed to send enable high");
        return;
    }
    ets_delay_us(50);

    // Step 3: Pull Enable LOW to capture data edge
    if (i2c_write_raw(data) != ESP_OK) {
        ESP_LOGE(TAG2, "Failed to release enable");
        return;
    }
    ets_delay_us(100);
}

// Split 8-bit instruction packet into two 4-bit transmissions
void lcd_send_cmd(uint8_t cmd) {
    lcd_send_nibble(cmd >> 4, 0);
    lcd_send_nibble(cmd & 0x0F, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
}

// Split 8-bit character data packet into two 4-bit transmissions
void lcd_send_data(uint8_t data) {
    lcd_send_nibble(data >> 4, PIN_RS);
    lcd_send_nibble(data & 0x0F, PIN_RS);
    ets_delay_us(50);
}

void lcd_clear(void) {
    lcd_send_cmd(0x01); 
    vTaskDelay(pdMS_TO_TICKS(5));
}

void lcd_set_cursor(uint8_t row, uint8_t col) {
    uint8_t address = (row == 0) ? (0x00 + col) : (0x40 + col);
    lcd_send_cmd(0x80 | address);
}

void lcd_send_string(const char *str) {
    while (*str) {
        lcd_send_data((uint8_t)(*str));
        str++;
    }
}

void lcd_init(void)
{
    vTaskDelay(pdMS_TO_TICKS(50));

    lcd_send_nibble(0x03, 0);
    vTaskDelay(pdMS_TO_TICKS(5));

    lcd_send_nibble(0x03, 0);
    vTaskDelay(pdMS_TO_TICKS(1));

    lcd_send_nibble(0x03, 0);
    vTaskDelay(pdMS_TO_TICKS(1));

    lcd_send_nibble(0x02, 0);
    vTaskDelay(pdMS_TO_TICKS(1));

    lcd_send_cmd(0x28);
    lcd_send_cmd(0x08);
    lcd_send_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(2));
    lcd_send_cmd(0x06);
    lcd_send_cmd(0x0C);

    vTaskDelay(pdMS_TO_TICKS(20));
}

static bool lcd_attach_device(i2c_master_bus_handle_t bus_handle, uint8_t addr)
{
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 50000,
    };

    esp_err_t err = i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG2, "Failed to add LCD device at 0x%02X: %s", addr, esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG2, "LCD device ready at 0x%02X", addr);
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
    invader_color = (rgb_color_t)(esp_random() % COLOR_MAX);
    
    // Scale speed ticks dynamically based on current performance
    invader_speed_ticks = 25 - (score * 2);
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
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "Score: %d", score);
    lcd_set_cursor(1, 0);
    lcd_send_string(buffer);
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

void lcd_task(void *pvParameters)
{
    int last_score = -1;
    char buffer[16];

    lcd_set_cursor(0,0);
    lcd_send_string("RGB Invaders");

    while (1)
    {
        if (score != last_score)
        {
            last_score = score;

            lcd_set_cursor(1,0);

            snprintf(buffer,
                     sizeof(buffer),
                     "Score:%-5d",
                     score);

            lcd_send_string("                ");
            lcd_set_cursor(1,0);
            lcd_send_string(buffer);
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

    const uint8_t lcd_addresses[] = { LCD_ADDR_27, LCD_ADDR_3F };
    bool lcd_ready = false;
    esp_err_t err = ESP_FAIL;

    for (size_t i = 0; i < sizeof(lcd_addresses) / sizeof(lcd_addresses[0]); ++i) {
        uint8_t addr = lcd_addresses[i];
        err = i2c_master_probe(bus_handle, addr, pdMS_TO_TICKS(100));
        if (err == ESP_OK) {
            ESP_LOGI(TAG2, "Found LCD device at 0x%02X", addr);
            if (lcd_attach_device(bus_handle, addr)) {
                lcd_ready = true;
                break;
            }
        }
    }

    if (!lcd_ready) {
        ESP_LOGE(TAG2, "No LCD device found at 0x27 or 0x3F");
    } else {
        ESP_LOGI(TAG2, "Probe result: %s", esp_err_to_name(err));

        // Force hardware startup initialization
        lcd_init();

        lcd_clear();
        vTaskDelay(pdMS_TO_TICKS(100));
        i2c_write_raw(BACKLIGHT);
        i2c_write_raw(0x00);
        i2c_write_raw(BACKLIGHT);
        i2c_write_raw(BACKLIGHT);
        i2c_write_raw(0x00);
        i2c_write_raw(BACKLIGHT);

        lcd_set_cursor(0, 0);
        lcd_send_string("Max Score: ");
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

    if (lcd_ready) {
        xTaskCreate(
            lcd_task,             // Function that implements the task
            "lcd_task",           // Text name for the task
            4096,                 // Stack depth in words
            NULL,                 // Parameter passed into the task
            tskIDLE_PRIORITY + 5,                    // Task priority
            NULL                  // Task handle (not needed here)
        );

        ESP_LOGI(TAG2, "lcd_task task created successfully. app_main exiting loop control to RTOS scheduler.");
    } else {
        ESP_LOGW(TAG2, "Skipping LCD task because no LCD was detected.");
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
