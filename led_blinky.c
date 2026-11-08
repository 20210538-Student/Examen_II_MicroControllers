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
 * Variables del pulsador
 ******************************************************************************/
static volatile bool check_bton = false;
static bool mode_run = true;
static bool mode_pending = false;
/*******************************************************************************
 * Variables del UART nonblocking RECEPTOR
 ******************************************************************************/
static uint8_t texto_run[]   = "RUN\r\n";
static uint8_t texto_pause[] = "PAUSE\r\n";

static lpuart_handle_t uart_handle; //static para que su memoria siga disponible mientras el driver transmite datos
static volatile bool UART_OCUPED = false; //volatile para que el compilador no optimice o modifique la variable en la compilacion y la lea siempre de memoria

static uint8_t encendido[] = "ENCENDIDO\r\n";
static uint8_t apagado[]   = "APAGADO\r\n";

/*******************************************************************************
 * Variable del CTIMER base del tiepo
 ******************************************************************************/
volatile uint32_t tiempo_ms = 0U;
/*!
 * @brief Main function
 */

/*******************************************************************************
 * Code
 ******************************************************************************/
void BTON_IRQHANDLER(void)
{
    uint32_t flags = GPIO_GpioGetInterruptFlags(BOARD_INITPINS_SW2_GPIO);

    if ((flags & BOARD_INITPINS_SW2_GPIO_PIN_MASK) != 0U)
    {
        GPIO_PortClearInterruptFlags(BOARD_INITPINS_SW2_GPIO, BOARD_INITPINS_SW2_GPIO_PIN_MASK);
        check_bton = true;
    }
}


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

static bool UART_sendtext(uint8_t *texto)
{
    if (UART_OCUPED || texto[0] == '\0')
    {
        return false;
    }

    lpuart_transfer_t message = {0};
    message.data = texto;
    message.dataSize = strlen((const char *)texto);

    UART_OCUPED = true;

    if (LPUART_TransferSendNonBlocking(
            LPUART0, &uart_handle, &message) != kStatus_Success)
    {
        UART_OCUPED = false;
        return false;
    }

    return true;
}
static bool SW2_VALID(uint32_t pulse_now)
{
    static uint32_t last_state = 1U;
    static uint32_t stable = 1U;
    static uint32_t from = 0U;

    if (!check_bton)
    {
        return false;
    }

    uint32_t lectura = GPIO_PinRead(BOARD_INITPINS_SW2_GPIO, BOARD_INITPINS_SW2_GPIO_PIN);

    if (lectura != last_state)
    {
        last_state = lectura;
        from = pulse_now;
    }

    if ((lectura != stable) &&
        ((uint32_t)(pulse_now - from) >= 30U))
    {
        stable = lectura;

        if (stable == 0U)
        {
            return true;  /* Pulsación confirmada. */
        }
    }

    return false;
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

        if (SW2_VALID(now))
        {
            mode_run = !mode_run;
            mode_pending = true;
        }

        if (mode_pending)
        {
            if (UART_sendtext(mode_run ? texto_run : texto_pause))
            {
                mode_pending = false;
            }
        }

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
            if (!mode_pending)
            {
                UART_sendtext(led_on ? encendido : apagado);
            }
        }
    }
}
