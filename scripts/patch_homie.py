# Applies the ESP32 Arduino core 2.x fixes from the README to Homie automatically,
# so the ESP32 environments build without editing .pio/libdeps by hand.
Import("env")
import os

boot = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"),
                    "Homie", "src", "Homie", "Boot", "BootNormal.cpp")

REPLACEMENTS = [
    ("WiFiEvent_t::SYSTEM_EVENT_STA_GOT_IP", "ARDUINO_EVENT_WIFI_STA_GOT_IP"),
    ("WiFiEvent_t::SYSTEM_EVENT_STA_DISCONNECTED", "ARDUINO_EVENT_WIFI_STA_DISCONNECTED"),
    ("info.disconnected.reason", "info.wifi_sta_disconnected.reason"),
]

if os.path.exists(boot):
    with open(boot) as f:
        src = f.read()
    patched = src
    for old, new in REPLACEMENTS:
        patched = patched.replace(old, new)
    if patched != src:
        with open(boot, "w") as f:
            f.write(patched)
        print("patch_homie: patched " + boot)
