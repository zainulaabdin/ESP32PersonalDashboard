// See es8311.h for why this is a direct port of Espressif's official driver
// rather than a third-party wrapper library.
#include "es8311.h"
#include "settings.h"
#include "speaker.h"
#include <Arduino.h>
#include <Wire.h>

#define ES8311_I2C_ADDR 0x18 // CE pin tied low (this board's default strapping)

// Register addresses - names/values kept identical to Espressif's
// es8311_reg.h so this stays diffable against the original.
#define ES8311_RESET_REG00 0x00
#define ES8311_CLK_MANAGER_REG01 0x01
#define ES8311_CLK_MANAGER_REG02 0x02
#define ES8311_CLK_MANAGER_REG03 0x03
#define ES8311_CLK_MANAGER_REG04 0x04
#define ES8311_CLK_MANAGER_REG05 0x05
#define ES8311_CLK_MANAGER_REG06 0x06
#define ES8311_CLK_MANAGER_REG07 0x07
#define ES8311_CLK_MANAGER_REG08 0x08
#define ES8311_SDPIN_REG09 0x09
#define ES8311_SDPOUT_REG0A 0x0A
#define ES8311_SYSTEM_REG0D 0x0D
#define ES8311_SYSTEM_REG0E 0x0E
#define ES8311_SYSTEM_REG12 0x12
#define ES8311_SYSTEM_REG13 0x13
#define ES8311_SYSTEM_REG14 0x14
#define ES8311_ADC_REG16 0x16
#define ES8311_ADC_REG17 0x17
#define ES8311_ADC_REG1C 0x1C
#define ES8311_DAC_REG32 0x32
#define ES8311_DAC_REG37 0x37

static uint8_t volumeToReg32(int volumePct); // defined at the end of the file

