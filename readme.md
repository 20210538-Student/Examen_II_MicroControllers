# Sistema de adquisición ADC y alarma por UART

**Autor:** Joseph Ramirez  
**Matrícula:** 20210538  
**Placa:** NXP FRDM-MCXA156  
**Microcontrolador:** MCXA156

Aplicación en C que adquiere un potenciómetro, calcula su porcentaje y controla una alarma con histéresis. Utiliza CTIMER0 como base temporal y UART por interrupciones para recibir comandos y transmitir el estado sin bloquear las tareas periódicas.

## Herramientas

Visual Studio Code con MCUXpresso for VS Code, MCUXpresso Config Tools, SDK para FRDM-MCXA156, toolchain Arm, CMake, Ninja y LinkServer con MCU-Link. La consola utiliza Serial Monitor de VS Code.

Las versiones instaladas se consultan en **Help → About**, los detalles de las extensiones y la información del SDK/Config Tools. Para las herramientas de terminal:

```sh
code --version
arm-none-eabi-gcc --version
cmake --version
ninja --version
```

## Pines y conexiones

| Función | Pin / señal | Pin de encapsulado | Configuración |
| --- | --- | --- | --- |
| LED de actividad | P3_13 / GPIO3_13 | 62 | Salida activa en bajo, inicial 1 |
| LED de alarma | P3_22 / GPIO3_22 | 53 | Salida activa en bajo, inicial 1 |
| Pulsador SW2 | P1_7 / GPIO1_7 | 100 | Pull-up, IRQ por flanco descendente |
| Potenciómetro | P2_15 / ADC0_A2 | 36 | Entrada analógica A2 |
| UART RX | P0_2 / LPUART0_RXD | 78 | Entrada, ALT2 |
| UART TX | P0_3 / LPUART0_TXD | 79 | Salida, ALT2 |

Los números de encapsulado corresponden a Config Tools; el cableado se identifica por las señales de la placa.

**Esquema de conexiones:**

| Elemento | Conexión |
| --- | --- |
| Potenciómetro de 10 kΩ | Extremos a 3,3 V y GND; cursor a P2_15 |
| SW2 | P1_7 a GND al pulsar; pull-up habilitado |
| LED de actividad | P3_13; GPIO 0 enciende y GPIO 1 apaga |
| LED de alarma | P3_22; GPIO 0 enciende y GPIO 1 apaga |
| LED externos | 3,3 V → resistencia limitadora → ánodo; cátodo → GPIO |
| Consola | PC conectado al USB de MCU-Link y su puerto COM virtual |

## ADC

ADC0 utiliza reloj de **12 MHz**, resolución de **12 bits**, referencia **VREFH** y escala nominal de **3,3 V**. La medición es single-ended en el canal **A2**, con comando 1 (`ADC_Pot`) y trigger 0 por software cada **100 ms**.

Config Tools establece adquisición de **3 ciclos ADCK = 250 ns**, una conversión por disparo, FIFO watermark 0 e IRQ `ADC0_IRQn` con prioridad 3. El resultado útil se obtiene con `convValue >> 3U`.

El porcentaje se actualiza con cada resultado ADC mediante aritmética entera de 32 bits:

```c
porcentaje = ((uint32_t)lectura * 100U + 2047U) / 4095U;
```

La fórmula implementa `redondear(100 × ADC / (2^12 − 1))`, con lecturas de 0 a 4095 y porcentajes de 0 a 100. VREFH es la referencia seleccionada; 3,3 V expresa su valor nominal, no una medición de laboratorio.

## TIMER y tareas periódicas

CTIMER0 opera en modo Match, arranca durante la inicialización y reinicia el contador al alcanzar Match 0, manteniéndolo en funcionamiento.

```text
Reloj de entrada:     12 MHz
División efectiva:   12  (PR + 1; PR = 11)
Reloj del contador:  12 MHz / 12 = 1 MHz
Duración del tick:   1 µs
Match 0:             1000 ticks × 1 µs = 1 ms
```

La IRQ de CTIMER0 tiene prioridad 2. Su callback `CTIMER0_TimebaseCB()` incrementa `tiempo_ms`; el bucle principal programa las tareas mediante diferencias de tiempo sin signo.

| Tarea | Intervalo |
| --- | --- |
| Base temporal | 1 ms |
| Adquisición ADC y actualización de porcentaje | 100 ms |
| Cambio de estado del LED de actividad | 500 ms |
| Telemetría automática | 1000 ms |

