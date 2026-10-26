#include <Homie.h>
#include <driver/gpio.h>
#include <Arduino.h>
#include <WiFiUdp.h>
#include <string>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <ArduinoJson.h>
#include "IRelectra.h"
#include "display.h"

// Debug logging - controlled by DEBUG_SERIAL and DEBUG_MQTT build flags
#if DEBUG_SERIAL && DEBUG_MQTT
  #define DEBUG_LOG(x) do { Serial.println(x); debugNode.setProperty("log").send(x); } while(0)
#elif DEBUG_SERIAL
  #define DEBUG_LOG(x) Serial.println(x)
#elif DEBUG_MQTT
  #define DEBUG_LOG(x) debugNode.setProperty("log").send(x)
#else
  inline void DEBUG_LOG(const String&) { }
#endif

HomieNode temperatureNode("temperature", "temperature","temperature");
HomieNode modeNode("mode", "mode","mode");
HomieNode fanNode("fan", "fan","fan");
HomieNode swingNode("swing", "swing","swing");
HomieNode ifeelNode("ifeel", "ifeel","ifeel");
HomieNode ifeelTempNode("ifeel-temperature", "ifeel_temperature","ifeel_temperature");
HomieNode powerNode("power", "power","power");
HomieNode stateNode("state", "state","state");
HomieNode rebootNode("reboot", "reboot","reboot");
#if DEBUG_MQTT
HomieNode debugNode("debug", "debug","debug");
#endif

// Pin assignments - configure per-environment in platformio.ini build_flags
// e.g.  -D PIN_IR=23
#ifndef PIN_POWER
  #define PIN_POWER 5
#endif
#ifndef PIN_IR
  #define PIN_IR 4
#endif
#ifndef PIN_GREEN_LED
  #define PIN_GREEN_LED 12
#endif
#ifndef PIN_RED_LED
  #define PIN_RED_LED 15
#endif
#ifndef PIN_IR_RECV
  #define PIN_IR_RECV 14
#endif

// Boards without status LEDs (ESP-01, T-Display-S3) skip the LED code
#if !defined(ARDUINO_ESP8266_ESP01) && !defined(NO_STATUS_LEDS)
  #define HAS_STATUS_LEDS
#endif

const uint8_t POWER_PIN = PIN_POWER;
const uint8_t IR_PIN = PIN_IR;
#ifdef HAS_STATUS_LEDS
const uint8_t GREEN_LED_PIN = PIN_GREEN_LED;
const uint8_t RED_LED_PIN = PIN_RED_LED;
#endif

#ifndef ELECTRAWIFI_NO_IR_RCV
const uint8_t IR_RECV_PIN = PIN_IR_RECV;
#endif

const uint8_t kTimeout = 10;
const uint16_t kCaptureBufferSize = 300;

WiFiUDP udpClient;
IRelectra ac(IR_PIN);

#ifndef ELECTRAWIFI_NO_IR_RCV
IRrecv irrecv(IR_RECV_PIN, kCaptureBufferSize, kTimeout, true);
decode_results ir_ticks;
#endif

const int IFEEL_INTERVAL = 2 * 60000; // 2 min
const int POWER_DEBOUNCE = 2000; // 2 sec
const int UPDATES_INTERVAL = 10 * 60000; // 10 min
ulong power_change_time = 0;
ulong ifeel_send_time = 0;
ulong updates_send_time = 0;


// Last state names sent out, used by the on-board buttons to pick the next value
String current_mode = "cool";
String current_fan = "auto";

