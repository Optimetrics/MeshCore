/*
 * variant.h
 * Copyright (C) 2023 Seeed K.K.
 * MIT License
 */

#pragma once

#include "WVariant.h"

////////////////////////////////////////////////////////////////////////////////
// Low frequency clock source

#define USE_LFXO    // 32.768 kHz crystal oscillator
#define VARIANT_MCK (64000000ul)

#define WIRE_INTERFACES_COUNT   (1)
// P0.21: driven high while running and low for power-off, as the stock firmware does
// (it calls it SX126X_POWER_EN); on the bench the radio and TCXO work either way
#define PIN_TXCO                (21)
////////////////////////////////////////////////////////////////////////////////
// Power

#define PIN_PWR_EN              (12)   // rail for e-ink, GPS, flash and sensors

#define BATTERY_PIN             (4)

#define ADC_RESOLUTION          (14)

////////////////////////////////////////////////////////////////////////////////
// Number of pins

#define PINS_COUNT              (48)
#define NUM_DIGITAL_PINS        (48)
#define NUM_ANALOG_INPUTS       (1)
#define NUM_ANALOG_OUTPUTS      (0)

////////////////////////////////////////////////////////////////////////////////
// UART pin definition

#define PIN_SERIAL1_RX          PIN_GPS_TX
#define PIN_SERIAL1_TX          PIN_GPS_RX
////////////////////////////////////////////////////////////////////////////////
// I2C pin definition

#define PIN_WIRE_SDA            (26)             // P0.26
#define PIN_WIRE_SCL            (27)             // P0.27

////////////////////////////////////////////////////////////////////////////////
// SPI pin definition

#define SPI_INTERFACES_COUNT    (2)

#define PIN_SPI_MISO            (23)
#define PIN_SPI_MOSI            (22)
#define PIN_SPI_SCK             (19)
#define PIN_SPI_NSS             (24)

////////////////////////////////////////////////////////////////////////////////
// Builtin LEDs

// both active high; the charger also lights the red LED dimly by itself while on USB
#define LED_RED                 (32 + 4)   // P1.04 (P1.06 is a second drive for the same LED, left alone)
#define LED_BLUE                (13)       // P0.13, also used as the LoRa TX LED

#define PIN_STATUS_LED          LED_RED
#define LED_BUILTIN             (14)       // P0.14, not connected: soaks up the core's flash-write blinks
#define PIN_LED                 LED_RED
#define LED_PIN                 LED_RED
#define LED_STATE_ON            HIGH

////////////////////////////////////////////////////////////////////////////////
// Builtin buttons

#define PIN_BUTTON1             (42)
#define BUTTON_PIN              PIN_BUTTON1
#define PIN_USER_BTN            BUTTON_PIN

#define PIN_BUTTON2             (32 + 7)   // second side button, active low (P0.11 is the T-Echo touch pad, not wired to a button here)
#define BUTTON_PIN2             PIN_BUTTON2

#define EXTERNAL_FLASH_DEVICES MX25R1635F
#define EXTERNAL_FLASH_USE_QSPI

////////////////////////////////////////////////////////////////////////////////
// Lora

#define USE_SX1262
#define LORA_CS                 (24)
#define SX126X_DIO1             (20)
#define SX126X_BUSY             (17)
#define SX126X_RESET            (25)
#define SX126X_DIO2_AS_RF_SWITCH

////////////////////////////////////////////////////////////////////////////////
// SPI1

#define PIN_SPI1_MISO           (38)   // the panel has no MISO; P1.06 only ever becomes an input here
#define PIN_SPI1_MOSI           (29)
#define PIN_SPI1_SCK            (31)

// GxEPD2 needs that for a panel that is not even used !
extern const int MISO;
extern const int MOSI;
extern const int SCK;

////////////////////////////////////////////////////////////////////////////////
// QSPI
#define PIN_QSPI_SCK (46)
#define PIN_QSPI_CS  (47)
#define PIN_QSPI_IO0 (44)  // MOSI if using two bit interface
#define PIN_QSPI_IO1 (45)  // MISO if using two bit interface
#define PIN_QSPI_IO2 (7)   // WP if using two bit interface (i.e. not used)
#define PIN_QSPI_IO3 (5)   // HOLD if using two bit interface (i.e. not used)

////////////////////////////////////////////////////////////////////////////////
// Display

#define DISP_MISO               (38)
#define DISP_MOSI               (29)
#define DISP_SCLK               (31)
#define DISP_CS                 (30)
#define DISP_DC                 (28)
#define DISP_RST                (2)
#define DISP_BUSY               (3)
#define DISP_BACKLIGHT          (43)

// names used by GxEPDDisplay when EINK_DISPLAY_MODEL is set
#define PIN_DISPLAY_CS          DISP_CS
#define PIN_DISPLAY_DC          DISP_DC
#define PIN_DISPLAY_RST         DISP_RST
#define PIN_DISPLAY_BUSY        DISP_BUSY

////////////////////////////////////////////////////////////////////////////////
// GPS

#define PIN_GPS_RX              (40)
#define PIN_GPS_TX              (41)
#define GPS_EN                  (34)   // standby line: high = awake, low = sleep
#define PIN_GPS_RESET           (37)   // P1.05, L76K reset, active low (not LoRa power)
#define PIN_GPS_STANDBY         (34)
#define PIN_GPS_SWITCH          (33)   // P1.01, high when the slide switch is on; the switch cuts GPS power
