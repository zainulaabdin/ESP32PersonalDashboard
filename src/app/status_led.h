#pragma once

// Board-back WS2812 RGB LED (GPIO42) as a status light. Highest-priority
// state wins:
//   OTA downloading/flashing   cyan    fast blink
//   Wi-Fi provisioning (QR)    blue    breathing
//   Wi-Fi not connected        orange  slow blink
//   Ask (record/answer/speak)  pink    blink
//   Today/calendar fetch       purple  blink
//   Bus fetch                  green   blink
//   Weather fetch              yellow  blink
//   OTA version check          cyan    blink
//   idle, connected            off
//   light sleep (screen off)   red     steady (CPU halted - can't animate)
//   deep sleep                 3 red flashes, then off

void statusLedInit();
void statusLedTick(); // call every loop()

// Called by power.cpp right before sleeping - the LED latches whatever it
// was last sent, so this is the colour it keeps while the CPU is halted.
void statusLedLightSleep();
void statusLedDeepSleep(); // blocks ~1s for the flashes