void send_updates() {
  String fan, mode, swing;
  if (ac.fan == FAN_LOW) {
    fan = "low";
  } else if (ac.fan == FAN_MED) {
    fan = "med";
  } else if (ac.fan == FAN_HIGH) {
    fan = "high";
  } else if (ac.fan == FAN_AUTO) {
    fan = "auto";
  } 
  
  ac.power_real = ac.power_setting;

  // The set mode is kept separately so the screen can show it while off
  String set_mode;
  if (ac.mode == MODE_COOL) {
    set_mode = "cool";
  } else if (ac.mode == MODE_HEAT) {
    set_mode = "heat";
  } else if (ac.mode == MODE_DRY) {
    set_mode = "dry";
  } else if (ac.mode == MODE_AUTO) {
    set_mode = "auto";
  } else if (ac.mode == MODE_FAN) {
    set_mode = "fan_only";
  }
  mode = ac.power_real ? set_mode : "off";
  current_mode = set_mode;
  current_fan = fan;

  if (ac.swing == SWING_ON && ac.swing_h == SWING_H_ON) {
    swing = "both";
  } else if (ac.swing == SWING_ON && ac.swing_h == SWING_H_OFF) {
    swing = "on";
  } else if (ac.swing == SWING_OFF && ac.swing_h == SWING_H_ON) {
    swing = "hor";
  } else {
    swing = "off";
  }
  
  DEBUG_LOG("Sending state update: power=" + String(ac.power_real ? "on" : "off") + 
         " mode=" + mode + 
         " fan=" + fan + 
         " swing=" + swing + 
         " temp=" + String(ac.temperature) + 
         " ifeel=" + String(ac.ifeel == IFEEL_ON ? "on" : "off"));
  
  powerNode.setProperty("state").send(ac.power_real ? "on": "off");
  fanNode.setProperty("state").send(fan);
  modeNode.setProperty("state").send(mode);
  swingNode.setProperty("state").send(swing);
  temperatureNode.setProperty("state").send(String(ac.temperature));
  ifeelNode.setProperty("state").send(ac.ifeel == IFEEL_ON ? "on" : "off");

#ifdef HAS_DISPLAY
  display_show(ac.power_real, set_mode, ac.temperature, fan);
#endif
}

// Reads the AC's "running" line. Runs from loop(), not from Homie's loop
// handler, so the screen follows the AC even while WiFi or MQTT is down.
void readPower() {
  ulong now = millis();

  // Set power led and update the power state after it's stable for a while
#ifdef POWER_PIN_ADC
  // With the internal pull-up the line sits near 3.3V while the AC is off and
  // the AC pulls it to about 0.7V when it runs. Read it as a voltage with
  // hysteresis so it never chatters.
  static bool power_level = false;
  int power_mv = analogReadMilliVolts(POWER_PIN);
  if (power_mv < 1200) {
    power_level = true;
  } else if (power_mv > 2000) {
    power_level = false;
  }
  uint power_state = power_level;
#else
  uint power_state = !digitalRead(POWER_PIN);
#endif
#ifdef HAS_STATUS_LEDS
  digitalWrite(GREEN_LED_PIN, power_state);
#endif
  if (power_state != ac.power_real) {
    if (power_change_time) {
      if (now - power_change_time > POWER_DEBOUNCE) {
        DEBUG_LOG("Power pin state changed: " + String(power_state ? "on" : "off"));
        ac.power_real = power_state;
        ac.power_setting = power_state;
        powerNode.setProperty("state").send(power_state ? "on": "off");
        send_updates();
        power_change_time = 0;
      }
    } else {
      power_change_time = now;
    }
  } else {
    power_change_time = 0;
  }
}

void loopHandler() {
  ulong now = millis();

  // Send ifeel if relevant
  if (now - ifeel_send_time >= IFEEL_INTERVAL) {
    if (ac.ifeel == IFEEL_ON) {
      DEBUG_LOG("Sending ifeel temperature: " + String(ac.ifeel_temperature));
      ac.SendElectra(true);
    }
    ifeel_send_time = now;
  }

  // Send updates
  /*
  if (now - updates_send_time >= UPDATES_INTERVAL) {
    send_updates();
    updates_send_time = now;
  }
  */

}

// Forwards commands from the original remote to the AC. Runs from loop(), not
// from Homie's loop handler, so the remote keeps working while WiFi or MQTT is down.
void handleIR() {
#ifndef ELECTRAWIFI_NO_IR_RCV
  if (irrecv.decode(&ir_ticks)) {
    uint64_t code = 0;
    code = DecodeElectraIR(ir_ticks);
    irrecv.resume();
#ifdef PIN_BUZZER
    // high beep: an Electra command was understood; low beep: IR was seen but not decoded
    tone(PIN_BUZZER, code ? 2700 : 1500, 60);
#endif
    if (code) {
      DEBUG_LOG("IR command received: 0x" + String((unsigned long)code, HEX));
      ac.UpdateFromIR(code);
      ac.SendElectra(false);
      send_updates();
    }
  }
#endif
}

