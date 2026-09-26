#pragma once
#include <stdint.h>

// Switches the bottom tab bar to a tab, exactly as if it had been tapped.
// Order: 0 Profile, 1 Today, 2 Bus, 3 Ask, 4 Settings. UI thread only.
void uiShowTab(uint32_t index);
