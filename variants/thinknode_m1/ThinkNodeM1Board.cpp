#include <Arduino.h>
#include <Wire.h>

#include "ThinkNodeM1Board.h"

#ifdef THINKNODE_M1

void ThinkNodeM1Board::begin() {
  NRF52Board::begin();

  Wire.begin();

#ifdef P_LORA_TX_LED
  pinMode(P_LORA_TX_LED, OUTPUT);
  digitalWrite(P_LORA_TX_LED, LOW);
#endif
}

uint16_t ThinkNodeM1Board::getBattMilliVolts() {
  uint32_t adcvalue = 0;

  analogReference(AR_INTERNAL_3_0);
  analogReadResolution(12);
  delay(10);

  // ADC range is 0..3000mV and resolution is 12-bit (0..4095);
  // average a few samples so the reading doesn't jitter
  for (int i = 0; i < VBAT_SAMPLES; i++) {
    adcvalue += analogRead(PIN_VBAT_READ);
  }
  // Convert the raw value to compensated mv, taking the resistor-
  // divider into account (providing the actual LIPO voltage)
  return (uint16_t)((float)adcvalue / VBAT_SAMPLES * REAL_VBAT_MV_PER_LSB);
}
#endif