#ifdef HA_DISCOVERY
// Announces the AC to Home Assistant through MQTT discovery, so it shows up as a
// climate entity without any YAML. Uses the same topics as the manual setup in the README.
void publishDiscovery() {
  const HomieInternals::ConfigStruct& cfg = Homie.getConfiguration();
  String base = String(cfg.mqtt.baseTopic) + cfg.deviceId + "/";
  String id = String("electrawifi_") + cfg.deviceId;

  String payload = "{";
  payload += "\"name\":null,";
  payload += "\"unique_id\":\"" + id + "\",";
  payload += "\"availability_topic\":\"" + base + "$state\",";
  payload += "\"payload_available\":\"ready\",\"payload_not_available\":\"lost\",";
  payload += "\"modes\":[\"off\",\"cool\",\"heat\",\"dry\",\"fan_only\",\"auto\"],";
  payload += "\"fan_modes\":[\"low\",\"med\",\"high\",\"auto\"],";
  payload += "\"swing_modes\":[\"off\",\"on\",\"hor\",\"both\"],";
  payload += "\"min_temp\":16,\"max_temp\":30,\"temp_step\":1,\"precision\":1.0,\"temperature_unit\":\"C\",";
  payload += "\"power_command_topic\":\"" + base + "power/state/set\",";
  payload += "\"payload_on\":\"on\",\"payload_off\":\"off\",";
  payload += "\"mode_command_topic\":\"" + base + "mode/state/set\",";
  payload += "\"mode_state_topic\":\"" + base + "mode/state\",";
  payload += "\"temperature_command_topic\":\"" + base + "temperature/state/set\",";
  payload += "\"temperature_state_topic\":\"" + base + "temperature/state\",";
  payload += "\"fan_mode_command_topic\":\"" + base + "fan/state/set\",";
  payload += "\"fan_mode_state_topic\":\"" + base + "fan/state\",";
  payload += "\"swing_mode_command_topic\":\"" + base + "swing/state/set\",";
  payload += "\"swing_mode_state_topic\":\"" + base + "swing/state\",";
  payload += "\"device\":{\"identifiers\":[\"" + id + "\"],\"manufacturer\":\"Electra\",\"model\":\"ElectraWifi\",\"name\":\"" + String(cfg.name) + "\"}";
  payload += "}";

  String topic = "homeassistant/climate/" + id + "/config";
  Homie.getMqttClient().publish(topic.c_str(), 1, true, payload.c_str());
  DEBUG_LOG("Published Home Assistant discovery to " + topic);
}
#endif

#if defined(HAS_DISPLAY) || defined(HA_DISCOVERY)
void onHomieEvent(const HomieEvent& event) {
  static bool wifi = false, mqtt = false;
  switch (event.type) {
    case HomieEventType::WIFI_CONNECTED: wifi = true; break;
    case HomieEventType::WIFI_DISCONNECTED: wifi = false; mqtt = false; break;
    case HomieEventType::MQTT_READY:
      mqtt = true;
#ifdef HA_DISCOVERY
      publishDiscovery();
      send_updates();
#endif
      break;
    case HomieEventType::MQTT_DISCONNECTED: mqtt = false; break;
    default: return;
  }
#ifdef HAS_DISPLAY
  display_link(wifi, mqtt);
#endif
}
#endif

bool powerHandler(const HomieRange& range, const String& value) {
  // This method is called when using the HA actions climate.turn_on and climate.turn_off
  DEBUG_LOG("MQTT received: power=" + value);
  if (value == "on") {
    ac.power_setting = true;
  } else if (value == "off") {
    ac.power_setting = false;
  } else {
    DEBUG_LOG("MQTT error: invalid power value");
    return false;
  }
  ac.SendElectra(false);
  ac.power_real = ac.power_setting;
  powerNode.setProperty("state").send(ac.power_real ? "on": "off");
  send_updates();
  return true;
}

bool temperatureHandler(const HomieRange& range, const String& value) {
  DEBUG_LOG("MQTT received: temperature=" + value);
  uint8_t temp = value.toInt();
  if (temp < 15 || temp > 30) { // setpoint temp has only 4 bits where 15 == 0b0000 and 30 == 0b1111
    DEBUG_LOG("MQTT error: temperature out of range (15-30)");
    return false;
  }
  ac.temperature = temp;
  ac.SendElectra(false);
  send_updates();
  return true;
}

