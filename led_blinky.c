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
static uint32_t telemetrias_pendientes = 0U;
static uint8_t rx_buffer[128];
static char comando_rx[33];
static size_t comando_longitud = 0U;
static bool comando_descartar = false;
static bool comando_cr = false;
/*******************************************************************************
 * Variables del UART nonblocking TRANSMISOR
 ******************************************************************************/
static uint8_t texto_run[] = "RUN\r\n";
static uint8_t texto_pause[] = "PAUSE\r\n";
static bool telemetria_habilitada = true;
static uint8_t respuesta_uart[160];
static bool respuesta_pendiente = false;
static uint32_t umbral = 70U;
static bool alarma_encendida = false;
static lpuart_handle_t uart_handle;       // static para que su memoria siga disponible mientras el driver transmite datos
static volatile bool UART_OCUPED = false; // volatile para que el compilador no optimice o modifique la variable en la compilacion y la lea siempre de memoria

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

void CTIMER0_TimebaseCB(uint32_t flags) // Base temporal de tiempo en ms
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
            return true; /* Pulsación confirmada. */
        }
    }

    return false;
}

static void Alarma_Set(bool encender)
{
    alarma_encendida = encender;

    /* LED activo en bajo. */
    GPIO_PinWrite(
        BOARD_INITPINS_Alarm_LED_GPIO,
        BOARD_INITPINS_Alarm_LED_GPIO_PIN,
        encender ? 0U : 1U);
}

/* Usar al entrar en RUN o cambiar THR. */
static void Alarma_Reevaluar(void)
{
    Alarma_Set(
        mode_run &&
        adc_valid &&
        porcentaje >= umbral);
}

static void Modo_Set(bool ejecutar)
{
    bool estaba_en_run = mode_run;
    mode_run = ejecutar;

    if (!mode_run)
    {
        Alarma_Set(false);
    }
    else if (!estaba_en_run)
    {
        Alarma_Reevaluar();
    }
}

/* Funcionamiento normal con histéresis. */
static void Alarma_Actualizar(void)
{
    if (!mode_run)
    {
        Alarma_Set(false);
        return;
    }

    if (!adc_valid)
    {
        return;
    }

    uint32_t pct = porcentaje;

    if (pct >= umbral)
    {
        Alarma_Set(true);
    }
    else if (pct < (umbral - 5U))
    {
        Alarma_Set(false);
    }
}

static void UART_PrepararRespuesta(const char *texto)
{
    int longitud = snprintf(
        (char *)respuesta_uart,
        sizeof(respuesta_uart),
        "%s\r\n",
        texto);

    respuesta_pendiente =
        (longitud > 0) &&
        ((size_t)longitud < sizeof(respuesta_uart));
}

static void UART_PrepararEstado(void)
{
    uint16_t lectura;
    uint32_t pct;
    uint32_t instante;

    uint32_t irq_previo = __get_PRIMASK();
    __disable_irq();

    lectura = adc_value;
    pct = porcentaje;
    instante = tiempo_ms;

    __set_PRIMASK(irq_previo);

    int longitud = snprintf(
        (char *)respuesta_uart,
        sizeof(respuesta_uart),
        "t=%lu;ADC=%u;PCT=%lu;THR=%lu;MODE=%s;ALARM=%u\r\n",
        (unsigned long)instante,
        (unsigned int)lectura,
        (unsigned long)pct,
        (unsigned long)umbral,
        mode_run ? "RUN" : "PAUSE",
        alarma_encendida ? 1U : 0U);

    respuesta_pendiente =
        (longitud > 0) &&
        ((size_t)longitud < sizeof(respuesta_uart));
}

static void UART_ProcesarComando(const char *comando)
{
    if (strcmp(comando, "STATUS") == 0)
    {
        UART_PrepararEstado();
    }
    else if (strcmp(comando, "START") == 0)
    {
        Modo_Set(true);
        UART_PrepararRespuesta("OK RUN");
    }
    else if (strcmp(comando, "STOP") == 0)
    {
        Modo_Set(false);
        UART_PrepararRespuesta("OK PAUSE");
    }
    else if (strcmp(comando, "OFF_TELE") == 0)
    {
        telemetria_habilitada = false;
        telemetrias_pendientes = 0U;

        UART_PrepararRespuesta("OK TELE OFF");
    }
    else if (strcmp(comando, "ON_TELE") == 0)
    {
        telemetria_habilitada = true;

        UART_PrepararRespuesta("OK TELE ON");
    }
    else if (strncmp(comando, "THR ", 4U) == 0)
    {
        const char *digito = comando + 4U;
        uint32_t nuevo_umbral = 0U;

        if (*digito == '\0')
        {
            UART_PrepararRespuesta("ERR");
            return;
        }

        while (*digito != '\0')
        {
            if ((*digito < '0') || (*digito > '9'))
            {
                UART_PrepararRespuesta("ERR");
                return;
            }

            nuevo_umbral =
                nuevo_umbral * 10U + (uint32_t)(*digito - '0');

            /* También evita desbordar al recibir muchos dígitos. */
            if (nuevo_umbral > 95U)
            {
                UART_PrepararRespuesta("ERR");
                return;
            }

            digito++;
        }

        if (nuevo_umbral < 10U)
        {
            UART_PrepararRespuesta("ERR");
            return;
        }

        /* Modificar únicamente después de validar todo. */
        bool cambio = (umbral != nuevo_umbral);
        umbral = nuevo_umbral;

        if (cambio)
        {
            Alarma_Reevaluar();
        }

        int longitud = snprintf(
            (char *)respuesta_uart,
            sizeof(respuesta_uart),
            "OK THR=%lu\r\n",
            (unsigned long)umbral);

        respuesta_pendiente = (longitud > 0) && ((size_t)longitud < sizeof(respuesta_uart));
    }
    else
    {
        UART_PrepararRespuesta("ERR");
    }
}

