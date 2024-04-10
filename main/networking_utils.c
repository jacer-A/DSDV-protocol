#include "networking_utils.h"

#define ESPNOW_MAXDELAY 512
#define PACKET_PERIOD 1000

static const char *TAG = "networking_utils";

static QueueHandle_t s_example_espnow_queue;
SemaphoreHandle_t Mutex_send_param; // ´send_param´ shouldn't be accessed concurrently

example_espnow_send_param_t *send_param;

static void example_espnow_deinit();

/* WiFi should start before using ESPNOW */
static void example_wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(ESPNOW_WIFI_MODE) );
    ESP_ERROR_CHECK( esp_wifi_start());
    ESP_ERROR_CHECK( esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

#if CONFIG_ESPNOW_ENABLE_LONG_RANGE
    ESP_ERROR_CHECK( esp_wifi_set_protocol(ESPNOW_WIFI_IF, WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G|WIFI_PROTOCOL_11N|WIFI_PROTOCOL_LR) );
#endif
}

/* ESPNOW sending or receiving callback function is called in WiFi task.
 * Users should not do lengthy operations from this task. Instead, post
 * necessary data to a queue and handle it from a lower priority task. */
static void example_espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    example_espnow_event_t evt;
    example_espnow_event_send_cb_t *send_cb = &evt.info.send_cb;

    if (mac_addr == NULL) {
        ESP_LOGE(TAG, "Send cb arg error");
        return;
    }

    evt.id = EXAMPLE_ESPNOW_SEND_CB;
    memcpy(send_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    send_cb->status = status;
    if (xQueueSend(s_example_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Send send queue fail");
    }
}

static void example_espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    example_espnow_event_t evt;
    example_espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
    uint8_t * mac_addr = recv_info->src_addr;

    if (mac_addr == NULL || data == NULL || len <= 0) {
        ESP_LOGE(TAG, "Receive cb arg error");
        return;
    }

    evt.id = EXAMPLE_ESPNOW_RECV_CB;
    memcpy(recv_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    recv_cb->data = malloc(len);
    if (recv_cb->data == NULL) {
        ESP_LOGE(TAG, "Malloc receive data fail");
        return;
    }
    memcpy(recv_cb->data, data, len);
    recv_cb->data_len = len;
    if (xQueueSend(s_example_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Send receive queue fail");
        free(recv_cb->data);
    }
}

static void add_peer(uint8_t* mac_addr, bool encrypt)
{
    /* If MAC address does not exist in peer list, add it to peer list. */
    if (esp_now_is_peer_exist(mac_addr) == false) {
        esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
        if (peer == NULL) {
            ESP_LOGE(TAG, "Malloc peer information fail");
            esp_now_deinit();
            vTaskDelete(NULL);
        }
        memset(peer, 0, sizeof(esp_now_peer_info_t));
        peer->channel = CONFIG_ESPNOW_CHANNEL;
        peer->ifidx = ESPNOW_WIFI_IF;
        peer->encrypt = encrypt;
        if(encrypt){
            memcpy(peer->lmk, CONFIG_ESPNOW_LMK, ESP_NOW_KEY_LEN);
        }
        memcpy(peer->peer_addr, mac_addr, ESP_NOW_ETH_ALEN);
        ESP_ERROR_CHECK( esp_now_add_peer(peer) );
        ESP_LOGI(TAG, "See peer for the first time and add it to my list.");
        free(peer);
    } /*else {
        ESP_LOGI(TAG, "Already known peer.");
    }*/
}

esp_err_t transmit_data(uint8_t *mac_addr, uint8_t *data, int data_len, bool encrypt, bool is_userData)
{
    if (xSemaphoreTake(Mutex_send_param, portMAX_DELAY) == pdTRUE) { // critical because ´send_param´ shouldn't be accessed concurrently
        /* Add peer information to peer list. */
        add_peer(mac_addr, encrypt);
            
        /* prepare the data to be sent. */
        memcpy(send_param->dest_mac, mac_addr, ESP_NOW_ETH_ALEN);
        example_espnow_data_t *buf = (example_espnow_data_t *)send_param->buffer;
        buf->is_userData = is_userData;
        memcpy(buf->payload, data, data_len);
        buf->crc = 0;
        int len= data_len + sizeof(example_espnow_data_t);
        buf->crc = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, len);
        
        /* send the data. */
        //ESP_LOGI(TAG, "sending data to "MACSTR"", MAC2STR(send_param->dest_mac));
        if (esp_now_send(send_param->dest_mac, send_param->buffer, len) != ESP_OK) {
            ESP_LOGE(TAG, "Send error");
            example_espnow_deinit();
            xSemaphoreGive(Mutex_send_param);
            return ESP_FAIL;
            //vTaskDelete(NULL);////////////////////////////
        }
        xSemaphoreGive(Mutex_send_param);
        return ESP_OK;
    }
    return ESP_FAIL;
}

static int check_data_correctness(uint8_t *data, int data_len)
{
	example_espnow_data_t *buf = (example_espnow_data_t *)data;
	uint16_t crc, crc_cal = 0;

	if (data_len < sizeof(example_espnow_data_t)) {
		ESP_LOGE(TAG, "Receive ESPNOW data too short, len:%d", data_len);
		return -1;
	}

	crc = buf->crc;
	buf->crc = 0;
	crc_cal = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, data_len);

	if (crc_cal == crc) {
		return 0;
	}

	return -1;
}

