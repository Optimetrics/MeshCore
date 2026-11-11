
#include "GxEPDDisplay.h"
#include "UTF8Transliterate.h"

// the fonts only have ASCII glyphs; translateUTF8ToBlocks() marks anything else with this
// and print() draws a small box for it
#define GLYPH_PLACEHOLDER '\x7F'

#ifndef DISPLAY_ROTATION
  #define DISPLAY_ROTATION 3
#endif

#ifdef ESP32
  SPIClass SPI1 = SPIClass(FSPI);
#endif

#ifndef EPD_WASHING_MACHINE_CYCLES
  #define EPD_WASHING_MACHINE_CYCLES 0
#endif

// Color scheme
ColorVal UIColor::window_bkg = GxEPD_WHITE;
ColorVal UIColor::title_bkg = GxEPD_WHITE;
ColorVal UIColor::title_txt = GxEPD_BLACK;
ColorVal UIColor::primary_txt = GxEPD_BLACK;
ColorVal UIColor::secondary_txt = GxEPD_BLACK;
ColorVal UIColor::warning_txt = GxEPD_BLACK;
ColorVal UIColor::popup_bkg = GxEPD_WHITE;
ColorVal UIColor::popup_txt = GxEPD_BLACK;
ColorVal UIColor::corp_blue = GxEPD_BLACK;

bool GxEPDDisplay::begin() {
  display.epd2.selectSPI(SPI1, SPISettings(4000000, MSBFIRST, SPI_MODE0));
#ifdef ESP32
  SPI1.begin(PIN_DISPLAY_SCLK, PIN_DISPLAY_MISO, PIN_DISPLAY_MOSI, PIN_DISPLAY_CS);
#else
  SPI1.begin();
#endif
  display.init(115200, true, 2, false);
  display.setRotation(DISPLAY_ROTATION);
  // no GFX auto-wrap: it would cap every measured width at the panel edge (so long text
  // "fits") and push overflow onto the next row; printWordWrap() does the wrapping here
  display.setTextWrap(false);
  setTextSize(1);  // Default to size 1

  display.setFullWindow();

  for (int i = 0; i < EPD_WASHING_MACHINE_CYCLES; i++) {
    display.fillScreen(GxEPD_BLACK);
    display.display(false);
    delay(2000);
    display.fillScreen(GxEPD_WHITE);
    display.display(false);
    delay(2000);
  }

  display.setPartialWindow(0, 0, display.width(), display.height());
  _partial_refresh_count = 0;
  _force_full_refresh = true; // first frame after power-on is a full refresh

  #if DISP_BACKLIGHT
  digitalWrite(DISP_BACKLIGHT, LOW);
  pinMode(DISP_BACKLIGHT, OUTPUT);
  #endif
  _init = true;
  _isOn = true;
  return true;
}

void GxEPDDisplay::turnOn() {
  if (!_init) begin();
  if (!_isOn) {
    _isOn = true;
    // waking from hibernate: redraw the whole screen with a full refresh to clear ghosting
    _force_full_refresh = true;
  }
}

void GxEPDDisplay::turnOn(int max_partial) {
  setMaxPartialRefresh(max_partial);
  turnOn();
}

// 24x24 sleep icon, MSB-first (matches GxEPDDisplay::drawXbm)
static const uint8_t sleep_icon_24x24[] = {
  0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0xe0, 0x0e, 0x03, 0xc0, 0x18,
  0x0f, 0x80, 0x70, 0x1f, 0x80, 0xff, 0x3f, 0x80, 0xff, 0x3f, 0x00, 0x00,
  0x7f, 0x3f, 0x00, 0x7f, 0x3f, 0x00, 0xff, 0x0e, 0x00, 0xff, 0x1c, 0x7c,
  0xff, 0xbf, 0x7c, 0xff, 0xbf, 0x38, 0xff, 0xc0, 0x7c, 0xff, 0xe0, 0x7c,
  0xff, 0xf0, 0x00, 0x7f, 0xfc, 0x00, 0x7f, 0xff, 0xf8, 0x3f, 0xff, 0xf0,
  0x1f, 0xff, 0xf0, 0x0f, 0xff, 0xe0, 0x07, 0xff, 0x80, 0x01, 0xff, 0x00
};

void GxEPDDisplay::drawSleepIcon() {
  const int W = 24, H = 24;
  int x = (int)(display.width() / scale_x) - W;   // top-right, logical coordinates
  int y = 0;

  display.fillRect((x - 5) * scale_x - 1, y * scale_y, (W+5) * scale_x + 2, (H-2) * scale_y + 1, GxEPD_WHITE);
  _curr_color = GxEPD_BLACK;
  drawXbm(x, y, sleep_icon_24x24, W, H);
  if (_busy_hook) display.epd2.setBusyCallback(busyCallback, this);
  display.display(true);
  display.epd2.setBusyCallback(NULL);
}

