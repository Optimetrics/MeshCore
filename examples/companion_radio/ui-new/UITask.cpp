#include "UITask.h"
#include <helpers/TxtDataHelpers.h>
#include "../MyMesh.h"
#include "target.h"
#include <time.h>
#ifdef ENABLE_WIFI_INTERFACE
  #include <WiFi.h>
#endif

#ifndef AUTO_OFF_MILLIS
  #define AUTO_OFF_MILLIS     15000   // 15 seconds
#endif
#define BOOT_SCREEN_MILLIS   3000   // 3 seconds

#ifdef PIN_STATUS_LED
#define LED_ON_MILLIS     20
#define LED_ON_MSG_MILLIS 200
#define LED_CYCLE_MILLIS  4000
#endif

#define LONG_PRESS_MILLIS   1200

// "12s" / "5m" / "3h"; coarse (for e-ink) never counts seconds, which would repaint the panel every few seconds
static void formatAge(char* buf, int secs, bool coarse) {
  if (secs < 60) {
    if (coarse) strcpy(buf, "<1m");
    else sprintf(buf, "%ds", secs);
  } else if (secs < 60*60) {
    sprintf(buf, "%dm", secs / 60);
  } else {
    sprintf(buf, "%dh", secs / (60*60));
  }
}

// Used both for recent adverts and discovered nodes
// the board's backlight_btn object; a board whose Enter button also drives the backlight has none
#if defined(BACKLIGHT_BTN) && !defined(UI_ENTER_BTN)
  #define UI_HAS_BACKLIGHT_BTN 1
#else
  #define UI_HAS_BACKLIGHT_BTN 0
#endif

#ifndef UI_RECENT_LIST_SIZE
  #define UI_RECENT_LIST_SIZE 4
#endif

#if UI_HAS_JOYSTICK || UI_HAS_ROTARY_INPUT
  #define PRESS_LABEL "press Enter"
#elif defined(UI_ENTER_BTN)
  #define PRESS_LABEL "upper btn"   // a second button is Enter
#else
  #define PRESS_LABEL "long press"
#endif

// a clock earlier than the day this firmware was built was never set (a chip-less board
// boots its fallback clock at a fixed 2024 date)
static uint32_t buildEpoch() {
  static uint32_t epoch = DateTime(__DATE__, "00:00:00").unixtime();
  return epoch;
}

// a serial link outside nRF52 TinyUSB always reports "connected", so it can't tell us a reader is there
#if (defined(ENABLE_USB_INTERFACE) && !(defined(NRF52_PLATFORM) && defined(USE_TINYUSB))) || defined(SERIAL_RX)
  #define UI_LINK_UNDETECTABLE 1
#endif

#include "icons.h"

class SplashScreen : public UIScreen {
  UITask* _task;
  unsigned long dismiss_after;
  char _version_info[12];

public:
  SplashScreen(UITask* task) : _task(task) {
    // strip off dash and commit hash by changing dash to null terminator
    // e.g: v1.2.3-abcdef -> v1.2.3
    const char *ver = FIRMWARE_VERSION;
    const char *dash = strchr(ver, '-');

    int len = dash ? dash - ver : strlen(ver);
    if (len >= sizeof(_version_info)) len = sizeof(_version_info) - 1;
    memcpy(_version_info, ver, len);
    _version_info[len] = 0;

    dismiss_after = millis() + BOOT_SCREEN_MILLIS;
  }

  int render(DisplayDriver& display) override {
    // meshcore logo
    display.setColor(UIColor::corp_blue);
    int logoWidth = 128;
    display.drawXbm((display.width() - logoWidth) / 2, 3, meshcore_logo, logoWidth, 13);

    // meshcore website
    const char* website = "https://meshcore.io";
    display.setColor(UIColor::primary_txt);
    display.setTextSize(1);
    uint16_t websiteWidth = display.getTextWidth(website);
    display.setCursor((display.width() - websiteWidth) / 2, 22);
    display.print(website);

    // version info
    display.setColor(UIColor::primary_txt);
    display.setTextSize(1);
    display.drawTextCentered(display.width()/2, 35, _version_info);

    display.setColor(UIColor::secondary_txt);
    display.setTextSize(1);
    display.drawTextCentered(display.width()/2, 48, FIRMWARE_BUILD_DATE);

    return 1000;
  }

  void poll() override {
    if (millis() >= dismiss_after) {
      _task->gotoHomeScreen();
    }
  }
};

#if ENV_INCLUDE_GPS == 1
// exact period with the largest unit that divides it: "30s", "5m", "2h"
static void formatGpsPeriod(char* buf, size_t size, uint32_t seconds) {
  if (seconds % 3600 == 0) {
    snprintf(buf, size, "%luh", (unsigned long)(seconds / 3600));
  } else if (seconds % 60 == 0) {
    snprintf(buf, size, "%lum", (unsigned long)(seconds / 60));
  } else {
    snprintf(buf, size, "%lus", (unsigned long)seconds);
  }
}

// age of the last fix, rounded down to a single unit: "45s", "12m", "3h"
static void formatFixAge(char* buf, size_t size, uint32_t seconds) {
  if (seconds < 60) {
    snprintf(buf, size, "%lus", (unsigned long)seconds);
  } else if (seconds < 3600) {
    snprintf(buf, size, "%lum", (unsigned long)(seconds / 60));
  } else {
    snprintf(buf, size, "%luh", (unsigned long)(seconds / 3600));
  }
}
#endif

class HomeScreen : public UIScreen {
  enum HomePage {
    FIRST,
    RECENT,
    RADIO,
#ifdef BLE_PIN_CODE
    BLUETOOTH,
#endif
    ADVERT,
#if ENV_INCLUDE_GPS == 1
    GPS,
#endif
#if ENV_INCLUDE_GPS == 1 && defined(UI_TRACK_PAGE)
    TRACK,
#endif
#if UI_SENSORS_PAGE == 1
    SENSORS,
#endif
#if UI_DISCOVER_SCREEN
    DISCOVERY,
#endif
#ifndef UI_NO_HIBERNATE
    SHUTDOWN,
#endif
    Count    // keep as last
  };

  UITask* _task;
  mesh::RTCClock* _rtc;
  SensorManager* _sensors;
  NodePrefs* _node_prefs;
  uint8_t _page;
  bool _shutdown_init;
  AdvertPath recent[UI_RECENT_LIST_SIZE];
  int _shown_batt_pct = -1;
  uint8_t _confirm_page = 0xFF;     // page waiting for a second Enter, if any
  unsigned long _confirm_at = 0;    // when the first Enter came

  // destructive actions need a second Enter within 3 s (a quick click is easy to hit in a pocket)
  bool confirmEnter() {
#ifdef UI_ENTER_BTN
    if (confirmPending()) {
      _confirm_page = 0xFF;
      return true;
    }
    _confirm_page = _page;
    _confirm_at = millis();
    return false;
#else
    return true;   // Enter is already a long press here
#endif
  }
  bool confirmPending() const {
    return _confirm_page == _page && millis() - _confirm_at < 3000;
  }
#if UI_DISCOVER_SCREEN
  DiscoveredNode discovered[DISCOVERED_NODES_TABLE_SIZE]; // not circular, latest discovered nodes are not kept
  uint32_t disc_node_req_tag = 0;
  uint32_t disc_nodes_count = 0;
  uint32_t discovery_req_time = 0;
#endif

#if defined(UI_TRACK_PAGE) || defined(UI_RECENT_DETAIL)
  static double distanceMeters(double lat1, double lon1, double lat2, double lon2) {
    const double r = 6371000.0, d2r = M_PI / 180.0;
    double dlat = (lat2 - lat1) * d2r, dlon = (lon2 - lon1) * d2r;
    double a = sin(dlat / 2) * sin(dlat / 2) + cos(lat1 * d2r) * cos(lat2 * d2r) * sin(dlon / 2) * sin(dlon / 2);
    if (a > 1) a = 1;   // rounding at the antipode
    return 2 * r * atan2(sqrt(a), sqrt(1 - a));
  }