void handle_communication_events(void *pvParameter)
{
    event_handler_t *event_handler= (event_handler_t*) pvParameter;

	example_espnow_event_t evt;

	while (xQueueReceive(s_example_espnow_queue, &evt, portMAX_DELAY) == pdTRUE) {
		switch (evt.id) {
		case EXAMPLE_ESPNOW_SEND_CB:
		{
            example_espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
			event_handler->do_on_send_event(send_cb);
			break;
		}
		case EXAMPLE_ESPNOW_RECV_CB:
		{
			example_espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
            if(check_data_correctness(recv_cb->data, recv_cb->data_len))
				ESP_LOGI(TAG, "Receive error data from: "MACSTR"", MAC2STR(recv_cb->mac_addr));
			else
				event_handler->do_on_receive_event(recv_cb);
			free(recv_cb->data);
			break;
		}
		default:
			ESP_LOGE(TAG, "Callback type error: %d", evt.id);
			break;
		}
	}
}


static esp_err_t example_espnow_init(void)
{
    s_example_espnow_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(example_espnow_event_t));
    if (s_example_espnow_queue == NULL) {
        ESP_LOGE(TAG, "Create mutex fail");
        return ESP_FAIL;
    }

    Mutex_send_param=  xSemaphoreCreateMutex();

    /* Initialize ESPNOW and register sending and receiving callback function. */
    ESP_ERROR_CHECK( esp_now_init() );
    ESP_ERROR_CHECK( esp_now_register_send_cb(example_espnow_send_cb) );
    ESP_ERROR_CHECK( esp_now_register_recv_cb(example_espnow_recv_cb) );
#if CONFIG_ESP_WIFI_STA_DISCONNECTED_PM_ENABLE
    ESP_ERROR_CHECK( esp_now_set_wake_window(65535) );
#endif
    /* Set primary master key. */
    ESP_ERROR_CHECK( esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK) );
              
    /* Initialize sending parameters. */
	send_param = malloc(sizeof(example_espnow_send_param_t));
	if (send_param == NULL) {
		ESP_LOGE(TAG, "Malloc send parameter fail");
		vSemaphoreDelete(s_example_espnow_queue);
		esp_now_deinit();
		return ESP_FAIL;
	}
	memset(send_param, 0, sizeof(example_espnow_send_param_t));
	send_param->buffer = malloc(ESP_NOW_MAX_DATA_LEN);
	if (send_param->buffer == NULL) {
		ESP_LOGE(TAG, "Malloc send buffer fail");
		free(send_param);
		vSemaphoreDelete(s_example_espnow_queue);
		esp_now_deinit();
		return ESP_FAIL;
	}

    return ESP_OK;
}

static void example_espnow_deinit()
{
    free(send_param->buffer);
    free(send_param);
    vSemaphoreDelete(s_example_espnow_queue);
    vSemaphoreDelete(Mutex_send_param);
    esp_now_deinit();
}

void setup_connectivity()
{
    /* WiFi should start before using ESPNOW */
    example_wifi_init();
    example_espnow_init();
}


