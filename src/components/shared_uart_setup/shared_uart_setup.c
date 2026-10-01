#include "include/shared_uart_setup.h"
#include "driver/uart.h"
#include "soc/gpio_num.h"
#include <inttypes.h>

const int uart_buffer_size = 1024 * 2;

const uart_config_t shared_uart_config = {
    .baud_rate = SHARED_BAUD_RATE,
    .data_bits = SHARED_DATA_BITS,
    .parity = SHARED_PARITY,
    .stop_bits = SHARED_STOP_BITS,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
};

void shared_setup_uart(QueueHandle_t *qh_ptr, int queue_size)
{
    ESP_ERROR_CHECK(uart_driver_install(
        SHARED_UART_NUM,
        uart_buffer_size,
        uart_buffer_size,
        queue_size,
        qh_ptr,
        0));

    ESP_ERROR_CHECK(uart_param_config(SHARED_UART_NUM, &shared_uart_config));

    ESP_ERROR_CHECK(uart_set_pin(
        SHARED_UART_NUM,
        TX_GPIO_NUM,
        RX_GPIO_NUM,
        RTS_GPIO_NUM,
        CTS_GPIO_NUM,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE));

    ESP_ERROR_CHECK(
        uart_set_mode(SHARED_UART_NUM, UART_MODE_RS485_COLLISION_DETECT));
}
