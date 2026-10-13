#ifdef HAS_DISPLAY

#include "display.h"
#include "hebrew_words.h"
#include <TFT_eSPI.h>

#define PIN_LCD_POWER 15
#define PIN_LCD_BL 38

// 320x170 in landscape
#define W 320
#define H 170
#define FRAME_MS 66  // about 15 frames a second

// "Glow" design: a ring around the set temperature on the left that fills
// from 16 to 30 degrees, the mode name and icon on the top right, fan bars
// under it, and a soft glow in the mode's color with slowly moving snow,
// sparks, drops, wind or bubbles behind everything. The whole frame is drawn
// into a sprite and pushed at once so nothing flickers.

static TFT_eSPI tft = TFT_eSPI();
static TFT_eSprite frame = TFT_eSprite(&tft);
static uint16_t* background = nullptr;  // cached glow, copied into each frame
static bool use_sprite = false;

struct ModeLook {
  uint16_t color, color2, deep;
};

static bool link_wifi = false;
static bool link_mqtt = false;

static bool have_state = false;
static bool cur_power = false;
static String cur_mode = "cool";
static int cur_temp = 24;
static String cur_fan = "auto";
static bool background_dirty = true;
static uint32_t last_frame = 0;

static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }

static ModeLook look_for(const String& mode) {
  if (mode == "heat") return {rgb(255, 161, 61), rgb(255, 63, 98), rgb(43, 11, 7)};
  if (mode == "dry") return {rgb(79, 230, 160), rgb(16, 167, 181), rgb(4, 35, 28)};
  if (mode == "auto") return {rgb(197, 140, 255), rgb(255, 95, 200), rgb(28, 12, 53)};
  if (mode == "fan_only") return {rgb(238, 246, 248), rgb(134, 174, 190), rgb(18, 27, 33)};
  return {rgb(63, 208, 255), rgb(42, 109, 255), rgb(7, 26, 51)};
}

static uint16_t blend(uint8_t alpha, uint16_t fg, uint16_t bg) { return tft.alphaBlend(alpha, fg, bg); }

// --- drawing helpers that work on the sprite (or the screen as a fallback) ---

static TFT_eSPI& gfx() { return use_sprite ? (TFT_eSPI&)frame : tft; }

static uint16_t read_px(int x, int y) {
  if (x < 0 || y < 0 || x >= W || y >= H) return TFT_BLACK;
  return use_sprite ? frame.readPixel(x, y) : TFT_BLACK;
}

// Draws a pre-rendered word, blending its soft edges into what is already there.
static void blit(const HebrewWord& word, int x, int y, uint16_t color) {
  for (int j = 0; j < word.h; j++) {
    for (int i = 0; i < word.w; i++) {
      uint8_t a = word.alpha[j * word.w + i];
      if (!a) continue;
      int px = x + i, py = y + j;
      if (px < 0 || py < 0 || px >= W || py >= H) continue;
      gfx().drawPixel(px, py, a == 255 ? color : blend(a, color, read_px(px, py)));
    }
  }
}

// Hebrew is right-aligned, so words are placed by their right edge.
static void blit_right(const HebrewWord& word, int x_right, int y, uint16_t color) {
  blit(word, x_right - word.w, y, color);
}

static int number_width(const HebrewWord* digits, int value, int gap) {
  String s = String(value);
  int w = 0;
  for (unsigned i = 0; i < s.length(); i++) w += digits[s[i] - '0'].w + (i ? gap : 0);
  return w;
}

static void draw_number(const HebrewWord* digits, int value, int x, int y, int gap, uint16_t color) {
  String s = String(value);
  for (unsigned i = 0; i < s.length(); i++) {
    const HebrewWord& d = digits[s[i] - '0'];
    blit(d, x, y, color);
    x += d.w + gap;
  }
}