  // 8-point bearing from 1 to 2, north-up (there's no compass, so not relative to where we face)
  static const char* compassPoint(double lat1, double lon1, double lat2, double lon2) {
    const double d2r = M_PI / 180.0;
    double y = sin((lon2 - lon1) * d2r) * cos(lat2 * d2r);
    double x = cos(lat1 * d2r) * sin(lat2 * d2r) - sin(lat1 * d2r) * cos(lat2 * d2r) * cos((lon2 - lon1) * d2r);
    double brg = atan2(y, x) / d2r;
    if (brg < 0) brg += 360;
    static const char* points[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return points[(int)((brg + 22.5) / 45) % 8];
  }
#endif

#if ENV_INCLUDE_GPS == 1 && defined(UI_TRACK_PAGE)

  // the three most recently heard favourites that share a position: how far, which way, when last heard
  void renderTracking(DisplayDriver& display) {
    display.setTextSize(1);
    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(0, 18, "favourites");

    ContactInfo best[3], c;
    int n = 0;
    int first = the_mesh.getTotalContactSlots() - the_mesh.getNumContacts();   // skip the anonymous slots
    for (int i = first; i < the_mesh.getTotalContactSlots(); i++) {
      if (!the_mesh.getContactByIdx(i, c) || !c.isFav() || (c.gps_lat == 0 && c.gps_lon == 0)) continue;
      int pos = n;
      while (pos > 0 && best[pos - 1].lastmod < c.lastmod) pos--;   // lastmod is our clock, not theirs
      if (pos >= 3) continue;
      for (int k = (n < 3 ? n : 2); k > pos; k--) best[k] = best[k - 1];
      best[pos] = c;
      if (n < 3) n++;
    }

    display.setColor(UIColor::primary_txt);
    if (n == 0) {
      display.drawTextLeftAlign(0, 32, "No favourites with");
      display.drawTextLeftAlign(0, 43, "a shared position");
      return;
    }
    bool have_pos = sensors.node_lat != 0 || sensors.node_lon != 0;
    char tmp[32], age[8], filtered[sizeof(c.name)];
    int y = 31;
    for (int i = 0; i < n; i++, y += 24) {
      int secs = (int)(_rtc->getCurrentTime() - best[i].lastmod);
      formatAge(age, secs < 0 ? 0 : secs, display.isEink());
      int age_w = display.getTextWidth(age);
      display.translateUTF8ToBlocks(filtered, best[i].name, sizeof(filtered));
      display.setColor(UIColor::primary_txt);
      display.drawTextEllipsized(0, y, display.width() - age_w - 4, filtered);
      display.setColor(UIColor::secondary_txt);
      display.setCursor(display.width() - age_w - 1, y);
      display.print(age);

      if (!have_pos) {
        strcpy(tmp, "no own position");
      } else {
        double lat = best[i].gps_lat / 1000000.0, lon = best[i].gps_lon / 1000000.0;
        double d = distanceMeters(sensors.node_lat, sensors.node_lon, lat, lon);
        const char* dir = compassPoint(sensors.node_lat, sensors.node_lon, lat, lon);
        if (d < 1000) sprintf(tmp, "%d m %s", (int)d, dir);
        else if (d < 100000) sprintf(tmp, "%.1f km %s", d / 1000, dir);
        else sprintf(tmp, "%d km %s", (int)(d / 1000), dir);
      }
      display.setColor(UIColor::primary_txt);
      display.drawTextLeftAlign(8, y + 11, tmp);
    }
  }
#endif

  // returns the x where the status icons start, so the node name can stop short of them
  int renderBatteryIndicator(DisplayDriver& display, uint16_t batteryMilliVolts) {
    // Convert millivolts to percentage
#ifndef BATT_MIN_MILLIVOLTS
  #define BATT_MIN_MILLIVOLTS 3000
#endif
#ifndef BATT_MAX_MILLIVOLTS
  #define BATT_MAX_MILLIVOLTS 4200
#endif
#ifdef UI_BATT_LIPO_CURVE
    // a LiPo sits around 3.7-3.9 V for most of its charge, so map through its
    // resting-voltage curve (10% steps) rather than a straight line
    static const uint16_t lipo_mv[] = { 3300, 3690, 3730, 3770, 3800, 3840, 3870, 3950, 4020, 4110, 4190 };
    int batteryPercentage = 100;
    for (int i = 0; i < 10; i++) {
      if (batteryMilliVolts < lipo_mv[i + 1]) {
        batteryPercentage = batteryMilliVolts <= lipo_mv[i] ? i * 10
            : i * 10 + (batteryMilliVolts - lipo_mv[i]) * 10 / (lipo_mv[i + 1] - lipo_mv[i]);
        break;
      }
    }
#else
    const int minMilliVolts = BATT_MIN_MILLIVOLTS;
    const int maxMilliVolts = BATT_MAX_MILLIVOLTS;
    int batteryPercentage = ((batteryMilliVolts - minMilliVolts) * 100) / (maxMilliVolts - minMilliVolts);
    if (batteryPercentage < 0) batteryPercentage = 0; // Clamp to 0%
    if (batteryPercentage > 100) batteryPercentage = 100; // Clamp to 100%
#endif
    // only move the icon on a real change, not on reading noise (each move repaints an e-ink panel)
    if (_shown_batt_pct < 0 || abs(batteryPercentage - _shown_batt_pct) >= 3
        || batteryPercentage == 0 || batteryPercentage == 100) {
      _shown_batt_pct = batteryPercentage;
    }
    batteryPercentage = _shown_batt_pct;

    // battery icon
    int iconWidth = 24;
    int iconHeight = 10;
    int iconX = display.width() - iconWidth - 5; // Position the icon near the top-right corner
    int iconY = 0;
    display.setColor(UIColor::title_txt);

    // battery outline
    display.drawRect(iconX, iconY, iconWidth, iconHeight);

    // battery "cap"
    display.fillRect(iconX + iconWidth, iconY + (iconHeight / 4), 3, iconHeight / 2);

    // fill the battery based on the percentage
    int fillWidth = (batteryPercentage * (iconWidth - 4)) / 100;
    display.fillRect(iconX + 2, iconY + 2, fillWidth, iconHeight - 4);

    // status icons go right to left from the battery, each in the next free 9px slot:
    // charging, muted, phone connected, GPS fix, unread mail
    int x = iconX;

    // while charging, show a bolt (or a plug once full) just left of the battery,
    // keeping the fill bar itself clean and uninterrupted
    bool charging = board.isExternalPowered();
    if (charging) {
      // There's no charge-complete signal on most boards, so "full" is a high
      // voltage band rather than an exact 100% (a real pack rarely reads 4.2V).
      const int BATT_FULL_PCT = 95;
      const uint8_t* symbol = (batteryPercentage >= BATT_FULL_PCT) ? plug_icon : charging_icon;
      display.setColor(UIColor::title_txt);
      display.drawXbm(x -= 9, iconY + 1, symbol, 8, 8);
    }

#ifdef PIN_BUZZER
    if (_task->isBuzzerQuiet()) {
      display.setColor(UIColor::warning_txt);
      display.drawXbm(x -= 9, iconY + 1, muted_icon, 8, 8);
    }
#endif
#ifdef UI_STATUS_ICONS
    display.setColor(UIColor::title_txt);
    if (_task->isBluetoothConnected()) {
      display.drawXbm(x -= 9, iconY + 1, ble_link_icon, 8, 8);
    }
  #if ENV_INCLUDE_GPS == 1
    LocationProvider* loc = sensors.getLocationProvider();
    if (loc != NULL && _task->getGPSState()) {
      // between duty-cycled fixes the last one still counts while it is within an interval
      const char* iv = _task->getSensorSetting("gps_interval");
      uint32_t max_age = 300;
      if (iv != NULL && (uint32_t)atol(iv) + 60 > max_age) max_age = atol(iv) + 60;
      if (loc->getLastFixAge() <= max_age) display.drawXbm(x -= 9, iconY + 1, gps_fix_icon, 8, 8);
    }
  #endif
    if (_task->getUnreadCount() > 0) {
      display.drawXbm(x -= 9, iconY + 1, mail_icon, 8, 8);
    }
#endif
    return x;
  }

  CayenneLPP sensors_lpp;
  int sensors_nb = 0;
  bool sensors_scroll = false;
  int sensors_scroll_offset = 0;
  int next_sensors_refresh = 0;

  void refresh_sensors() {
    if (millis() > next_sensors_refresh) {
      sensors_lpp.reset();
      sensors_nb = 0;
      sensors_lpp.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);
      sensors.querySensors(0xFF, sensors_lpp);
      LPPReader reader (sensors_lpp.getBuffer(), sensors_lpp.getSize());
      uint8_t channel, type;
      while(reader.readHeader(channel, type)) {
        reader.skipData(type);
        sensors_nb ++;
      }
      sensors_scroll = sensors_nb > UI_RECENT_LIST_SIZE;
      if (display.isEink()) // preserve eink displays
        next_sensors_refresh = millis() + 60000; // refresh sensor values every 1 min
      else
        next_sensors_refresh = millis() + 5000; // refresh sensor values every 5 sec
    }
  }

public:
  HomeScreen(UITask* task, mesh::RTCClock* rtc, SensorManager* sensors, NodePrefs* node_prefs)
     : _task(task), _rtc(rtc), _sensors(sensors), _node_prefs(node_prefs), _page(0),
       _shutdown_init(false), sensors_lpp(200) {  }

#if UI_DISCOVER_SCREEN
  bool sendDiscoverRequest() {
    uint8_t cmd_bytes[6];
    cmd_bytes[0] = CTL_TYPE_NODE_DISCOVER_REQ | 1; // DISCOVER_REQ | prefix only
    cmd_bytes[1] = 0xFF;     // Repeaters
    the_mesh.getRNG()->random((uint8_t *) &disc_node_req_tag, 4);  // generate random tag
    memcpy(&cmd_bytes[2], &disc_node_req_tag, 4);
    disc_nodes_count = 0;
    mesh::Packet* req = the_mesh.createControlData(cmd_bytes, sizeof(cmd_bytes));
    if (req) {
      the_mesh.sendZeroHop(req);
      discovery_req_time = millis();
      return true;
    }
    return false;
  }

