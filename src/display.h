#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>

// Status screen for boards with a built-in display (LILYGO T-Display-S3).
// Compiled only when HAS_DISPLAY is defined.
void display_begin();
void display_show(bool power, const String& mode, int temperature, const String& fan);

#endif
