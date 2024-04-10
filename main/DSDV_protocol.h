#ifndef DSDV_PROTOCOL_H
#define DSDV_PROTOCOL_H

#include "networking_utils.h"


#define MAX_NODES           10
#define BROADCASTING_PERIOD 5000 // [ms]


void start_dsdv_routing();
esp_err_t transmit_user_data(uint8_t *mac_addr, uint8_t *data, int data_len);

#endif