  void handleDiscoverResponse(const mesh::Packet* packet) {
    if (disc_nodes_count < DISCOVERED_NODES_TABLE_SIZE && memcmp(&packet->payload[2], &disc_node_req_tag, 4) == 0) {
      auto d = &discovered[disc_nodes_count++];
      memcpy(d->pubkey_prefix, &packet->payload[6], 8);
      d->type = packet->payload[0] & 0xF;
      d->snr_out = ((int8_t)packet->payload[1]) / 4.0;
      d->snr_in = radio_driver.getLastSNR();
      ContactInfo* c = the_mesh.lookupContactByPubKey(&packet->payload[6], 8);
      if (c != NULL) {
        strncpy(d->name, c->name, 32);
      } else {
        d->name[0] = 0;
      }
    }
  }
#endif

  void poll() override {
    if (_shutdown_init && !_task->isButtonPressed()) {  // must wait for USR button to be released
      _task->shutdown();
    }
  }

  int render(DisplayDriver& display) override {
    display.setColor(UIColor::title_bkg);
    display.fillRect(0, 0, display.width(), 12);
    char tmp[80];
    // battery and status icons first, so the node name can stop short of them
    int icons_x = renderBatteryIndicator(display, _task->getBattMilliVolts());

    // node name
    display.setTextSize(1);
    display.setColor(UIColor::title_txt);
    char filtered_name[sizeof(_node_prefs->node_name)];
    display.translateUTF8ToBlocks(filtered_name, _node_prefs->node_name, sizeof(filtered_name));
    display.drawTextEllipsized(0, 2, icons_x - 2, filtered_name);

    // curr page indicator
    if (UIColor::title_bkg == UIColor::window_bkg) {
      display.setColor(UIColor::title_txt);
    } else {
      display.setColor(UIColor::title_bkg);
    }
    int y = 14;
    int x = display.width() / 2 - 5 * (HomePage::Count-1);
    for (uint8_t i = 0; i < HomePage::Count; i++, x += 10) {
      if (i == _page) {
        display.fillRect(x-1, y-1, 4, 4);
      } else {
        display.fillRect(x, y, 2, 2);
      }
    }

    if (_page == HomePage::FIRST) {
      display.setColor(UIColor::primary_txt);
      #ifdef ENABLE_WIFI_INTERFACE
      // smaller unread message ui for wifi variants
      display.setTextSize(1);
      sprintf(tmp, "Unread Msg: %d", _task->getMsgCount());
      display.drawTextCentered(display.width() / 2, 22, tmp);
      #else
      // larger unread message ui for non wifi variants
      display.setTextSize(2);
      sprintf(tmp, "MSG: %d", _task->getUnreadCount());
      display.drawTextCentered(display.width() / 2, 22, tmp);
      #endif
      if (_node_prefs->ui_show_clock) {
        display.setTextSize(3);
        uint32_t now = _rtc->getCurrentTime();
        if (now < buildEpoch()) {   // not synced from the phone, GPS or a clock chip yet
          display.drawTextCentered(display.width() / 2, 60, "--:--");
          display.setTextSize(1);
          display.drawTextCentered(display.width() / 2, 80, "time not set");
        } else {
        now += (int32_t)(_node_prefs->tz_offset * 3600);
        DateTime dt (now);
        sprintf(tmp, "%02d:%02d", dt.hour(), dt.minute());
        display.drawTextCentered(display.width() / 2, 60, tmp);
        display.setTextSize(1);
        sprintf(tmp, "%02d/%02d/%d", dt.day(), dt.month(), dt.year());
        display.drawTextCentered(display.width() / 2, 80, tmp);
      }
      }
      #ifdef ENABLE_WIFI_INTERFACE
        IPAddress ip = WiFi.localIP();
        snprintf(tmp, sizeof(tmp), "IP: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
        display.setTextSize(1);
        display.drawTextCentered(display.width() / 2, 32, tmp);
      #endif
      // BT pin stays up until a phone pairs over BLE, even while a USB client is attached
      if (the_mesh.getBLEPin() != 0 && _task->isBluetoothEnabled() && !_task->isBluetoothConnected()) {
        display.setColor(UIColor::warning_txt);
        sprintf(tmp, "Pin:%d", the_mesh.getBLEPin());
        if (_node_prefs->ui_show_clock) {
          // big enough to read off while pairing, when there is room below the clock
          display.setTextSize(display.height() > 64 ? 2 : 1);
          display.drawTextCentered(display.width() / 2, display.height() > 64 ? 100 : 110, tmp);
        } else {
          display.setTextSize(2);
          display.drawTextCentered(display.width() / 2, 43, tmp);
        }
      } else if (_task->hasConnection()) {
        display.setColor(UIColor::warning_txt);
        display.setTextSize(1);
        if (_node_prefs->ui_show_clock)
          display.drawTextCentered(display.width() / 2, 110, "< Connected >");
        else
          display.drawTextCentered(display.width() / 2, 43, "< Connected >");
      }
    } else if (_page == HomePage::RECENT) {
      the_mesh.getRecentlyHeard(recent, UI_RECENT_LIST_SIZE);
      display.setColor(UIColor::primary_txt);
      int y = 20;
      for (int i = 0; i < UI_RECENT_LIST_SIZE; i++, y += 11) {
        auto a = &recent[i];
        if (a->name[0] == 0) continue;  // empty slot
        int secs = _rtc->getCurrentTime() - a->recv_timestamp;
        formatAge(tmp, secs, display.isEink());

        int timestamp_width = display.getTextWidth(tmp);
        int max_name_width = display.width() - timestamp_width - 1;
#ifdef UI_RECENT_DETAIL
        // right to left before the age: distance when both ends have a position, then
        // signal bars for a direct neighbour or the hop count for one further away
        int right = display.width() - timestamp_width - 4;
        char detail[12];
        ContactInfo* ci = the_mesh.lookupContactByPubKey(a->pubkey_prefix, sizeof(a->pubkey_prefix));
        if (ci != NULL && (ci->gps_lat != 0 || ci->gps_lon != 0) && (sensors.node_lat != 0 || sensors.node_lon != 0)) {
          double d = distanceMeters(sensors.node_lat, sensors.node_lon, ci->gps_lat / 1000000.0, ci->gps_lon / 1000000.0);
          if (d < 1000) sprintf(detail, "%dm", (int)d);
          else if (d < 100000) sprintf(detail, "%.1fk", d / 1000);
          else sprintf(detail, "%dk", (int)(d / 1000));
          int w = display.getTextWidth(detail);
          display.setCursor(right - w, y);
          display.print(detail);
          right -= w + 3;
        }
        int hops = a->path_len & 63;
        if (hops == 0) {
          int bars = a->snr_x4 >= 20 ? 4 : a->snr_x4 >= 0 ? 3 : a->snr_x4 >= -20 ? 2 : a->snr_x4 >= -40 ? 1 : 0;
          for (int b = 0; b < 4; b++) {   // rising bars, unlit ones as a stub
            int h = b < bars ? 2 + 2 * b : 1;
            display.fillRect(right - 11 + b * 3, y + 9 - h, 2, h);
          }
          right -= 14;
        } else {
          sprintf(detail, "%dhop", hops);   // not "h", which reads as hours next to the age
          int w = display.getTextWidth(detail);
          display.setCursor(right - w, y);
          display.print(detail);
          right -= w + 3;
        }
        max_name_width = right;
#endif

        char filtered_recent_name[sizeof(a->name)];
        display.translateUTF8ToBlocks(filtered_recent_name, a->name, sizeof(filtered_recent_name));
        display.drawTextEllipsized(0, y, max_name_width, filtered_recent_name);
        display.setCursor(display.width() - timestamp_width - 1, y);
        display.print(tmp);
      }
    } else if (_page == HomePage::RADIO) {
      display.setColor(UIColor::primary_txt);
      display.setTextSize(1);
      // freq / sf
      display.setCursor(0, 20);
      sprintf(tmp, "FQ: %06.3f   SF: %d", _node_prefs->freq, _node_prefs->sf);
      display.print(tmp);

      display.setCursor(0, 31);
      sprintf(tmp, "BW: %03.2f     CR: %d", _node_prefs->bw, _node_prefs->cr);
      display.print(tmp);

      // tx power,  noise floor
      display.setCursor(0, 42);
      sprintf(tmp, "TX: %ddBm", _node_prefs->tx_power_dbm);
      display.print(tmp);
      display.setCursor(0, 53);
      sprintf(tmp, "Noise floor: %d", radio_driver.getNoiseFloor());
      display.print(tmp);
#ifdef BLE_PIN_CODE
    } else if (_page == HomePage::BLUETOOTH) {
      display.setColor(UIColor::corp_blue);
      display.drawXbm((display.width() - 32) / 2, 18,
          _task->isBluetoothEnabled() ? bluetooth_on : bluetooth_off,
          32, 32);
      display.setColor(UIColor::secondary_txt);
      display.setTextSize(1);
      if (confirmPending()) {
        display.setColor(UIColor::warning_txt);
        display.drawTextCentered(display.width() / 2, 64 - 11, "again: turn BT off");
      } else {
        display.drawTextCentered(display.width() / 2, 64 - 11, "toggle: " PRESS_LABEL);
      }
#endif
    } else if (_page == HomePage::ADVERT) {
      display.setColor(UIColor::corp_blue);
      display.drawXbm((display.width() - 32) / 2, 18, advert_icon, 32, 32);
      display.setColor(UIColor::secondary_txt);
      display.drawTextCentered(display.width() / 2, 64 - 11, "advert: " PRESS_LABEL);
#if ENV_INCLUDE_GPS == 1
    } else if (_page == HomePage::GPS) {
      LocationProvider* nmea = sensors.getLocationProvider();
      char buf[50];
      int y = 18;
      bool gps_state = _task->getGPSState();
      bool hw_gps_state = nmea != NULL && nmea->isHardwareEnabled();
      uint32_t period = nmea != NULL ? nmea->getGpsPeriod() : 0;
      const char* sw = _task->getSensorSetting("gps_switch");    // boards that report their GPS switch
      const char* awake = _task->getSensorSetting("gps_awake");  // and whether it's asleep between fixes
      if (sw != NULL) {
        if (sw[0] == '0') strcpy(buf, "gps off (switch)");
        else if (!gps_state) strcpy(buf, "gps off");
        else if (awake != NULL && awake[0] == '0') strcpy(buf, "gps asleep");
        else strcpy(buf, "gps on");
      } else if (nmea != NULL && nmea->hasHardwareEnableState() && gps_state != hw_gps_state) {
        strcpy(buf, gps_state ? "gps off(hw)" : "gps off(sw)");
      } else if (!gps_state) {
        strcpy(buf, "gps off");
      } else if (period == 0) {
        strcpy(buf, "gps on");
      } else {
        char per[12];
        formatGpsPeriod(per, sizeof(per), period);
        snprintf(buf, sizeof(buf), "%s (%s)", nmea != NULL && nmea->isActive() ? "active" : "sleep", per);
      }
      display.setColor(UIColor::primary_txt);
      display.drawTextLeftAlign(0, y, buf);
      if (nmea == NULL) {
        y = y + 12;
        display.setColor(UIColor::secondary_txt);
        display.drawTextLeftAlign(0, y, "Can't access GPS");
      } else {
        // fix freshness: "fix" while current, otherwise how long ago the last one was
        uint32_t fix_age = nmea->getLastFixAge();
        bool fresh = nmea->isValid() && fix_age <= 5;
        if (fresh) {
          strcpy(buf, "fix");
        } else if (fix_age == UINT32_MAX) {
          strcpy(buf, "no fix");
        } else {
          char age[8];
          formatAge(age, fix_age, display.isEink());
          sprintf(buf, "fix %s", age);
        }
        display.setColor(UIColor::primary_txt);
        display.drawTextRightAlign(display.width()-1, y, buf);
        y = y + 12;
        display.setColor(UIColor::secondary_txt);
        display.drawTextLeftAlign(0, y, "sat");
        display.setColor(UIColor::primary_txt);
        int hdop = nmea->getHDOPx10();
        if (hdop >= 0 && hdop < 255 && display.height() > 64) {
          sprintf(buf, "%d  hdop %d.%d", (int)nmea->satellitesCount(), hdop / 10, hdop % 10);
        } else {
          sprintf(buf, "%d", (int)nmea->satellitesCount());
        }
        display.drawTextRightAlign(display.width()-1, y, buf);
        y = y + 12;
        // the live fix if there is one, else the last known position
        double lat = nmea->getLatitude()/1000000., lon = nmea->getLongitude()/1000000.;
        if (!fresh) {
          lat = sensors.node_lat;
          lon = sensors.node_lon;
        }
        display.setColor(UIColor::secondary_txt);
        display.drawTextLeftAlign(0, y, "pos");   // the first line already says how old the fix is
        display.setColor(UIColor::primary_txt);
        if (lat == 0 && lon == 0) strcpy(buf, "-");
        else sprintf(buf, "%.4f %.4f", lat, lon);
        display.drawTextRightAlign(display.width()-1, y, buf);
        y = y + 12;
        display.setColor(UIColor::secondary_txt);
        display.drawTextLeftAlign(0, y, "alt");
        display.setColor(UIColor::primary_txt);
        sprintf(buf, "%.2f", fresh ? nmea->getAltitude()/1000. : sensors.node_altitude);
        display.drawTextRightAlign(display.width()-1, y, buf);
        y = y + 12;
      }
#endif
#if ENV_INCLUDE_GPS == 1 && defined(UI_TRACK_PAGE)
    } else if (_page == HomePage::TRACK) {
      renderTracking(display);
#endif
#if UI_SENSORS_PAGE == 1
    } else if (_page == HomePage::SENSORS) {
      int y = 18;
      refresh_sensors();
      char buf[30];
      char name[30];
      LPPReader r(sensors_lpp.getBuffer(), sensors_lpp.getSize());

      for (int i = 0; i < sensors_scroll_offset; i++) {
        uint8_t channel, type;
        r.readHeader(channel, type);
        r.skipData(type);
      }

      for (int i = 0; i < (sensors_scroll?UI_RECENT_LIST_SIZE:sensors_nb); i++) {
        uint8_t channel, type;
        if (!r.readHeader(channel, type)) { // reached end, reset
          r.reset();
          r.readHeader(channel, type);
        }

        display.setCursor(0, y);
        float v;
        switch (type) {
          case LPP_GPS: // GPS
            float lat, lon, alt;
            r.readGPS(lat, lon, alt);
            strcpy(name, "gps"); sprintf(buf, "%.4f %.4f", lat, lon);
            break;
          case LPP_VOLTAGE:
            r.readVoltage(v);
            strcpy(name, "voltage"); sprintf(buf, "%6.2f", v);
            break;
          case LPP_CURRENT:
            r.readCurrent(v);
            strcpy(name, "current"); sprintf(buf, "%.3f", v);
            break;
          case LPP_TEMPERATURE:
            r.readTemperature(v);
            strcpy(name, "temperature"); sprintf(buf, "%.2f", v);
            break;
          case LPP_RELATIVE_HUMIDITY:
            r.readRelativeHumidity(v);
            strcpy(name, "humidity"); sprintf(buf, "%.2f", v);
            break;
          case LPP_BAROMETRIC_PRESSURE:
            r.readPressure(v);
            strcpy(name, "pressure"); sprintf(buf, "%.2f", v);
            break;
          case LPP_ALTITUDE:
            r.readAltitude(v);
            strcpy(name, "altitude"); sprintf(buf, "%.0f", v);
            break;
          case LPP_POWER:
            r.readPower(v);
            strcpy(name, "power"); sprintf(buf, "%6.2f", v);
            break;
          default:
            r.skipData(type);
            strcpy(name, "unk"); sprintf(buf, "");
        }
        display.setCursor(0, y);
        display.setColor(UIColor::secondary_txt);
        display.print(name);
        display.setColor(UIColor::primary_txt);
        display.setCursor(
          display.width()-display.getTextWidth(buf)-1, y
        );
        display.print(buf);
        y = y + 12;
      }
      if (sensors_scroll) sensors_scroll_offset = (sensors_scroll_offset+1)%sensors_nb;
      else sensors_scroll_offset = 0;
#endif
#if UI_DISCOVER_SCREEN
    } else if (_page == HomePage::DISCOVERY) {
      int count = disc_nodes_count;
      display.setColor(UIColor::primary_txt);
      int y = 20;
      for (int i = 0; i < count; i++, y += 11) {
        char name[32];
        auto a = &discovered[i];
        if ((a->name[0] == 0) || !_node_prefs->ui_nd_name) {
          mesh::Utils::toHex(name, a->pubkey_prefix, 4);
        } else {
          strncpy(name, a->name, 32);
        }
        char filtered_name[sizeof(name)];
        char snr_s[12];
        if (strlen(name) <= 8) { // display snr_o
          sprintf(snr_s, "%02.1f>%02.1f", a->snr_out, a->snr_in);
        } else {
          sprintf(snr_s, "%02.1f", a->snr_in);
        }
        int snr_width = display.getTextWidth(snr_s);
        int max_name_width = display.width() - snr_width - 1;
        display.translateUTF8ToBlocks(filtered_name, name, sizeof(filtered_name));
        display.drawTextEllipsized(0, y, max_name_width, filtered_name);
        display.setCursor(display.width() - snr_width - 1, y);
        display.print(snr_s);
      }
      if (millis() < discovery_req_time + 5000) {
        return 1000; // more frequent updates just after req
      } else if (count < DISCOVERED_NODES_TABLE_SIZE -1) { // show only 5 sec after last disc
        y = 10 + 11 * DISCOVERED_NODES_TABLE_SIZE;
        display.drawTextCentered(display.width() / 2, y, "discover: " PRESS_LABEL);
      }
#endif
#ifndef UI_NO_HIBERNATE
    } else if (_page == HomePage::SHUTDOWN) {
      display.setColor(UIColor::corp_blue);
      display.setTextSize(1);
      if (_shutdown_init) {
        display.setColor(UIColor::warning_txt);
        display.drawTextCentered(display.width() / 2, 34, "hibernating...");
      } else {
        display.setColor(UIColor::secondary_txt);
        display.drawXbm((display.width() - 32) / 2, 18, power_icon, 32, 32);
        if (confirmPending()) {
          display.setColor(UIColor::warning_txt);
          display.drawTextCentered(display.width() / 2, 64 - 11, "again: hibernate");
        } else {
          display.drawTextCentered(display.width() / 2, 64 - 11, "hibernate:" PRESS_LABEL);
        }
      }
#endif
    }
#if ENV_INCLUDE_GPS == 1 && defined(UI_TRACK_PAGE)
    if (_page == HomePage::TRACK) return 30000;   // positions only change with adverts
#endif
    if (confirmPending()) return 1000;   // take the "again:" hint down when the window closes
    return 5000;   // next render after 5000 ms
  }

  bool handleInput(char c) override {
    if (c == KEY_LEFT || c == KEY_PREV) {
      _page = (_page + HomePage::Count - 1) % HomePage::Count;
      _confirm_page = 0xFF;   // leaving a page drops its pending confirm
      return true;
    }
    if (c == KEY_NEXT || c == KEY_RIGHT) {
      _page = (_page + 1) % HomePage::Count;
      _confirm_page = 0xFF;
      // page-name pop-ups cost an e-ink panel two extra refreshes, so skip them there
      if (_page == HomePage::RECENT && !_task->isEinkDisplay()) {
        _task->showAlert("Recent adverts", 800);
      }
#if UI_DISCOVER_SCREEN
      if (_page == HomePage::DISCOVERY && !_task->isEinkDisplay()) {
        _task->showAlert("Repeater disc", 800);
      }
#endif
      return true;
    }
    if (c == KEY_ENTER && _page == HomePage::FIRST) {
      _task->showInbox();
      return true;
    }
#ifdef BLE_PIN_CODE
    if (c == KEY_ENTER && _page == HomePage::BLUETOOTH) {
      if (_task->isBluetoothEnabled()) {  // toggle Bluetooth on/off
        if (!confirmEnter()) return true;   // turning it off drops the phone: ask first
        _task->disableBluetooth();
        _task->notify(UIEventType::toggleOff);
      } else {
        _task->enableBluetooth();
        _task->notify(UIEventType::toggleOn);
      }
      if (_task->hasDisplay()) {   // headless builds can't see this page, so never make it stick there
        _node_prefs->ble_disabled = _task->isBluetoothEnabled() ? 0 : 1;
        the_mesh.savePrefs();   // keep the choice across reboots
      }
      return true;
    }
#endif
    if (c == KEY_ENTER && _page == HomePage::ADVERT) {
      _task->notify(UIEventType::ack);
      if (the_mesh.advert()) {
        _task->showAlert("Advert sent!", 1000);
      } else {
        _task->showAlert("Advert failed..", 1000);
      }
      return true;
    }
#if ENV_INCLUDE_GPS == 1
    if (c == KEY_ENTER && _page == HomePage::GPS) {
      if (_node_prefs->ui_gps_action == 1) _task->triggerGPS(); else _task->toggleGPS();
      return true;
    }
#endif
#if UI_SENSORS_PAGE == 1
    if (c == KEY_ENTER && _page == HomePage::SENSORS) {
      _task->toggleGPS();
      next_sensors_refresh=0;
      return true;
    }
#endif
#if UI_DISCOVER_SCREEN
    if (c == KEY_ENTER && _page == HomePage::DISCOVERY) {
      if (millis() > discovery_req_time + 5000) { // rate limiter
        sendDiscoverRequest();
      }
      return true;
    }
    if (c == KEY_SELECT && _page == HomePage::DISCOVERY) {
      _node_prefs->ui_nd_name = !_node_prefs->ui_nd_name;
      return true;
    }
#endif
#ifndef UI_NO_HIBERNATE
    if (c == KEY_ENTER && _page == HomePage::SHUTDOWN) {
      if (confirmEnter()) _shutdown_init = true;  // need to wait for button to be released
      return true;
    }
#endif
    return false;
  }
};

#ifndef UI_MSG_PREVIEW_SIZE
  #define UI_MSG_PREVIEW_SIZE 78
#endif

// the last MAX_INBOX_MSGS messages, newest first; they stay after being read or synced to the phone
class MsgPreviewScreen : public UIScreen {
  UITask* _task;
  mesh::RTCClock* _rtc;