static const HebrewWord& mode_word(const String& mode, bool big) {
  if (mode == "heat") return big ? HW_MODE_HEAT : HW_SMALL_HEAT;
  if (mode == "dry") return big ? HW_MODE_DRY : HW_SMALL_DRY;
  if (mode == "auto") return big ? HW_MODE_AUTO : HW_SMALL_AUTO;
  if (mode == "fan_only") return big ? HW_MODE_FAN : HW_SMALL_FAN;
  return big ? HW_MODE_COOL : HW_SMALL_COOL;
}

static int fan_level(const String& fan) {
  if (fan == "low") return 1;
  if (fan == "med") return 2;
  if (fan == "high") return 3;
  return 0;  // auto
}

static float rnd(int i, int k) {
  float s = sinf(i * 127.1f + k * 311.7f) * 43758.5453f;
  return s - floorf(s);
}

static void line(float ax, float ay, float bx, float by, float width, uint16_t color) {
  gfx().drawWideLine(ax, ay, bx, by, width, color, use_sprite ? 0x00FFFFFF : TFT_BLACK);
}

// --- icons, drawn from simple shapes ---

static void icon_snow(int cx, int cy, float r, float t, uint16_t c) {
  float rot = t * 0.4f;
  for (int k = 0; k < 3; k++) {
    float a = rot + k * PI / 3, ca = cosf(a), sa = sinf(a);
    line(cx - ca * r, cy - sa * r, cx + ca * r, cy + sa * r, 2.5f, c);
    for (int s = -1; s <= 1; s += 2) {
      float bx = cx + s * ca * r * 0.58f, by = cy + s * sa * r * 0.58f;
      for (int side = -1; side <= 1; side += 2) {
        float b = a + side * PI / 4;
        line(bx, by, bx + s * cosf(b) * r * 0.4f, by + s * sinf(b) * r * 0.4f, 2.0f, c);
      }
    }
  }
}

static void icon_sun(int cx, int cy, float r, float t, uint16_t c) {
  gfx().fillSmoothCircle(cx, cy, r * 0.44f, c);
  float p = 0.08f * sinf(t * 3);
  for (int k = 0; k < 8; k++) {
    float a = k * PI / 4 + t * 0.2f;
    line(cx + cosf(a) * r * 0.66f, cy + sinf(a) * r * 0.66f,
         cx + cosf(a) * r * (0.96f + p), cy + sinf(a) * r * (0.96f + p), 2.5f, c);
  }
}

static void icon_drop(int cx, int cy, float r, float t, uint16_t c) {
  cy += (int)(sinf(t * 2.4f) * r * 0.08f);
  gfx().fillSmoothCircle(cx, cy + r * 0.3f, r * 0.68f, c);
  gfx().fillTriangle(cx, cy - r, cx - r * 0.62f, cy + r * 0.1f, cx + r * 0.62f, cy + r * 0.1f, c);
}

static void icon_fan(int cx, int cy, float r, float angle, uint16_t c) {
  for (int k = 0; k < 3; k++) {
    float a = angle + k * 2 * PI / 3;
    float bx = cx + cosf(a) * r * 0.55f, by = cy + sinf(a) * r * 0.55f;
    gfx().fillSmoothCircle(bx, by, r * 0.36f, c);
    float pa = a + PI / 2;
    gfx().fillTriangle(cx, cy, bx + cosf(pa) * r * 0.36f, by + sinf(pa) * r * 0.36f,
                       bx - cosf(pa) * r * 0.36f, by - sinf(pa) * r * 0.36f, c);
  }
  gfx().fillSmoothCircle(cx, cy, r * 0.16f, TFT_BLACK);
}

static void icon_auto(int cx, int cy, float r, float t, uint16_t c) {
  int start = ((int)(t * 70)) % 360;
  gfx().drawArc(cx, cy, r, r - 2.5f, start, (start + 270) % 360, c, use_sprite ? 0x00FFFFFF : TFT_BLACK, true);
  gfx().setTextColor(c);
  gfx().setTextDatum(MC_DATUM);
  gfx().drawString("A", cx, cy + 1, 2);
}

