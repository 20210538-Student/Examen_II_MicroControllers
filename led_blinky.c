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
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>


/*******************************************************************************
 * Definitions
 ******************************************************************************/

/*******************************************************************************
 * Prototypes
 ******************************************************************************/

/*******************************************************************************
 * Variables del ADC
 ******************************************************************************/
static volatile uint16_t adc_value = 0U;
static volatile bool adc_valid = false;
static volatile uint32_t porcentaje = 0U;
static uint8_t texto_adc[160];
/*******************************************************************************
 * Variables del pulsador
 ******************************************************************************/
static volatile bool check_bton = false;
static bool mode_run = true;
static bool mode_pending = false;
/*******************************************************************************
 * Variables del UART nonblocking RECEPTOR
 ******************************************************************************/
static uint8_t rx_buffer[128];
/*******************************************************************************
 * Variables del UART nonblocking TRANSMISOR
 ******************************************************************************/
static uint8_t texto_run[]   = "RUN\r\n";
static uint8_t texto_pause[] = "PAUSE\r\n";

static lpuart_handle_t uart_handle; //static para que su memoria siga disponible mientras el driver transmite datos
static volatile bool UART_OCUPED = false; //volatile para que el compilador no optimice o modifique la variable en la compilacion y la lea siempre de memoria

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

void ADC0_IRQHandler(void)
{
    lpadc_conv_result_t resultado;

    while (LPADC_GetConvResult(ADC0, &resultado))
    {
        uint16_t lectura = (uint16_t)(resultado.convValue >> 3U);

        adc_value = lectura;
        porcentaje = ((uint32_t)lectura * 100U + 2047U) / 4095U;
        adc_valid = true;
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

static bool UART_sendbuffer(uint8_t *data, size_t data_size)
{
    if (UART_OCUPED || data_size == 0U)
    {
        return false;
    }

    lpuart_transfer_t message = {0};
    message.data = data;
    message.dataSize = data_size;

    UART_OCUPED = true;

    if (LPUART_TransferSendNonBlocking(
            LPUART0, &uart_handle, &message) != kStatus_Success)
    {
        UART_OCUPED = false;
        return false;
    }

    return true;
}

static bool UART_sendtext(uint8_t *texto)
{
    if (texto[0] == '\0')
    {
        return false;
    }

    return UART_sendbuffer(texto, strlen((const char *)texto));
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

static void UART_write_text(void)
{
    static uint8_t eco[2] = {0};
    static bool eco_pendiente = false;

    if (mode_pending || UART_OCUPED)
    {
        return;
    }

    if (!eco_pendiente &&
        LPUART_TransferGetRxRingBufferLength(
            LPUART0, &uart_handle) > 0U)
    {
        lpuart_transfer_t recepcion = {0};
        size_t recibidos = 0U;

        recepcion.data = eco;
        recepcion.dataSize = 1U;

        status_t resultado = LPUART_TransferReceiveNonBlocking(
            LPUART0, &uart_handle, &recepcion, &recibidos);

        if ((resultado == kStatus_Success) && (recibidos == 1U))
        {
            eco[1] = '\0';
            eco_pendiente = (eco[0] != '\0');
        }
    }

    if (eco_pendiente)
    {
        if (UART_sendtext(eco))
        {
            eco_pendiente = false;
        }
    }
}


int main(void)
{
    BOARD_InitHardware(); // Inicializacion de hardware y perifericos
    /* Inicializo en  en el contexto de un pullup. */
    GPIO_PinWrite(BOARD_INITPINS_Activity_LED_GPIO, BOARD_INITPINS_Activity_LED_GPIO_PIN, 1U);
    GPIO_PinWrite(BOARD_INITPINS_Alarm_LED_GPIO, BOARD_INITPINS_Alarm_LED_GPIO_PIN, 1U);
    LPUART_TransferCreateHandle(LPUART0, &uart_handle, UART_Callback, NULL);
    /* BOARD_InitDebugConsole configura LPUART0; asegurar que RX siga habilitado. */
    LPUART_EnableRx(LPUART0, true);
    LPUART_TransferStartRingBuffer(LPUART0, &uart_handle, rx_buffer, sizeof(rx_buffer));
    EnableIRQ(LPUART0_SERIAL_RX_TX_IRQN);

    uint32_t last_change = tiempo_ms;
    uint32_t last_telemetry = tiempo_ms;
    uint32_t last_adc = tiempo_ms;
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

        /* Adquisición ADC cada 100 ms, también en PAUSE. */
        if ((uint32_t)(now - last_adc) >= 100U)
        {
            last_adc += 100U;
            /* Trigger 0 -> comando 1 -> entrada A2 / P2_15. */
            LPADC_DoSoftwareTrigger(ADC0, 1UL << 0);
        }

        if ((uint32_t)(now - last_telemetry) >= 1000U)
        {
            last_telemetry += 1000U;

            if (!mode_pending && !UART_OCUPED && adc_valid)
            {
            int longitud = snprintf(
            (char *)texto_adc,
            sizeof(texto_adc),
            "ADC=%u;PCT=%u\r\n",
            (unsigned int)adc_value,
            (unsigned int)porcentaje);

                if ((longitud > 0) &&
                    ((size_t)longitud < sizeof(texto_adc)))
                {
                    UART_sendtext(texto_adc);
                }
            }
        }
        UART_write_text();
    }
}