  struct MsgEntry {
    unsigned long rx_millis;   // not the RTC: the clock may only get set after it arrived
    char origin[62];
    char msg[UI_MSG_PREVIEW_SIZE];
    bool read;
  };
  #define MAX_INBOX_MSGS   32
  int num_unread = 0;
  int count = 0;                   // messages held, up to MAX_INBOX_MSGS
  int head = MAX_INBOX_MSGS - 1;   // index of the newest message
  int cursor = 0;                  // shown message: 0 = newest
  MsgEntry entries[MAX_INBOX_MSGS];
  int _page_start = 0;    // where the shown part of a long message starts
  int _page_len = 0;      // how much of it fitted on screen
  bool _page_more = false;
  bool _user_view = false;  // the user opened the inbox or pressed a key here (not just an auto-show)

  MsgEntry* at(int pos) { return &entries[(head - pos + MAX_INBOX_MSGS) % MAX_INBOX_MSGS]; }

  void markShownRead() {
    if (count == 0) return;
    if (cursor >= count) cursor = count - 1;
    auto p = at(cursor);
    if (!p->read) {
      p->read = true;
      num_unread--;
    }
  }

public:
  MsgPreviewScreen(UITask* task, mesh::RTCClock* rtc) : _task(task), _rtc(rtc) { }