static void icon_moon(int cx, int cy, float r, uint16_t c) {
  uint16_t behind = read_px(cx + r * 0.42f, cy - r * 0.3f);
  gfx().fillSmoothCircle(cx, cy, r * 0.8f, c);
  gfx().fillSmoothCircle(cx + r * 0.42f, cy - r * 0.3f, r * 0.8f, behind);
}

static void mode_icon(const String& mode, int cx, int cy, float r, float t, uint16_t c) {
  if (mode == "heat") icon_sun(cx, cy, r, t, c);
  else if (mode == "dry") icon_drop(cx, cy, r, t, c);
  else if (mode == "auto") icon_auto(cx, cy, r, t, c);
  else if (mode == "fan_only") icon_fan(cx, cy, r, t * 5, c);
  else icon_snow(cx, cy, r, t, c);
}

// --- pieces of the screen ---

static void build_background() {
  ModeLook m = look_for(cur_mode);
  uint16_t top = cur_power ? m.deep : rgb(9, 11, 18);
  for (int y = 0; y < H; y++) {
    uint16_t row = blend(255 - y * 255 / H, top, TFT_BLACK);
    for (int x = 0; x < W; x++) {
      uint16_t c = row;
      if (cur_power) {
        float dx = x - 86, dy = y - 90, d = sqrtf(dx * dx + dy * dy);
        if (d < 150) c = blend((uint8_t)(90 * (1 - d / 150)), m.color, row);
      }
      gfx().drawPixel(x, y, c);
    }
  }
  if (use_sprite && background) memcpy(background, frame.getPointer(), W * H * 2);
  background_dirty = false;
}

static void draw_particles(const String& mode, float t, uint16_t color, uint16_t bg) {
  for (int i = 0; i < 20; i++) {
    float x0 = rnd(i, 1) * W, y0 = rnd(i, 2) * (H + 20), sp = 8 + rnd(i, 3) * 14, sz = 0.8f + rnd(i, 4) * 1.8f;
    float x, y;
    uint16_t c;
    if (mode == "heat") {
      float p = fmodf(y0 + t * sp * 1.4f, H + 20) / (H + 20);
      x = x0 + sinf(t * 2 + i) * 4; y = H + 10 - p * (H + 20);
      c = blend((uint8_t)(160 * (1 - p)), (i % 3) ? color : rgb(255, 224, 138), bg);
    } else if (mode == "dry") {
      x = x0; y = fmodf(y0 + t * sp * 2.2f, H + 20) - 10;
      c = blend(80, color, bg);
      gfx().fillEllipse(x, y, sz * 0.8f + 0.5f, sz * 1.8f, c);
      continue;
    } else if (mode == "fan_only") {
      x = fmodf(x0 + t * sp * 6, W + 60) - 30; y = rnd(i, 6) * H;
      float len = 14 + rnd(i, 7) * 26;
      gfx().drawFastHLine(x, y, len, blend(40 + 30 * rnd(i, 8), color, bg));
      continue;
    } else if (mode == "auto") {
      float p = fmodf(y0 + t * sp * 0.7f, H + 20) / (H + 20);
      x = x0 + sinf(t + i) * 8; y = H + 10 - p * (H + 20);
      gfx().drawCircle(x, y, sz * 1.8f, blend((uint8_t)(90 * (1 - p * 0.6f)), (i % 2) ? color : rgb(255, 143, 224), bg));
      continue;
    } else if (mode == "off") {
      x = x0; y = rnd(i, 9) * H;
      c = blend((uint8_t)(20 + 56 * (0.5f + 0.5f * sinf(t * 1.5f + i * 2))), rgb(207, 216, 255), bg);
    } else {  // cool: falling snow
      x = x0 + sinf(t * 0.8f + i) * 6; y = fmodf(y0 + t * sp, H + 20) - 10;
      c = blend((uint8_t)(64 + 90 * rnd(i, 5)), TFT_WHITE, bg);
    }
    gfx().fillCircle(x, y, sz < 1.4f ? 1 : 2, c);
  }
}