static bool UART_FormatearEstado(uint8_t *destino, size_t capacidad)
{
    uint16_t lectura;
    uint32_t pct;
    uint32_t instante;

    /* Copiar ADC y porcentaje de la misma muestra. */
    uint32_t irq_previo = __get_PRIMASK();
    __disable_irq();

    lectura = adc_value;
    pct = porcentaje;
    instante = tiempo_ms;

    __set_PRIMASK(irq_previo);

    int longitud = snprintf(
        (char *)destino,
        capacidad,
        "t=%lu;ADC=%u;PCT=%lu;THR=%lu;MODE=%s;ALARM=%u\r\n",
        (unsigned long)instante,
        (unsigned int)lectura,
        (unsigned long)pct,
        (unsigned long)umbral,
        mode_run ? "RUN" : "PAUSE",
        alarma_encendida ? 1U : 0U);

    return (longitud > 0) &&
           ((size_t)longitud < capacidad);
}

static void UART_write_text(void)
{

    if (mode_pending || UART_OCUPED)
    {
        return;
    }

    if (respuesta_pendiente)
    {
        if (UART_sendtext(respuesta_uart))
        {
            respuesta_pendiente = false;
        }

        return;
    }

    if (LPUART_TransferGetRxRingBufferLength(
            LPUART0, &uart_handle) == 0U)
    {
        return;
    }

    uint8_t caracter;
    size_t recibidos = 0U;

    lpuart_transfer_t recepcion = {0};
    recepcion.data = &caracter;
    recepcion.dataSize = 1U;

    status_t resultado = LPUART_TransferReceiveNonBlocking(
        LPUART0, &uart_handle, &recepcion, &recibidos);

    if ((resultado != kStatus_Success) || (recibidos != 1U))
    {
        return;
    }

    if (caracter == '\n')
    {
        if (comando_descartar)
        {
            UART_PrepararRespuesta("ERR");
        }
        else if (comando_longitud > 0U)
        {
            comando_rx[comando_longitud] = '\0';
            UART_ProcesarComando(comando_rx);
        }

        /* Una línea vacía se ignora. */
        comando_longitud = 0U;
        comando_descartar = false;
        comando_cr = false;

        if (respuesta_pendiente &&
            UART_sendtext(respuesta_uart))
        {
            respuesta_pendiente = false;
        }

        return;
    }

    if (comando_descartar)
    {
        return;
    }

    /* Después de CR solamente se permite LF. */
    if (comando_cr)
    {
        comando_descartar = true;
        return;
    }

    if (caracter == '\r')
    {
        comando_cr = true;
        return;
    }

    if ((caracter < 32U) ||
        (caracter > 126U) ||
        (comando_longitud >= 32U))
    {
        /* Descartar el resto de la línea hasta LF. */
        comando_descartar = true;
        return;
    }

    comando_rx[comando_longitud++] = (char)caracter;
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
            Modo_Set(!mode_run);
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

        /* Registrar los periodos de telemetría que se hayan cumplido. */
        uint32_t transcurrido = (uint32_t)(now - last_telemetry);

        if (transcurrido >= 1000U)
        {
            uint32_t periodos = transcurrido / 1000U;

            last_telemetry += periodos * 1000U;

            if (telemetria_habilitada)
            {
                telemetrias_pendientes += periodos;
            }
        }
        Alarma_Actualizar();
        UART_write_text();
        if (telemetria_habilitada &&
            (telemetrias_pendientes > 0U) &&
            !mode_pending &&
            !respuesta_pendiente &&
            !UART_OCUPED &&
            adc_valid)
        {
            if (UART_FormatearEstado(texto_adc, sizeof(texto_adc)))
            {
                if (UART_sendtext(texto_adc))
                {
                    telemetrias_pendientes--;
                }
            }
        }
    }
}