  void addPreview(uint8_t path_len, const char* from_name, const char* msg) {
    head = (head + 1) % MAX_INBOX_MSGS;
    if (count < MAX_INBOX_MSGS) count++;
    else if (!entries[head].read) num_unread--;   // the oldest one drops off unread

    auto p = &entries[head];
    p->rx_millis = millis();
    if (path_len == 0xFF) {
      sprintf(p->origin, "(D) %s:", from_name);
    } else {
      sprintf(p->origin, "(%d) %s:", (uint32_t) (path_len & 63), from_name);   // hop count, without the hash-size bits
    }
    StrHelper::strncpy(p->msg, msg, sizeof(p->msg));
    p->read = false;
    num_unread++;
    if (!_task->readerConnected() || count == 1) {   // no phone: the new message is the one shown
      cursor = 0;
      _page_start = 0;
      _page_more = false;
      _user_view = false;   // shown automatically, so not read yet
    } else if (cursor < count - 1) {
      cursor++;   // a phone only gets a banner: keep the reader on the same message
    }
  }

  int unreadCount() const { return num_unread; }
  int size() const { return count; }

  // the phone has synced them, so they've been seen there
  void markAllRead() {
    for (int i = 0; i < count; i++) at(i)->read = true;
    num_unread = 0;
  }

  void showNewest() { cursor = 0; _page_start = 0; _user_view = true; }

  int render(DisplayDriver& display) override {
    char tmp[24];
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.setColor(UIColor::corp_blue);
    if (count == 0) {
      display.print("Inbox");
      display.drawRect(0, 11, display.width(), 1);
      display.setColor(UIColor::secondary_txt);
      display.drawTextCentered(display.width() / 2, 30, "No messages yet");
      return 60000;
    }
    if (cursor >= count) cursor = count - 1;
    if (_user_view) markShownRead();   // an auto-shown message stays unread until someone looks
    auto p = at(cursor);
    sprintf(tmp, "Msg %d/%d", cursor + 1, count);
    display.print(tmp);

    int secs = (int)((millis() - p->rx_millis) / 1000);
    formatAge(tmp, secs, display.isEink());
    display.setCursor(display.width() - display.getTextWidth(tmp) - 2, 0);
    display.print(tmp);

    display.drawRect(0, 11, display.width(), 1);  // horiz line

    display.setColor(UIColor::secondary_txt);
    char filtered_origin[sizeof(p->origin)];
    display.translateUTF8ToBlocks(filtered_origin, p->origin, sizeof(filtered_origin));
    display.drawTextEllipsized(0, 14, display.width(), filtered_origin);   // one line, whatever the name

    display.setCursor(0, 25);
    display.setColor(UIColor::primary_txt);
    char filtered_msg[sizeof(p->msg)];
    display.translateUTF8ToBlocks(filtered_msg, p->msg, sizeof(filtered_msg));
    int len = strlen(filtered_msg);
    if (_page_start > len) _page_start = 0;
    int wrap_w = display.isEink() ? display.width() - 8 : display.width();   // keep clear of the "more" arrow
    _page_len = display.printWordWrap(&filtered_msg[_page_start], wrap_w);
    _page_more = _page_start + _page_len < len;
    if (_page_more) {   // small down arrow in the corner: next press shows the rest
      for (int r = 0; r < 3; r++) {
        display.fillRect(display.width() - 7 + r, display.height() - 5 + r, 5 - 2*r, 1);
      }
    }

    return display.isEink() ? 10000 : 1000; // 10 s for eink
  }

