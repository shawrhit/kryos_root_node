#include <inttypes.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_mesh.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "psa/crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "kryos_root_node";

/* SPI Pins per KryOS Bible */
#define SPI_HOST_ID SPI3_HOST
#define SPI_MOSI_PIN GPIO_NUM_23  // RPi MOSI -> ESP MOSI
#define SPI_MISO_PIN GPIO_NUM_19  // ESP MISO -> RPi MISO
#define SPI_SCLK_PIN GPIO_NUM_18  // RPi SCLK -> ESP SCLK
#define SPI_CS_PIN   GPIO_NUM_4   // RPi CE0  -> ESP CS
#define INTERRUPT_PIN GPIO_NUM_25 // Active Low to RPi

/* Mesh Config per Bible */
#define MESH_ID_0  0x4b
#define MESH_ID_1  0x52
#define MESH_ID_2  0x59
#define MESH_ID_3  0x4f
#define MESH_ID_4  0x53
#define MESH_ID_5  0x31
#define MESH_CHANNEL 6
#define MESH_SOFTAP_PASSWD "KryOSMesh"

#define KRYOS_PROTOCOL_MAGIC           0x4B59u
#define KRYOS_PROTOCOL_VERSION         1u
#define KRYOS_MSG_CONSENSUS            3
#define KRYOS_MSG_LEADER_ELECT         4
#define KRYOS_MSG_LEADER_SELECT        5

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t node_id;
    int16_t rssi_dbm;
    uint8_t mac[6];
} kryos_election_frame_t;

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t selected_node_id;
    uint8_t reserved[3];
} kryos_select_frame_t;

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t version;
    uint8_t type;
    uint32_t round_id;
    uint32_t timestamp_s;
    int32_t consensus_milli_c;
    uint8_t node_mask;
    uint8_t rejected_mask;
    uint8_t fault_mask;
    uint8_t verified_count;
    uint8_t quorum_ok;
    uint8_t network_quality;
    uint8_t status;
    uint8_t reserved;
    uint8_t hmac[32];
} kryos_consensus_frame_t;

static const uint8_t KRYOS_MASTER_PSK[32] = {
    0x4b, 0x72, 0x79, 0x4f, 0x53, 0x5f, 0x4d, 0x61,
    0x73, 0x74, 0x65, 0x72, 0x5f, 0x53, 0x65, 0x63,
    0x72, 0x65, 0x74, 0x5f, 0x4b, 0x65, 0x79, 0x5f,
    0x32, 0x30, 0x32, 0x36, 0x5f, 0x30, 0x35, 0x5f
};

typedef struct __attribute__((packed)) {
    uint32_t round_id;
    uint32_t timestamp;
    float temp_c;
    uint8_t node_mask;
    uint8_t rejected_mask;
    uint8_t status_flags;
    uint8_t hmac[32];
} payload_t;

/* Node PSK for Mesh Internal HMAC-SHA256 (32 bytes) */
static const uint8_t KRYOS_NODE_PSK[32] = {
    0x4b, 0x72, 0x79, 0x4f, 0x53, 0x5f, 0x4e, 0x6f,
    0x64, 0x65, 0x5f, 0x53, 0x65, 0x63, 0x72, 0x65,
    0x74, 0x5f, 0x4b, 0x65, 0x79, 0x5f, 0x32, 0x30,
    0x32, 0x36, 0x5f, 0x30, 0x35, 0x5f, 0x32, 0x32 
};

static esp_err_t verify_mesh_hmac(const void *data, size_t data_len, const uint8_t *hmac_in)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id;
    psa_status_t status;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, 256);
    status = psa_import_key(&attributes, KRYOS_NODE_PSK, 32, &key_id);
    if (status != PSA_SUCCESS) return ESP_FAIL;
    status = psa_mac_verify(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), data, data_len, hmac_in, 32);
    psa_destroy_key(key_id);
    return (status == PSA_SUCCESS) ? ESP_OK : ESP_FAIL;
}
static uint8_t s_rx_buf[256];
static bool s_mesh_started = false;
static uint8_t s_selected_leader_id = 0;
static int16_t s_best_candidate_rssi = -120;
static uint32_t s_last_candidate_rx_s = 0;
static uint32_t s_leadership_start_s = 0;

static uint32_t now_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

