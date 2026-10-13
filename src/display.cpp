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

// "Tiles" design: three rounded tiles. The big one on the left shows the set
// temperature in dark digits on a fade of the mode's colors, with a light
// band sliding across it. Top right: the mode icon and name. Bottom right:
// the fan bars, with an "auto" badge on their top right corner when the fan
// speed is automatic. The whole frame is drawn into a sprite and pushed at
// once so nothing flickers.

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
  if (mode == "heat") return big ? HW_TILE_HEAT : HW_SMALL_HEAT;
  if (mode == "dry") return big ? HW_TILE_DRY : HW_SMALL_DRY;
  if (mode == "auto") return big ? HW_TILE_AUTO : HW_SMALL_AUTO;
  if (mode == "fan_only") return big ? HW_TILE_FAN : HW_SMALL_FAN;
  return big ? HW_TILE_COOL : HW_SMALL_COOL;
}

static int fan_level(const String& fan) {
  if (fan == "low") return 1;
  if (fan == "med") return 2;
  if (fan == "high") return 3;
  return 0;  // auto
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

// 4x4 ordered dither, so soft color fades don't break into visible bands on
// the screen's 16-bit colors
static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static uint16_t mix_dither(float f, uint16_t c1, uint16_t c2, int x, int y) {
  float r1 = (c1 >> 11) * 255 / 31.0f, g1 = ((c1 >> 5) & 63) * 255 / 63.0f, b1 = (c1 & 31) * 255 / 31.0f;
  float r2 = (c2 >> 11) * 255 / 31.0f, g2 = ((c2 >> 5) & 63) * 255 / 63.0f, b2 = (c2 & 31) * 255 / 31.0f;
  float d = BAYER[y & 3][x & 3] / 16.0f - 0.47f;
  int r = (int)((r1 + (r2 - r1) * f) / 8.226f + d);   // 255/31
  int g = (int)((g1 + (g2 - g1) * f) / 4.048f + d);   // 255/63
  int b = (int)((b1 + (b2 - b1) * f) / 8.226f + d);
  r = constrain(r, 0, 31); g = constrain(g, 0, 63); b = constrain(b, 0, 31);
  return (r << 11) | (g << 5) | b;
}

// Coverage (0..255) of a pixel by a rounded rectangle, for soft corners.
static uint8_t round_rect_cover(int px, int py, int x, int y, int w, int h, int r) {
  float cx = constrain((float)px + 0.5f, (float)(x + r), (float)(x + w - r));
  float cy = constrain((float)py + 0.5f, (float)(y + r), (float)(y + h - r));
  float dx = px + 0.5f - cx, dy = py + 0.5f - cy;
  float d = sqrtf(dx * dx + dy * dy) - r;
  if (d <= -0.5f) return 255;
  if (d >= 0.5f) return 0;
  return (uint8_t)(255 * (0.5f - d));
}

static const uint16_t SCREEN_BG = 0x0041;  // near black, #06080a

// Big temperature tile: a diagonal fade between the mode's two colors.
#define T1X 6
#define T1Y 6
#define T1W 186
#define T1H 158

static void build_background() {
  ModeLook m = look_for(cur_mode);
  gfx().fillRect(0, 0, W, H, SCREEN_BG);
  for (int y = T1Y; y < T1Y + T1H; y++) {
    for (int x = T1X; x < T1X + T1W; x++) {
      uint8_t cover = round_rect_cover(x, y, T1X, T1Y, T1W, T1H, 16);
      if (!cover) continue;
      uint16_t c;
      if (cur_power) {
        float f = ((x - T1X) + (y - T1Y)) / (float)(T1W + T1H);
        c = mix_dither(f, m.color, m.color2, x, y);
      } else {
        c = rgb(18, 22, 26);
      }
      gfx().drawPixel(x, y, cover == 255 ? c : blend(cover, c, SCREEN_BG));
    }
  }
  if (use_sprite && background) memcpy(background, frame.getPointer(), W * H * 2);
  background_dirty = false;
}

// A soft light band that slides across the temperature tile now and then.
static void draw_shine(float t) {
  float sx = fmodf(t * 70, 520) - 160;
  for (int y = T1Y; y < T1Y + T1H; y++) {
    float base = sx + 40 - (y * 60.0f / H);  // the band leans to the right
    int x0 = max((int)base, T1X), x1 = min((int)(base + 70), T1X + T1W);
    for (int x = x0; x < x1; x++) {
      if (round_rect_cover(x, y, T1X, T1Y, T1W, T1H, 16) < 255) continue;
      float k = 1 - fabsf((x - base) / 35.0f - 1);
      if (k <= 0) continue;
      gfx().drawPixel(x, y, blend((uint8_t)(72 * k), TFT_WHITE, read_px(x, y)));
    }
  }
}

static void draw_link_dot(int x, int y, float t, uint16_t ring) {
  uint16_t c = link_mqtt ? rgb(62, 224, 122) : (link_wifi ? rgb(255, 174, 43) : rgb(255, 75, 75));
  if (!link_mqtt) c = blend((uint8_t)(140 + 115 * sinf(t * 4)), c, ring);
  gfx().fillSmoothCircle(x, y, 6, ring);
  gfx().fillSmoothCircle(x, y, 4, c);
}

static void draw_fan_bars(int x, int bottom, int w, int h, const String& fan, uint16_t color, uint16_t dim, float t) {
  const int n = 4, gap = 6;
  int bw = (w - gap * (n - 1)) / n;
  int level = fan_level(fan);
  for (int i = 0; i < n; i++) {
    int bh = h * (0.4f + 0.2f * i);
    uint16_t c = dim;
    if (level == 0) c = blend((uint8_t)(90 + 165 * (0.5f + 0.5f * sinf(t * 4 - i * 0.9f))), color, dim);
    else if (i < level) c = color;
    gfx().fillSmoothRoundRect(x + i * (bw + gap), bottom - bh, bw, bh, 3, c, use_sprite ? 0x00FFFFFF : SCREEN_BG);
  }
}

static void draw_degrees_c(int x, int y, uint16_t color) {
  gfx().drawCircle(x + 4, y + 4, 4, color);
  gfx().drawCircle(x + 4, y + 4, 3, color);
  gfx().setTextColor(color);
  gfx().setTextDatum(TL_DATUM);
  gfx().drawString("C", x + 11, y, 4);
}

static void draw_frame(float t) {
  bool on = cur_power;
  ModeLook m = look_for(cur_mode);
  if (background_dirty || !use_sprite || !background) build_background();
  else memcpy(frame.getPointer(), background, W * H * 2);
  const uint16_t ink = rgb(8, 16, 24);
  const uint16_t fan_tile = rgb(21, 26, 31);

  if (on) {
    // temperature tile
    if (use_sprite) draw_shine(t);
    blit_right(HW_LABEL_SET, T1X + T1W - 12, 18, blend(150, ink, m.color));
    int tw = number_width(HW_DIG_HUGE, cur_temp, 3);
    int tx = 84 - tw / 2;
    draw_number(HW_DIG_HUGE, cur_temp, tx, 60, 3, ink);
    draw_degrees_c(min(tx + tw + 4, T1X + T1W - 36), 56, blend(190, ink, m.color));
    draw_link_dot(20, 21, t, ink);

    // mode tile
    uint16_t mode_bg = blend(36, m.color, SCREEN_BG);
    gfx().fillSmoothRoundRect(198, 6, 116, 76, 14, blend(115, m.color, SCREEN_BG));
    gfx().fillSmoothRoundRect(200, 8, 112, 72, 12, mode_bg);
    mode_icon(cur_mode, 256, 32, 15, t, m.color);
    const HebrewWord& name = mode_word(cur_mode, true);
    blit(name, 256 - name.w / 2, 52, m.color);

    // fan tile: just the bars, with a badge on the top right when the speed is automatic
    gfx().fillSmoothRoundRect(198, 88, 116, 76, 14, fan_tile);
    draw_fan_bars(220, 152, 72, 50, cur_fan, m.color, blend(30, TFT_WHITE, fan_tile), t);
    if (fan_level(cur_fan) == 0) {
      gfx().fillSmoothCircle(292, 104, 14, fan_tile);
      gfx().fillSmoothCircle(292, 104, 12, m.color);
      icon_auto(292, 104, 9, t, ink);
    }
  } else {
    // off: a moon and "off" in the big tile, the last setting in dim colors
    icon_moon(99, 62, 22, blend((uint8_t)(150 + 50 * sinf(t * 1.5f)), rgb(217, 224, 255), rgb(18, 22, 26)));
    blit(HW_OFF_OFF, 99 - HW_OFF_OFF.w / 2, 98, blend(140, TFT_WHITE, rgb(18, 22, 26)));
    draw_link_dot(20, 21, t, rgb(18, 22, 26));
    const uint16_t dim_tile = rgb(16, 19, 23), dim = blend(77, TFT_WHITE, dim_tile);
    gfx().fillSmoothRoundRect(198, 6, 116, 76, 14, dim_tile);
    mode_icon(cur_mode, 256, 30, 13, 0, dim);
    const HebrewWord& word = mode_word(cur_mode, false);
    int nw = number_width(HW_DIG_SMALL, cur_temp, 1);
    int total = word.w + 14 + nw;
    int left = 256 - total / 2;
    draw_number(HW_DIG_SMALL, cur_temp, left, 56, 1, dim);
    gfx().drawCircle(left + nw + 4, 58, 2, dim);
    blit_right(word, left + total, 56, dim);
    gfx().fillSmoothRoundRect(198, 88, 116, 76, 14, dim_tile);
    draw_fan_bars(220, 152, 72, 50, cur_fan == "auto" ? "high" : cur_fan, blend(40, TFT_WHITE, dim_tile), blend(18, TFT_WHITE, dim_tile), 0);
  }

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

  blit(HW_BOOT_BRAND, (W - HW_BOOT_BRAND.w) / 2, 55, TFT_WHITE);
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
