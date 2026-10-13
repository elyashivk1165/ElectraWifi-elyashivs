#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>

// Status screen for boards with a built-in display (LILYGO T-Display-S3).
// Compiled only when HAS_DISPLAY is defined. Words are shown in Hebrew.
void display_begin();
// mode: cool|heat|dry|auto|fan_only (the last set mode, also while off)
// fan: low|med|high|auto
void display_show(bool power, const String& mode, int temperature, const String& fan);
void display_link(bool wifi, bool mqtt);

#endif