void GxEPDDisplay::turnOff(bool blank) {
  if (blank) {
    blankScreen();
  } else {
    drawSleepIcon();
  }
  _isOn = false;
  last_display_crc_value=0;
  display.hibernate();
}

void GxEPDDisplay::turnOffKeepImage() {
  _isOn = false;
  display.hibernate();  // e-ink holds the last frame with no power
}

void GxEPDDisplay::blankScreen() {
  // do full refresh before powering off to clear screen
  // no full refresh needed at wakeup
  if (_busy_hook) display.epd2.setBusyCallback(busyCallback, this);
  display.clearScreen(0xFF); // Clears microcontroller side RAM
  display.writeScreenBuffer(0xFF); // Forces 0xFF (White) into the display controller's history registers
  display.epd2.setBusyCallback(NULL);
  _partial_refresh_count = 0;
  last_display_crc_value = 0;
}

void GxEPDDisplay::clear() {
  display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
  display_crc.reset();
}

void GxEPDDisplay::startFrame(ColorVal bkg) {
  display.fillScreen(bkg);
  display.setTextColor(_curr_color = UIColor::primary_txt);
  display_crc.reset();
  display.setPartialWindow(0, 0, display.width(), display.height());
  if (_max_partial_refresh > 0 && _partial_refresh_count >= _max_partial_refresh) {
    // partial refresh budget used up, redraw this frame with a full refresh to clear ghosting
    _force_full_refresh = true;
  }
}

void GxEPDDisplay::setTextSize(int sz) {
  display_crc.update<int>(sz);
  switch(sz) {
    case 1:  // Small
      display.setFont(&FreeSans9pt7b);
      break;
    case 2:  // Medium Bold
      display.setFont(&FreeSansBold12pt7b);
      break;
    case 3:  // Large
      display.setFont(&FreeSans18pt7b);
      break;
    default:
      display.setFont(&FreeSans9pt7b);
      break;
  }
  const GFXfont* font = (sz == 2) ? &FreeSansBold12pt7b : (sz == 3) ? &FreeSans18pt7b : &FreeSans9pt7b;
  _line_height = (int)(font->yAdvance * 0.8f / scale_y + 0.5f);
}

void GxEPDDisplay::setColor(ColorVal c) {
  display_crc.update<ColorVal> (c);
  display.setTextColor(_curr_color = c);
}

void GxEPDDisplay::setCursor(int x, int y) {
  display_crc.update<int>(x);
  display_crc.update<int>(y);
  display.setCursor((x+offset_x)*scale_x, (y+offset_y)*scale_y);
}

void GxEPDDisplay::print(const char* str) {
  display_crc.update<char>(str, strlen(str));
  const char* p = str;
  while (*p) {
    const char* box = strchr(p, GLYPH_PLACEHOLDER);
    if (box == NULL) {
      display.print(p);
      break;
    }
    display.write((const uint8_t*)p, box - p);
    // a box the size of a digit, so an emoji or other unsupported character stays visible
    int16_t bx, by;
    uint16_t bw, bh;
    display.getTextBounds("0", 0, 0, &bx, &by, &bw, &bh);
    int16_t cx = display.getCursorX(), cy = display.getCursorY();
    display.drawRect(cx + 1, cy + by, bw, bh, _curr_color);
    display.setCursor(cx + bw + 3, cy);
    p = box + 1;
  }
}

int GxEPDDisplay::printWordWrap(const char* str, int max_width) {
  // wrap at spaces from the current cursor position, stopping at the bottom of the screen
  int x = lroundf(display.getCursorX() / scale_x - offset_x);
  int y = lroundf(display.getCursorY() / scale_y - offset_y);
  int avail = max_width - x;
  const char* p = str;
  char line[100];
  while (*p && (y + offset_y) * scale_y <= display.height() - 2) {
    int len = 0;
    while (p[len]) {   // add whole words while they fit
      int end = len;
      while (p[end] == ' ') end++;
      while (p[end] && p[end] != ' ') end++;
      if (end >= (int)sizeof(line)) break;
      memcpy(line, p, end);
      line[end] = 0;
      if (getTextWidth(line) > avail) break;
      len = end;
    }
    if (len == 0) {   // one word wider than the line: break it mid-word
      while (p[len] && len < (int)sizeof(line) - 1) {
        line[len] = p[len];
        line[len + 1] = 0;
        if (getTextWidth(line) > avail) break;
        len++;
      }
      if (len == 0) len = 1;   // always make progress
    }
    memcpy(line, p, len);
    line[len] = 0;
    setCursor(x, y);
    print(line);
    p += len;
    while (*p == ' ') p++;   // a line break swallows the spaces at it
    y += _line_height;
  }
  return p - str;
}