static void draw_link_dot(float t) {
  uint16_t c = link_mqtt ? rgb(62, 224, 122) : (link_wifi ? rgb(255, 174, 43) : rgb(255, 75, 75));
  if (!link_mqtt) c = blend((uint8_t)(140 + 115 * sinf(t * 4)), c, TFT_BLACK);
  gfx().fillSmoothCircle(10, 10, 4, c);
}

static void draw_fan_bars(int x, int bottom, int w, int h, const String& fan, uint16_t color, uint16_t dim, float t) {
  const int n = 4, gap = 5;
  int bw = (w - gap * (n - 1)) / n;
  int level = fan_level(fan);
  for (int i = 0; i < n; i++) {
    int bh = h * (0.4f + 0.2f * i);
    uint16_t c = dim;
    if (level == 0) c = blend((uint8_t)(90 + 165 * (0.5f + 0.5f * sinf(t * 4 - i * 0.9f))), color, dim);
    else if (i < level) c = color;
    gfx().fillSmoothRoundRect(x + i * (bw + gap), bottom - bh, bw, bh, 3, c, use_sprite ? 0x00FFFFFF : TFT_BLACK);
  }
}

static void draw_ring(int cx, int cy, int r, int temp, const ModeLook& m, bool on) {
  uint32_t bg = use_sprite ? 0x00FFFFFF : TFT_BLACK;
  // TFT_eSPI arcs start at 6 o'clock and go clockwise: 45..315 is the open-bottom ring
  gfx().drawArc(cx, cy, r + 5, r - 5, 45, 315, blend(22, TFT_WHITE, TFT_BLACK), bg, true);
  if (!on) return;
  int span = constrain((temp - 16) * 270 / 14, 4, 270);
  const int step = 15;
  for (int a = 0; a < span; a += step) {
    int e = min(a + step + 1, span);
    uint16_t c = blend((uint8_t)(255 * (a + e) / 2 / 270), m.color, m.color2);
    gfx().drawArc(cx, cy, r + 5, r - 5, 45 + a, 45 + e, c, bg, true);
  }
  float ka = (45 + span) * DEG_TO_RAD;
  int kx = cx - sinf(ka) * r, ky = cy + cosf(ka) * r;
  gfx().fillSmoothCircle(kx, ky, 7, TFT_WHITE);
  gfx().fillSmoothCircle(kx, ky, 3, m.color);
}

