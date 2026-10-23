#pragma once

#include <MeshCore.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/ui/UIScreen.h>
#include <helpers/SensorManager.h>
#include <helpers/MultiSerialInterface.h>
#include <Arduino.h>
#include <helpers/sensors/LPPDataHelpers.h>

#ifndef LED_STATE_ON
  #define LED_STATE_ON 1
#endif

#ifdef PIN_BUZZER
  #include <helpers/ui/buzzer.h>
#endif
#ifdef PIN_VIBRATION
  #include <helpers/ui/GenericVibration.h>
#endif

#include "../AbstractUITask.h"
#include "../NodePrefs.h"

#ifdef UI_NO_DISCOVER_SCREEN
  #error "UI_NO_DISCOVER_SCREEN is obsolete - use -D UI_DISCOVER_SCREEN=0 instead"
#endif

#ifndef UI_DISCOVER_SCREEN
  #define UI_DISCOVER_SCREEN  1
#endif

#if UI_DISCOVER_SCREEN
  #ifdef UI_RECENT_LIST_SIZE
    #define DISCOVERED_NODES_TABLE_SIZE UI_RECENT_LIST_SIZE
  #else
    #define DISCOVERED_NODES_TABLE_SIZE 4
  #endif

struct DiscoveredNode {
  float snr_in;
  float snr_out;
  uint8_t pubkey_prefix[9];
  uint8_t type;
  char name[32];
};
#endif

class UITask : public AbstractUITask {
  DisplayDriver* _display;
  SensorManager* _sensors;
#ifdef PIN_BUZZER
  genericBuzzer buzzer;
#endif
#ifdef PIN_VIBRATION
  GenericVibration vibration;
#endif
  unsigned long _next_refresh, _auto_off;
  NodePrefs* _node_prefs;
  char _alert[80];
  unsigned long _alert_expiry;
  int _msgcount;
  unsigned long ui_started_at, next_batt_chck;
  int next_backlight_btn_check = 0;
#ifdef BACKLIGHT_TIMEOUT_MILLIS
  unsigned long backlight_off_at = 0;
#endif
#ifdef PIN_STATUS_LED
  int led_state = 0;
  unsigned long led_changed_at = 0;
  unsigned long led_phase_millis = 0;
  int last_led_increment = 0;
#endif
#ifdef AUTO_SHUTDOWN_MILLIVOLTS
  uint8_t low_batt_readings = 0;
#endif
  bool _shutting_down = false;
#ifdef UI_EINK_IDLE_CLEANUP_SECS
  unsigned long _last_input_at = 0, _last_cleanup_at = 0;
  bool _used_since_cleanup = false;
  void noteInput() { _last_input_at = millis(); _used_since_cleanup = true; }
#endif

#ifdef PIN_USER_BTN_ANA
  unsigned long _analogue_pin_read_millis = millis();
#endif

  UIScreen* splash;
  UIScreen* home;
  UIScreen* msg_preview;
  UIScreen* curr;

  void userLedHandler();

  // Button action handlers
  char checkDisplayOn(char c);
  char handleLongPress(char c);
  char handleDoubleClick(char c);
  char handleTripleClick(char c);

  void setCurrScreen(UIScreen* c);

public:

  UITask(mesh::MainBoard* board, MultiSerialInterface* serial) : AbstractUITask(board, serial), _display(NULL), _sensors(NULL) {
    next_batt_chck = _next_refresh = 0;
    ui_started_at = 0;
    curr = NULL;
  }
  void begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs);

  void gotoHomeScreen() { setCurrScreen(home); }
  void showAlert(const char* text, int duration_millis);
  int  getMsgCount() const { return _msgcount; }
  bool hasDisplay() const { return _display != NULL; }
  bool isEinkDisplay() const { return _display != NULL && _display->isEink(); }
  bool isButtonPressed() const;

  bool isBuzzerQuiet() { 
#ifdef PIN_BUZZER
    return buzzer.isQuiet();
#else
    return true;
#endif
  }

  void toggleBuzzer();
  bool getGPSState();
  void toggleGPS();

  // MyMesh::Listener
  void onMessageRecv(mesh::Packet *pkt, const ContactInfo &from, uint8_t txt_type, uint32_t sender_timestamp, const char* text) override;
  void onChannelMessageRecv(mesh::Packet *pkt, ChannelDetails& channel_details, const char* text) override;
  void onQueueSizeChanged(int offline_queue_size) override;
  void onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t* path) override;
  void onControlDataRecv(const mesh::Packet* pkt) override;

  // from AbstractUITask
  void notify(UIEventType t = UIEventType::none) override;
  void loop() override;

  void shutdown(bool restart = false, bool low_battery = false);
};
