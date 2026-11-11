#include <Arduino.h>
#include "target.h"
#include <helpers/ArduinoHelpers.h>
#include <helpers/sensors/MicroNMEALocationProvider.h>
#include <helpers/sensors/NmeaGpsDriver.h>

ThinkNodeM1Board board;

RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, SPI);

WRAPPER_CLASS radio_driver(radio, board);

VolatileRTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);
// The sensor manager below owns the GPS: the slide switch is the authority and it sleeps
// the L76K on its standby line between fixes. GPS_SWITCH_IS_HARDWARE is left at its
// default (1) so the driver doesn't also poll the switch and fight it.
NmeaGpsDriver gps_driver(Serial1);

// Shared code (the GPS CLI, saved prefs, the UI toggle) turns the GPS on and off through
// setGpsOn(); send that to the sensor manager so the switch check and duty cycling apply.
class ThinkNodeM1LocationProvider : public MicroNMEALocationProvider {
public:
  using MicroNMEALocationProvider::MicroNMEALocationProvider;
  void setGpsOn(bool on) override;
  bool isGpsOn() const override;
};

ThinkNodeM1LocationProvider nmea(gps_driver, &rtc_clock);
ThinkNodeM1SensorManager sensors = ThinkNodeM1SensorManager(nmea);

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true, true);   // no external pull-up on the M1 buttons
  #ifdef UI_ENTER_BTN
  MomentaryButton enter_btn(UI_ENTER_BTN, 1000, true, true, false);   // single clicks only, so they act at once
  #elif defined(BACKLIGHT_BTN)
  MomentaryButton backlight_btn(BACKLIGHT_BTN, 1000, true, true);
  #endif
#endif

bool radio_init() {
  rtc_clock.begin(Wire);
  return radio.std_init(&SPI);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);  // create new random identity
}

// GPS duty cycling: below this interval the receiver just stays on
#define GPS_DUTY_MIN_INTERVAL_SECS  300
#define GPS_SEEK_MILLIS             (180 * 1000UL)   // give up on a fix after this, sleep until the next try
#define GPS_HOLD_MILLIS             (10 * 1000UL)    // stay up after a fix to settle it and sync the clock

void ThinkNodeM1SensorManager::start_gps() {
  // the slide switch cuts GPS power: never drive its UART or control lines into an unpowered L76K
  if (!gps_active && digitalRead(PIN_GPS_SWITCH) == HIGH) {
    gps_active = true;
    _location->begin();   // the driver opens Serial1 and raises the standby and reset lines
    _location->reset();   // fresh power: reset, then multi-constellation and sentence setup (GPS_L76K)
    gps_awake = true;
    awake_since = millis();
    fix_at = 0;
  }
}

void ThinkNodeM1SensorManager::stop_gps() {
  if (gps_active) {
    gps_active = false;
    gps_awake = false;
    _location->stop();
    if (Serial1) Serial1.end();             // already ended if it was asleep between fixes (end() on a stopped UART never returns)
    nrf_gpio_cfg_default(PIN_SERIAL1_TX);   // stop the idle-high TX line back-feeding the GPS
  }
}

// between fixes: the L76K stops on its standby line and keeps its almanac for a quick next fix
void ThinkNodeM1SensorManager::sleep_gps() {
  _location->syncTime();   // drop the fix: after waking, the clock must only be set from a new one
  digitalWrite(PIN_GPS_STANDBY, LOW);
  if (Serial1) Serial1.end();
  gps_awake = false;
  scheduleWake();
}

// next wake one interval after the last one started, but never sooner than 30 s from now
void ThinkNodeM1SensorManager::scheduleWake() {
  unsigned long now = millis();
  unsigned long next = awake_since + gps_interval_secs * 1000UL;
  wake_at = (long)(next - now) > 30000 ? next : now + 30000;
}

void ThinkNodeM1SensorManager::wake_gps() {
  Serial1.begin(9600);
  digitalWrite(PIN_GPS_STANDBY, HIGH);
  gps_awake = true;
  awake_since = millis();
  fix_at = 0;
}

bool ThinkNodeM1SensorManager::dutyCycling() const {
  return gps_interval_secs >= GPS_DUTY_MIN_INTERVAL_SECS;
}

bool ThinkNodeM1SensorManager::begin() {
  // Initialize GPS switch pin
  pinMode(PIN_GPS_SWITCH, INPUT);
  last_gps_switch_state = digitalRead(PIN_GPS_SWITCH);

  // Initialize GPS power pin
  pinMode(GPS_EN, OUTPUT);

  // Check initial switch state to determine if GPS should be active
  if (last_gps_switch_state == HIGH) {  // Switch is HIGH when ON
    start_gps();
  }

  return true;
}

