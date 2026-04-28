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

    uint32_t value = 0x12345678;
    WORD_ALIGNED_ATTR uint8_t tx_data[4]; 
    WORD_ALIGNED_ATTR uint8_t rx_data[4]; // Required by ESP-IDF even if not reading

    while (true) {
        // Prepare endianness (matches your RPi driver's be32_to_cpup)
        tx_data[0] = (uint8_t)(value >> 24);
        tx_data[1] = (uint8_t)(value >> 16);
        tx_data[2] = (uint8_t)(value >> 8);
        tx_data[3] = (uint8_t)value;

        spi_slave_transaction_t t = {0};
        t.length = sizeof(tx_data) * 8;
        t.tx_buffer = tx_data;
        t.rx_buffer = rx_data;

        // 1. Queue the transaction in the hardware shift register
        ESP_ERROR_CHECK(spi_slave_queue_trans(SPI_HOST_ID, &t, portMAX_DELAY));

        // 2. Alert the Pi that data is waiting
        gpio_set_level(INTERRUPT_PIN, 0);

        // 3. Wait for the Pi (Master) to clock it out
        spi_slave_transaction_t *ret_trans;
        ESP_ERROR_CHECK(spi_slave_get_trans_result(SPI_HOST_ID, &ret_trans, portMAX_DELAY));

        // 4. De-assert interrupt line
        gpio_set_level(INTERRUPT_PIN, 1);

        ESP_LOGI(TAG, "Sent 0x%08" PRIx32 " to Pi", value);

        value++;
        vTaskDelay(pdMS_TO_TICKS(SEND_PERIOD_MS));
    }
}