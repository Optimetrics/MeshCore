#pragma once

#include <Arduino.h>

// A second CLI channel over a PIN-protected BLE UART (Nordic UART Service), for
// watching a repeater without a USB cable. Only a client that has paired with the
// PIN (authenticated, MITM-protected) can subscribe or send commands.
//
// Everything written to it goes to Serial too, so it can stand in for Serial as
// the packet log stream (see MESH_LOG_STREAM).
class BLEConsole : public Stream {
  char _rx[160];
  uint8_t _rx_len = 0;
  uint8_t _tx[244];
  uint16_t _tx_len = 0;

  void sendLine();
  void bleWrite(const char* s);

public:
  void begin(const char* name, uint32_t pin);
  bool isConnected();

  // true once a complete command line has arrived over BLE
  bool readLine(char* dest, size_t size);
  // echo the command and its reply back to the BLE client only
  void sendReply(const char* command, const char* reply);

  using Print::write;
  size_t write(uint8_t c) override;
  size_t write(const uint8_t* buf, size_t len) override;
  int available() override { return Serial.available(); }
  int read() override { return Serial.read(); }
  int peek() override { return Serial.peek(); }
  void flush() override;
};

extern BLEConsole ble_console;
