#include "BLEConsole.h"

#include <bluefruit.h>

static BLEUart console_uart;

BLEConsole ble_console;
Stream& mesh_log_stream = ble_console;

void BLEConsole::begin(const char* name, uint32_t pin) {
  char pin_str[8];
  snprintf(pin_str, sizeof(pin_str), "%06lu", (unsigned long)(pin % 1000000));

  Bluefruit.autoConnLed(false);
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.begin(1, 0);
  Bluefruit.setTxPower(4);

  char dev_name[32];
  snprintf(dev_name, sizeof(dev_name), "MeshCore-RPT-%s", name);
  Bluefruit.setName(dev_name);

  // display-only IO caps: the client has to type the PIN, giving an authenticated link
  Bluefruit.Security.setMITM(true);
  Bluefruit.Security.setPIN(pin_str);
  Bluefruit.Security.setIOCaps(true, false, false);

  console_uart.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_ENC_WITH_MITM);
  console_uart.begin();

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(console_uart);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);  // 0.625ms units
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);
}

bool BLEConsole::isConnected() {
  return Bluefruit.connected() && console_uart.notifyEnabled();
}

void BLEConsole::bleWrite(const char* s) {
  console_uart.write((const uint8_t*)s, strlen(s));
}

void BLEConsole::sendLine() {
  if (_tx_len > 0 && isConnected()) {
    console_uart.write(_tx, _tx_len);
  }
  _tx_len = 0;
}

size_t BLEConsole::write(uint8_t c) {
  return write(&c, 1);
}

size_t BLEConsole::write(const uint8_t* buf, size_t len) {
  Serial.write(buf, len);
  if (!isConnected()) {
    _tx_len = 0;
    return len;
  }
  // printHex() writes two chars at a time; batch into lines so each notification is useful
  for (size_t i = 0; i < len; i++) {
    _tx[_tx_len++] = buf[i];
    if (buf[i] == '\n' || _tx_len == sizeof(_tx)) sendLine();
  }
  return len;
}

void BLEConsole::flush() {
  Serial.flush();
  sendLine();
}

bool BLEConsole::readLine(char* dest, size_t size) {
  while (console_uart.available()) {
    char c = console_uart.read();
    if (c == '\r' || c == '\n') {
      if (_rx_len == 0) continue;
      _rx[_rx_len] = 0;
      strncpy(dest, _rx, size - 1);
      dest[size - 1] = 0;
      _rx_len = 0;
      return true;
    }
    if (_rx_len < sizeof(_rx) - 1) _rx[_rx_len++] = c;
  }
  return false;
}

void BLEConsole::sendReply(const char* command, const char* reply) {
  if (!isConnected()) return;
  sendLine();
  bleWrite(command);
  bleWrite("\n");
  if (reply[0]) {
    bleWrite("  -> ");
    bleWrite(reply);
    bleWrite("\n");
  }
}
