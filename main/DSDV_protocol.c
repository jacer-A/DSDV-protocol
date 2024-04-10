#include "esp_timer.h"

#include "DSDV_protocol.h"


typedef struct {
    uint8_t destination_addr[ESP_NOW_ETH_ALEN];
    uint8_t nextHop_addr[ESP_NOW_ETH_ALEN];
    uint8_t hop_count;
    uint16_t seq_num;
    int64_t last_update_time;
} __attribute__((packed)) RoutingEntry_t;


typedef struct {
    uint8_t dest_mac[ESP_NOW_ETH_ALEN];
    uint8_t payload[0];
} __attribute__((packed)) user_data_t;


static const char *TAG = "DSDV_protocol";

static uint8_t s_example_broadcast_mac[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint8_t own_mac_addr[ESP_NOW_ETH_ALEN];

static RoutingEntry_t routing_table[MAX_NODES]= {0};
static RoutingEntry_t routing_table_old[MAX_NODES]= {0};
static int entries_nbr= 0;
    
static void update_routing_table(RoutingEntry_t recvd_routing_entry, uint8_t *nextHop_addr);
static void send_incremental_updates();
inline static void print_routing_table();


static void do_on_send_event(example_espnow_event_send_cb_t *send_cb)
{
	return;
    ESP_LOGI(TAG, "Sent data to "MACSTR", status1: %d", MAC2STR(send_cb->mac_addr), send_cb->status);
}

static void do_on_receive_event(example_espnow_event_recv_cb_t *recv_cb)
{   
    // parse received data
    uint8_t *nextHop_addr = recv_cb->mac_addr;
    example_espnow_data_t *data = (example_espnow_data_t *)recv_cb->data;
    uint8_t is_userData= data->is_userData;
    int payload_len = recv_cb->data_len - sizeof(example_espnow_data_t);
    uint8_t *payload= malloc(payload_len);
    if (payload==NULL) {
        ESP_LOGW(TAG, "do_on_receive_event(): ERROR: malloc failed");
        return;
    }
    memcpy(payload, data->payload, payload_len);

    if (is_userData) // forward user message if necessary
    {
        user_data_t *recvd_user_data= (user_data_t*) payload;
        if (memcmp(own_mac_addr, recvd_user_data->dest_mac, ESP_NOW_ETH_ALEN) == 0 || memcmp(s_example_broadcast_mac, recvd_user_data->dest_mac, ESP_NOW_ETH_ALEN) == 0)
            ESP_LOGW(TAG, "Received user message from: "MACSTR", len: %d, content: %s", MAC2STR(recv_cb->mac_addr), payload_len-sizeof(user_data_t), ((char*)recvd_user_data)+ESP_NOW_ETH_ALEN);//(char*)recvd_user_data->payload);
        else
        {
            int index;
            for (index= 1; index < entries_nbr; index++)
                if (memcmp(routing_table[index].destination_addr, recvd_user_data->dest_mac, ESP_NOW_ETH_ALEN) == 0)
                    break;
            
            if (index == entries_nbr || routing_table[index].hop_count == UINT8_MAX)
                ESP_LOGW(TAG, "Failed to find a path to "MACSTR"", MAC2STR(recvd_user_data->dest_mac));
            else
            {
                ESP_LOGW(TAG, "Forwarding user message to "MACSTR". Number of hops left: %d", MAC2STR(routing_table[index].nextHop_addr), routing_table[index].hop_count);
                transmit_data(routing_table[index].nextHop_addr, payload, payload_len, true, true);
            }
        }
    }
    else // update table if necessary
    {
        RoutingEntry_t *recvd_routing_entry= (RoutingEntry_t*) payload;
        update_routing_table(*recvd_routing_entry, nextHop_addr);

        ESP_LOGI(TAG, "Received routing entry from: "MACSTR", len: %d, content:", MAC2STR(recv_cb->mac_addr), payload_len);
        ESP_LOGI("", "| "MACSTR" | "MACSTR" | %-10d | %-10d |", 
            MAC2STR(recvd_routing_entry->destination_addr),
            MAC2STR(recvd_routing_entry->nextHop_addr),
            recvd_routing_entry->hop_count,
            recvd_routing_entry->seq_num
        );
    }
    free(payload);
}


void start_dsdv_routing(void)
{
    // add own routing entry to table
    esp_read_mac(own_mac_addr, ESP_MAC_WIFI_SOFTAP);
    own_mac_addr[5]--;
    RoutingEntry_t *own_routing_entry= &routing_table[0];
    memcpy(own_routing_entry->destination_addr, own_mac_addr, ESP_NOW_ETH_ALEN);
    memcpy(own_routing_entry->nextHop_addr, own_mac_addr, ESP_NOW_ETH_ALEN);
    own_routing_entry->hop_count= 0;
    own_routing_entry->seq_num= 0;
    own_routing_entry->last_update_time= 0;
    entries_nbr++;

    // start wifi
    setup_connectivity();

    // create task to handle send and receive events
    event_handler_t event_handler;
    event_handler.do_on_send_event= &do_on_send_event;
    event_handler.do_on_receive_event= &do_on_receive_event;
    xTaskCreate(handle_communication_events, "handle_communication_events", 4096, (void*)&event_handler, 4, NULL);
    
    while(true)
    {
        // broadcast own routing entry
        own_routing_entry->seq_num += 2;
        print_routing_table();
        transmit_data(s_example_broadcast_mac, (uint8_t*)own_routing_entry, sizeof(RoutingEntry_t), false, false);
        vTaskDelay(BROADCASTING_PERIOD / portTICK_PERIOD_MS);

        // send incremental routing table updates
        send_incremental_updates();

        // check for stale neighbours
        int64_t current_time= esp_timer_get_time();
        for (int i = 0; i < entries_nbr; i++)
            if (routing_table[i].hop_count == 1)
                if (current_time - routing_table[i].last_update_time > BROADCASTING_PERIOD * 2 * 1000) {
                    routing_table[i].hop_count= UINT8_MAX;
                    routing_table[i].seq_num += 1;
                }
    }
}

esp_err_t transmit_user_data(uint8_t *mac_addr, uint8_t *data, int data_len)
{
    esp_err_t ret=ESP_FAIL;

    user_data_t *user_data = malloc(sizeof(user_data_t) + data_len);
    if (user_data == NULL) {
        ESP_LOGW(TAG, "transmit_user_data(): ERROR: malloc failed");
        return ret;
    }
    memcpy(user_data->dest_mac, mac_addr, ESP_NOW_ETH_ALEN);
    memcpy(user_data->payload, data, data_len);
    
    if (memcmp(own_mac_addr, mac_addr, ESP_NOW_ETH_ALEN) == 0)
    {
        ret= ESP_FAIL;
    }
    else if (memcmp(s_example_broadcast_mac, mac_addr, ESP_NOW_ETH_ALEN) == 0)
    {
        ESP_LOGW(TAG, "Broadcasting user message");
        ret= transmit_data(s_example_broadcast_mac, (uint8_t*)user_data, sizeof(user_data_t) + data_len, false, true);
    }
    else
    {
        int index;
        for (index= 1; index < entries_nbr; index++)
            if (memcmp(routing_table[index].destination_addr, mac_addr, ESP_NOW_ETH_ALEN) == 0)
                break;
        
        if (index == entries_nbr || routing_table[index].hop_count == UINT8_MAX)
            ESP_LOGW(TAG, "Failed to find a path to "MACSTR"", MAC2STR(mac_addr));
        else
        {
            ESP_LOGW(TAG, "Forwarding user message to "MACSTR". Number of hops left: %d", MAC2STR(routing_table[index].nextHop_addr), routing_table[index].hop_count);
            ret= transmit_data(routing_table[index].nextHop_addr, (uint8_t*)user_data, sizeof(user_data_t) + data_len, true, true);
        }
    }
    free(user_data);
    return ret;
}


static void update_routing_table(RoutingEntry_t recvd_routing_entry, uint8_t *nextHop_addr)
{    
    int index;
    for (index = 0; index < entries_nbr; index++)
        if (memcmp(routing_table[index].destination_addr, recvd_routing_entry.destination_addr, ESP_NOW_ETH_ALEN) == 0) // known destination
            break;
    
    if (index == entries_nbr)
    { 
        // add new entry to table
        RoutingEntry_t *new_routing_entry= &routing_table[index];
        memcpy(new_routing_entry->destination_addr, recvd_routing_entry.destination_addr, ESP_NOW_ETH_ALEN);
        memcpy(new_routing_entry->nextHop_addr, nextHop_addr, ESP_NOW_ETH_ALEN);
        new_routing_entry->hop_count= recvd_routing_entry.hop_count + 1;
        new_routing_entry->seq_num= recvd_routing_entry.seq_num;
        new_routing_entry->last_update_time= esp_timer_get_time();
        entries_nbr++;
    }
    else
    {
        // Update existing entry if necessary
        RoutingEntry_t *curnt_routing_entry= &routing_table[index];
        if (recvd_routing_entry.seq_num > curnt_routing_entry->seq_num)
        {
            if (index == 0)
                curnt_routing_entry->seq_num= recvd_routing_entry.seq_num % 2 ? recvd_routing_entry.seq_num+1 : recvd_routing_entry.seq_num;
            else
            {
                memcpy(curnt_routing_entry->nextHop_addr, nextHop_addr, ESP_NOW_ETH_ALEN);
                curnt_routing_entry->hop_count= recvd_routing_entry.hop_count + 1;
                curnt_routing_entry->seq_num= recvd_routing_entry.seq_num;
                curnt_routing_entry->last_update_time= esp_timer_get_time();
            }
        }
        else if (recvd_routing_entry.seq_num == curnt_routing_entry->seq_num)
        {
            if (recvd_routing_entry.hop_count + 1 < curnt_routing_entry->hop_count)
            {
                memcpy(curnt_routing_entry->nextHop_addr, nextHop_addr, ESP_NOW_ETH_ALEN);
                curnt_routing_entry->hop_count= recvd_routing_entry.hop_count + 1;
                curnt_routing_entry->last_update_time= esp_timer_get_time();
            }
            else if (memcmp(curnt_routing_entry->nextHop_addr, nextHop_addr, ESP_NOW_ETH_ALEN) == 0)
            {
                curnt_routing_entry->hop_count= recvd_routing_entry.hop_count + 1;
                curnt_routing_entry->last_update_time= esp_timer_get_time();
            }                
        }
        else if (memcmp(curnt_routing_entry->destination_addr, nextHop_addr, ESP_NOW_ETH_ALEN) == 0)
        {
            memcpy(curnt_routing_entry->nextHop_addr, nextHop_addr, ESP_NOW_ETH_ALEN);
            curnt_routing_entry->hop_count= recvd_routing_entry.hop_count + 1;
            curnt_routing_entry->seq_num += curnt_routing_entry->seq_num % 2 ? 1 : 0;
            curnt_routing_entry->last_update_time= esp_timer_get_time();
        }   
    }
}

static void send_incremental_updates()
{
    for (int i = 1; i < entries_nbr; i++) {
        if (memcmp((uint8_t*)&routing_table[i], (uint8_t*)&routing_table_old[i], sizeof(RoutingEntry_t)) != 0) {
            for (int j = 1; j < entries_nbr; j++)
                if (routing_table[j].hop_count == 1)// && (i!=j || routing_table_old[i].seq_num % 2))
                    transmit_data(routing_table[j].destination_addr, (uint8_t*)&routing_table[i], sizeof(RoutingEntry_t), true, false);
            vTaskDelay(BROADCASTING_PERIOD / MAX_NODES / portTICK_PERIOD_MS);
            memcpy((uint8_t*)&routing_table_old[i], (uint8_t*)&routing_table[i], sizeof(RoutingEntry_t));
        }
    }
}

inline static void print_routing_table()
{
    ESP_LOGI("", "\nRouting Table:");
    ESP_LOGI("", "| %-17s | %-17s | %-10s | %-10s | %-16s |", "Destination", "Next Hop", "Hop Count", "Seq. Num", "Last UpdateTime");
    ESP_LOGI("", "|-------------------|-------------------|------------|------------|------------------|");
    for (int i = 0; i < entries_nbr; i++)
        ESP_LOGI("", "| "MACSTR" | "MACSTR" | %-10d | %-10d | %-16lld |", 
            MAC2STR(routing_table[i].destination_addr),
            MAC2STR(routing_table[i].nextHop_addr),
            routing_table[i].hop_count,
            routing_table[i].seq_num,
            routing_table[i].last_update_time / 1000000
        );
    ESP_LOGI("", "\n");
}