  bool handleInput(char c) override {
    markShownRead();   // a press here means the message on screen has been seen
    _user_view = true;
    if (c == KEY_NEXT || c == KEY_RIGHT) {   // rest of a long message, else the next older one
      if (_page_more) {
        _page_start += _page_len;
      } else if (cursor < count - 1) {
        cursor++;
        _page_start = 0;
      } else {
        _task->gotoHomeScreen();   // past the oldest
      }
      return true;
    }
    if (c == KEY_PREV || c == KEY_LEFT) {   // back to the start of this one, else the next newer one
      if (_page_start > 0) {
        _page_start = 0;
      } else if (cursor > 0) {
        cursor--;
      } else {
        _task->gotoHomeScreen();   // before the newest
      }
      return true;
    }
    if (c == KEY_ENTER) {   // back home; the messages stay
      _page_start = 0;
      _task->gotoHomeScreen();
      return true;
    }
    return false;
  }
};

void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs) {
  _display = display;
  _sensors = sensors;

#if defined(PIN_USER_BTN)
  user_btn.begin();
#endif
#ifdef UI_ENTER_BTN
  enter_btn.begin();
#endif
#if defined(PIN_USER_BTN_ANA)
  analog_btn.begin();
#endif
#if UI_HAS_BACKLIGHT_BTN
  backlight_btn.begin();
#endif

  _node_prefs = node_prefs;
  _auto_off = millis() + _node_prefs->ui_auto_off_delay * 1000;
  _backlight_off = millis() + _node_prefs->ui_backlight_delay * 1000;

  if (_display != NULL) {
    _display->turnOn(_node_prefs->ui_max_partial);
  }

#ifdef PIN_BUZZER
  buzzer.begin();
  buzzer.quiet(_node_prefs->buzzer_quiet);
  buzzer.startup();
#endif

#ifdef PIN_VIBRATION
  vibration.begin();
#endif

  ui_started_at = millis();
#if defined(UI_WAKE_ON_BUTTON) && defined(PIN_USER_BTN)
  // the press that woke us is still held: don't let it count as the CLI rescue long-press
  if (user_btn.isPressed()) ui_started_at = millis() - 8000;
#endif
  _alert_expiry = 0;

  splash = new SplashScreen(this);
  home = new HomeScreen(this, &rtc_clock, sensors, node_prefs);
  msg_preview = new MsgPreviewScreen(this, &rtc_clock);
  setCurrScreen(splash);
}

void UITask::showAlert(const char* text, int duration_millis) {
  strcpy(_alert, text);
  _alert_expiry = millis() + duration_millis;
}

void UITask::notify(UIEventType t) {
#if defined(PIN_BUZZER)
switch(t){
  case UIEventType::contactMessage:
    // gemini's pick
    buzzer.play("MsgRcv3:d=4,o=6,b=200:32e,32g,32b,16c7");
    break;
  case UIEventType::channelMessage:
    buzzer.play("kerplop:d=16,o=6,b=120:32g#,32c#");
    break;
  case UIEventType::ack:
    buzzer.play("ack:d=32,o=8,b=120:c");
    break;
  case UIEventType::toggleOn:
    buzzer.play("on:d=32,o=6,b=200:c,e,g");
    break;
  case UIEventType::toggleOff:
    buzzer.play("off:d=32,o=6,b=200:g,e,c");
    break;
  case UIEventType::roomMessage:
  case UIEventType::newContactMessage:
  case UIEventType::none:
  default:
    break;
}
#endif

#ifdef PIN_VIBRATION
  // Trigger vibration for all UI events except none
  if (t != UIEventType::none) {
    vibration.trigger();
  }
#endif
}

void UITask::onMessageRecv(mesh::Packet *pkt, const ContactInfo &from, uint8_t txt_type, uint32_t sender_timestamp, const char* text) {
  // we only want to show text messages on display, not cli data
  if (!(txt_type == TXT_TYPE_PLAIN || txt_type == TXT_TYPE_SIGNED_PLAIN)) return;

  uint8_t path_len = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
  ((MsgPreviewScreen *) msg_preview)->addPreview(path_len, from.name, text);
  showNewMessage(from.name);
  // a DM always beeps, phone or not; room-server posts (signed) are group traffic, so only without one
  if (txt_type == TXT_TYPE_PLAIN || !hasConnection()) {
    notify(UIEventType::contactMessage);
  }
}

void UITask::onChannelMessageRecv(mesh::Packet *pkt, ChannelDetails& channel_details, const char* text) {
  uint8_t path_len = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
  ((MsgPreviewScreen *) msg_preview)->addPreview(path_len, channel_details.name, text);
  showNewMessage(channel_details.name);
  if (!hasConnection()) {   // with a phone connected, it notifies for channels
    notify(UIEventType::channelMessage);
  }
}

// whether someone is reading on a connected phone or app, so the device shouldn't take over its screen
bool UITask::readerConnected() const {
#ifdef UI_LINK_UNDETECTABLE
  return isBluetoothConnected();   // the serial link is no evidence of a reader
#else
  return hasConnection();
#endif
}

// with no phone, the device is where it's read, so show it; with one, just a banner
void UITask::showNewMessage(const char* from) {
  if (!readerConnected() && !_cli_rescue_shown) {
    setCurrScreen(msg_preview);
  } else if (_display != NULL) {
    char name[32], banner[48];
    _display->translateUTF8ToBlocks(name, from, sizeof(name));
    snprintf(banner, sizeof(banner), "Msg: %s", name);
    showAlert(banner, 1500);
  }

  if (_display != NULL) {
    if (!_display->isOn() && !hasConnection()) {
      _display->turnOn(_node_prefs->ui_max_partial);
    }
    if (_display->isOn()) {
    _auto_off = millis() + _node_prefs->ui_auto_off_delay * 1000;  // extend the auto-off timer
    _next_refresh = 100;  // trigger refresh
    }
  }
}

void UITask::showInbox() {
  ((MsgPreviewScreen *) msg_preview)->showNewest();
  setCurrScreen(msg_preview);
}

int UITask::getUnreadCount() const {
  return msg_preview ? ((MsgPreviewScreen *) msg_preview)->unreadCount() : 0;
}

void UITask::onQueueSizeChanged(int msgcount) {
  _msgcount = msgcount;
  if (msgcount == 0 && msg_preview) {   // the phone has them all now
    ((MsgPreviewScreen *) msg_preview)->markAllRead();
  }
}

void UITask::onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t* path) {
  if (!hasConnection()) {
    notify(UIEventType::newContactMessage);
  }
}

void UITask::onControlDataRecv(const mesh::Packet* packet) {
#if UI_DISCOVER_SCREEN
  if (packet->payload_len >= 14 && (packet->payload[0] & 0xF0) == CTL_TYPE_NODE_DISCOVER_RESP) {
    ((HomeScreen *) home)->handleDiscoverResponse(packet);
  }
#endif
}

void UITask::userLedHandler() {
#ifdef PIN_STATUS_LED
  unsigned long cur_time = millis();
  if (cur_time - led_changed_at >= led_phase_millis) {   // rollover-safe
    led_changed_at = cur_time;
  #ifdef UI_STATUS_LED_UNREAD_ONLY
    int waiting = getUnreadCount();   // messages not yet seen on the device (or synced to the phone)
  #else
    int waiting = _msgcount;
  #endif
    if (led_state == 0) {
      led_state = 1;
      if (waiting > 0) {
        last_led_increment = LED_ON_MSG_MILLIS;
      } else {
        last_led_increment = LED_ON_MILLIS;
      }
      led_phase_millis = last_led_increment;
    } else {
      led_state = 0;
      led_phase_millis = LED_CYCLE_MILLIS - last_led_increment;
    }
  #ifdef UI_STATUS_LED_UNREAD_ONLY
    bool lit = led_state == 1 && waiting > 0;   // dark unless messages are waiting
  #else
    bool lit = led_state == 1;
  #endif
    digitalWrite(PIN_STATUS_LED, lit ? LED_STATE_ON : !LED_STATE_ON);
  }
#endif
}

void UITask::setCurrScreen(UIScreen* c) {
  if (_cli_rescue_shown) return;
  curr = c;
  _next_refresh = 100;
}

