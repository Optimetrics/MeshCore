#pragma once

#include <MeshCore.h>
#include <Arduino.h>
#include <SPI.h>
#include <helpers/NRF52Board.h>

// built-ins
#define VBAT_MV_PER_LSB   (0.73242188F)   // 3.0V ADC range and 12-bit ADC resolution = 3000mV/4096

#define VBAT_DIVIDER      (0.5F)          // 150K + 150K voltage divider on VBAT
#define VBAT_DIVIDER_COMP (2.02F)         // Compensation factor for the VBAT divider (Elecrow/Meshtastic value)
#define VBAT_SAMPLES      (16)

#define PIN_VBAT_READ     (4)
#define REAL_VBAT_MV_PER_LSB (VBAT_DIVIDER_COMP * VBAT_MV_PER_LSB)

#define DFU_MAGIC_SKIP    (0x6D)          // bootloader GPREGRET value: start the app, skip the DFU checks

class ThinkNodeM1Board : public NRF52Board {
public:
  ThinkNodeM1Board() : NRF52Board("THINKNODE_M1_OTA") {}
  void begin();
  uint16_t getBattMilliVolts() override;

  #if defined(P_LORA_TX_LED)
  void onBeforeTransmit() override {
    digitalWrite(P_LORA_TX_LED, HIGH);   // turn TX LED on
  }
  void onAfterTransmit() override {
    digitalWrite(P_LORA_TX_LED, LOW);   // turn TX LED off
  }
  #endif

  const char* getManufacturerName() const override {
    return "Elecrow ThinkNode-M1";
  }

  void shutdownPeripherals() override {
    // display off, radio asleep, GPS stopped
    NRF52Board::shutdownPeripherals();

    // System OFF keeps every output latched, so turn the lights off and cut the
    // e-ink/GPS/flash rail, as the stock firmware does
    driveLow(DISP_BACKLIGHT);
    driveLow(LED_RED);
    driveLow(LED_BLUE);
    driveLow(PIN_TXCO);

    // nothing may stay driven high into the unpowered parts, or they get back-fed
    if (Serial1) Serial1.end();
  #ifdef DISPLAY_CLASS
    SPI1.end();   // only ever started by the e-ink driver
  #endif
    NRF_QSPI->TASKS_DEACTIVATE = 1;
    NRF_QSPI->ENABLE = 0;
    const uint8_t released[] = { PIN_SERIAL1_TX, PIN_GPS_STANDBY, PIN_GPS_RESET,
                                 DISP_CS, DISP_DC, DISP_RST, DISP_MOSI, DISP_SCLK,
                                 PIN_QSPI_SCK, PIN_QSPI_CS, PIN_QSPI_IO0, PIN_QSPI_IO1, PIN_QSPI_IO2, PIN_QSPI_IO3 };
    for (uint8_t pin : released) nrf_gpio_cfg_default(pin);
    driveLow(PIN_PWR_EN);

    // either side button wakes it, straight into the app: a held wake button must not
    // be taken for the bootloader's DFU button
    nrf_gpio_cfg_sense_input(PIN_BUTTON1, NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);
    nrf_gpio_cfg_sense_input(PIN_BUTTON2, NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);
    uint8_t sd_enabled = 0;
    sd_softdevice_is_enabled(&sd_enabled);
    if (sd_enabled) {
      sd_power_gpregret_clr(0, 0xFF);
      sd_power_gpregret_set(0, DFU_MAGIC_SKIP);
    } else {
      NRF_POWER->GPREGRET = DFU_MAGIC_SKIP;
    }
  }

private:
  static void driveLow(uint8_t pin) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }
};