static void draw_frame(float t) {
  bool on = cur_power;
  ModeLook m = look_for(cur_mode);
  if (background_dirty || !use_sprite || !background) build_background();
  else memcpy(frame.getPointer(), background, W * H * 2);
  uint16_t glow_bg = on ? blend(70, m.color, m.deep) : rgb(5, 6, 10);

  draw_particles(on ? cur_mode : "off", t, m.color, glow_bg);

  const int cx = 86, cy = 92, r = 62;
  draw_ring(cx, cy, r, cur_temp, m, on);

  if (on) {
    // set temperature in the ring, with a small degree mark
    int tw = number_width(HW_DIG_BIG, cur_temp, 2);
    int tx = cx - 2 - tw / 2;
    draw_number(HW_DIG_BIG, cur_temp, tx, 70, 2, TFT_WHITE);
    gfx().drawCircle(tx + tw + 8, 66, 4, m.color);
    gfx().drawCircle(tx + tw + 8, 66, 3, m.color);
    uint16_t label = blend(90, TFT_WHITE, TFT_BLACK);
    draw_number(HW_DIG_TINY, 16, cx - 40 - 6, cy + r - 4, 1, label);
    draw_number(HW_DIG_TINY, 30, cx + 40 - 6, cy + r - 4, 1, label);

    // mode name and icon, top right
    mode_icon(cur_mode, 292, 36, 16, t, m.color);
    blit_right(mode_word(cur_mode, true), 268, 22, m.color);
    gfx().drawFastHLine(176, 66, 130, blend(26, TFT_WHITE, m.deep));

    // fan: bars, a spinning fan, and an "A" badge when the speed is automatic
    uint16_t dim = blend(30, TFT_WHITE, TFT_BLACK);
    draw_fan_bars(186, 152, 84, 48, cur_fan, m.color, dim, t);
    int level = fan_level(cur_fan);
    icon_fan(294, 128, 15, level == 0 ? t * 4 : t * (1.2f + 1.2f * level), blend(217, TFT_WHITE, TFT_BLACK));
    if (level == 0) {
      gfx().fillSmoothCircle(270, 100, 11, TFT_BLACK);
      gfx().fillSmoothCircle(270, 100, 9, m.color);
      gfx().setTextColor(rgb(8, 16, 24));
      gfx().setTextDatum(MC_DATUM);
      gfx().drawString("A", 270, 101, 2);
    }
  } else {
    // off: a moon in the ring, "good night" and the last setting on the right
    icon_moon(cx, cy - 14, 22, rgb(217, 224, 255));
    blit(HW_OFF_OFF, cx - HW_OFF_OFF.w / 2, cy + 14, blend(190, TFT_WHITE, TFT_BLACK));
    blit_right(HW_NIGHT_NIGHT, 306, 42, rgb(200, 206, 232));
    uint16_t last = blend(190, m.color, TFT_BLACK);
    mode_icon(cur_mode, 290, 112, 11, 0, blend(150, m.color, TFT_BLACK));
    const HebrewWord& word = mode_word(cur_mode, false);
    blit_right(word, 272, 105, last);
    int dx = 272 - word.w - 10 - 8 - number_width(HW_DIG_SMALL, cur_temp, 1);
    draw_number(HW_DIG_SMALL, cur_temp, dx, 105, 1, last);
    gfx().drawCircle(dx + number_width(HW_DIG_SMALL, cur_temp, 1) + 4, 107, 2, last);
  }

  draw_link_dot(t);
  if (use_sprite) frame.pushSprite(0, 0);
}

void display_begin() {
  pinMode(PIN_LCD_POWER, OUTPUT);
  digitalWrite(PIN_LCD_POWER, HIGH);
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  frame.setColorDepth(16);
  use_sprite = frame.createSprite(W, H) != nullptr;
  if (use_sprite) {
    background = (uint16_t*)(psramFound() ? ps_malloc(W * H * 2) : malloc(W * H * 2));
    frame.fillSprite(TFT_BLACK);
  }

  blit(HW_MODE_BRAND, (W - HW_MODE_BRAND.w) / 2, 55, TFT_WHITE);
  blit(HW_SMALL_STARTING, (W - HW_SMALL_STARTING.w) / 2, 100, rgb(107, 119, 116));
  if (use_sprite) frame.pushSprite(0, 0);
}

void display_show(bool power, const String& mode, int temperature, const String& fan) {
  if (!have_state || power != cur_power || mode != cur_mode) background_dirty = true;
  have_state = true;
  cur_power = power;
  cur_mode = mode;
  cur_temp = temperature;
  cur_fan = fan;
  last_frame = 0;  // draw the new state right away
  display_tick();
}

void display_link(bool wifi, bool mqtt) {
  link_wifi = wifi;
  link_mqtt = mqtt;
}

void display_tick() {
  if (!have_state) return;
  // without a sprite every frame would flicker, so only draw on changes
  if (!use_sprite && last_frame) return;
  uint32_t now = millis();
  if (last_frame && now - last_frame < FRAME_MS) return;
  last_frame = now ? now : 1;
  draw_frame(now / 1000.0f);
}

#endif
