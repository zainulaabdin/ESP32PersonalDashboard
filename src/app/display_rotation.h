#pragma once

// 180-degree screen flip (Settings tab's rotate icon) - implemented in
// main.cpp, which owns the TFT and the touch read callback. Flipping swaps
// TFT rotation 1 <-> 3 and mirrors touch coordinates to match.
void displaySetFlipped(bool flipped);
bool displayIsFlipped();
