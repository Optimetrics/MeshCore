#include "T096Board.h"

#include <Arduino.h>
#include <Wire.h>
#include <target.h>

#ifdef NRF52_POWER_MANAGEMENT
// Static configuration for power management
// Values come from variant.h defines
const PowerMgtConfig power_config = {
  .lpcomp_ain_channel = PWRMGT_LPCOMP_AIN,
  .lpcomp_refsel = PWRMGT_LPCOMP_REFSEL,
  .voltage_bootlock = PWRMGT_VOLTAGE_BOOTLOCK
};

void T096Board::initiateShutdown(uint8_t reason) {
#if ENV_INCLUDE_GPS == 1
  pinMode(PIN_GPS_EN, OUTPUT);
  digitalWrite(PIN_GPS_EN, !PIN_GPS_EN_ACTIVE);
#endif
  variant_shutdown();

  bool enable_lpcomp = (reason == SHUTDOWN_REASON_LOW_VOLTAGE ||
                        reason == SHUTDOWN_REASON_BOOT_PROTECT);
  pinMode(PIN_BAT_CTL, OUTPUT);
  digitalWrite(PIN_BAT_CTL, enable_lpcomp ? HIGH : LOW);

  if (enable_lpcomp) {
    configureVoltageWake(power_config.lpcomp_ain_channel, power_config.lpcomp_refsel);
  }

  enterSystemOff(reason);
}
#endif // NRF52_POWER_MANAGEMENT

void T096Board::begin() {
  NRF52Board::begin();

#ifdef NRF52_POWER_MANAGEMENT
  // Boot voltage protection check (may not return if voltage too low)
  checkBootVoltage(&power_config);
#endif

#if defined(PIN_BOARD_SDA) && defined(PIN_BOARD_SCL)
  Wire.setPins(PIN_BOARD_SDA, PIN_BOARD_SCL);
#endif

  Wire.begin();

  pinMode(P_LORA_TX_LED, OUTPUT);
  digitalWrite(P_LORA_TX_LED, LOW);

  periph_power.begin();
  loRaFEMControl.init();
  delay(1);
}

void T096Board::onBeforeTransmit() {
    digitalWrite(P_LORA_TX_LED, HIGH);   // turn TX LED on
    loRaFEMControl.setTxModeEnable();
}

void T096Board::onAfterTransmit() {
    digitalWrite(P_LORA_TX_LED, LOW);   //turn TX LED off
    loRaFEMControl.setRxModeEnable();
}

uint16_t T096Board::getBattMilliVolts() {
    int adcvalue = 0;
    analogReadResolution(12);
    analogReference(AR_INTERNAL_3_0);
    pinMode(PIN_VBAT_READ, INPUT);
    pinMode(PIN_BAT_CTL, OUTPUT);
    digitalWrite(PIN_BAT_CTL, 1);

    delay(10);
    adcvalue = analogRead(PIN_VBAT_READ);
    digitalWrite(PIN_BAT_CTL, 0);

    return (uint16_t)((float)adcvalue * MV_LSB * 4.9);
}
void T096Board::variant_shutdown() {
 nrf_gpio_cfg_default(PIN_VEXT_EN);
    nrf_gpio_cfg_default(PIN_TFT_CS);
    nrf_gpio_cfg_default(PIN_TFT_DC);
    nrf_gpio_cfg_default(PIN_TFT_SDA);
    nrf_gpio_cfg_default(PIN_TFT_SCL);
    nrf_gpio_cfg_default(PIN_TFT_RST);
    nrf_gpio_cfg_default(PIN_TFT_LEDA_CTL);

    nrf_gpio_cfg_default(PIN_LED);

    nrf_gpio_cfg_default(P_LORA_KCT8103L_PA_CSD);
    nrf_gpio_cfg_default(P_LORA_KCT8103L_PA_CTX);
    pinMode(P_LORA_PA_POWER, OUTPUT);
    digitalWrite(P_LORA_PA_POWER, LOW);

    digitalWrite(PIN_BAT_CTL, LOW);
    nrf_gpio_cfg_default(LORA_CS);
    nrf_gpio_cfg_default(SX126X_DIO1);
    nrf_gpio_cfg_default(SX126X_BUSY);
    nrf_gpio_cfg_default(SX126X_RESET);

    nrf_gpio_cfg_default(PIN_SPI_MISO);
    nrf_gpio_cfg_default(PIN_SPI_MOSI);
    nrf_gpio_cfg_default(PIN_SPI_SCK);

    // nrf_gpio_cfg_default(PIN_GPS_PPS);
    nrf_gpio_cfg_default(PIN_GPS_RESET);
    nrf_gpio_cfg_default(PIN_GPS_EN);
    nrf_gpio_cfg_default(PIN_GPS_RX);
    nrf_gpio_cfg_default(PIN_GPS_TX);
}

void T096Board::powerOff() {
  loRaFEMControl.setSleepModeEnable();
  nrf_gpio_cfg_default(PIN_GPS_EN); // 363uA down to 39uA
  NRF52Board::powerOff();
}

const char* T096Board::getManufacturerName() const {
  return "Heltec T096";
}

