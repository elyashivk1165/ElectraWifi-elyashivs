#ifdef HAS_DISPLAY

#include "display.h"
#include <TFT_eSPI.h>

#define PIN_LCD_POWER 15
#define PIN_LCD_BL 38

static TFT_eSPI tft = TFT_eSPI();

void display_begin() {
  pinMode(PIN_LCD_POWER, OUTPUT);
  digitalWrite(PIN_LCD_POWER, HIGH);
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("ElectraWifi", tft.width() / 2, tft.height() / 2, 4);
}

void display_show(bool power, const String& mode, int temperature, const String& fan) {
  tft.fillScreen(TFT_BLACK);
  const int cx = tft.width() / 2;

  if (!power) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("OFF", cx, tft.height() / 2, 4);
    return;
  }

  uint16_t color = TFT_WHITE;
  if (mode == "cool") {
    color = TFT_CYAN;
  } else if (mode == "heat") {
    color = TFT_ORANGE;
  }

  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(String(temperature) + "C", cx, 60, 7);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(mode + "  |  fan: " + fan, cx, 135, 4);
}

#endif