/*
  hardware-agnostic pre-shutdown activity should be done here
*/
void UITask::shutdown(bool restart, bool low_battery){
  if (_shutting_down) return;   // one shot: the hibernate page polls this until the board is off
  _shutting_down = true;
  the_mesh.flushPendingWrites();   // don't lose a contact change still waiting on its lazy save

  #ifdef PIN_BUZZER
  /* note: we have a choice here -
     we can do a blocking buzzer.loop() with non-deterministic consequences
     or we can set a flag and delay the shutdown for a couple of seconds
     while a non-blocking buzzer.loop() plays out in UITask::loop()
  */
  buzzer.shutdown();
  uint32_t buzzer_timer = millis(); // fail-safe shutdown
  while (buzzer.isPlaying() && (millis() - 2500) < buzzer_timer)
    buzzer.loop();

  #endif // PIN_BUZZER

  if (restart) {
    _board->reboot();
  } else {
    // leave a final screen up: an e-ink panel keeps it with the power off
    if (_display != NULL) {
      if (!_display->isOn()) _display->turnOn();   // a frame drawn while off is never shown
      _display->forceFullRefresh();
      _display->startFrame();
      _display->setColor(UIColor::primary_txt);
      _display->setTextSize(1);
      char filtered_name[sizeof(_node_prefs->node_name)];
      _display->translateUTF8ToBlocks(filtered_name, _node_prefs->node_name, sizeof(filtered_name));
      _display->drawTextCentered(_display->width() / 2, 2, filtered_name);
      _display->setTextSize(2);
      _display->setColor(low_battery ? UIColor::warning_txt : UIColor::primary_txt);
      _display->drawTextCentered(_display->width() / 2, 20, low_battery ? "Battery low" : "Powered off");
      _display->setTextSize(1);
      _display->setColor(UIColor::secondary_txt);
    #ifdef UI_WAKE_ON_BUTTON   // the board arms its buttons to wake it from power-off
      _display->drawTextCentered(_display->width() / 2, 40, low_battery ? "charge it, then" : "press a button");
      _display->drawTextCentered(_display->width() / 2, 52, low_battery ? "press a button" : "to wake");
    #else
      if (low_battery) _display->drawTextCentered(_display->width() / 2, 40, "charge it");
    #endif
      _display->endFrame();
      if (low_battery && !_display->isEink()) delay(3000);   // a lit display goes dark below, so give time to read it
      _display->turnOffKeepImage();
    }

    // Power off board including radio, display, GPS and components
    _board->powerOff();
    _board->reboot();   // System OFF didn't take; restart rather than keep running half shut down
  }
}

bool UITask::isButtonPressed() const {
#ifdef UI_ENTER_BTN
  if (enter_btn.isPressed()) return true;   // a button held into power-off would wake it straight back up
#endif
#ifdef PIN_USER_BTN
  return user_btn.isPressed();
#else
  return false;
#endif
}

#ifdef UI_ENTER_BTN
void UITask::setBacklight(bool on) {
  _backlight_on = on;
  _backlight_on_at = millis();
#if defined(DISP_BACKLIGHT)
  digitalWrite(DISP_BACKLIGHT, on);
#elif defined(EXP_PIN_BACKLIGHT)
  expander.digitalWrite(EXP_PIN_BACKLIGHT, on);
#endif
}
#endif

void UITask::loop() {
  char c = 0;
#if UI_HAS_JOYSTICK
  int ev = user_btn.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_ENTER);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_ENTER);  // REVISIT: could be mapped to different key code
  }
  ev = joystick_left.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_LEFT);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_LEFT);
  }
  ev = joystick_right.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_RIGHT);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_RIGHT);
  }
  ev = back_btn.check();
  if (ev == BUTTON_EVENT_TRIPLE_CLICK) {
    c = handleTripleClick(KEY_SELECT);
  }
#elif defined(PIN_USER_BTN)
  int ev = user_btn.check();
  #ifdef UI_HAS_NAV_INPUT
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_ENTER);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    display.turnOff(true);
  } else if (ev == BUTTON_EVENT_DOUBLE_CLICK) {
    c = handleDoubleClick(KEY_SELECT);
  } else if (ev == BUTTON_EVENT_TRIPLE_CLICK) {
    c = handleTripleClick(KEY_SELECT);
  }
  #else
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_NEXT);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
  #ifdef UI_ENTER_BTN
    // Enter is on the other button, so a hold here is free for a clean e-ink refresh
    if (handleLongPress(KEY_ENTER) != 0 && _display != NULL) {   // unless it opened the CLI rescue
      noteInput();
      _display->forceFullRefresh();
      _next_refresh = 0;
    }
  #else
    c = handleLongPress(KEY_ENTER);
  #endif
  } else if (ev == BUTTON_EVENT_DOUBLE_CLICK) {
    c = handleDoubleClick(KEY_PREV);
  } else if (ev == BUTTON_EVENT_TRIPLE_CLICK) {
    c = handleTripleClick(KEY_SELECT);
  }
  #endif
#endif
#ifdef UI_ENTER_BTN
  int eev = enter_btn.check();
  if (eev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_ENTER);
  } else if (eev == BUTTON_EVENT_LONG_PRESS) {
    noteInput();
    setBacklight(!_backlight_on);   // hold: light for a while, hold again: off
  }
#endif
#if defined(UI_HAS_ROTARY_INPUT)
  RotaryInputEvent rotaryEv = rotary_input.poll();
  if (c == 0 && _display != NULL) {
    if (rotaryEv == RotaryInputEvent::Next) {
      c = checkDisplayOn(KEY_NEXT);
    } else if (rotaryEv == RotaryInputEvent::Prev) {
      c = checkDisplayOn(KEY_PREV);
    }
  }
#endif
#if defined(PIN_USER_BTN_ANA)
  if (abs(millis() - _analogue_pin_read_millis) > 10) {
    int ev = analog_btn.check();
    if (ev == BUTTON_EVENT_CLICK) {
      c = checkDisplayOn(KEY_NEXT);
    } else if (ev == BUTTON_EVENT_LONG_PRESS) {
      c = handleLongPress(KEY_ENTER);
    } else if (ev == BUTTON_EVENT_DOUBLE_CLICK) {
      c = handleDoubleClick(KEY_PREV);
    } else if (ev == BUTTON_EVENT_TRIPLE_CLICK) {
      c = handleTripleClick(KEY_SELECT);
    }
    _analogue_pin_read_millis = millis();
  }
#endif
#if defined(UI_ENTER_BTN) && defined(BACKLIGHT_TIMEOUT_MILLIS)
  if (_backlight_on && millis() - _backlight_on_at >= BACKLIGHT_TIMEOUT_MILLIS) setBacklight(false);
#elif defined(DISP_BACKLIGHT) || defined(EXP_PIN_BACKLIGHT)
#if UI_HAS_BACKLIGHT_BTN
  if (millis() > next_backlight_btn_check) {
    if (backlight_btn.isPressed()) {
      if (!_display->isOn()) {
        _display->turnOn(_node_prefs->ui_max_partial);
        _next_refresh = 0;
      }
      _auto_off = millis() + _node_prefs->ui_auto_off_delay * 1000;   // extend auto-off timer
      _backlight_off = millis() + _node_prefs->ui_backlight_delay * 1000; // extend auto_off for backlight
#ifdef UI_EINK_IDLE_CLEANUP_SECS
      noteInput();   // reading by the backlight counts as use
#endif
    }
    next_backlight_btn_check = millis() + 100;
  }
  int bev = backlight_btn.check();
  if (bev == BUTTON_EVENT_DOUBLE_CLICK) {
    _display->forceFullRefresh();
  } else if (bev == BUTTON_EVENT_TRIPLE_CLICK) {
    _display->turnOff(true);
    _backlight_off = 0;
  }
#endif

  bool backlight_state;
  if (_node_prefs->ui_backlight_delay == -1) {
    backlight_state = true;
  } else if (_node_prefs->ui_backlight_delay < -1) {
    backlight_state = false;
  } else if (_node_prefs->ui_backlight_delay == 0) {
    backlight_state = false;
    #if UI_HAS_BACKLIGHT_BTN
      backlight_state = backlight_btn.isPressed();
    #endif
  } else {
    backlight_state = (millis() < _backlight_off);
  }

#if defined(DISP_BACKLIGHT)
  digitalWrite(DISP_BACKLIGHT, backlight_state);
#elif defined(EXP_PIN_BACKLIGHT)
  expander.digitalWrite(EXP_PIN_BACKLIGHT, backlight_state);
#endif
#endif

  if (c != 0 && curr) {
    curr->handleInput(c);
    _auto_off = millis() + _node_prefs->ui_auto_off_delay * 1000;   // extend auto-off timer
    _next_refresh = 100;  // trigger refresh
#ifdef UI_EINK_IDLE_CLEANUP_SECS
    noteInput();
#endif
  }

#if ENV_INCLUDE_GPS == 1
  // a hardware GPS switch (where the board reports one) is the master: keep the saved
  // setting in step with it, and confirm each flip
  if (millis() - _gps_switch_checked_at >= 500) {
    _gps_switch_checked_at = millis();
    const char* sw = getSensorSetting("gps_switch");
    if (sw != NULL) {
      int8_t on = sw[0] == '1';
      if (on != _gps_switch_seen) {
        bool first_look = _gps_switch_seen < 0;
        _gps_switch_seen = on;
        _sensors->setSettingValue("gps", on ? "1" : "0");   // so a flip the board's own poll missed still takes
        if (_node_prefs->gps_enabled != on) {
          _node_prefs->gps_enabled = on;
          the_mesh.savePrefs();
        }
        if (!first_look) {
          notify(on ? UIEventType::toggleOn : UIEventType::toggleOff);
          showAlert(on ? "GPS on" : "GPS off", 800);
          _next_refresh = 0;   // draw it now
        }
      }
    }
  }