bool T096Board::setLoRaFemLnaEnabled(bool enable) {
  if (!loRaFEMControl.isLnaCanControl()) {
    return false;
  }

  loRaFEMControl.setLNAEnable(enable);
  loRaFEMControl.setRxModeEnable();
  return true;
}

bool T096Board::isLoRaFemLnaEnabled() const {
  return loRaFEMControl.isLNAEnabled();
}

void T096Board::attachDynamicPrefs(KeyValueStore* prefs) {
  _prefs = prefs;

  char radio_fem_rxgain[8] = { 0 };
  _prefs->getByKey("fem_rxgain", radio_fem_rxgain, 7);  // get initial values

  setLoRaFemLnaEnabled(strcmp(radio_fem_rxgain, "1") == 0);
}

// Median of a burst of instantaneous RSSI readings, in dBm. A median shrugs off a packet
// that happens to arrive mid-burst.
static int medianNoiseDbm() {
  const int N = 48;
  int16_t v[N];
  for (int i = 0; i < N; i++) {
    v[i] = (int16_t)radio_driver.getCurrentRSSI();
    delay(4);
  }
  for (int i = 1; i < N; i++) {   // insertion sort
    int16_t x = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
    v[j + 1] = x;
  }
  return v[N / 2];
}

// 'noisetest': read the receiver's noise floor with one thing changed at a time, to find out
// whether something on the board is raising it. Blocks for about 3 seconds and puts every
// setting back. Nothing is saved to prefs.
void T096Board::runNoiseTest(char* reply) {
  delay(150);
  int base = medianNoiseDbm();

  // FEM LNA bypassed. The reading drops by the LNA's gain if the floor is amplified thermal noise.
  bool lna_was = loRaFEMControl.isLNAEnabled();
  loRaFEMControl.setLNAEnable(false);
  loRaFEMControl.setRxModeEnable();
  delay(150);
  int lna_off = medianNoiseDbm();
  loRaFEMControl.setLNAEnable(lna_was);
  loRaFEMControl.setRxModeEnable();

  // SX1262 boosted RX gain off
  bool boost_was = radio_driver.getRxBoostedGainMode();
  radio_driver.setRxBoostedGainMode(false);
  delay(150);
  int boost_off = medianNoiseDbm();
  radio_driver.setRxBoostedGainMode(boost_was);

  // Vext rail (display + GPS supply) on, and the GPS running
  uint32_t gps_en = g_ADigitalPinMap[PIN_GPS_EN];
  uint32_t gps_en_was = nrf_gpio_pin_out_read(gps_en);
  bool gps_en_was_output = nrf_gpio_pin_dir_get(gps_en) == NRF_GPIO_PIN_DIR_OUTPUT;
  periph_power.claim();
  pinMode(PIN_GPS_EN, OUTPUT);
  digitalWrite(PIN_GPS_EN, PIN_GPS_EN_ACTIVE);
  delay(1500);
  int gps_on = medianNoiseDbm();
  if (gps_en_was_output) {
    digitalWrite(PIN_GPS_EN, gps_en_was ? HIGH : LOW);
  } else {
    nrf_gpio_cfg_default(gps_en);   // nothing was driving it before
  }
  periph_power.release();

  delay(150);
  int after = medianNoiseDbm();   // back to the starting state: should match 'base'

  sprintf(reply, "> base %d, lna_off %d, boost_off %d, gps_on %d, after %d (dBm)",
          base, lna_off, boost_off, gps_on, after);
}

bool T096Board::handleCommand(const char* command, uint32_t sender_timestamp, char* reply) {
  if (strcmp(command, "noisetest") == 0) {
    if (sender_timestamp != 0) {
      strcpy(reply, "Error: local console only");   // it blocks for about 3 seconds
    } else if (!radio_driver.isInRecvMode()) {
      strcpy(reply, "Error: radio busy, try again");
    } else {
      runNoiseTest(reply);
    }
    return true;
  }
  if (strcmp(command, "get radio.fem.rxgain") == 0) {
    if (!loRaFEMControl.isLnaCanControl()) {
      strcpy(reply, "Error: unsupported");
    } else {
      sprintf(reply, "> %s", isLoRaFemLnaEnabled() ? "on" : "off");
    }
    return true;
  }
  if (memcmp(command, "set radio.fem.rxgain ", 21) == 0) {
    if (!loRaFEMControl.isLnaCanControl()) {
      strcpy(reply, "Error: unsupported");
    } else if (memcmp(&command[21], "on", 2) == 0) {
      if (setLoRaFemLnaEnabled(true)) {
        if (_prefs) _prefs->setByKey("fem_rxgain", "1");
        strcpy(reply, "OK - LoRa FEM RX gain on");
      } else {
        strcpy(reply, "Error: failed to apply LoRa FEM RX gain");
      }
    } else if (memcmp(&command[21], "off", 3) == 0) {
      if (setLoRaFemLnaEnabled(false)) {
        if (_prefs) _prefs->setByKey("fem_rxgain", "0");
        strcpy(reply, "OK - LoRa FEM RX gain off");
      } else {
        strcpy(reply, "Error: failed to apply LoRa FEM RX gain");
      }
    } else {
      strcpy(reply, "Error: state must be on or off");
    }
    return true;
  }

  return false; // not handled
}