void GxEPDDisplay::translateUTF8ToBlocks(char* dest, const char* src, size_t dest_size) {
  utf8_to_ascii(dest, src, dest_size, GLYPH_PLACEHOLDER);   // "é" -> "e", emoji -> box
}

void GxEPDDisplay::fillRect(int x, int y, int w, int h) {
  display_crc.update<int>(x);
  display_crc.update<int>(y);
  display_crc.update<int>(w);
  display_crc.update<int>(h);
  display.fillRect(x*scale_x, y*scale_y, w*scale_x, h*scale_y, _curr_color);
}

void GxEPDDisplay::drawRect(int x, int y, int w, int h) {
  display_crc.update<int>(x);
  display_crc.update<int>(y);
  display_crc.update<int>(w);
  display_crc.update<int>(h);
  display.drawRect(x*scale_x, y*scale_y, w*scale_x, h*scale_y, _curr_color);
}

void GxEPDDisplay::drawXbm(int x, int y, const uint8_t* bits, int w, int h) {
  display_crc.update<int>(x);
  display_crc.update<int>(y);
  display_crc.update<int>(w);
  display_crc.update<int>(h);
  display_crc.update<uint8_t>(bits, w * h / 8);
  // Calculate the base position in display coordinates
  uint16_t startX = x * scale_x;
  uint16_t startY = y * scale_y;
  
  // Width in bytes for bitmap processing
  uint16_t widthInBytes = (w + 7) / 8;
  
  // Process the bitmap row by row
  for (uint16_t by = 0; by < h; by++) {
    // Calculate the target y-coordinates for this logical row
    int y1 = startY + (int)(by * scale_y);
    int y2 = startY + (int)((by + 1) * scale_y);
    int block_h = y2 - y1;
    
    // Scan across the row bit by bit
    for (uint16_t bx = 0; bx < w; bx++) {
      // Calculate the target x-coordinates for this logical column
      int x1 = startX + (int)(bx * scale_x);
      int x2 = startX + (int)((bx + 1) * scale_x);
      int block_w = x2 - x1;
      
      // Get the current bit
      uint16_t byteOffset = (by * widthInBytes) + (bx / 8);
      uint8_t bitMask = 0x80 >> (bx & 7);
      bool bitSet = pgm_read_byte(bits + byteOffset) & bitMask;
      
      // If the bit is set, draw a block of pixels
      if (bitSet) {
        // Draw the block as a filled rectangle
        display.fillRect(x1, y1, block_w, block_h, _curr_color);
      }
    }
  }
}

uint16_t GxEPDDisplay::getTextWidth(const char* str) {
  int16_t x1, y1;
  uint16_t w, h;
  if (strchr(str, GLYPH_PLACEHOLDER) == NULL) {
    display.getTextBounds(str, 0, 0, &x1, &y1, &w, &h);
    return ceil((w + 1) / scale_x);
  }
  // measure without the placeholders (the font has no glyph for them), then add their boxes
  char text[256];
  int n = 0, boxes = 0;
  for (const char* p = str; *p && n < (int)sizeof(text) - 1; p++) {
    if (*p == GLYPH_PLACEHOLDER) boxes++;
    else text[n++] = *p;
  }
  text[n] = 0;
  uint16_t bw = 0;
  if (n > 0) {
    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    bw = w;
  }
  display.getTextBounds("0", 0, 0, &x1, &y1, &w, &h);
  return ceil((bw + boxes * (w + 3) + 1) / scale_x);
}

void GxEPDDisplay::endFrame() {
  if (_isOn == false) return;
  uint32_t crc = display_crc.finalize();
  if (_force_full_refresh) {
    // keep the rest of the firmware running while the panel refreshes
    if (_busy_hook) display.epd2.setBusyCallback(busyCallback, this);
    display.display(false); // full refresh, rewrites both controller buffers
    display.epd2.setBusyCallback(NULL);
    _force_full_refresh = false;
    _partial_refresh_count = 0;
  } else if (crc != last_display_crc_value) {
    // keep the rest of the firmware running while the panel refreshes (~0.5s partial)
    if (_busy_hook) display.epd2.setBusyCallback(busyCallback, this);
    display.display(true);
    display.powerOff();  // a partial refresh leaves the panel's booster running; nothing needs it until the next frame
    display.epd2.setBusyCallback(NULL);
    _partial_refresh_count++;
  }
  last_display_crc_value = crc;
}