static bool writeReg(uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(ES8311_I2C_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

static bool readReg(uint8_t reg, uint8_t *value)
{
    Wire.beginTransmission(ES8311_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) // repeated start, keep the bus held
        return false;
    if (Wire.requestFrom((int)ES8311_I2C_ADDR, 1) != 1)
        return false;
    *value = Wire.read();
    return true;
}

// Read-modify-write helper - most of the clock registers below only touch
// a handful of bits and must preserve the rest, exactly like the official
// driver does.
static bool rmwReg(uint8_t reg, uint8_t keepMask, uint8_t setBits)
{
    uint8_t v;
    if (!readReg(reg, &v))
        return false;
    v = (v & keepMask) | setBits;
    return writeReg(reg, v);
}

bool es8311InitForMicCapture()
{
    bool ok = true;

    // Reset to default, then power-on command - exact sequence + delay from
    // the official driver's es8311_init().
    ok &= writeReg(ES8311_RESET_REG00, 0x1F);
    delay(20);
    ok &= writeReg(ES8311_RESET_REG00, 0x00);
    ok &= writeReg(ES8311_RESET_REG00, 0x80);

    // Decisive sanity check before trusting anything else: the datasheet
    // (Everest ES8311 rev 8.0) documents fixed chip-ID register values
    // (0xFD=0x83, 0xFE=0x11) independent of any configuration. If these
    // don't read back correctly, I2C isn't actually reaching a live
    // ES8311 at all (wrong address/power-sequencing/bad connection) and
    // every other register write below is meaningless - added because
    // near-silent recordings on real hardware turned out to need this
    // kind of ground-truth check rather than more register guessing.
    uint8_t chipId1 = 0, chipId2 = 0;
    bool idOk = readReg(0xFD, &chipId1) && readReg(0xFE, &chipId2);
    Serial.printf("es8311: chip ID read %s: 0xFD=0x%02X (expect 0x83), 0xFE=0x%02X (expect 0x11)\n",
                  idOk ? "ok" : "FAILED", chipId1, chipId2);

    // --- Clock config: MCLK taken from the MCLK pin (this board wires
    // IO4 to the codec's MCLK pin), not inverted, not derived from SCLK. ---
    ok &= writeReg(ES8311_CLK_MANAGER_REG01, 0x3F); // enable all clocks, MCLK-pin source (no BIT7/BIT6)
    ok &= rmwReg(ES8311_CLK_MANAGER_REG06, 0xFF, 0x00); // sclk not inverted (clears bit5, preserves rest)

    // --- Sample-rate clock dividers for this board's fixed 6.144MHz MCLK /
    // 16kHz sample rate combination (384x MCLK multiple, matching the
    // vendor's own proven-working reference firmware, see mic_capture.cpp's
    // I2S_MCLK_MULTIPLE_384 comment - this used to be 4.096MHz/256x, which
    // was never actually confirmed working). Coefficient row taken
    // verbatim from the official driver's coeff_div[] table: pre_div=3,
    // pre_multi=1, adc_div=1, dac_div=1, fs_mode=0, lrck_h=0x00,
    // lrck_l=0xff, bclk_div=4, adc_osr=0x10, dac_osr=0x10 - this project
    // only ever runs at this one rate, so the full ~90-row table wasn't
    // ported. ---
    ok &= rmwReg(ES8311_CLK_MANAGER_REG02, 0x07, 0x48);       // (pre_div-1)<<5 | pre_multi<<3 = 0x40 | 0x08
    ok &= writeReg(ES8311_CLK_MANAGER_REG03, 0x10);           // fs_mode<<6 | adc_osr
    ok &= writeReg(ES8311_CLK_MANAGER_REG04, 0x10);           // dac_osr
    ok &= writeReg(ES8311_CLK_MANAGER_REG05, 0x00);           // (adc_div-1)<<4 | (dac_div-1)
    ok &= rmwReg(ES8311_CLK_MANAGER_REG06, 0xE0, 0x03);       // bclk_div-1 = 3
    ok &= rmwReg(ES8311_CLK_MANAGER_REG07, 0xC0, 0x00);       // lrck_h
    ok &= writeReg(ES8311_CLK_MANAGER_REG08, 0xFF);           // lrck_l

    // --- Format: slave serial port, 16-bit I2S in/out. ---
    ok &= rmwReg(ES8311_RESET_REG00, 0xBF, 0x00); // slave serial port (clear bit6)
    ok &= writeReg(ES8311_SDPIN_REG09, 0x0C);     // 16-bit resolution: (3<<2)
    ok &= writeReg(ES8311_SDPOUT_REG0A, 0x0C);

    // --- Power-up analog circuitry + ADC path (mic). ---
    ok &= writeReg(ES8311_SYSTEM_REG0D, 0x01); // power up analog circuitry
    ok &= writeReg(ES8311_SYSTEM_REG0E, 0x02); // enable analog PGA, enable ADC modulator
    ok &= writeReg(ES8311_SYSTEM_REG12, 0x00); // power-up DAC (official default init step, DAC path unused here)
    ok &= writeReg(ES8311_SYSTEM_REG13, 0x10); // enable output to HP drive
    ok &= writeReg(ES8311_ADC_REG1C, 0x6A);    // ADC equalizer bypass, cancel DC offset
    ok &= writeReg(ES8311_DAC_REG37, 0x08);    // bypass DAC equalizer

    // --- Microphone: analog mic (not PDM digital), max analog PGA gain,
    // plus a moderate 24dB ADC gain (es8311_mic_gain_t's ES8311_MIC_GAIN_24DB
    // = 4) - a reasonable default for a phone-distance voice question. ---
    ok &= writeReg(ES8311_ADC_REG17, 0xC8); // ADC gain (official default)
    ok &= writeReg(ES8311_SYSTEM_REG14, 0x1A); // analog mic enabled, max PGA gain
    ok &= writeReg(ES8311_ADC_REG16, 4);       // 24dB mic gain

    if (!ok)
        Serial.println("es8311: one or more I2C register writes failed");

    // Read back what actually landed - catches a write that reports success
    // at the I2C transaction level but doesn't stick (e.g. a stale/partial
    // power-up state), which a plain "did endTransmission() return 0" check
    // can't see.
    uint8_t r0d = 0, r0e = 0, r14 = 0;
    readReg(ES8311_SYSTEM_REG0D, &r0d);
    readReg(ES8311_SYSTEM_REG0E, &r0e);
    readReg(ES8311_SYSTEM_REG14, &r14);
    Serial.printf("es8311: readback REG0D=0x%02X (want 0x01) REG0E=0x%02X (want 0x02) REG14=0x%02X (want 0x1A)\n",
                  r0d, r0e, r14);

    return ok;
}

bool es8311InitForSpeaker()
{
    bool ok = true;

    // Same reset/power-on sequence as es8311InitForMicCapture() - see that
    // function's comments for why each step is there.
    ok &= writeReg(ES8311_RESET_REG00, 0x1F);
    delay(20);
    ok &= writeReg(ES8311_RESET_REG00, 0x00);
    ok &= writeReg(ES8311_RESET_REG00, 0x80);

    uint8_t chipId1 = 0, chipId2 = 0;
    bool idOk = readReg(0xFD, &chipId1) && readReg(0xFE, &chipId2);
    Serial.printf("es8311: (speaker) chip ID read %s: 0xFD=0x%02X (expect 0x83), 0xFE=0x%02X (expect 0x11)\n",
                  idOk ? "ok" : "FAILED", chipId1, chipId2);

    ok &= writeReg(ES8311_CLK_MANAGER_REG01, 0x3F);
    ok &= rmwReg(ES8311_CLK_MANAGER_REG06, 0xFF, 0x00);

    // --- Clock dividers for 24kHz at the same 6.144MHz MCLK the mic path
    // uses (256x multiple here vs. the mic's 384x - see mic_capture.cpp's
    // speaker equivalent for the I2S-side config). Coefficient row taken
    // verbatim from the vendor's own es8311.cpp coeff_div[] table (the
    // {6144000, 24000, ...} row, confirmed via their real, running demo
    // firmware - not derived/guessed): pre_div=1, pre_multi=0, adc_div=1,
    // dac_div=1, fs_mode=0, lrck_h=0x00, lrck_l=0xff, bclk_div=4,
    // adc_osr=0x10, dac_osr=0x10. Every one of these except pre_div/
    // pre_multi (REG02) is byte-identical to the mic path's 16kHz/384x
    // config below - a coincidence of both landing on the same MCLK,
    // confirmed by working through the vendor's table by hand, not
    // assumed. ---
    ok &= rmwReg(ES8311_CLK_MANAGER_REG02, 0x07, 0x00); // (pre_div-1)<<5 | pre_multi<<3 = 0x00 | 0x00
    ok &= writeReg(ES8311_CLK_MANAGER_REG03, 0x10);     // fs_mode<<6 | adc_osr
    ok &= writeReg(ES8311_CLK_MANAGER_REG04, 0x10);     // dac_osr
    ok &= writeReg(ES8311_CLK_MANAGER_REG05, 0x00);     // (adc_div-1)<<4 | (dac_div-1)
    ok &= rmwReg(ES8311_CLK_MANAGER_REG06, 0xE0, 0x03); // bclk_div-1 = 3
    ok &= rmwReg(ES8311_CLK_MANAGER_REG07, 0xC0, 0x00); // lrck_h
    ok &= writeReg(ES8311_CLK_MANAGER_REG08, 0xFF);     // lrck_l

    ok &= rmwReg(ES8311_RESET_REG00, 0xBF, 0x00); // slave serial port
    ok &= writeReg(ES8311_SDPIN_REG09, 0x0C);     // 16-bit resolution
    ok &= writeReg(ES8311_SDPOUT_REG0A, 0x0C);

    ok &= writeReg(ES8311_SYSTEM_REG0D, 0x01); // power up analog circuitry
    ok &= writeReg(ES8311_SYSTEM_REG0E, 0x02); // enable analog PGA, enable ADC modulator (official default init step)
    ok &= writeReg(ES8311_SYSTEM_REG12, 0x00); // power-up DAC - this is the path actually used here
    ok &= writeReg(ES8311_SYSTEM_REG13, 0x10); // enable output to HP/speaker drive
    ok &= writeReg(ES8311_ADC_REG1C, 0x6A);    // ADC equalizer bypass (official default init step)
    ok &= writeReg(ES8311_DAC_REG37, 0x08);    // bypass DAC equalizer

    // Volume: vendor's own es8311_voice_volume_set() formula
    // (volume * 256 / 100) - 1, now driven by the Settings tab's slider
    // (settingsGetVolumePct()) instead of a hardcoded value - that slider
    // already existed and persisted to NVS, but nothing ever read it (see
    // settings.cpp's own now-corrected comment), which is the real reason
    // a user report of "volume settings has no effect" was completely
    // accurate. A hardcoded 100 (tried briefly while chasing a separate
    // "too quiet" report) turned out to sound distorted - this register is
    // a hard digital gain multiplier, not a soft-limited one, so pushing
    // it to its max does risk clipping depending on how "hot" OpenAI's own
    // TTS output already is. Defaulting to the slider's own default (50%)
    // and letting the user dial it in is the actually-correct fix, not
    // guessing at a single "right" number.
    ok &= writeReg(ES8311_DAC_REG32, volumeToReg32(settingsGetVolumePct()));

    if (!ok)
        Serial.println("es8311: (speaker) one or more I2C register writes failed");

    return ok;
}

// Slider -> DAC volume register (REG32). Why the range is so narrow:
//
// This register's audible range on THIS board's actual hardware (DAC +
    // FM8002E amp + small speaker) is a narrow band near the register's
    // top end, not spread across its full 0-255 span - confirmed by real
    // evidence across two different taper shapes, not assumed: a cubic
    // (x^3) taper landed the audible/non-clipping window at roughly
    // reg32=156-218 (a real report: "usable 85-95%"), and a squared (x^2)
    // taper capped at 210 landed it at roughly reg32=151-210 (a real
    // report: "usable 85-100%, silent below"). Two different curve SHAPES
    // produced the same narrow ~150-210 audible register window - meaning
    // curve shape was never the real lever. Any taper spanning the FULL
    // 0-255 register range wastes most of the slider's 0-100% domain
    // mapping into a register sub-range that's inaudible in practice on
    // this hardware (below ~150) before it ever reaches the audible band.
    //
    // Fix: map the slider's 0-100% directly onto the REGISTER'S OWN
    // observed-useful sub-range (FLOOR..CEILING) instead of onto 0..255.
    // A little below the observed FLOOR (150) to give the quiet end of the
    // slider genuine headroom to be quiet-but-audible rather than sitting
    // right at the edge of the cliff; CEILING stays below the observed
    // ~218-255 clipping zone (a real hardware headroom limit on this
    // board, already present even under the vendor's own pure-linear
    // formula - not a curve artifact).
    // Floor raised again - real evidence: with FLOOR=120, the slider was
    // STILL silent from 0-50% ("lower side you need to adjust further"),
    // meaning the true audible threshold on this hardware sits higher than
    // 120. At FLOOR=120/CEILING=205, 50% maps to reg32=162 - if that's
    // still silent, the real cutoff is somewhere above 162. Moving the
    // floor up close to the ceiling narrows the slider's range but keeps
    // every position genuinely audible, which matters more than a wide
    // range that's mostly dead air.
static uint8_t volumeToReg32(int volumePct)
{
    const uint8_t REG32_FLOOR = 170;
    const uint8_t REG32_CEILING = 205;
    if (volumePct > 100)
        volumePct = 100;
    if (volumePct <= 3)
        return 0; // explicit hard mute, matching the earlier direct request
    float fraction = volumePct / 100.0f;
    return (uint8_t)(REG32_FLOOR + fraction * (float)(REG32_CEILING - REG32_FLOOR));
}

void es8311ApplyLiveVolume()
{
    if (speakerIsBusy()) // codec is only powered while playing
        writeReg(ES8311_DAC_REG32, volumeToReg32(settingsGetVolumePct()));
}