static esp_err_t compute_spi_hmac(const void *data, size_t data_len, uint8_t *hmac_out)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id;
    psa_status_t status;
    size_t hmac_len;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, 256);
    status = psa_import_key(&attributes, KRYOS_MASTER_PSK, 32, &key_id);
    if (status != PSA_SUCCESS) return ESP_FAIL;
    status = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), data, data_len, hmac_out, 32, &hmac_len);
    psa_destroy_key(key_id);
    return (status == PSA_SUCCESS) ? ESP_OK : ESP_FAIL;
}

static esp_err_t init_interrupt_pin(void)
{
    gpio_config_t io_conf = { .pin_bit_mask = 1ULL << INTERRUPT_PIN, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io_conf);
    return gpio_set_level(INTERRUPT_PIN, 1);
}

static esp_err_t init_spi_slave(void)
{
    spi_bus_config_t buscfg = { .mosi_io_num = SPI_MOSI_PIN, .miso_io_num = SPI_MISO_PIN, .sclk_io_num = SPI_SCLK_PIN };
    spi_slave_interface_config_t slvcfg = { .mode = 0, .spics_io_num = SPI_CS_PIN, .queue_size = 1 };
    return spi_slave_initialize(SPI_HOST_ID, &buscfg, &slvcfg, SPI_DMA_DISABLED);
}

static void mesh_rx_task(void *arg)
{
    mesh_addr_t from = {0};
    mesh_data_t data = { .data = s_rx_buf, .size = sizeof(s_rx_buf) };
    int flag = 0;
    static WORD_ALIGNED_ATTR uint8_t rx_dummy[sizeof(payload_t)];
    ESP_LOGI(TAG, "Mesh RX task started");
    while (true) {
        data.size = sizeof(s_rx_buf);
        if (esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0) != ESP_OK) continue;
        
        uint8_t type = data.data[3]; // magic(2) + version(1) + type(1)
        
        // Handle Election Claims (Sensor -> Root)
        if (type == KRYOS_MSG_LEADER_ELECT) {
            kryos_election_frame_t *elect = (kryos_election_frame_t *)data.data;
            uint32_t now = now_s();
            
            bool is_current_king = (elect->node_id == s_selected_leader_id);
            bool king_dead = (s_selected_leader_id != 0 && (now - s_last_candidate_rx_s > 15));
            bool term_expired = (now - s_leadership_start_s > 60);
            
            if (is_current_king) {
                s_last_candidate_rx_s = now;
                s_best_candidate_rssi = elect->rssi_dbm;
            } else if (king_dead || s_selected_leader_id == 0) {
                // King is dead or not elected yet: Elect immediately
                s_selected_leader_id = elect->node_id;
                s_best_candidate_rssi = elect->rssi_dbm;
                s_last_candidate_rx_s = now;
                s_leadership_start_s = now;
                ESP_LOGW(TAG, "KING IS DEAD OR NEW. Long live the King: Node %d (RSSI: %d dBm)", s_selected_leader_id, s_best_candidate_rssi);
            } else if (term_expired && elect->rssi_dbm > (s_best_candidate_rssi + 5)) {
                // Term expired and significantly better candidate: Switch
                s_selected_leader_id = elect->node_id;
                s_best_candidate_rssi = elect->rssi_dbm;
                s_last_candidate_rx_s = now;
                s_leadership_start_s = now;
                ESP_LOGW(TAG, "Term expired. New Leader elected: Node %d (RSSI: %d dBm)", s_selected_leader_id, s_best_candidate_rssi);
            }
            continue;
        }

        kryos_consensus_frame_t *frame = (kryos_consensus_frame_t *)data.data;
        if (frame->magic != KRYOS_PROTOCOL_MAGIC || frame->type != KRYOS_MSG_CONSENSUS) continue;

        // Security: Verify Mesh HMAC (DISABLED FOR NOW)
        /*
        if (verify_mesh_hmac(frame, offsetof(kryos_consensus_frame_t, hmac), frame->hmac) != ESP_OK) {
            ESP_LOGW(TAG, "HMAC AUTH FAIL from leader " MACSTR " - Dropping", MAC2STR(from.addr));
            continue;
        }
        */

        // In the consensus frame, we can identify the sender by their bit in the mask OR
        // we can just check if their Node ID matches our selection.
        
        ESP_LOGI(TAG, "Received consensus round %" PRIu32 " from leader " MACSTR, frame->round_id, MAC2STR(from.addr));
        ESP_LOGI(TAG, "DATA: Temp=%.3f C, Nodes=0x%02x, Rejected=0x%02x, Status=0x%02x",
                 (float)frame->consensus_milli_c / 1000.0f, frame->node_mask, 
                 frame->rejected_mask, frame->status);
        static payload_t payload;
        payload.round_id = frame->round_id;
        payload.timestamp = frame->timestamp_s;
        payload.temp_c = (float)frame->consensus_milli_c / 1000.0f;
        payload.node_mask = frame->node_mask;
        payload.rejected_mask = frame->rejected_mask;
        payload.status_flags = frame->status;
        compute_spi_hmac(&payload, offsetof(payload_t, hmac), payload.hmac);
        spi_slave_transaction_t t = { .length = sizeof(payload) * 8, .tx_buffer = &payload, .rx_buffer = rx_dummy };
        if (spi_slave_queue_trans(SPI_HOST_ID, &t, portMAX_DELAY) == ESP_OK) {
            gpio_set_level(INTERRUPT_PIN, 0);
            spi_slave_transaction_t *ret_trans;
            spi_slave_get_trans_result(SPI_HOST_ID, &ret_trans, portMAX_DELAY);
            gpio_set_level(INTERRUPT_PIN, 1);
            ESP_LOGI(TAG, "SPI transfer complete for round %" PRIu32, frame->round_id);
        }
    }
}