bool ThinkNodeM1SensorManager::querySensors(uint8_t requester_permissions, CayenneLPP& telemetry) {
  // no position yet is reported as none, not as 0,0
  if ((requester_permissions & TELEM_PERM_LOCATION) && (node_lat != 0 || node_lon != 0)) {
    telemetry.addGPS(TELEM_CHANNEL_SELF, node_lat, node_lon, node_altitude);
  }
  return true;
}

void ThinkNodeM1SensorManager::loop() {
  static long next_gps_update = 0;
  static long last_switch_check = 0;

  // the switch cuts GPS power: stop at once whenever it reads off, whatever the last poll saw
  if (gps_active && digitalRead(PIN_GPS_SWITCH) == LOW) {
    stop_gps();
    last_gps_switch_state = LOW;
  }

  // Check GPS switch state every second
  if (millis() - last_switch_check > 1000) {
    bool current_switch_state = digitalRead(PIN_GPS_SWITCH);
    
    // Detect switch state change
    if (current_switch_state != last_gps_switch_state) {
      last_gps_switch_state = current_switch_state;
      
      if (current_switch_state == HIGH) {  // Switch is ON
        MESH_DEBUG_PRINTLN("GPS switch ON");
        start_gps();
      } else {  // Switch is OFF
        MESH_DEBUG_PRINTLN("GPS switch OFF");
        stop_gps();
      }
    }
    
    last_switch_check = millis();
  }

  if (!gps_active) {
    return;  // GPS is not active, skip further processing
  }

  unsigned long now = millis();
  if (!gps_awake) {
    if (!dutyCycling() || (long)(now - wake_at) >= 0) wake_gps();
    return;
  }

  _location->loop();

  // a fix counts once a fresh GGA/RMC with a valid position arrived since this wake
  bool fresh_fix = _location->isValid() && _location->getLastFixAge() <= 2
                   && _location->getLastFixAge() * 1000UL <= now - awake_since;
  if (fresh_fix && fix_at == 0) fix_at = now;

  if (millis() > next_gps_update) {
    if (fresh_fix) {
      node_lat = ((double)_location->getLatitude())/1000000.;
      node_lon = ((double)_location->getLongitude())/1000000.;
      node_altitude = ((double)_location->getAltitude()) / 1000.0;
      MESH_DEBUG_PRINTLN("lat %f lon %f", node_lat, node_lon);
    }
    next_gps_update = millis() + 1000;
  }

  if (dutyCycling()) {
    bool settled = fix_at != 0 && now - fix_at >= GPS_HOLD_MILLIS;
    if (settled || (fix_at == 0 && now - awake_since >= GPS_SEEK_MILLIS)) sleep_gps();   // a late fix still gets its hold
  }
}

void ThinkNodeM1LocationProvider::setGpsOn(bool on) {
  sensors.setSettingValue("gps", on ? "1" : "0");
}

bool ThinkNodeM1LocationProvider::isGpsOn() const {
  return sensors.isGpsActive();
}

int ThinkNodeM1SensorManager::getNumSettings() const {
  return 4;
}

const char* ThinkNodeM1SensorManager::getSettingName(int i) const {
  switch (i) {
    case 0: return "gps";
    case 1: return "gps_interval";
    case 2: return "gps_switch";   // read-only: the hardware slide switch
    case 3: return "gps_awake";    // read-only: 0 while asleep between fixes
  }
  return NULL;
}

const char* ThinkNodeM1SensorManager::getSettingValue(int i) const {
  switch (i) {
    case 0: return gps_active ? "1" : "0";
    case 1:
      sprintf(interval_str, "%lu", (unsigned long)gps_interval_secs);
      return interval_str;
    case 2: return digitalRead(PIN_GPS_SWITCH) == HIGH ? "1" : "0";
    case 3: return gps_awake ? "1" : "0";
  }
  return NULL;
}

bool ThinkNodeM1SensorManager::setSettingValue(const char* name, const char* value) {
  if (strcmp(name, "gps") == 0) {
    if (strcmp(value, "0") == 0) {
      stop_gps();
    } else {
      if (digitalRead(PIN_GPS_SWITCH) != HIGH) return false;   // can't run it with the switch off
      start_gps();
    }
    return true;
  }
  if (strcmp(name, "gps_interval") == 0) {
    gps_interval_secs = strtoul(value, NULL, 10);
    if (gps_active && !gps_awake) {
      if (dutyCycling()) scheduleWake();   // the new interval applies to this sleep too
      else wake_gps();                     // back to continuous: wake now
    }
    return true;
  }
  return false;  // not supported
}