bool modeHandler(const HomieRange& range, const String& value) {
  // This method is called when using the HA action climate.set_hvac_mode
  // Since climate.set_hvac_mode can be used to switch between `off` and other modes instead of climate.turn_on/off, it needs to call powerHandler.
  DEBUG_LOG("MQTT received: mode=" + value);
  if (value == "cool") {
    ac.mode = MODE_COOL;
  } else if (value == "heat") {
    ac.mode = MODE_HEAT;
  } else if (value == "auto") {
    ac.mode = MODE_AUTO;
  } else if (value == "dry") {
    ac.mode = MODE_DRY;
  } else if (value == "fan" || value == "fan_only") {
    ac.mode = MODE_FAN;
  } else if (value == "off") {
    return powerHandler(range, "off");
  } else {
    DEBUG_LOG("MQTT error: invalid mode value");
    return false;
  }
  return powerHandler(range, "on");
}

bool fanHandler(const HomieRange& range, const String& value) {
  DEBUG_LOG("MQTT received: fan=" + value);
  if (value == "low") {
    ac.fan = FAN_LOW;
  } else if (value == "med") {
    ac.fan = FAN_MED;
  } else if (value == "high") {
    ac.fan = FAN_HIGH;
  } else if (value == "auto") {
    ac.fan = FAN_AUTO;
  } else {
    DEBUG_LOG("MQTT error: invalid fan value");
    return false;
  }
  ac.SendElectra(false);
  send_updates();
  return true;
}

bool ifeelHandler(const HomieRange& range, const String& value) {
  DEBUG_LOG("MQTT received: ifeel=" + value);
  if (value == "on") {
    ac.ifeel = IFEEL_ON;
  } else if (value == "off") {
    ac.ifeel = IFEEL_OFF;
  } else {
    DEBUG_LOG("MQTT error: invalid ifeel value");
    return false;
  }
  ac.SendElectra(false);
  send_updates();
  return true;
}

bool ifeelTempHandler(const HomieRange& range, const String& value) {
  DEBUG_LOG("MQTT received: ifeel_temperature=" + value);
  uint8_t temp = value.toInt();
  if (temp < 5 || temp > 36) { // ifeel temp has only 5 bits where 0 == 0b00000 and 36 == 0b11111
    DEBUG_LOG("MQTT error: ifeel_temperature out of range (5-36)");
    return false;
  }
  ac.ifeel_temperature = temp;
  send_updates();
  return true;
}

bool swingHandler(const HomieRange& range, const String& value) {
  DEBUG_LOG("MQTT received: swing=" + value);
  if (value == "on") {
    ac.swing = SWING_ON;
    ac.swing_h = SWING_H_OFF;
  } else if (value == "both") {
    ac.swing = SWING_ON;
    ac.swing_h = SWING_H_ON;
  } else if (value == "hor") {
    ac.swing = SWING_OFF;
    ac.swing_h = SWING_H_ON;
  }  else if (value == "off"){
    ac.swing = SWING_OFF;
    ac.swing_h = SWING_H_OFF;
  } else {
    DEBUG_LOG("MQTT error: invalid swing value");
    return false;
  }
  ac.SendElectra(false);
  send_updates();
  return true;
}


