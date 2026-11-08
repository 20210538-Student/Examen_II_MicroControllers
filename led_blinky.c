/*
 * Copyright 2019 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "board.h"
#include "app.h"
#include "fsl_debug_console.h"
#include "fsl_lpuart.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "peripherals.h"
#include "fsl_ctimer.h"
#include "fsl_lpadc.h"
#include <stdint.h>
#include <stdbool.h>


/*******************************************************************************
 * Definitions
 ******************************************************************************/

/*******************************************************************************
 * Prototypes
 ******************************************************************************/

/*******************************************************************************
 * Variables
 ******************************************************************************/

/*******************************************************************************
 * Code
 ******************************************************************************/
static lpuart_handle_t uart_handle; //static para que su memoria siga disponible mientras el driver transmite datos
static volatile bool UART_OCUPED = false; //volatile para que el compilador no optimice o modifique la variable en la compilacion y la lea siempre de memoria

static uint8_t encendido[] = "ENCENDIDO\r\n";
static uint8_t apagado[]   = "APAGADO\r\n";

/*!
 * @brief Main function
 */
volatile uint32_t tiempo_ms = 0U;

void CTIMER0_TimebaseCB(uint32_t flags) //Base temporal de tiempo en ms
{
    (void)flags;
    tiempo_ms++;
}

static void UART_Callback(LPUART_Type *base, lpuart_handle_t *handle, status_t status, void *userData)
{
    (void)base;
    (void)handle;
    (void)userData;

    if (status == kStatus_LPUART_TxIdle)
    {
        UART_OCUPED = false;
    }
}

int main(void)
{
    BOARD_InitHardware(); // Inicializacion de hardware y perifericos

    /* Inicializo en bajo. */
    GPIO_PinWrite(BOARD_INITPINS_Activity_LED_GPIO, BOARD_INITPINS_Activity_LED_GPIO_PIN, 1U);
    GPIO_PinWrite(BOARD_INITPINS_Alarm_LED_GPIO, BOARD_INITPINS_Alarm_LED_GPIO_PIN, 1U);
    LPUART_TransferCreateHandle(LPUART0, &uart_handle, UART_Callback, NULL);

    uint32_t last_change = tiempo_ms;
    uint32_t last_telemetry = tiempo_ms;
    bool led_on = false;

    while (1)
    {
        /* Una lectura del contador para esta iteración. */
        uint32_t now = tiempo_ms;

        if ((uint32_t)(now - last_change) >= 500U)
        {
            last_change += 500U;

            GPIO_PortToggle(
                BOARD_INITPINS_Activity_LED_GPIO,
                BOARD_INITPINS_Activity_LED_GPIO_PIN_MASK);
            led_on = !led_on;
        }
        if ((uint32_t)(now - last_telemetry) >= 1000U)
        {
            last_telemetry += 1000U;
            if (!UART_OCUPED)
            {
                lpuart_transfer_t message = {0};

                if (led_on)
                {
                    message.data = encendido;
                    message.dataSize = sizeof(encendido) - 1U;
                }
                else
                {
                    message.data = apagado;
                    message.dataSize = sizeof(apagado) - 1U;
                }

                UART_OCUPED = true;

                if (LPUART_TransferSendNonBlocking(
                        LPUART0, &uart_handle, &message) != kStatus_Success)
                {
                    UART_OCUPED = false;
                }
            }
        }
    }
}
