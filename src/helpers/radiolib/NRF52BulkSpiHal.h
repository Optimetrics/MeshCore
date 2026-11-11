#pragma once

#if defined(NRF52_PLATFORM)

#include <RadioLib.h>
#include <SPI.h>

// RadioLib's stock ArduinoHal::spiTransfer() sends one byte per SPI.transfer(uint8_t)
// call. On nRF52 every one of those is a separate EasyDMA transaction with its own
// setup and wait, so most of the time goes to per-byte overhead. This HAL hands the
// whole buffer to the SPIM peripheral in one DMA transfer instead.
//
// RadioLib always passes RAM buffers here (it copies commands and data into stack
// arrays first), which EasyDMA requires.
class NRF52BulkSpiHal : public ArduinoHal {
public:
  NRF52BulkSpiHal(SPIClass& spi, uint32_t freq_hz)
    : ArduinoHal(spi, SPISettings(freq_hz, MSBFIRST, SPI_MODE0)) { }

  void spiTransfer(uint8_t* out, size_t len, uint8_t* in) override {
    spi->transfer(out, in, len);
  }
};

#endif