bool jsonHandler(const HomieRange& range, const String& value) {
  DEBUG_LOG("MQTT received: json=" + value);
  StaticJsonDocument<200> parsed;
  auto error = deserializeJson(parsed,value);
  if (error) {
    DEBUG_LOG("MQTT error: failed to parse JSON");
    return false;
  }

  String fan = parsed["fan"];
  String mode = parsed["mode"];
  String power = parsed["power"];
  String ifeel = parsed["ifeel"];
  String temp_str = parsed["temperature"];
  String swing = parsed["swing"];
  uint8_t temp = temp_str.toInt();

  if (mode == "cool") {
    ac.mode = MODE_COOL;
  } else if (mode == "heat") {
    ac.mode = MODE_HEAT;
  } else if (mode == "auto") {
    ac.mode = MODE_AUTO;
  } else if (mode == "dry") {
    ac.mode = MODE_DRY;
  } else if (mode == "fan") {
    ac.mode = MODE_FAN;
  } else {
    return false;
  }

  if (fan == "low") {
    ac.fan = FAN_LOW;
  } else if (fan == "med") {
    ac.fan = FAN_MED;
  } else if (fan == "high") {
    ac.fan = FAN_HIGH;
  } else if (fan == "auto") {
    ac.fan = FAN_AUTO;
  } else {
    return false;
  }

  if (power == "on") {
    ac.power_setting = true;
  } else if (power == "off") {
    ac.power_setting = false;
  } else {
    return false;
  }

  if (swing == "on") {
    ac.swing = SWING_ON;
    ac.swing_h = SWING_H_OFF;
  } else if (swing == "both") {
    ac.swing = SWING_ON;
    ac.swing_h = SWING_H_ON;
  } else if (swing == "hor") {
    ac.swing = SWING_OFF;
    ac.swing_h = SWING_H_ON;
  }  else if (swing == "off") {
    ac.swing = SWING_OFF;
    ac.swing_h = SWING_H_OFF;
  } else {
    return false;
  }

  if (ifeel == "on") {
    ac.ifeel = IFEEL_ON;
  } else if (ifeel == "off") {
    ac.ifeel = IFEEL_OFF;
  } else {
    return false;
  }

  if (temp < 15 || temp > 30) { // setpoint temp has only 4 bits where 15 == 0b0000 and 30 == 0b1111
    return false;
  }
  ac.temperature = temp;

  ac.SendElectra(false);
  ac.power_real = ac.power_setting;

  send_updates();
  return true;
}

bool rebootHandler(const HomieRange& range, const String& value) {
  if (value == "true") {
    DEBUG_LOG("Rebooting...");
    ESP.restart();
    return true;
  }
  DEBUG_LOG("MQTT error: invalid reboot value (expected \"true\")");
  return false;
}

#ifdef HAS_BUTTONS
// Two buttons on the board. Mode: off -> cool -> heat -> dry -> fan -> auto -> off.
// Fan: low -> med -> high -> auto. Presses only change the screen; the command
// is sent once the buttons have been left alone for a moment.
const char* const MODE_CYCLE[] = {"off", "cool", "heat", "dry", "fan_only", "auto"};
const char* const FAN_CYCLE[] = {"low", "med", "high", "auto"};
const int MODE_COUNT = sizeof(MODE_CYCLE) / sizeof(MODE_CYCLE[0]);
const int FAN_COUNT = sizeof(FAN_CYCLE) / sizeof(FAN_CYCLE[0]);
const ulong BUTTON_DEBOUNCE = 40;
const ulong BUTTON_APPLY_DELAY = 1200;

String pending_mode, pending_fan;
ulong pending_time = 0;

int cycle_index(const char* const cycle[], int count, const String& value) {
  for (int i = 0; i < count; i++) {
    if (value == cycle[i]) return i;
  }
  return 0;
}

bool button_pressed(uint8_t pin, bool& last_state, ulong& last_change) {
  bool state = digitalRead(pin) == LOW;
  ulong now = millis();
  if (state != last_state && now - last_change > BUTTON_DEBOUNCE) {
    last_state = state;
    last_change = now;
    return state;
  }
  return false;
}

