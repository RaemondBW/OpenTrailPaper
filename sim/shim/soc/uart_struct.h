#pragma once
#include <stdint.h>
typedef struct { struct { uint32_t val; } clk_conf; } uart_dev_t;
extern uart_dev_t UART0, UART1, UART2;
#ifndef REG_READ
#define REG_READ(r) ((uint32_t)(uintptr_t)(r) * 0u)
#endif
#ifdef __cplusplus
extern const uint32_t GPIO_PIN_MUX_REG[49];
#else
extern const uint32_t GPIO_PIN_MUX_REG[49];
#endif
#ifndef SIM_UART_ERRORS
#define SIM_UART_ERRORS
enum { UART_NO_ERROR, UART_BREAK_ERROR, UART_BUFFER_FULL_ERROR, UART_FIFO_OVF_ERROR,
       UART_FRAME_ERROR, UART_PARITY_ERROR };
typedef int hardwareSerial_error_t;
#endif
