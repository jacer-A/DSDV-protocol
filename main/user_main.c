#include "DSDV_protocol.h"


void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    // start DSDV routing
    xTaskCreate(start_dsdv_routing, "start_dsdv_routing", 4096, NULL, 4, NULL); vTaskDelay(500 / portTICK_PERIOD_MS);

    
    uint8_t s_example_broadcast_mac[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t mac_addr_0[ESP_NOW_ETH_ALEN] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    uint8_t mac_addr_1[ESP_NOW_ETH_ALEN] = { 0x34, 0x85, 0x18, 0xB9, 0x1B, 0x00 };
    uint8_t mac_addr_2[ESP_NOW_ETH_ALEN] = { 0x48, 0x27, 0xE2, 0x3B, 0x3A, 0xB4 };
    
    char usr_msg[]= "Bye World!";
    char usr_msg_0[]= "Valorant? ew";
    char usr_msg_1[]= "did it work?";
    char usr_msg_2[]= "e^(-iπ)+1=0";
    
    // send user messages
    while(true)
    {
        transmit_user_data(s_example_broadcast_mac, (uint8_t*)usr_msg, sizeof(usr_msg)); vTaskDelay(3000 / portTICK_PERIOD_MS);
        transmit_user_data(mac_addr_0, (uint8_t*)usr_msg_0, sizeof(usr_msg_0)); vTaskDelay(3000 / portTICK_PERIOD_MS);
        transmit_user_data(mac_addr_1, (uint8_t*)usr_msg_1, sizeof(usr_msg_1)); vTaskDelay(3000 / portTICK_PERIOD_MS);
        transmit_user_data(mac_addr_2, (uint8_t*)usr_msg_2, sizeof(usr_msg_2)); vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
}