Se utilizan interrupciones breves y procesamiento de comandos en el bucle principal, sin retardos bloqueantes.

## Pulsador y alarma

SW2 alterna RUN/PAUSE con estabilidad mínima de **30 ms**, una acción por pulsación y validación de la liberación. `BTON_IRQHANDLER()` atiende GPIO1 con prioridad 0.

El sistema inicia en **RUN**, con **THR=70**, alarma apagada y telemetría habilitada. La primera muestra válida permite evaluar la alarma.

- En RUN, PCT ≥ THR enciende la alarma; PCT < THR − 5 la apaga. En la banda intermedia conserva su estado.
- En PAUSE, la alarma permanece apagada; continúan ADC, actividad, telemetría habilitada y comandos.
- Al volver a RUN o cambiar THR en RUN, se evalúa la última muestra válida contra el umbral. Después se aplica la histéresis normal.
- START estando en RUN conserva la decisión de alarma. Un cambio de THR en PAUSE mantiene la alarma apagada.

## UART y comandos

LPUART0 funciona a **115200 baudios, 8N1, sin control de flujo**, con reloj de 12 MHz. `BOARD_InitDebugConsole()`, llamada desde `BOARD_InitHardware()`, configura el periférico. La aplicación crea el handle, habilita RX, inicia el ring buffer de **128 bytes** y habilita su IRQ. TX utiliza `LPUART_TransferSendNonBlocking()` y callback de finalización.

Se reciben líneas ASCII de hasta **32 caracteres**, terminadas en LF o CRLF. Los comandos distinguen mayúsculas. Las líneas vacías se ignoran; las demasiado largas se descartan hasta LF y responden `ERR`. Las respuestas terminan en CRLF. El requisito de respuesta es completar la línea en **100 ms como máximo desde LF**.

| Comando | Acción / respuesta |
| --- | --- |
| `STATUS` | Envía una línea completa de estado |
| `THR n` | Entero de 10 a 95; responde `OK THR=n` |
| `START` | Establece RUN; responde `OK RUN` |
| `STOP` | Establece PAUSE y apaga la alarma; responde `OK PAUSE` |
| `OFF_TELE` | Detiene telemetría automática y elimina reportes pendientes; responde `OK TELE OFF` |
| `ON_TELE` | Reanuda telemetría en el siguiente periodo; responde `OK TELE ON` |
| Comando inválido | Responde `ERR` y conserva los parámetros |

`THR` rechaza decimales, texto adicional y valores fuera del rango. `OFF_TELE` y `ON_TELE` son extensiones del protocolo: `STATUS` sigue disponible con la telemetría desactivada.

STATUS y telemetría comparten este formato:

```text
t=1200;ADC=2866;PCT=70;THR=70;MODE=RUN;ALARM=1
```

`t` representa milisegundos desde el inicio de la base temporal; `ADC` es la última lectura; `PCT` su porcentaje; `THR` el umbral; `MODE` el modo y `ALARM` el estado lógico 0/1. Las respuestas y la telemetría usan búferes persistentes para conservar líneas completas. Los reportes periódicos ocupados quedan pendientes y se forman con el estado más reciente al transmitirse.

## Compilación y ejecución

1. Abrir el proyecto en VS Code con MCUXpresso y seleccionar el SDK de FRDM-MCXA156 y su toolchain.
2. En Config Tools, generar la configuración de Pins, ADC0, CTIMER0 y NVIC.
3. Ejecutar **Build** desde MCUXpresso. Para una carpeta de construcción ya configurada también se utiliza `cmake --build <directorio-de-build>`.
4. Conectar la placa mediante MCU-Link, seleccionar la configuración de depuración y programar el ejecutable con LinkServer. Dejar el firmware en ejecución.
5. Abrir el COM virtual en Serial Monitor con **115200, 8N1 y terminación LF o CRLF**. El puerto usado durante el desarrollo es COM12.
6. Variar el potenciómetro y observar ADC/PCT, actividad y alarma. Probar `STATUS`, `THR 75`, `STOP`, `START`, `OFF_TELE` y `ON_TELE`.

Las comprobaciones funcionales incluyen antirrebote, histéresis, continuidad en PAUSE, validación de líneas y respuesta a comandos. La verificación temporal contempla los periodos de 100/500/1000 ms y la respuesta UART dentro de 100 ms.

VIDEO_LINK_DEMOSTRATIVO: https://vimeo.com/1244987722
