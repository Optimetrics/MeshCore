#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <ThinkNodeM1Board.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include <helpers/SensorManager.h>
#include <helpers/sensors/LocationProvider.h>
#ifdef DISPLAY_CLASS
  #include <helpers/ui/GxEPDDisplay.h>
  #include <helpers/ui/MomentaryButton.h>
#endif

class ThinkNodeM1SensorManager : public SensorManager {
  bool gps_active = false;          // switched on: awake, or asleep between fixes
  bool gps_awake = false;
  bool last_gps_switch_state = false;
  uint32_t gps_interval_secs = 0;   // from the app: sleep between fixes, 0 = run continuously
  unsigned long awake_since = 0, fix_at = 0, wake_at = 0;
  mutable char interval_str[12];
  LocationProvider* _location;

  void start_gps();
  void stop_gps();
  void wake_gps();
  void sleep_gps();
  void scheduleWake();
  bool dutyCycling() const;
public:
  ThinkNodeM1SensorManager(LocationProvider &location): _location(&location) { }
  LocationProvider* getLocationProvider() override { return _location; }
  bool begin() override;
  bool querySensors(uint8_t requester_permissions, CayenneLPP& telemetry) override;
  void loop() override;
  int getNumSettings() const override;
  const char* getSettingName(int i) const override;
  const char* getSettingValue(int i) const override;
  bool setSettingValue(const char* name, const char* value) override;
};

extern ThinkNodeM1Board board;
extern WRAPPER_CLASS radio_driver;
extern AutoDiscoverRTCClock rtc_clock;
extern ThinkNodeM1SensorManager sensors;

#ifdef DISPLAY_CLASS
  extern DISPLAY_CLASS display;
  extern MomentaryButton user_btn;
  #ifdef UI_ENTER_BTN
  extern MomentaryButton enter_btn;
  #endif
#endif

bool radio_init();
mesh::LocalIdentity radio_new_identity();
