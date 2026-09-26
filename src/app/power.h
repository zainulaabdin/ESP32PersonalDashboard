#pragma once

// Schedule (Singapore local time, matches main.cpp's configTime offset):
//   08:00-20:00 Mon-Fri - normal operation. After 15 min with no touch, the
//     backlight blanks and the board enters light sleep, waking instantly
//     on the next touch.
//   20:00-08:00 Mon-Fri, and all of Sat/Sun - deep sleep (a full reboot on
//     wake, no state survives), woken by a timer set for the next 08:00
//     weekday.
void powerInit();
// Call once per loop() iteration. May block for the length of a light
// sleep, or never return at all if it enters deep sleep (the board reboots
// instead).
void powerTick();
// Call whenever my_touchpad_read() sees a real touch, so the 15-minute
// inactivity timer resets on real interaction.
void powerNoteTouch();
// Forces the board to sleep immediately (Settings tab's manual "sleep"
// button), using the same schedule check as the normal inactivity timeout
// to decide light vs deep sleep - so it wakes on touch exactly like an
// automatic sleep would.
void powerSleepNow();
