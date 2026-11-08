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

/*!
 * @brief Main function
 */
volatile uint32_t tiempo_ms = 0U;

void CTIMER0_TimebaseCB(uint32_t flags)
{
    (void)flags;
    tiempo_ms++;
}

int main(void)
{

    BOARD_InitHardware(); //Inicializacion de hardware y perifericos

    /* Inicializo en bajo. */
    GPIO_PinWrite(BOARD_INITPINS_Activity_LED_GPIO, BOARD_INITPINS_Activity_LED_GPIO_PIN, 1U);
    GPIO_PinWrite(BOARD_INITPINS_Alarm_LED_GPIO, BOARD_INITPINS_Alarm_LED_GPIO_PIN, 1U);
    uint32_t last_change = tiempo_ms;

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
        }
        
        }
    }