static void mesh_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_id == MESH_EVENT_STARTED) {
        s_mesh_started = true;
        ESP_LOGI(TAG, "MESH_STARTED - Bridge Online");
    }
}

static void initialize_mesh(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif_sta = NULL;
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(&netif_sta, NULL));
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_mesh_set_topology(MESH_TOPO_TREE));
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(3));
    ESP_ERROR_CHECK(esp_mesh_set_capacity_num(10));

    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    cfg.router.allow_router_switch = false;
    cfg.mesh_ap.max_connection = 6;
    cfg.router.ssid_len = 5;
    memcpy(cfg.router.ssid, "KRYOS", 5);
    memcpy(cfg.router.password, "KryOSMesh", 9);

    static const uint8_t mesh_id[6] = {MESH_ID_0, MESH_ID_1, MESH_ID_2, MESH_ID_3, MESH_ID_4, MESH_ID_5};
    memcpy((uint8_t *)&cfg.mesh_id, mesh_id, 6);
    cfg.channel = MESH_CHANNEL;
    memcpy((uint8_t *)&cfg.mesh_ap.password, MESH_SOFTAP_PASSWD, strlen(MESH_SOFTAP_PASSWD));

    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    ESP_ERROR_CHECK(esp_mesh_set_type(MESH_ROOT));
    ESP_ERROR_CHECK(esp_mesh_fix_root(true));
    ESP_ERROR_CHECK(esp_mesh_set_self_organized(false, false));
    ESP_ERROR_CHECK(esp_mesh_start());
}

static void kingmaker_task(void *arg)
{
    ESP_LOGI(TAG, "Kingmaker task started");
    while (true) {
        if (s_mesh_started && s_selected_leader_id != 0) {
            kryos_select_frame_t select = {
                .magic = KRYOS_PROTOCOL_MAGIC,
                .version = KRYOS_PROTOCOL_VERSION,
                .type = KRYOS_MSG_LEADER_SELECT,
                .selected_node_id = s_selected_leader_id,
            };
            
            mesh_data_t data = {
                .data = (uint8_t *)&select,
                .size = sizeof(select),
                .proto = MESH_PROTO_BIN,
                .tos = MESH_TOS_P2P,
            };
            
            static const mesh_addr_t BCAST_ADDR = { .addr = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff} };
            esp_mesh_send(&BCAST_ADDR, &data, MESH_DATA_FROMDS, NULL, 0);
            ESP_LOGD(TAG, "Broadcasting Leader Selection: Node %d", s_selected_leader_id);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== KryOS Root Node Bridge Starting ===");
    nvs_flash_init();
    psa_crypto_init();
    init_interrupt_pin();
    init_spi_slave();
    initialize_mesh();
    xTaskCreate(mesh_rx_task, "mesh_rx", 4096, NULL, 5, NULL);
    xTaskCreate(kingmaker_task, "kingmaker", 4096, NULL, 5, NULL);
}