#endif

  userLedHandler();

#ifdef PIN_BUZZER
  if (buzzer.isQuiet() != (bool)_node_prefs->buzzer_quiet) buzzer.quiet(_node_prefs->buzzer_quiet);
  if (buzzer.isPlaying())  buzzer.loop();
#endif

  if (curr) curr->poll();

  if (_display != NULL && _display->isOn()) {
    if (millis() >= _next_refresh && curr) {
      _display->startFrame();
      int delay_millis = curr->render(*_display);
      if (millis() < _alert_expiry) {  // render alert popup
        _display->setTextSize(1);
        int y = _display->height() / 3;
        int p = _display->height() / 32;
        _display->setColor(UIColor::popup_bkg);
        _display->fillRect(p, y, _display->width() - p*2, y);
        _display->setColor(UIColor::popup_txt);  // draw box border
        _display->drawRect(p, y, _display->width() - p*2, y);
        int box_w = _display->width() - p*2;
        if (_display->getTextWidth(_alert) > box_w - 4) {   // e.g. a long sender name in "Msg: ..."
          _display->drawTextEllipsized(p + 2, y + p*3, box_w - 4, _alert);
        } else {
          _display->drawTextCentered(_display->width() / 2, y + p*3, _alert);
        }
        _next_refresh = _alert_expiry;   // will need refresh when alert is dismissed
      } else {
        _next_refresh = millis() + delay_millis;
      }
      _display->endFrame();
    }
#ifdef UI_EINK_IDLE_CLEANUP_SECS
    // e-ink ghosting cleanup: one full refresh once the buttons have been left alone for a
    // while (and hourly while idle), instead of a flash in the middle of using the device
    {
      unsigned long now = millis();
      bool idle = now - _last_input_at >= UI_EINK_IDLE_CLEANUP_SECS * 1000UL;
      if (_display->getPartialRefreshCount() == 0) {
        // nothing to clean yet: the hourly clock starts at the first partial, and a press that drew nothing is spent
        if (idle) {
          _last_cleanup_at = now;
          _used_since_cleanup = false;
        }
      } else if (idle && (_used_since_cleanup || now - _last_cleanup_at >= 60UL * 60 * 1000)) {
        _display->forceFullRefresh();
        _next_refresh = 0;
        _last_cleanup_at = now;
        _used_since_cleanup = false;
      }
    }
#endif
    if (_node_prefs->ui_auto_off_delay > 0) {
#ifdef KEEP_DISPLAY_ON_USB
      // Opt-in: refresh the auto-off deadline while externally powered, so the
      // timer counts from the moment external power is removed. Off by default
      // because OLED panels burn in quickly; only enable for LCD targets or
      // where the display is replaceable.
      if (board.isExternalPowered()) {
        _auto_off = millis() + _node_prefs->ui_auto_off_delay * 1000;
      }
#endif
      if (millis() > _auto_off) {
        if (_node_prefs->ui_auto_off_blank &&
              !(_node_prefs->ui_auto_off_keep_msgs 
                          && curr == msg_preview)) { 
            // don't blank if messages are waiting
            _display->turnOff(true);
        } else {
          _display->turnOff();
        }
      }
    }
  }

#ifdef PIN_VIBRATION
  vibration.loop();
#endif

#ifdef AUTO_SHUTDOWN_MILLIVOLTS
  if (millis() > next_batt_chck) {
    uint16_t milliVolts = getBattMilliVolts();
    if (milliVolts > 0 && milliVolts < AUTO_SHUTDOWN_MILLIVOLTS && !board.isExternalPowered()) {
      // a TX burst can sag the cell for a moment, so only act on three low readings in a row
      if (++low_batt_readings >= 3) {
        shutdown(false, true);
      }
    } else {
      low_batt_readings = 0;
    }
    next_batt_chck = millis() + 8000;
  }
#endif
}

char UITask::checkDisplayOn(char c) {
  if (_display != NULL) {
    if (!_display->isOn()) {
      _display->turnOn(_node_prefs->ui_max_partial);   // turn display on and consume event
      c = 0;
    }
    _auto_off = millis() + _node_prefs->ui_auto_off_delay * 1000;   // extend auto-off timer
    if (_node_prefs->ui_backlight_on_keys) {
      _backlight_off = millis() + _node_prefs->ui_backlight_delay * 1000;
    }
    _next_refresh = 0;  // trigger refresh
#ifdef UI_EINK_IDLE_CLEANUP_SECS
    noteInput();
#endif
  }
  return c;
}

// shown while the companion is in CLI rescue mode, which otherwise looks just like a hang
class CliRescueScreen : public UIScreen {
  UITask* _task;
public:
  CliRescueScreen(UITask* task) : _task(task) { }
  int render(DisplayDriver& display) override {
    display.setColor(UIColor::warning_txt);
    display.setTextSize(2);
    display.drawTextCentered(display.width() / 2, 4, "CLI rescue");
    display.setTextSize(1);
    display.setColor(UIColor::primary_txt);
    display.drawTextCentered(display.width() / 2, 28, "serial console on USB");
    display.drawTextCentered(display.width() / 2, 40, "app link is paused");
    display.setColor(UIColor::secondary_txt);
    display.drawTextCentered(display.width() / 2, 52, "restart: " PRESS_LABEL);
    return 60000;
  }
  bool handleInput(char c) override {
    if (c == KEY_ENTER) _task->shutdown(true);   // the way out: write anything pending, then reboot
    return true;
  }
};

char UITask::handleLongPress(char c) {
  if (millis() - ui_started_at < 8000) {   // long press in first 8 seconds since startup -> CLI/rescue
    the_mesh.enterCLIRescue();
    static CliRescueScreen rescue(this);
    setCurrScreen(&rescue);
    _cli_rescue_shown = true;   // only a reboot leaves rescue mode, so nothing may replace its screen
    c = 0;   // consume event
  }
  return c;
}

char UITask::handleDoubleClick(char c) {
  MESH_DEBUG_PRINTLN("UITask: double-click triggered");
  checkDisplayOn(c);
  return c;
}

char UITask::handleTripleClick(char c) {
  MESH_DEBUG_PRINTLN("UITask: triple click triggered");
  checkDisplayOn(c);
  toggleBuzzer();
  c = 0;
  return c;
}

bool UITask::getGPSState() {
  const char* v = getSensorSetting("gps");
  return v != NULL && strcmp(v, "1") == 0;
}

const char* UITask::getSensorSetting(const char* name) {
  if (_sensors != NULL) {
    int num = _sensors->getNumSettings();
    for (int i = 0; i < num; i++) {
      if (strcmp(_sensors->getSettingName(i), name) == 0) {
        return _sensors->getSettingValue(i);
      }
    }
  }
  return NULL;
}

void UITask::toggleGPS() {
    if (_sensors != NULL) {
    // toggle GPS on/off
    int num = _sensors->getNumSettings();
    for (int i = 0; i < num; i++) {
      if (strcmp(_sensors->getSettingName(i), "gps") == 0) {
        if (strcmp(_sensors->getSettingValue(i), "1") == 0) {
          _node_prefs->getGpsPrefs()->setGpsEnabled(*_sensors, false);
          notify(UIEventType::toggleOff);
        } else {
          _node_prefs->getGpsPrefs()->setGpsEnabled(*_sensors, true);
          if (strcmp(_sensors->getSettingValue(i), "1") != 0) {   // e.g. a hardware GPS switch that is off
            _node_prefs->getGpsPrefs()->setGpsEnabled(*_sensors, false);
            showAlert("GPS switch is off", 1000);
            _next_refresh = 0;
            break;
          }
          notify(UIEventType::toggleOn);
        }
        the_mesh.savePrefs();
        showAlert(_node_prefs->getGpsPrefs()->isGpsEnabled() ? "GPS: Enabled" : "GPS: Disabled", 800);
        _next_refresh = 0;
        break;
      }
    }
  }
}

void UITask::triggerGPS() {
  LocationProvider* location = _sensors != NULL ? _sensors->getLocationProvider() : NULL;
  if (location != NULL && !(location->hasHardwareEnableState() && !location->isHardwareEnabled())
      && location->startOnce()) {
    notify(UIEventType::ack);
  }
  _next_refresh = 0;
}

void UITask::toggleBuzzer() {
    // Toggle buzzer quiet mode
  #ifdef PIN_BUZZER
    if (buzzer.isQuiet()) {
      buzzer.quiet(false);
      notify(UIEventType::ack);
    } else {
      buzzer.quiet(true);
    }
    _node_prefs->buzzer_quiet = buzzer.isQuiet();
    the_mesh.savePrefs();
    showAlert(buzzer.isQuiet() ? "Buzzer: OFF" : "Buzzer: ON", 800);
    _next_refresh = 0;  // trigger refresh
  #endif
}
