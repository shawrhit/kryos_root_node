#include <inttypes.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "root_node_slave";

#define SPI_HOST_ID SPI3_HOST
#define SPI_MOSI_PIN GPIO_NUM_23  // RPi MOSI -> ESP MOSI
#define SPI_MISO_PIN GPIO_NUM_19  // ESP MISO -> RPi MISO
#define SPI_SCLK_PIN GPIO_NUM_18  // RPi SCLK -> ESP SCLK
#define SPI_CS_PIN   GPIO_NUM_4   // RPi CE0  -> ESP CS
#define INTERRUPT_PIN GPIO_NUM_25 // Active Low to RPi
#define SEND_PERIOD_MS 5000

typedef struct __attribute__((packed)) {
    uint32_t round_id;
    uint32_t timestamp;
    float temp_c;
    uint8_t node_mask;
    uint8_t rejected_mask;
    uint8_t status_flags;
} payload_t;

static esp_err_t init_interrupt_pin(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << INTERRUPT_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "interrupt GPIO config failed");
    return gpio_set_level(INTERRUPT_PIN, 1); // Default high
}

static esp_err_t init_spi_slave(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = SPI_MOSI_PIN,
        .miso_io_num = SPI_MISO_PIN,
        .sclk_io_num = SPI_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    spi_slave_interface_config_t slvcfg = {
        .mode = 0, // Must match RPi SPI mode
        .spics_io_num = SPI_CS_PIN,
        .queue_size = 1,
        .flags = 0,
    };

    // Initialize as Slave
    ESP_RETURN_ON_ERROR(spi_slave_initialize(SPI_HOST_ID, &buscfg, &slvcfg, SPI_DMA_DISABLED),
                        TAG, "SPI slave init failed");
    return ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK(init_interrupt_pin());
    ESP_ERROR_CHECK(init_spi_slave());

    WORD_ALIGNED_ATTR uint8_t rx_data[sizeof(payload_t)]; // Required by ESP-IDF even if not reading

    while (true) {
        payload_t payload;
        payload.round_id = 464693; //680.5 years till overflow lmao
        payload.timestamp = 1234567890;
        payload.temp_c = 4.5;
        payload.node_mask = 0xFF;
        payload.rejected_mask = 0x00;
        payload.status_flags = 0x01;

        // Log raw hex payload
        uint8_t *payload_bytes = (uint8_t *)&payload;
        ESP_LOGI(TAG, "Raw payload (%d bytes): ", sizeof(payload));
        for (int i = 0; i < sizeof(payload); i++) {
            printf("%02x ", payload_bytes[i]);
        }
        printf("\n");

        spi_slave_transaction_t t = {0};
        t.length = sizeof(payload) * 8;
        t.tx_buffer = &payload;
        t.rx_buffer = rx_data;

        // 1. Queue the transaction in the hardware shift register
        ESP_LOGI(TAG, "Queuing payload: round_id=%lu, temp=%.1f°C, status=0x%02x", 
                 payload.round_id, payload.temp_c, payload.status_flags);
        ESP_ERROR_CHECK(spi_slave_queue_trans(SPI_HOST_ID, &t, portMAX_DELAY));

        // 2. Alert the Pi that data is waiting
        gpio_set_level(INTERRUPT_PIN, 0);
        ESP_LOGI(TAG, "INT line asserted (LOW)");

        // 3. Wait for the Pi (Master) to clock it out
        spi_slave_transaction_t *ret_trans;
        ESP_ERROR_CHECK(spi_slave_get_trans_result(SPI_HOST_ID, &ret_trans, portMAX_DELAY));
        ESP_LOGI(TAG, "Transaction complete - data transmitted");

        // 4. De-assert interrupt line
        gpio_set_level(INTERRUPT_PIN, 1);
        ESP_LOGI(TAG, "INT line de-asserted (HIGH)");

        vTaskDelay(pdMS_TO_TICKS(SEND_PERIOD_MS));
    }
}