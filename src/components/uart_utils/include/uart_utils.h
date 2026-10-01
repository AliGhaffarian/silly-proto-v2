#pragma once

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "soc/gpio_num.h"

#define READ_SLEEP_TICKS 100

struct locked_uart_port {
    uart_port_t port;
    SemaphoreHandle_t port_mu;
};