void handleButtons() {
  static bool mode_down = false, fan_down = false;
  static ulong mode_change = 0, fan_change = 0;
  bool changed = false;

  if (button_pressed(PIN_BTN_MODE, mode_down, mode_change)) {
    if (!pending_time) {
      pending_mode = ac.power_real ? current_mode : "off";
      pending_fan = current_fan;
    }
    int i = cycle_index(MODE_CYCLE, MODE_COUNT, pending_mode);
    pending_mode = MODE_CYCLE[(i + 1) % MODE_COUNT];
    changed = true;
  }
  if (button_pressed(PIN_BTN_FAN, fan_down, fan_change)) {
    if (!pending_time) {
      pending_mode = ac.power_real ? current_mode : "off";
      pending_fan = current_fan;
    }
    int i = cycle_index(FAN_CYCLE, FAN_COUNT, pending_fan);
    pending_fan = FAN_CYCLE[(i + 1) % FAN_COUNT];
    changed = true;
  }

  if (changed) {
#ifdef PIN_BUZZER
    // short beep so a press is heard even when the AC does not answer;
    // a 2.7 kHz tone sounds on both active and passive buzzers
    tone(PIN_BUZZER, 2700, 60);
#endif
    pending_time = millis();
    bool on = pending_mode != "off";
    display_show(on, on ? pending_mode : current_mode, ac.temperature, pending_fan);
    return;
  }

  if (pending_time && millis() - pending_time > BUTTON_APPLY_DELAY) {
    pending_time = 0;
    DEBUG_LOG("Buttons: mode=" + pending_mode + " fan=" + pending_fan);
    if (pending_fan == "low") {
      ac.fan = FAN_LOW;
    } else if (pending_fan == "med") {
      ac.fan = FAN_MED;
    } else if (pending_fan == "high") {
      ac.fan = FAN_HIGH;
    } else {
      ac.fan = FAN_AUTO;
    }
    HomieRange range = {false, 0};
    modeHandler(range, pending_mode);
  }
}
#endif

void setup() {
  Serial.begin(115200);
  Serial << endl << endl;
#ifdef HAS_DISPLAY
  display_begin();
#endif
  //Homie.disableLogging();
  
#if DEBUG_SERIAL || DEBUG_MQTT
  // Set up IR debug logging callback
  setIRDebugCallback([](const String& msg) {
    #if DEBUG_SERIAL
      Serial.println(msg);
    #endif
    #if DEBUG_MQTT
      debugNode.setProperty("log").send(msg);
    #endif
  });
#endif

#ifndef HAS_STATUS_LEDS
  Homie.disableLedFeedback();
#endif
  Homie_setFirmware("ElectraWifi", "1.0.0");
  Homie.setLoopFunction(loopHandler);
#if defined(HAS_DISPLAY) || defined(HA_DISCOVERY)
  Homie.onEvent(onHomieEvent);
#endif
  temperatureNode.advertise("state").settable(temperatureHandler);
  fanNode.advertise("state").settable(fanHandler);
  modeNode.advertise("state").settable(modeHandler);
  ifeelNode.advertise("state").settable(ifeelHandler);
  ifeelTempNode.advertise("state").settable(ifeelTempHandler);
  powerNode.advertise("state").settable(powerHandler);
  swingNode.advertise("state").settable(swingHandler);
  stateNode.advertise("json").settable(jsonHandler);
  rebootNode.advertise("trigger").settable(rebootHandler);
#if DEBUG_MQTT
  debugNode.advertise("log");
#endif
  
#ifdef POWER_PIN_ADC
  // The AC only pulls this LED line down (empty socket: 0.02 V off, 0.7 V on);
  // nothing pulls it up, so the line goes straight to the pin and the chip's
  // own pull-up takes the place of the original display's LED, like
  // INPUT_PULLUP on the ESP8266. It is read with the ADC and hysteresis
  // because the 0.7 V "on" level sits too close to a digital threshold.
  pinMode(POWER_PIN, INPUT_PULLUP);
  analogSetPinAttenuation(POWER_PIN, ADC_11db);
  analogReadMilliVolts(POWER_PIN);        // first read configures the ADC
  gpio_pullup_en((gpio_num_t)POWER_PIN);  // the ADC setup may drop the pull-up
#else
  pinMode(POWER_PIN, INPUT_PULLUP);
#endif
#ifdef HAS_BUTTONS
  pinMode(PIN_BTN_MODE, INPUT_PULLUP);
  pinMode(PIN_BTN_FAN, INPUT_PULLUP);
#endif
#ifdef PIN_BUZZER
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
#endif
#ifdef HAS_STATUS_LEDS
  pinMode(GREEN_LED_PIN, OUTPUT);
  Homie.setLedPin(RED_LED_PIN, HIGH);
#endif
#ifndef ELECTRAWIFI_NO_IR_RCV
  irrecv.setUnknownThreshold(100); 
  irrecv.enableIRIn();
#endif
  Homie.setup();
}

void loop() {
  Homie.loop();
  readPower();
  handleIR();
#ifdef HAS_BUTTONS
  handleButtons();
#endif
#ifdef HAS_DISPLAY
  display_tick();
#endif
}