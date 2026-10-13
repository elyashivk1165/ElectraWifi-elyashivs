#ifdef HAS_DISPLAY

#include "display.h"
#include "hebrew_words.h"
#include <TFT_eSPI.h>

#define PIN_LCD_POWER 15
#define PIN_LCD_BL 38

// 320x170 in landscape
#define W 320
#define H 170
#define MARGIN 12

static TFT_eSPI tft = TFT_eSPI();

static const uint16_t COLOR_DIM = tft.color565(107, 119, 116);
static const uint16_t COLOR_OK = tft.color565(74, 222, 128);
static const uint16_t COLOR_BAD = tft.color565(240, 80, 80);
static const uint16_t COLOR_WARN = tft.color565(255, 170, 40);

static bool link_wifi = false;
static bool link_mqtt = false;

// Draws a pre-rendered Hebrew word on the black background, tinted with color.
// x is the word's right edge, since Hebrew is right-aligned.
static void draw_word(const HebrewWord& word, int x_right, int y, uint16_t color) {
  uint8_t r = (color >> 11) << 3, g = ((color >> 5) & 0x3F) << 2, b = (color & 0x1F) << 3;
  uint16_t* buf = (uint16_t*)malloc(word.w * word.h * sizeof(uint16_t));
  if (!buf) return;
  for (uint32_t i = 0; i < (uint32_t)word.w * word.h; i++) {
    uint8_t a = word.alpha[i];
    buf[i] = tft.color565(r * a / 255, g * a / 255, b * a / 255);
  }
  tft.pushImage(x_right - word.w, y, word.w, word.h, buf);
  free(buf);
}

static uint16_t mode_color(const String& mode) {
  if (mode == "cool") return tft.color565(53, 200, 255);
  if (mode == "heat") return tft.color565(255, 154, 46);
  if (mode == "dry") return tft.color565(120, 220, 160);
  if (mode == "auto") return tft.color565(180, 140, 255);
  return tft.color565(220, 225, 224);
}

static const HebrewWord& mode_word(const String& mode, bool big) {
  if (mode == "cool") return big ? HW_BIG_COOL : HW_SMALL_COOL;
  if (mode == "heat") return big ? HW_BIG_HEAT : HW_SMALL_HEAT;
  if (mode == "dry") return big ? HW_BIG_DRY : HW_SMALL_DRY;
  if (mode == "auto") return big ? HW_BIG_AUTO : HW_SMALL_AUTO;
  return big ? HW_BIG_FAN : HW_SMALL_FAN;
}

static int fan_level(const String& fan) {
  if (fan == "low") return 1;
  if (fan == "med") return 2;
  if (fan == "high") return 3;
  return 4;
}

// One small dot in the corner: green = Home Assistant connected,
// orange = WiFi only, red = no WiFi.
static void draw_status_dot() {
  uint16_t color = link_mqtt ? COLOR_OK : (link_wifi ? COLOR_WARN : COLOR_BAD);
  tft.fillCircle(8, 8, 4, color);
}

static void draw_degrees(int x, int y, uint16_t color) {
  tft.drawCircle(x + 5, y + 5, 4, color);
  tft.drawCircle(x + 5, y + 5, 3, color);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString("C", x + 13, y, 4);
}

void display_begin() {
  pinMode(PIN_LCD_POWER, OUTPUT);
  digitalWrite(PIN_LCD_POWER, HIGH);
  tft.init();
  tft.setRotation(1);
  tft.setSwapBytes(true);
  tft.fillScreen(TFT_BLACK);
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  draw_word(HW_BIG_BRAND, (W + HW_BIG_BRAND.w) / 2, 50, TFT_WHITE);
  draw_word(HW_SMALL_STARTING, (W + HW_SMALL_STARTING.w) / 2, 100, COLOR_DIM);
}

void display_show(bool power, const String& mode, int temperature, const String& fan) {
  tft.fillScreen(TFT_BLACK);
  draw_status_dot();
  tft.setTextDatum(TL_DATUM);

  if (!power) {
    draw_word(HW_BIG_OFF, (W + HW_BIG_OFF.w) / 2, 50, COLOR_DIM);
    // last setting: "<mode>  24°C"
    String temp = String(temperature);
    int temp_w = tft.textWidth(temp, 4) + 30;
    const HebrewWord& word = mode_word(mode, false);
    int total = word.w + 14 + temp_w;
    int left = (W - total) / 2;
    tft.setTextColor(COLOR_DIM, TFT_BLACK);
    tft.drawString(temp, left, 110, 4);
    draw_degrees(left + tft.textWidth(temp, 4) + 2, 110, COLOR_DIM);
    draw_word(word, left + total, 112, COLOR_DIM);
    return;
  }

  uint16_t color = mode_color(mode);

  // big set temperature on the left, vertically centred
  String temp = String(temperature);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(temp, MARGIN, 42, 8);
  draw_degrees(MARGIN + tft.textWidth(temp, 8) + 4, 44, TFT_WHITE);

  // mode on the right, fan bars under it
  draw_word(mode_word(mode, true), W - MARGIN, 30, color);
  int level = fan_level(fan);
  const int bar_w = 16, gap = 7, bottom = 140;
  int x = W - MARGIN - (4 * bar_w + 3 * gap);
  for (int i = 0; i < 4; i++) {
    int h = 12 + i * 9;
    tft.fillRect(x + i * (bar_w + gap), bottom - h, bar_w, h, i < level ? color : COLOR_DIM);
  }
  if (fan == "auto") {
    draw_word(HW_SMALL_AUTO, W - MARGIN, bottom + 6, COLOR_DIM);
  }

  tft.fillRect(0, H - 4, W, 4, color);
}

void display_link(bool wifi, bool mqtt) {
  if (wifi == link_wifi && mqtt == link_mqtt) return;
  link_wifi = wifi;
  link_mqtt = mqtt;
  draw_status_dot();
}

#endif
