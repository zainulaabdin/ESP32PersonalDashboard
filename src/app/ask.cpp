// Ask tab: tap the mic, speak a short question, get a short answer.
// Entirely on-device - no server backend. ONE OpenAI call per question:
// gpt-audio-mini via /v1/chat/completions, which takes the recorded audio
// directly and returns both a text answer and spoken audio in the same
// response (see askWithAudio()'s own comment - this replaced an earlier
// 3-call transcribe -> chat -> TTS chain).
//
// Same FreeRTOS-background-task + tick()-polled-from-loop() pattern as
// bus.cpp/today.cpp: recording + the HTTPS call are slow, so they never
// touch lv_* functions directly. Unlike those two tabs (a single bool
// fetchInProgress/fetchJustCompleted pair), this one has several distinct
// visible states (listening/thinking/speaking), so a shared `state` enum
// is written as the interaction progresses and askTick() just re-renders
// whenever that value changes - safe without a mutex because only ONE
// writer is ever active for a given question at a time (askTask during
// recording, then the Network Worker task's askNetworkJob() from the
// network call through to DONE/ERROR - never both at once), and the UI
// only reads answerText/statusError once state has already reached
// DONE/ERROR (i.e. after the writer is done writing them).
#include "ask.h"
#include "mic_capture.h"
#include "speaker.h"
#include "status_bar.h"
#include "icons/ask_icon_mic.h"
#include "icons/ask_icon_sound_on.h"
#include "icons/ask_icon_sound_off.h"
#include "icons/ask_icon_replay.h"
#include "icons/ask_icon_replay_empty.h"
#include "icons/ask_icon_back.h"
#include "icons/ask_icon_forward.h"
#include "icons/ask_icon_wake_on.h"
#include "icons/ask_icon_wake_off.h"
#include "wake_word.h"
#include "ui_nav.h"
#include "weather.h"
#include "settings.h"
#include "night_mode.h"
#include "power.h"
#include "bus.h"
#include "today.h"
#include "net_lock.h"
#include "network_worker.h"
#include "secrets_select.h"
#include "app_config.h"
#include "diag.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>

// This tab's own tint, same pattern as Bus (green)/Settings (orange) - see
// main.cpp for the matching tab-bar icon recolor.
#define ASK_TINT 0xE70C6A

#define MAX_RECORD_SECONDS 8 // matches mic_capture.cpp's own hard cap

enum AskState
{
    ASK_IDLE,
    // Real bug fixed here: this used to jump straight to ASK_LISTENING the
    // instant the mic was tapped, but a wait for any in-flight Bus/Today
    // fetch to clear (see askWork()'s comment on why - core-0 contention
    // with real-time I2S audio) runs BEFORE recording actually starts. The
    // UI said "Listening..." during that entire silent wait, so anything
    // the user said before recording actually began was never captured -
    // user-reported: tapped mic while Today was loading, spoke, and it
    // "hung on Listening" (recording hadn't started yet, nothing was being
    // heard). This state covers that wait; ASK_LISTENING is now only set
    // once micCaptureRecord() is actually about to run.
    ASK_WAITING_FOR_NETWORK,
    ASK_LISTENING,
    // Covers the single combined transcribe+answer(+speak) call - no
    // longer split into separate transcribing/thinking/synthesizing states
    // since askWithAudio() is one atomic HTTPS round trip now, not three
    // sequential ones.
    ASK_THINKING,
    // Set right before speakerPlayPcm() actually starts playing - the
    // audio only becomes available partway through ASK_THINKING (after the
    // single call's response finishes downloading and gets decoded), so
    // this still needs to be its own state rather than folded into THINKING.
    ASK_SPEAKING,
    ASK_DONE,
    ASK_ERROR,
};

static lv_obj_t *statusLabel;
static lv_obj_t *answerLabel;
static lv_obj_t *soundToggleIcon;

// Sound on/off toggle - checked (sound_on) by default. When off, the TTS
// network call itself is skipped entirely (not just playback), per
// explicit request - avoids spending an OpenAI TTS call for an answer that
// will never be played. Session-only (not persisted to NVS) - no request
// to remember this across reboots, and defaulting to "on" every boot is
// the safer choice for a feature this new.
static bool soundEnabled = true;

// Last successfully synthesized answer's raw PCM, kept alive in PSRAM for
// the replay button - see askWithAudio()'s own comment for why this
// replaced a SPIFFS-backed file (a real, unfixable-by-retrying SPIFFS
// limitation with files this large). Session-only by design - never meant
// to survive a reboot, only "hear that again" within the current session.
// Sample rate stored alongside it (not a fixed constant anymore) - the
// gpt-audio-mini response is a real WAV file with its own header, parsed
// directly rather than assumed, unlike the old /v1/audio/speech endpoint's
// fixed-24kHz raw PCM.
static int16_t *lastAnswerPcm = nullptr;
static size_t lastAnswerSampleCount = 0;
static uint32_t lastAnswerSampleRate = 24000;

static volatile AskState state = ASK_IDLE;
static char answerText[400] = "";
static char statusError[80] = "";
static volatile bool taskRunning = false;
static TaskHandle_t askTaskHandle = NULL;

// Real bug fixed here: cancelling during ASK_THINKING/ASK_SPEAKING (see
// startOrStopAsk()'s "cancel" branch) has no way to actually stop the
// already-queued/running Network Worker job - there's no handle to it, and
// killing the Network Worker task itself would break every other tab's
// fetches too. Without this, a cancelled question's askNetworkJob() would
// still finish later, overwrite answerText, flip state to DONE, and play
// its answer through the speaker as if it were the CURRENT question - and
// if the user had already started a new recording by then, that new
// recording's pendingAskRec.wavData could be freed out from under it (or
// leaked) by the stale job's own cleanup. Each question submitted gets the
// next generation number; askNetworkJob() checks it's still the current
// one before touching any shared state, so a cancelled/superseded job's
// result is silently discarded instead of corrupting the next question.
static volatile uint32_t askGeneration = 0;

// Bridges askWork() (still its own task, still owns recording via
// micCaptureRecord() and speaking via speakerPlayPcm() inside
// askWithAudio() - untouched per requirement #12) to the shared Network
// Worker (network_worker.h) for just the actual OpenAI HTTPS call.
//
// askTask's real job (per the required architecture) is record -> submit
// -> exit immediately - it does NOT wait for the network job to finish.
// That means everything the job needs (the recorded audio, the built
// context string) must outlive askTask itself, so these are static/
// module-level storage, not askWork()'s own stack locals - a stack-local
// would become invalid the instant askTask exits and its stack is torn
// down, while the job might not even start running until long after that
// (it could still be queued behind Bus/Calendar/Weather). The queued job
// function itself can't take parameters (matches the existing
// NetworkJobFn signature bus.cpp/today.cpp also use), so these statics are
// how askNetworkJob() finds its "arguments" once the Worker actually pops
// it off the queue.
static MicRecording pendingAskRec;
// Real bug fixed here: 700 bytes was too small for the worst case -
// timeBuf(~15) + fixed prose(~50) + todayCtx(<=320) + busCtx(<=320) +
// the trailing "ignore if irrelevant" instruction(~180) can add up to
// ~885 bytes, and since that trailing instruction sits at the END of
// buildAskContext()'s format string, snprintf's truncation silently cut
// exactly the sentence telling the model to ignore irrelevant context -
// worst possible time to lose it (when context is largest). Sized with
// real headroom instead of guessing a slightly bigger number.
static char pendingAskContext[1400];
// Set for a typed/shortcut question instead of a recording (see
// askTextQuestion()); pendingAskRec.wavData is null then.
static char pendingAskText[160] = "";
// Screen-off strip: speak a fixed sentence (no tools, no model decision)
// instead of asking the model - see shortcutClicked().
static bool pendingSayOnly = false;
// Back icon tapped while listening: stop the recording and throw it away.
static volatile bool recordingCancelled = false;

// ---- Voice commands (OpenAI function calling) ------------------------------
// The model can answer with an action instead of speech. The action runs on
// the UI thread (askTick()); a tool-call reply has no audio, so a second,
// small text request speaks the model's own one-sentence confirmation.
static const char ASK_TOOLS_JSON[] =
    "\"tools\":["
    "{\"type\":\"function\",\"function\":{\"name\":\"show_screen\",\"description\":\"Show a screen on the dashboard.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"screen\":{\"type\":\"string\",\"enum\":[\"profile\",\"calendar\",\"bus\",\"ask\",\"settings\",\"weather\"]},"
    "\"confirmation\":{\"type\":\"string\",\"description\":\"One short sentence confirming the action.\"}},\"required\":[\"screen\",\"confirmation\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"screen_off\",\"description\":\"Turn the screen off / put the dashboard to sleep.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"confirmation\":{\"type\":\"string\"}},\"required\":[\"confirmation\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"set_volume\",\"description\":\"Set speaker volume 0-100.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"percent\":{\"type\":\"integer\"},\"confirmation\":{\"type\":\"string\"}},\"required\":[\"percent\",\"confirmation\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"set_mute\",\"description\":\"Mute or unmute spoken answers.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"muted\":{\"type\":\"boolean\"},\"confirmation\":{\"type\":\"string\"}},\"required\":[\"muted\",\"confirmation\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"set_screen_mode\",\"description\":\"Dark/light screen: night, day, or auto (by sunset/sunrise).\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\",\"enum\":[\"auto\",\"night\",\"day\"]},\"confirmation\":{\"type\":\"string\"}},\"required\":[\"mode\",\"confirmation\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"select_bus_stop\",\"description\":\"Show arrivals for a 5-digit bus stop code.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\"},\"confirmation\":{\"type\":\"string\"}},\"required\":[\"code\",\"confirmation\"]}}}"
    "],";

static char pendingActionName[24] = "";
static char pendingActionArgs[200] = "";
static volatile bool actionPending = false;
static volatile bool sleepAfterAnswer = false;
static bool pendingAskWantAudio = false;
static uint32_t pendingAskGeneration = 0; // see askGeneration's own comment
static int consecutiveNegativeFailures = 0; // see askWithAudio()'s own comment on the last-resort Wi-Fi reconnect

// Short rolling conversation memory - without this, every question was
// answered with zero awareness of what was just discussed, making a
// natural follow-up ("what about its population?") come out disconnected/
// wrong. gpt-audio-mini's audio-input response gives no separate
// transcript of what the USER said, only the combined restate+answer text
// this app already asks for and stores in answerText - so history is built
// from the model's own prior answers (each already starts by restating
// that turn's question, so it carries real context) rather than true
// separate user/assistant turns. Session-only, capped small (3 turns) to
// keep the request body and token cost bounded on a device this size.
#define ASK_HISTORY_TURNS 3
static char askHistory[ASK_HISTORY_TURNS][400];
static int askHistoryCount = 0; // how many of the slots above are populated
static int askHistoryNext = 0;  // ring-buffer write cursor

static void pushAskHistory(const char *text)
{
    strlcpy(askHistory[askHistoryNext], text, sizeof(askHistory[0]));
    askHistoryNext = (askHistoryNext + 1) % ASK_HISTORY_TURNS;
    if (askHistoryCount < ASK_HISTORY_TURNS)
        askHistoryCount++;
}

// Typewriter reveal of answerText on the UI thread (askTick(), never the
// background task - only lv_* calls from the UI thread are safe). The full
// answer is already known and correct by the time ASK_SPEAKING/ASK_DONE is
// reached, so this is a pure display effect, not real token-by-token
// streaming from the network - but gives the same "typing" feel without
// touching the request/response format at all.
// Real bug fixed here, found from a garbled/spliced-looking answer in a
// real serial log ("What do I know abw-cost Wi-Fi microchip..." - two
// different answers' text visibly mixed mid-word): the reveal loop in
// askTick() (UI thread) was reading the shared answerText[] buffer
// directly while askWork() (background task) could already be mid-write
// into that SAME buffer for the NEXT question - extractJsonStringField()
// writes it byte-by-byte, not atomically, and the previous reveal wasn't
// guaranteed to have finished (or even been marked inactive) before the
// next question's network response started landing. Fixed by snapshotting
// the full answer into this UI-thread-only buffer at the single documented
// safe point (state reaching SPEAKING/DONE, after askWork() is done
// writing answerText for that question) and revealing from the snapshot
// only - the shared buffer is never read char-by-char over time again.
static char revealSnapshot[400];
static size_t revealChars = 0;
static uint32_t revealLastMs = 0;
static bool revealActive = false;
static uint32_t revealMsPerChar = 28; // recomputed per-answer from the real audio duration when audio played;
                                       // this default only covers the sound-off (text-only) path
static unsigned long taskStartedAtMs = 0;

// ---- OpenAI calls --------------------------------------------------------

// OpenAI's natural-language replies commonly include "smart" Unicode
// punctuation (curly quotes, en/em dashes) that LVGL's compiled-in
// Montserrat font has no glyph for - confirmed on-device, the first real
// answer ("I'm doing well...") showed a broken glyph where the apostrophe
// should be. Replaces the common 3-byte UTF-8 sequences with their plain-
// ASCII equivalents, in place (each is exactly 3 bytes, shrinking to 1, so
// a memmove is needed to close the gap).
static void sanitizeForDisplay(char *text)
{
    struct Replacement
    {
        unsigned char seq[3];
        char replacement;
    };
    static const Replacement replacements[] = {
        {{0xE2, 0x80, 0x98}, '\''}, // U+2018 left single quote
        {{0xE2, 0x80, 0x99}, '\''}, // U+2019 right single quote
        {{0xE2, 0x80, 0x9C}, '"'},  // U+201C left double quote
        {{0xE2, 0x80, 0x9D}, '"'},  // U+201D right double quote
        {{0xE2, 0x80, 0x93}, '-'},  // U+2013 en dash
        {{0xE2, 0x80, 0x94}, '-'},  // U+2014 em dash
    };

    unsigned char *p = (unsigned char *)text;
    while (*p)
    {
        bool matched = false;
        for (const Replacement &r : replacements)
        {
            if (p[0] == r.seq[0] && p[1] == r.seq[1] && p[2] == r.seq[2])
            {
                *p = (unsigned char)r.replacement;
                memmove(p + 1, p + 3, strlen((char *)(p + 3)) + 1); // +1 to include the null terminator
                matched = true;
                break;
            }
        }
        p += 1;
        (void)matched;
    }
}

// Builds a compact snapshot of on-device state (bus/calendar data this app
// already has cached, plus basic device status) to hand the model as
// context, so questions like "when's the next 173" or "what's my next
// meeting" can be answered from real data instead of the model just
// guessing/declining. Bus and calendar data are loaded once at boot
// (busInit()/todayInit() both force an initial fetch regardless of which
// tab is active - see their own comments) and kept fresh afterward only
// while their own tab is actually being viewed (existing power-conscious
// behavior, unchanged) - so this can be a few minutes stale if the user's
// been on the Ask tab a while, same staleness a human glancing at a
// slightly-old screen would see.
static void buildAskContext(char *out, size_t outSize)
{
    time_t now = time(nullptr);
    struct tm sgt;
    time_t shifted = now + 8 * 3600;
    gmtime_r(&shifted, &sgt);
    char timeBuf[24];
    strftime(timeBuf, sizeof(timeBuf), "%a %I:%M %p", &sgt);

    char busCtx[320];
    char todayCtx[320];
    char weatherCtx[320];
    busGetContextSummary(busCtx, sizeof(busCtx));
    todayGetContextSummary(todayCtx, sizeof(todayCtx));
    weatherGetContextSummary(weatherCtx, sizeof(weatherCtx));

    snprintf(out, outSize,
             "Device context as of %s SGT (Wi-Fi %s): %s %s %s "
             "Only use the above if the question is actually about the bus, "
             "the calendar or the weather - ignore it otherwise. If it's relevant "
             "but doesn't contain the answer, say so briefly rather than guessing.",
             timeBuf, WiFi.status() == WL_CONNECTED ? "connected" : "offline",
             todayCtx, busCtx, weatherCtx);
}

// Skips whitespace then a ':' then more whitespace, landing on whatever
// comes next (expected to be the opening '"' of a string value). Real bug
// fixed here: both functions below used to search for the literal
// substring `"key":"` (no space), assuming compact JSON - but a real
// response came back PRETTY-PRINTED (`"key": "value"`, with a space after
// the colon), so the literal strstr() silently failed to match even
// though the response was completely valid, and "Couldn't get an answer"
// showed despite a real, correct answer sitting right there in the log.
// Tolerating arbitrary whitespace around the colon works regardless of
// which formatting the API happens to send.
static const char *skipToColonValue(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p != ':')
        return nullptr;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

// Scans a null-terminated JSON-text buffer for `"key" : "value"` (any
// whitespace around the colon - see skipToColonValue()), handling standard
// JSON string escapes (\", \\, \n, \r, \t - the only ones this model's
// plain-English answers realistically produce) so the extracted text is
// genuinely correct, not just "stops at the first quote". Used instead of
// a full JsonDocument parse for the response's "transcript"/"content"
// field specifically, since the SAME response also carries the ~200-400KB
// base64 "data" field - loading the whole thing into ArduinoJson would
// duplicate that huge string into the document's own memory pool on top
// of the raw HTTP buffer already holding it, real risk on a board that's
// fought internal-heap pressure all session for far smaller allocations.
// `key` is just the quoted field name, e.g. "\"transcript\"" - no colon.
static bool extractJsonStringField(const char *json, const char *key, char *out, size_t outSize)
{
    const char *p = strstr(json, key);
    if (!p)
        return false;
    p += strlen(key);
    p = skipToColonValue(p);
    if (!p || *p != '"')
        return false;
    p++;
    size_t used = 0;
    while (*p && *p != '"' && used + 1 < outSize)
    {
        if (*p == '\\' && p[1])
        {
            p++;
            switch (*p)
            {
            case 'n':
                out[used++] = '\n';
                break;
            case 'r':
                out[used++] = '\r';
                break;
            case 't':
                out[used++] = '\t';
                break;
            case '"':
            case '\\':
            case '/':
                out[used++] = *p;
                break;
            default:
                out[used++] = *p; // best-effort for anything else (e.g. \uXXXX - left as-is, rare in plain answers)
                break;
            }
        }
        else
        {
            out[used++] = *p;
        }
        p++;
    }
    out[used] = '\0';
    return used > 0;
}

// Locates a base64 "data" field's raw character span within a JSON-text
// buffer, WITHOUT copying it - base64's alphabet (A-Z a-z 0-9 + / =) never
// needs JSON escaping, so unlike extractJsonStringField() this can just
// return pointer+length directly into the existing response buffer, and
// the caller decodes straight from there. `key` is just the quoted field
// name, e.g. "\"data\"" - no colon (see skipToColonValue()'s comment).
static bool findJsonBase64Field(const char *json, const char *key, const char **outStart, size_t *outLen)
{
    const char *p = strstr(json, key);
    if (!p)
        return false;
    p += strlen(key);
    p = skipToColonValue(p);
    if (!p || *p != '"')
        return false;
    p++;
    const char *end = strchr(p, '"');
    if (!end)
        return false;
    *outStart = p;
    *outLen = (size_t)(end - p);
    return true;
}

// Minimal WAV header parse - just enough to find the real sample rate and
// the start/length of the actual PCM data, since gpt-audio-mini's audio
// output (response_format "wav" - the Chat Completions audio API only
// offers "wav" or "mp3", unlike the old /v1/audio/speech endpoint's raw
// "pcm" option) comes back as a genuine RIFF/WAVE file, not headerless PCM
// like before. Searches for the "fmt " and "data" chunk IDs rather than
// assuming a fixed 44-byte layout, in case OpenAI's encoder inserts any
// extra chunks before the data (some WAV encoders do).
static bool parseWavHeader(const uint8_t *wav, size_t wavSize, uint32_t *outSampleRate, const uint8_t **outDataStart, size_t *outDataSize)
{
    if (wavSize < 44 || memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0)
        return false;

    size_t pos = 12;
    bool haveFmt = false;
    while (pos + 8 <= wavSize)
    {
        char chunkId[5] = {0};
        memcpy(chunkId, wav + pos, 4);
        uint32_t chunkSize;
        memcpy(&chunkSize, wav + pos + 4, 4); // little-endian, matches this board's own native byte order
        size_t chunkDataStart = pos + 8;

        if (strcmp(chunkId, "fmt ") == 0 && chunkDataStart + 16 <= wavSize)
        {
            uint32_t sampleRate;
            memcpy(&sampleRate, wav + chunkDataStart + 4, 4);
            *outSampleRate = sampleRate;
            haveFmt = true;
        }
        else if (strcmp(chunkId, "data") == 0 && chunkDataStart <= wavSize)
        {
            *outDataStart = wav + chunkDataStart;
            size_t available = wavSize - chunkDataStart;
            *outDataSize = (chunkSize < available) ? chunkSize : available;
            return haveFmt; // only valid once we've also seen fmt - data normally comes after it
        }
        pos = chunkDataStart + chunkSize + (chunkSize % 2); // chunks are word-aligned
    }
    return false;
}

#define OPENAI_AUDIO_MAX_RESPONSE_BYTES 1500000 // real responses have hit 827KB+ with a longer recording and the
                                                 // question-echo prompt - this lives in PSRAM (ps_malloc), not the
                                                 // scarce internal heap, so there's no reason to keep it tight

// Single combined call replacing the old transcribe -> chat -> TTS chain
// (3 separate HTTPS round trips) with one POST to /v1/chat/completions
// using gpt-audio-mini - confirmed via OpenAI's own docs/examples (not
// guessed) to accept audio input and return both text and audio output in
// one response. Real, direct benefit for this board specifically: 3 TLS
// handshakes down to 1 removes most of the internal-heap-fragmentation
// risk this session fought repeatedly ("SSL - Memory allocation failed"
// cascades), and removes the delay(300) back-to-back-TLS workarounds
// entirely - there's no "back-to-back" anymore.
//
// Request shape (confirmed from a real working example, not assumed):
//   {"model":"gpt-audio-mini","modalities":["text","audio"],
//    "audio":{"voice":"alloy","format":"wav"},
//    "messages":[{"role":"system","content":"<instructions+context>"},
//                {"role":"user","content":[{"type":"input_audio",
//                  "input_audio":{"data":"<base64 wav>","format":"wav"}}]}]}
// Response: choices[0].message.audio.data (base64 WAV) and
//           choices[0].message.audio.transcript (the spoken answer as text).
//
// Real, deliberate behavior change from the old 3-call flow: the old
// bus-stop-name-switching feature (busFindStopInHistoryByName() on the
// transcript, switching the active stop BEFORE asking) needed the
// transcript available before the question was answered - impossible now
// since transcription and answering happen atomically in one call. Not
// preserved; the model answers using whichever stop is already active.

// Streams the JSON request body (prefix + base64-encoded WAV + suffix) to
// HTTPClient::sendRequest(type, Stream*, size) in small pieces instead of
// requiring one ~342KB contiguous buffer. Root cause this replaces: a
// single http.POST(body, totalSize) call still needs mbedTLS to write that
// whole body out in TLS records regardless of how the caller's buffer is
// laid out, and real serial evidence (largest free internal block=36852
// bytes vs a 342553-byte payload) showed sends failing outright under
// internal-heap fragmentation. This class produces bytes on demand in
// small, bounded chunks - the base64 encoding itself happens
// kBase64ChunkRawBytes (768) raw bytes at a time (a multiple of 3, so each
// chunk's base64 output has no leftover/padding until the very last one),
// not as one giant pre-encoded blob - so nothing this class touches ever
// needs a large contiguous allocation, internal or PSRAM.
class AskRequestBodyStream : public Stream
{
public:
    static const size_t kBase64ChunkRawBytes = 768; // multiple of 3 - clean base64 blocks, no cross-chunk padding
    static const size_t kBase64ChunkOutCap = 1100;  // ceil(768/3)*4 = 1024, plus headroom

    AskRequestBodyStream(const char *prefix, size_t prefixLen,
                          const uint8_t *wavData, size_t wavSize,
                          const char *suffix, size_t suffixLen)
        : prefix_(prefix), prefixLen_(prefixLen),
          wavData_(wavData), wavSize_(wavSize),
          suffix_(suffix), suffixLen_(suffixLen)
    {
    }

    int available() override
    {
        return (int)remaining();
    }

    int read() override
    {
        uint8_t b;
        if (readBytes(&b, 1) != 1)
            return -1;
        return b;
    }

    int peek() override
    {
        // Not used by HTTPClient::sendRequest()'s send loop (it only calls
        // available()/read()) - implemented minimally to satisfy Stream's
        // pure virtual interface, not exercised in this code path.
        return -1;
    }

    size_t write(uint8_t) override { return 0; } // write-only direction not used; this is a read source for sendRequest()

    // Matches Stream::readBytes()'s contract closely enough for
    // HTTPClient's own internal use of it - fills as many bytes as are
    // currently available (up to len), pulling from whichever phase
    // (prefix/base64/suffix) is active, refilling the base64 chunk buffer
    // from raw WAV bytes on demand.
    size_t readBytes(uint8_t *buf, size_t len)
    {
        size_t got = 0;
        while (got < len)
        {
            if (prefixPos_ < prefixLen_)
            {
                buf[got++] = (uint8_t)prefix_[prefixPos_++];
                continue;
            }
            if (base64ChunkPos_ >= base64ChunkLen_)
            {
                if (!refillBase64Chunk())
                {
                    // No more raw WAV bytes to encode - move on to suffix.
                    if (suffixPos_ < suffixLen_)
                    {
                        buf[got++] = (uint8_t)suffix_[suffixPos_++];
                        continue;
                    }
                    break; // fully exhausted
                }
            }
            buf[got++] = base64Chunk_[base64ChunkPos_++];
        }
        return got;
    }

private:
    const char *prefix_;
    size_t prefixLen_;
    size_t prefixPos_ = 0;

    const uint8_t *wavData_;
    size_t wavSize_;
    size_t wavPos_ = 0;

    const char *suffix_;
    size_t suffixLen_;
    size_t suffixPos_ = 0;

    uint8_t base64Chunk_[kBase64ChunkOutCap];
    size_t base64ChunkLen_ = 0;
    size_t base64ChunkPos_ = 0;

    size_t remaining() const
    {
        size_t r = (prefixLen_ - prefixPos_) + (base64ChunkLen_ - base64ChunkPos_) + (suffixLen_ - suffixPos_);
        // Remaining un-encoded WAV bytes expand to base64 - approximate
        // (4/3 ratio) is fine since sendRequest() only uses available() to
        // decide how much to try reading right now, not as an exact total.
        r += ((wavSize_ - wavPos_) * 4 + 2) / 3;
        return r;
    }

    bool refillBase64Chunk()
    {
        if (wavPos_ >= wavSize_)
            return false;
        size_t rawLen = wavSize_ - wavPos_;
        if (rawLen > kBase64ChunkRawBytes)
            rawLen = kBase64ChunkRawBytes;
        size_t outLen = 0;
        int ret = mbedtls_base64_encode(base64Chunk_, sizeof(base64Chunk_), &outLen, wavData_ + wavPos_, rawLen);
        if (ret != 0)
        {
            Serial.printf("ask: AskRequestBodyStream base64 encode FAILED - ret=%d, rawLen=%u, requiredOutLen=%u, bufCap=%u\n",
                          ret, (unsigned)rawLen, (unsigned)outLen, (unsigned)sizeof(base64Chunk_));
            base64ChunkLen_ = 0;
            base64ChunkPos_ = 0;
            wavPos_ += rawLen; // still advance so we don't loop forever, even though this chunk's data is lost
            return true;
        }
        wavPos_ += rawLen;
        base64ChunkLen_ = outLen;
        base64ChunkPos_ = 0;
        return true;
    }
};

static bool askWithAudio(const MicRecording &rec, const char *context, bool wantAudio, char *outAnswer, size_t outAnswerSize,
                         const char *textQuestion = nullptr, bool allowTools = true, const char *systemOverride = nullptr)
{
    // Enlarged alongside pendingAskContext[] above (own comment there) - this
    // embeds that whole context string plus ~250 bytes of its own fixed
    // prose, so it needs the same real headroom, not just the context
    // buffer's own fix in isolation.
    char systemContent[1300];
    if (systemOverride)
        strlcpy(systemContent, systemOverride, sizeof(systemContent));
    else
        snprintf(systemContent, sizeof(systemContent),
                 "First restate the user's question in one short sentence, then answer "
                 "in 1-3 short plain-text sentences, no markdown, sized for a small "
                 "embedded display. Answer only the current question cleanly - do not "
                 "reuse or blend wording from earlier answers in this conversation. "
                 "If the user asks the dashboard to do something (show a screen, turn the "
                 "screen off, volume, mute, dark/light mode, a bus stop), call the matching "
                 "tool instead of answering. %s",
                 context);

    // Escaping via ArduinoJson for just this one small string (not the
    // request as a whole - see this function's own header comment on why
    // the large base64 audio stays out of ArduinoJson entirely). Safe here
    // since systemContent is well under 1KB.
    JsonDocument escDoc;
    escDoc.set(systemContent);
    String escapedSystem;
    serializeJson(escDoc, escapedSystem);

    // Prior turns as their own {"role":"assistant","content":"..."} JSON
    // objects, comma-separated, inserted right after the system message -
    // see askHistory's own comment for why these are all "assistant" turns
    // (no separate transcript of what the USER said is available from this
    // API). Empty string when there's no history yet (first question).
    String historyMessages = "";
    for (int i = 0; i < askHistoryCount; i++)
    {
        int idx = (askHistoryNext - askHistoryCount + i + ASK_HISTORY_TURNS) % ASK_HISTORY_TURNS;
        JsonDocument histDoc;
        histDoc.set(askHistory[idx]);
        String escapedHist;
        serializeJson(histDoc, escapedHist);
        historyMessages += ",{\"role\":\"assistant\",\"content\":";
        historyMessages += escapedHist;
        historyMessages += "}";
    }

    // THE actual root cause of the persistent one-byte-short uploads,
    // finally isolated via a real per-chunk byte-count capture (a run
    // showed base64BytesProducedTotal=341392 of 341393 "expected" - the
    // stream's real chunked output was correct all along; the "expected"
    // number itself was wrong). Reading mbedtls/library/base64.c directly:
    // mbedtls_base64_encode(NULL, 0, &olen, ...) always takes the
    // "buffer too small" path (line 48: `dlen < n+1 || NULL == dst` - a
    // NULL dst makes this true unconditionally), which sets
    // *olen = n + 1 (base64.c:49) - ONE MORE than the true encoded output
    // length, reserved for mbedTLS's own trailing NUL terminator it always
    // writes after the last real base64 character. This NULL-probe idiom
    // is documented for "how big must my buffer be" (n+1, to hold the NUL
    // too), NOT for "how many real output bytes will there be" - using it
    // directly as base64ActualLen silently counted that phantom NUL byte
    // as part of the HTTP body, making every Content-Length exactly 1 byte
    // larger than what the stream could ever actually deliver. Subtracting
    // 1 corrects this - confirmed algebraically: base64 output length must
    // always be a multiple of 4 (padded), and 341393 (the old, wrong
    // value) is NOT a multiple of 4, while 341392 is (341392/4=85348).
    size_t base64ActualLen = 0;
    if (rec.wavData)
    {
        mbedtls_base64_encode(NULL, 0, &base64ActualLen, (const unsigned char *)rec.wavData, rec.wavSize);
        if (base64ActualLen > 0)
            base64ActualLen -= 1; // undo mbedTLS's +1 NUL-terminator allowance - see comment above
    }

    // Typed/shortcut question: plain text user message, no audio upload.
    String userMessage;
    if (textQuestion)
    {
        JsonDocument qDoc;
        qDoc.set(textQuestion);
        String escapedQuestion;
        serializeJson(qDoc, escapedQuestion);
        userMessage = "{\"role\":\"user\",\"content\":" + escapedQuestion + "}]}";
    }
    const char *tools = allowTools ? ASK_TOOLS_JSON : "";

    // wantAudio (from soundEnabled) drops the whole "audio" field and
    // requests text-only output when muted - not just discarding audio
    // client-side, but actually not asking the server to generate it at
    // all, per explicit request: no point paying for audio synthesis on
    // an answer that will never be played.
    const char *userStart = textQuestion ? userMessage.c_str()
                                         : "{\"role\":\"user\",\"content\":[{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"";
    char *prefix = (char *)ps_malloc(4000 + escapedSystem.length() + historyMessages.length() + userMessage.length());
    if (!prefix)
        return false;
    size_t prefixCap = 4000 + escapedSystem.length() + historyMessages.length() + userMessage.length();
    struct PsramFree
    {
        void *p;
        ~PsramFree() { free(p); }
    } prefixGuard{prefix}; // freed on every return path below
    int prefixLen = wantAudio
        ? snprintf(prefix, prefixCap,
                   "{\"model\":\"gpt-audio-mini\",\"modalities\":[\"text\",\"audio\"],"
                   "\"audio\":{\"voice\":\"alloy\",\"format\":\"wav\"},"
                   "\"max_completion_tokens\":1000," // audio output tokens (codec) eat most of a low budget before
                                                       // text finishes - 300 was truncating mid-sentence every time
                   "%s\"messages\":[{\"role\":\"system\",\"content\":%s}%s,%s",
                   tools, escapedSystem.c_str(), historyMessages.c_str(), userStart)
        : snprintf(prefix, prefixCap,
                   "{\"model\":\"gpt-audio-mini\",\"modalities\":[\"text\"],"
                   "\"max_completion_tokens\":400,"
                   "%s\"messages\":[{\"role\":\"system\",\"content\":%s}%s,%s",
                   tools, escapedSystem.c_str(), historyMessages.c_str(), userStart);
    if (prefixLen < 0 || (size_t)prefixLen >= prefixCap)
    {
        Serial.println("ask: askWithAudio request prefix truncated - aborting");
        return false;
    }
    const char *suffix = textQuestion ? "" : "\",\"format\":\"wav\"}}]}]}";
    size_t suffixLen = strlen(suffix);

    // Real Content-Length for the complete streamed body - no buffer this
    // size is ever allocated; AskRequestBodyStream produces these same
    // bytes on demand in small chunks (see its own comment above).
    size_t totalSize = (size_t)prefixLen + base64ActualLen + suffixLen;

    // Real, 100%-reproducible bug already found and fixed once before in
    // this exact file's old speakAnswer() (see git history) - net_lock.h
    // isn't a recursive mutex, so a NetLockGuard held for this whole
    // function's scope would still be held when speakerPlayPcm() (called
    // near the end of this function) tries to acquire the SAME lock again,
    // guaranteed to fail every time, not just occasionally. Same fix:
    // scope netLock to a nested block covering only the network work,
    // releasing it (via the destructor, at the closing brace) BEFORE
    // playback is ever attempted. respBuf/received are declared outside
    // this block since the parsing/playback code after it still needs them.
    uint8_t *respBuf = nullptr;
    size_t received = 0;
    {
        // See net_lock.h - same reasoning as every other HTTPS/I2S call in
        // this file. 10s, not the 60s default - matching this file's
        // established fix.
        NetLockGuard netLock(10000);
        if (!netLock.acquired())
        {
            Serial.println("ask: askWithAudio could not acquire network lock in time");
            return false;
        }

        // Heap-allocated (not stack values) so a failed-send retry can
        // genuinely destroy this exact object (freeing its mbedTLS
        // ssl_context) before constructing a replacement - a first attempt
        // at this kept the original `client`/`http` alive in the same
        // scope as a second retry pair, so the old object's TLS buffers
        // were never actually released before the retry tried to allocate
        // new ones, and the retry's own connect() kept failing for exactly
        // the same reason as the original failure. `delete` + reassignment
        // below is what actually runs ~WiFiClientSecure() at the right
        // moment now, not just a comment claiming it does.
        // Real root cause of the HTTP -3 failures, finally found by reading
        // ssl_client.cpp's send_ssl_data() directly: WiFiClientSecure's
        // _timeout becomes sslclient_context.socket_timeout at connect()
        // time (ssl_client.cpp:88), and send_ssl_data()'s own write loop
        // (ssl_client.cpp:374-387) silently `return -1` on a timeout with
        // NO log_e()/mbedTLS error text at all (only genuine mbedTLS error
        // codes go through handle_error(), which DOES log - the total
        // absence of any ssl_client.cpp log line in every failing run's
        // serial capture is the real evidence this is a plain timeout, not
        // a TLS/memory error). 15000ms was set for a completely different
        // reason (the response-download chunked-decode loop's per-read
        // timeout, see below) and was never evaluated against the UPLOAD
        // side's real needs - real captures showed uploads stalling after
        // as little as 13KB of a ~310KB body, consistent with this link's
        // logged RSSI (-54 to -59 dBm, borderline) occasionally needing
        // much longer than 15s to drain a single TLS write. Raised to
        // 60000ms, matching this codebase's own established "a single
        // HTTPS call can legitimately take up to 60s on this network"
        // assumption (NetLockGuard's default timeout, bus.cpp/today.cpp's
        // watchdogs) rather than inventing a new number.
        WiFiClientSecure *client = new WiFiClientSecure();
        client->setInsecure();
        client->setHandshakeTimeout(8);
        client->setTimeout(60000);
        HTTPClient *http = new HTTPClient();
        http->setConnectTimeout(8000);
        http->setTimeout(60000); // matches client->setTimeout() above - the upload is the slow part now, not just the download
        http->setReuse(false); // no keep-alive - force a clean socket teardown on any failure rather than risking a stale/half-open connection being reused
        if (!http->begin(*client, "https://api.openai.com/v1/chat/completions"))
        {
            Serial.println("ask: askWithAudio http.begin() failed");
            delete http;
            delete client;
            return false;
        }
        // Real bug found in this file's own previous attempt at this fix:
        // HTTPClient::begin() only parses/stores the URL - the actual TCP+
        // TLS connect happens inside HTTPClient's PRIVATE connect(), called
        // internally by sendRequest()/GET()/POST(). Since this function
        // deliberately bypasses those (see the big comment below on why),
        // `client` was never actually connected, so every client->print()/
        // write() below silently did nothing (0 bytes) - confirmed by a
        // real capture showing "total bytes written=0 of 342554" instantly,
        // not a timeout. Connecting `client` directly, the same way
        // HTTPClient::connect() itself does internally (client->connect(host,
        // port, timeout), WiFiClientSecure already has setInsecure() from
        // above), fixes this.
        if (!client->connect("api.openai.com", 443, 8000))
        {
            Serial.println("ask: askWithAudio client->connect() failed");
            http->end();
            delete http;
            delete client;
            return false;
        }
        char authHeader[256]; // see this file's established comment on this size (a real sk-proj-... key needs ~172)
        snprintf(authHeader, sizeof(authHeader), "Bearer %s", cfgOpenAiKey());

        // Real root cause of the HTTP -3 failures, found via verbose
        // (CORE_DEBUG_LEVEL=5) serial capture: HTTPClient::sendRequest(type,
        // Stream*, size)'s own internal send loop (HTTPClient.cpp:762)
        // calls connected() before EVERY chunk, and connected() internally
        // peeks the read side (WiFiClientSecure::connected() -> data_to_read()
        // -> mbedtls_ssl_read(ctx, NULL, 0)) - a real capture showed this
        // exact peek failing with mbedTLS code -80 (BAD_INPUT_DATA) right at
        // the tail of the upload, after 342553 of 342554 bytes had already
        // been successfully written (genuinely one byte short, not a real
        // send failure) - confirmed by HTTPClient's own log: "Stream payload
        // bytesWritten 342553 and size 342554 mismatch!". This is a bug in
        // that framework loop's own connected()-polling pattern under this
        // exact NULL/0 mbedtls_ssl_read() idiom, not something fixable by
        // changing what we hand to sendRequest(). Fixed by bypassing that
        // loop entirely: send the request line + headers + streamed body
        // directly over `client` ourselves (one write per body chunk, no
        // connected()/available() check between them), then parse the
        // response by hand (status line + skip headers) before falling
        // through to the SAME chunked-body-read loop this file already had.
        // http->begin() above already did the TLS connect; http is used
        // only for its Content-Length/Authorization header text below and
        // is not asked to send anything itself.
        String requestLine = String("POST ") + "/v1/chat/completions" + " HTTP/1.1\r\n";
        String headers = String("Host: api.openai.com\r\n") +
                          "User-Agent: ESP32HTTPClient\r\n" +
                          "Connection: close\r\n" +
                          "Content-Type: application/json\r\n" +
                          "Authorization: " + authHeader + "\r\n" +
                          "Content-Length: " + String((unsigned)totalSize) + "\r\n\r\n";

        int code = -1;
        size_t totalWritten = 0;
        bool sendOk = client->print(requestLine) && client->print(headers);
        if (sendOk)
        {
            AskRequestBodyStream bodyStream(prefix, (size_t)prefixLen, rec.wavData, rec.wavSize, suffix, suffixLen);
            uint8_t chunkBuf[1024];
            while (totalWritten < totalSize)
            {
                size_t want = totalSize - totalWritten;
                if (want > sizeof(chunkBuf))
                    want = sizeof(chunkBuf);
                size_t got = bodyStream.readBytes(chunkBuf, want);
                if (got == 0)
                    break; // stream exhausted early - shouldn't happen given totalSize is exact, but don't loop forever
                size_t chunkWritten = 0;
                // Real bug found via verbose capture: a plain "one retry
                // then give up on 0" (matching HTTPClient's own internal
                // pattern) was not persistent enough - a real run showed
                // the LAST write of the whole request (537 bytes, tail of
                // the suffix) genuinely short-write then fail entirely,
                // landing at exactly 342553 of 342554 bytes sent. Retrying
                // a zero-byte write a bounded number of times with a short
                // delay (giving the TLS/TCP send buffer time to drain, the
                // same reasoning HTTPClient.cpp's own single retry uses,
                // just applied more persistently) instead of failing after
                // one attempt.
                int zeroWriteRetries = 0;
                while (chunkWritten < got)
                {
                    size_t w = client->write(chunkBuf + chunkWritten, got - chunkWritten);
                    if (w == 0)
                    {
                        zeroWriteRetries++;
                        if (zeroWriteRetries > 5)
                        {
                            sendOk = false;
                            break;
                        }
                        delay(20);
                        continue;
                    }
                    zeroWriteRetries = 0;
                    chunkWritten += w;
                }
                if (!sendOk)
                    break;
                totalWritten += chunkWritten;
            }
        }
        if (sendOk && totalWritten == totalSize)
        {
            // Real bug found here: readStringUntil('\n') returned an empty
            // string INSTANTLY (not after any real wait) even though the
            // full request had genuinely been sent - client->setTimeout()
            // governs Stream::_timeout, but a real capture showed this
            // path failing far faster than even a short timeout would
            // explain, meaning the actual problem was never a timeout
            // value - it's that nothing was waited for at all before the
            // first read attempt. OpenAI's response to a large audio+chat
            // request is not instant (model inference + audio synthesis
            // takes real seconds), so this now explicitly polls
            // client->available() in a real bounded loop (up to 45s, well
            // under this call's own 60s http/client timeouts) BEFORE
            // attempting to read the status line, instead of trusting a
            // single blocking readStringUntil() call to somehow already
            // know to wait that long.
            unsigned long waitStart = millis();
            while (client->connected() && client->available() == 0 && millis() - waitStart < 45000)
                delay(20);

            String statusLine = client->readStringUntil('\n');
            if (statusLine.startsWith("HTTP/1.1 ") || statusLine.startsWith("HTTP/1.0 "))
                code = statusLine.substring(9, 12).toInt();
            else
                code = -3; // malformed/empty status line (e.g. the wait above timed out with nothing received) - matches the retry block's own handling of this same case
            String headerLine;
            int headerCount = 0;
            do
            {
                headerLine = client->readStringUntil('\n');
                if (headerLine.length() > 1)
                    headerCount++;
            } while (headerLine.length() > 1 && headerCount < 40); // blank line (just "\r") ends the headers; cap to avoid an infinite loop on a malformed response
        }
        else
        {
            code = -3; // matches HTTPC_ERROR_SEND_PAYLOAD_FAILED's meaning - send genuinely failed/short
        }

        // Real measured root cause (not guessed): a negative code here
        // (e.g. -3 "Failed to send chunk!") happens when the largest
        // contiguous internal-heap block is smaller than what mbedTLS needs
        // for its TLS write buffer while sending this ~250-350KB audio
        // payload. A retry that reused the same WiFiClientSecure/HTTPClient
        // objects (just http.end()+delay+http.begin() again) still failed
        // its OWN reconnect with "start_ssl_client: -1" - real evidence
        // that neither http.end() nor write()'s internal stop() actually
        // frees the sslclient_context's mbedTLS buffers, only the raw
        // socket (confirmed by reading WiFiClientSecure.cpp/ssl_client.cpp
        // - only the object's real destructor does that, via stop() ->
        // stop_ssl_socket()). A second attempt at this created a SEPARATE
        // retryClient/retryHttp pair but left the ORIGINAL client/http
        // objects alive in the same enclosing scope the whole time -
        // exactly the same bug, just with new names, since the original
        // objects' destructors still hadn't run when the retry connected.
        // Actually fixed now: client/http are heap-allocated pointers
        // (declared above), and on a negative code the ORIGINAL objects
        // are `delete`d here - which genuinely runs their destructors and
        // releases their mbedTLS state - before new ones are constructed
        // for the retry, a real release-then-allocate ordering.
        if (code < 0)
        {
            Serial.println("ask: askWithAudio POST failed, retrying once with a fresh TLS client");
            http->end();
            client->stop(); // explicit raw-socket close before delete, not just relying on destructor ordering - lets the TCP stack start its own close/TIME_WAIT handling immediately
            delete http;    // runs ~HTTPClient()
            delete client;  // runs ~WiFiClientSecure() -> stop() (no-op, already stopped) -> stop_ssl_socket(), freeing mbedTLS buffers

            // Heap is confirmed NOT the cause (real 5-sample capture across
            // 2.5s showed a perfectly flat, adequate largest-free-block
            // every time) - the actual root cause was send_ssl_data()'s
            // silent write-timeout (see client->setTimeout() above), so
            // the heap-sampling delay loop that used to be here has been
            // removed; a short settle delay is still worthwhile before
            // reconnecting to the same host.
            vTaskDelay(pdMS_TO_TICKS(500));

            client = new WiFiClientSecure();
            client->setInsecure();
            client->setHandshakeTimeout(8);
            client->setTimeout(60000); // matches the initial attempt's fix above - real root cause was a silent write timeout, not memory
            http = new HTTPClient();
            http->setConnectTimeout(8000);
            http->setTimeout(60000);
            http->setReuse(false);
            // Same fix as the first attempt above - http->begin() alone
            // does not connect; client itself must be connected before any
            // write will actually send data.
            bool beginOk = http->begin(*client, "https://api.openai.com/v1/chat/completions");
            bool connectOk = beginOk && client->connect("api.openai.com", 443, 8000);
            if (!beginOk)
                Serial.println("ask: askWithAudio retry http.begin() failed");
            else if (!connectOk)
                Serial.println("ask: askWithAudio retry client->connect() failed");
            if (connectOk)
            {
                // Same manual send as the first attempt above (see its own
                // comment for why) - a fresh AskRequestBodyStream since the
                // first attempt's instance is already fully consumed
                // (single-pass), streaming the identical body again.
                bool retrySendOk = client->print(requestLine) && client->print(headers);
                size_t retryWritten = 0;
                if (retrySendOk)
                {
                    AskRequestBodyStream retryBodyStream(prefix, (size_t)prefixLen, rec.wavData, rec.wavSize, suffix, suffixLen);
                    uint8_t chunkBuf[1024];
                    while (retryWritten < totalSize)
                    {
                        size_t want = totalSize - retryWritten;
                        if (want > sizeof(chunkBuf))
                            want = sizeof(chunkBuf);
                        size_t got = retryBodyStream.readBytes(chunkBuf, want);
                        if (got == 0)
                            break;
                        size_t chunkWritten = 0;
                        int zeroWriteRetries = 0;
                        while (chunkWritten < got)
                        {
                            size_t w = client->write(chunkBuf + chunkWritten, got - chunkWritten);
                            if (w == 0)
                            {
                                zeroWriteRetries++;
                                if (zeroWriteRetries > 5)
                                {
                                    retrySendOk = false;
                                    break;
                                }
                                delay(20);
                                continue;
                            }
                            zeroWriteRetries = 0;
                            chunkWritten += w;
                        }
                        if (!retrySendOk)
                            break;
                        retryWritten += chunkWritten;
                    }
                }
                if (retrySendOk && retryWritten == totalSize)
                {
                    unsigned long retryWaitStart = millis();
                    while (client->connected() && client->available() == 0 && millis() - retryWaitStart < 45000)
                        delay(20);

                    String statusLine = client->readStringUntil('\n');
                    if (statusLine.startsWith("HTTP/1.1 ") || statusLine.startsWith("HTTP/1.0 "))
                        code = statusLine.substring(9, 12).toInt();
                    else
                        code = -3;
                    String headerLine;
                    int headerCount = 0;
                    do
                    {
                        headerLine = client->readStringUntil('\n');
                        if (headerLine.length() > 1)
                            headerCount++;
                    } while (headerLine.length() > 1 && headerCount < 40);
                }
                else
                {
                    code = -3;
                }
            }
        }

        Serial.printf("ask: askWithAudio HTTP POST returned %d\n", code);
        if (code != 200)
        {
            http->end();
            delete http;
            delete client;
            // A negative code (not a real HTTP status - e.g. -1 "connection
            // refused"/send failure) can be this project's own documented
            // internal-heap-fragmentation pattern (see CLAUDE.md's Ask-tab
            // postmortem) - but a single transient failure is common and
            // usually self-resolves on the next attempt, so WiFi.disconnect()
            // (tears down the WHOLE netif, disrupting Bus/Today too) is kept
            // as a genuine last resort: only after 2 CONSECUTIVE negative-code
            // failures, not the first one. A success anywhere resets the
            // counter (see the bottom of this function).
            if (code < 0)
            {
                consecutiveNegativeFailures++;
                Serial.printf("ask: askWithAudio negative HTTP code (%d consecutive)\n", consecutiveNegativeFailures);
                if (consecutiveNegativeFailures >= 2)
                {
                    Serial.println("ask: askWithAudio 2+ consecutive negative-code failures - forcing Wi-Fi reconnect as last resort");
                    WiFi.disconnect();
                    consecutiveNegativeFailures = 0;
                }
            }
            return false;
        }
        consecutiveNegativeFailures = 0; // a real HTTP response (even non-200) means the connection itself worked

        // Same manual chunked-transfer-decode loop already proven correct
        // for the old TTS download (see this file's git history) -
        // OpenAI's infra chunks dynamically-sized JSON responses the same
        // way regardless of endpoint, and reading raw off the stream
        // without decoding chunk framing was the exact bug that corrupted
        // every TTS response earlier this session. Reading into a plain
        // byte buffer (not a String/JsonDocument) for the same reason as
        // the request: this response contains ~200-400KB of base64 audio
        // inline.
        respBuf = (uint8_t *)ps_malloc(OPENAI_AUDIO_MAX_RESPONSE_BYTES + 1);
        if (!respBuf)
        {
            Serial.println("ask: askWithAudio response ps_malloc failed");
            http->end();
            delete http;
            delete client;
            return false;
        }
        WiFiClient *stream = http->getStreamPtr();
        unsigned long readStart = millis();
        bool chunkError = false;
        // Fixed stack buffer for each chunk-size line, not a fresh
        // Arduino String per chunk (readStringUntil() was allocating and
        // freeing a small heap block from the INTERNAL heap on every single
        // chunk of a response that can be split into dozens of chunks -
        // real, repeated small alloc/free churn on the exact heap this
        // board's mbedTLS buffers also compete for, a genuine contributor
        // to the internal-heap fragmentation this file's own header comment
        // documents. A chunk-size hex line is always short (a handful of
        // hex digits + optional extensions), so a small fixed buffer is
        // correct, not just an optimization.
        char chunkHeaderBuf[32];
        while (millis() - readStart < 25000)
        {
            size_t hdrLen = stream->readBytesUntil('\n', chunkHeaderBuf, sizeof(chunkHeaderBuf) - 1);
            chunkHeaderBuf[hdrLen] = '\0';
            if (hdrLen == 0)
            {
                chunkError = true;
                break;
            }
            // trim trailing \r (and any stray whitespace) in place - same
            // effect as String::trim(), no allocation
            while (hdrLen > 0 && (chunkHeaderBuf[hdrLen - 1] == '\r' || chunkHeaderBuf[hdrLen - 1] == ' '))
                chunkHeaderBuf[--hdrLen] = '\0';
            long chunkLen = strtol(chunkHeaderBuf, NULL, 16);
            if (chunkLen <= 0)
                break; // terminating 0-size chunk
            if (received + (size_t)chunkLen >= OPENAI_AUDIO_MAX_RESPONSE_BYTES)
            {
                Serial.println("ask: askWithAudio response exceeded cap");
                chunkError = true;
                break;
            }
            size_t got = stream->readBytes(respBuf + received, (size_t)chunkLen);
            if (got != (size_t)chunkLen)
            {
                chunkError = true;
                break;
            }
            received += got;
            vTaskDelay(1); // readBytes() busy-waits; ~1MB of chunks back to back would otherwise starve core 0's idle task (task watchdog)
            char trailing[2];
            if (stream->readBytes((uint8_t *)trailing, 2) != 2 || trailing[0] != '\r' || trailing[1] != '\n')
            {
                chunkError = true;
                break;
            }
        }
        http->end();
        delete http;
        delete client;
        addNetworkBytes((uint32_t)received);
        if (chunkError)
        {
            Serial.printf("ask: askWithAudio chunked-decode failed after %u bytes (partial response, discarding)\n", (unsigned)received);
            free(respBuf);
            return false;
        }
    } // netLock released here, before speakerPlayPcm() acquires its own

    respBuf[received] = '\0'; // safe: buffer is sized +1 for exactly this

    // Voice command: the model called a tool instead of answering (such a
    // reply has no audio and is small). Hand the action to the UI thread,
    // then speak the model's own confirmation with a small text request.
    if (allowTools && strstr((const char *)respBuf, "\"tool_calls\""))
    {
        JsonDocument toolDoc;
        DeserializationError err = deserializeJson(toolDoc, (const char *)respBuf);
        free(respBuf);
        if (err)
            return false;
        JsonObject fn = toolDoc["choices"][0]["message"]["tool_calls"][0]["function"];
        const char *name = fn["name"] | "";
        const char *args = fn["arguments"] | "{}";
        JsonDocument argDoc;
        deserializeJson(argDoc, args);
        const char *confirmation = argDoc["confirmation"] | "Done.";
        Serial.printf("ask: action %s %s\n", name, args);
        strlcpy(pendingActionName, name, sizeof(pendingActionName));
        strlcpy(pendingActionArgs, args, sizeof(pendingActionArgs));
        actionPending = true;
        strlcpy(outAnswer, confirmation, outAnswerSize);
        sanitizeForDisplay(outAnswer);
        if (!wantAudio || strcmp(name, "set_mute") == 0)
            return true;
        char confirmCopy[160];
        strlcpy(confirmCopy, outAnswer, sizeof(confirmCopy));
        MicRecording none = {nullptr, 0};
        // Speaks the sentence (and sets ASK_SPEAKING itself); a failure
        // there still leaves the text confirmation.
        askWithAudio(none, "", true, outAnswer, outAnswerSize, confirmCopy, false,
                     "Say the user's message back exactly as written, nothing else.");
        strlcpy(outAnswer, confirmCopy, outAnswerSize);
        return true;
    }

    // Field location depends on whether audio was requested: with
    // modalities:["text","audio"], the model's message.content is null and
    // the actual answer text lives at message.audio.transcript instead
    // (confirmed from a real working example - see askWithAudio()'s own
    // header comment). With modalities:["text"] only (wantAudio=false),
    // this is a standard Chat Completions response - the answer is the
    // ordinary message.content string, no "audio" object exists at all.
    const char *textField = wantAudio ? "\"transcript\"" : "\"content\"";
    if (!extractJsonStringField((const char *)respBuf, textField, outAnswer, outAnswerSize))
    {
        Serial.println("ask: askWithAudio response has no answer text field");
        // Truncated, not the full response - this can be several hundred KB
        // of base64 audio (real bug found: printing it whole over a
        // 115200-baud serial link can block this task for ~70s, inside the
        // Network Worker, risking the watchdog). First 300 bytes is enough
        // to see which JSON shape actually came back.
        char preview[301];
        size_t previewLen = received < sizeof(preview) - 1 ? received : sizeof(preview) - 1;
        memcpy(preview, respBuf, previewLen);
        preview[previewLen] = '\0';
        Serial.println(preview);
        free(respBuf);
        return false;
    }
    sanitizeForDisplay(outAnswer);

    if (!wantAudio)
    {
        free(respBuf);
        return true;
    }

    const char *audioB64Start = nullptr;
    size_t audioB64Len = 0;
    bool havePlayableAudio = findJsonBase64Field((const char *)respBuf, "\"data\"", &audioB64Start, &audioB64Len);

    if (!havePlayableAudio)
    {
        free(respBuf);
        return true; // text answer is still valid even without audio - non-fatal, matches this file's established pattern
    }

    // Upper bound (every 4 base64 chars -> at most 3 bytes) instead of a
    // mbedtls sizing pass - that pass walks the whole ~1MB string too.
    size_t wavLen = audioB64Len / 4 * 3 + 3;
    uint8_t *wavBuf = (uint8_t *)ps_malloc(wavLen);
    if (!wavBuf)
    {
        Serial.printf("ask: askWithAudio WAV decode ps_malloc(%u) failed - PSRAM free=%u largest_free_block=%u\n",
                      (unsigned)wavLen,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        free(respBuf);
        return true; // text answer still valid
    }
    // Decoded in 4KB chunks (a multiple of 4, so each chunk is a complete
    // base64 unit; padding only ever appears in the last one), yielding in
    // between. One mbedtls_base64_decode() call over a long answer's ~1MB
    // of base64 (its constant-time decoder is slow) kept core 0 busy past
    // the 5s task watchdog - real log: "task_wdt ... IDLE0 ... netWorker",
    // backtrace in mbedtls_ct_base64_enc_char from here, then a reboot.
    size_t wavActualLen = 0;
    int decodeResult = 0;
    const size_t B64_CHUNK = 4096;
    for (size_t off = 0; off < audioB64Len && decodeResult == 0; off += B64_CHUNK)
    {
        size_t n = audioB64Len - off < B64_CHUNK ? audioB64Len - off : B64_CHUNK;
        size_t outLen = 0;
        decodeResult = mbedtls_base64_decode(wavBuf + wavActualLen, wavLen - wavActualLen, &outLen,
                                             (const unsigned char *)audioB64Start + off, n);
        wavActualLen += outLen;
        vTaskDelay(1);
    }
    free(respBuf); // audioB64Start pointed into this - must not be used after this point

    if (decodeResult != 0)
    {
        Serial.printf("ask: askWithAudio base64 decode failed, mbedtls error %d\n", decodeResult);
        free(wavBuf);
        return true;
    }

    uint32_t sampleRate = 0;
    const uint8_t *pcmStart = nullptr;
    size_t pcmSize = 0;
    if (!parseWavHeader(wavBuf, wavActualLen, &sampleRate, &pcmStart, &pcmSize))
    {
        Serial.println("ask: askWithAudio WAV header parse failed");
        free(wavBuf);
        return true;
    }

    // Copy just the PCM portion into its own buffer (lastAnswerPcm's
    // lifetime is independent of wavBuf, which is freed right after) -
    // same PSRAM-buffer-for-replay pattern this file already established
    // (see lastAnswerPcm's own comment for why this replaced SPIFFS).
    int16_t *pcmCopy = (int16_t *)ps_malloc(pcmSize);
    if (!pcmCopy)
    {
        Serial.println("ask: askWithAudio PCM copy ps_malloc failed");
        free(wavBuf);
        return true;
    }
    memcpy(pcmCopy, pcmStart, pcmSize);
    free(wavBuf);

    if (lastAnswerPcm)
        free(lastAnswerPcm);
    lastAnswerPcm = pcmCopy;
    lastAnswerSampleCount = pcmSize / sizeof(int16_t);
    lastAnswerSampleRate = sampleRate;

    state = ASK_SPEAKING; // only now - see the AskState enum's comment on why this moved out of askWork()
    speakerPlayPcm(lastAnswerPcm, lastAnswerSampleCount, lastAnswerSampleRate);
    return true;
}

// The actual Network Worker job (network_worker.h) - wraps askWithAudio()
// UNCHANGED (requirement #12: don't modify mic/ES8311/speaker/STT/GPT/TTS
// except to move the network call into the worker - askWithAudio() itself
// isn't touched, including its own speakerPlayPcm() call at the end, which
// still runs right here on the Worker task exactly as before). Reads its
// "arguments" from the pending* statics (written by askWork() before
// submitting, see their own comment on why they're static not stack-local)
// since NetworkJobFn takes no parameters.
//
// Owns ALL of the post-network finalization that used to run in askWork()
// after its blocking wait - state transitions, freeing the recorded WAV
// buffer, pushing conversation history - because askTask (and askWork())
// no longer exist by the time this job actually runs; it may still be
// queued behind Bus/Calendar/Weather long after the mic tap that started
// it, per the required architecture (record -> submit -> exit immediately).
static void askNetworkJob()
{
    // Snapshot at submit time (see askGeneration's own comment) - if the
    // user cancels this question before this job gets to run/finish,
    // askGeneration will have moved on and none of this function's results
    // should be applied to shared state any more.
    uint32_t myGeneration = pendingAskGeneration;

    if (myGeneration != askGeneration)
    {
        // Cancelled before this job even started running - a cancel-then-
        // immediately-retap sequence can already have overwritten
        // pendingAskRec.wavData with a NEW recording's pointer via
        // askWork()'s micCaptureRecord() call, so this stale job must NOT
        // touch pendingAskRec at all (neither read it into askWithAudio()
        // nor free() it) - doing either would risk corrupting or freeing
        // memory the new question now owns. This does mean the cancelled
        // question's own WAV buffer (a small PSRAM allocation, this board
        // has several MB free) is never freed - an intentional, small,
        // one-time leak traded for correctness, since there's no safe way
        // from here to tell whether pendingAskRec.wavData still points at
        // the cancelled recording or a newer one.
        Serial.println("ask: askNetworkJob discarded before starting - question was cancelled/superseded");
        return;
    }

    // askWithAudio() (unchanged) runs speakerPlayPcm() internally near its
    // own end before returning - so by the time either exit path below is
    // reached, STT + answer generation + TTS + speaker playback are ALL
    // already finished. taskRunning is cleared on BOTH paths here, since
    // this is now the true end of the entire Ask interaction (see this
    // function's own header comment and askIsBusy()'s own comment).
    bool ok = pendingSayOnly
        ? askWithAudio(pendingAskRec, "", pendingAskWantAudio, answerText, sizeof(answerText), pendingAskText, false,
                       "Say the user's message back exactly as written, nothing else.")
        : askWithAudio(pendingAskRec, pendingAskContext, pendingAskWantAudio, answerText, sizeof(answerText),
                       pendingAskRec.wavData ? nullptr : pendingAskText);
    if (pendingSayOnly)
        strlcpy(answerText, pendingAskText, sizeof(answerText)); // show the sentence itself
    pendingSayOnly = false;
    free(pendingAskRec.wavData);

    if (myGeneration != askGeneration)
    {
        // Cancelled while this job was actually running (network call in
        // flight) - startOrStopAsk()'s cancel path already reset
        // taskRunning/state/the loading spinner, so nothing here should
        // touch them. pendingAskRec.wavData was already freed just above,
        // which is safe here specifically because taskRunning stayed true
        // for this job's ENTIRE run (a new question can't start recording
        // - and therefore can't overwrite pendingAskRec - until taskRunning
        // is false again, and it only goes false via this function or a
        // cancel that happens AFTER this point).
        Serial.println("ask: askNetworkJob result discarded - question was cancelled/superseded");
        return;
    }

    setLoadingVisible(false);
    if (!ok)
    {
        snprintf(statusError, sizeof(statusError), "Couldn't get an answer - check Wi-Fi");
        diagReport(DIAG_OPENAI, false, "no answer");
        state = ASK_ERROR;
        taskRunning = false; // exit path 1 of 2 - error
        return;
    }
    Serial.printf("ask: answer: %s\n", answerText);
    diagReport(DIAG_OPENAI, true);
    pushAskHistory(answerText); // remembered for the NEXT question - see askHistory's own comment
    state = ASK_DONE;
    taskRunning = false; // exit path 2 of 2 - success
}

// ---- Orchestration task ---------------------------------------------------

// Runs on its own FreeRTOS task - records audio, then submits the actual
// network call as a job to the shared Network Worker (askNetworkJob(),
// network_worker.h) and returns immediately, without waiting for it.
static void askWork()
{
    // Real, previously-fought problem: "if I rush to Ask before letting the
    // calendar load" used to glitch the recorded audio, because bus.cpp's/
    // today.cpp's own fetch tasks competed with this task for core 0's CPU
    // time. Chased through several designs (a manual wait loop, a hand-
    // rolled event group, NetLockGuard polling) before this refactor to a
    // single shared Network Worker task (network_worker.h) - now there is
    // only ever ONE network-job task running at a time across the whole
    // app, so recording (which was never network activity itself) can just
    // start immediately with no wait, and the actual HTTPS call below is
    // submitted as a queued job instead of run directly here.
    //
    // Stage-by-stage timestamps (elapsed ms since the mic was tapped) kept
    // from before - still useful for telling which stage (record/network
    // job/speak) actually consumed time in a slow run.
    Serial.printf("ask: [%lums] starting recording\n", millis() - taskStartedAtMs);
    // Real bug found via serial evidence: micCaptureRecord() can block for
    // a while (up to ~21s) waiting on its own internal-heap-contention lock
    // if a Calendar/Bus fetch is already in flight - setting ASK_LISTENING
    // here unconditionally reintroduced the exact "said Listening... but
    // recording hadn't actually started yet" bug this enum's comment
    // already describes being fixed once before, just via a different
    // wait this time. ASK_WAITING_FOR_NETWORK covers that wait; the
    // callback flips to ASK_LISTENING only once recording genuinely starts.
    state = ASK_WAITING_FOR_NETWORK;
    bool recorded = micCaptureRecord(&pendingAskRec, MAX_RECORD_SECONDS, []() { state = ASK_LISTENING; });
    diagReport(DIAG_AUDIO, recorded, recorded ? "" : "mic recording failed");
    if (recordingCancelled)
    {
        recordingCancelled = false;
        free(pendingAskRec.wavData);
        pendingAskRec = {nullptr, 0};
        Serial.println("ask: recording cancelled");
        state = ASK_IDLE;
        taskRunning = false;
        return;
    }
    if (!recorded)
    {
        snprintf(statusError, sizeof(statusError), "Didn't catch that - try again");
        state = ASK_ERROR;
        // Real bug: taskRunning is normally cleared by askNetworkJob() once
        // it finishes, but that job is never submitted on this failure path -
        // without clearing it here, taskRunning stays stuck true and the UI
        // treats the next mic tap as a cancel instead of a new question,
        // until the 240s watchdog eventually recovers it.
        taskRunning = false;
        return;
    }
    Serial.printf("ask: [%lums] captured %u bytes\n", millis() - taskStartedAtMs, (unsigned)pendingAskRec.wavSize);

    // Required architecture: record -> prepare the buffer -> submit -> exit
    // immediately. This task does NOT wait for the network job to run or
    // finish - askNetworkJob() (network_worker.h picks it up whenever it's
    // this job's turn) owns everything from here on, including state
    // transitions to DONE/ERROR and freeing pendingAskRec.wavData. Recorded
    // audio and context are written into static storage (see their own
    // comment on why), not this function's own stack, since that stack
    // goes away the moment this function returns, which may be well before
    // the job actually runs if something higher up the queue - well,
    // nothing outranks ASK's own priority 100, but a same-tier job already
    // in flight - is still going.
    buildAskContext(pendingAskContext, sizeof(pendingAskContext));
    pendingAskWantAudio = soundEnabled;
    pendingAskGeneration = askGeneration; // this job belongs to the CURRENT generation - see askGeneration's own comment
    state = ASK_THINKING; // visible immediately - the job may still be queued, not necessarily running yet
    setLoadingSpinnerColor(ASK_TINT);
    setLoadingVisible(true);
    // Submitted at ASK priority (100, highest): if any other job (BUS/
    // CALENDAR/WEATHER/USER_REFRESH) is currently running on the Network
    // Worker, ASK does not cancel or interrupt it but waits in the queue
    // and runs automatically the moment the current job finishes, ahead of
    // anything lower-priority still queued. Never deduplicated (each
    // question is its own real job).
    networkWorkerSubmit(NET_JOB_ASK, "ASK", askNetworkJob, false);
    Serial.printf("ask: [%lums] recording task finished\n", millis() - taskStartedAtMs);
}

static void askTask(void *)
{
    askWork();
    // taskRunning is deliberately NOT cleared here - this task's job is
    // only record+submit+exit. The whole Ask interaction (including the
    // still-queued/running network job and its own speaker playback) is
    // not actually over yet - askNetworkJob() (network_worker.h picks it
    // up later) clears taskRunning itself once the entire flow truly
    // finishes. See askIsBusy()'s own comment.
    askTaskHandle = NULL;
    vTaskDelete(NULL);
}

static void setShortcutsHidden(bool hidden); // question strips, defined further down
static void attemptCreateAskTask(); // defined below startOrStopAsk() - see its own comment for why this exists

// Tap while idle/done/error starts a new question; tap again while
// listening stops the recording early instead of waiting for the 8s cap.
static void startOrStopAsk()
{
    if (taskRunning)
    {
        if (state == ASK_LISTENING)
        {
            // Actually recording - stop early, same as before.
            micRequestStop();
            return;
        }
        // Any other in-progress state (WAITING_FOR_NETWORK, THINKING,
        // SPEAKING) - micRequestStop() does nothing here, since nothing is
        // actually recording yet/any more to stop; it would silently no-op
        // forever, which is exactly the "tapped again, nothing happened"
        // bug reported. A tap here is treated as "cancel this question" -
        // reset to idle immediately, rather than leaving the user stuck
        // with no way out except waiting out a multi-minute backstop
        // timeout. Real bug fixed here: during THINKING/SPEAKING, the
        // network job has already moved off askTask onto the shared
        // Network Worker task, which this file has no handle to and can't
        // forcibly stop - so this used to just reset UI state while the
        // already-queued/running job kept going in the background, later
        // overwriting answerText and playing its answer as if it were the
        // still-current question. Bumping askGeneration makes
        // askNetworkJob() recognize it's now stale and discard its own
        // result instead (see askGeneration's own comment).
        Serial.printf("ask: [%lums] cancel requested (state=%d)\n", millis() - taskStartedAtMs, (int)state);
        askGeneration++;
        if (askTaskHandle)
        {
            // Still relevant if cancel lands during the brief recording/
            // submit window itself - vTaskDelete() on another task doesn't
            // unwind its C++ stack, so anything it was holding (a TLS
            // socket, net_lock) would otherwise leak/stay held forever.
            vTaskDelete(askTaskHandle);
            askTaskHandle = NULL;
            WiFi.disconnect();
            netLockForceRelease();
        }
        taskRunning = false;
        setLoadingVisible(false);
        state = ASK_IDLE;
        return;
    }
    if (WiFi.status() != WL_CONNECTED)
    {
        snprintf(statusError, sizeof(statusError), "Offline - connect to Wi-Fi first");
        state = ASK_ERROR;
        return;
    }
    if (strcmp(cfgOpenAiKey(), "SET_ME_OPENAI_API_KEY") == 0)
    {
        snprintf(statusError, sizeof(statusError), "No OpenAI API key set yet");
        state = ASK_ERROR;
        return;
    }

    taskRunning = true;
    taskStartedAtMs = millis();
    // askWork() sets ASK_WAITING_FOR_NETWORK then ASK_LISTENING itself once
    // recording genuinely starts (it may have to wait behind an in-flight
    // Calendar/Bus fetch first - see its own comment), but that happens on
    // the new task, which hasn't necessarily run its first line yet by the
    // time this function returns - setting an initial state here too so the
    // UI never has a stale ASK_IDLE/ASK_DONE frame visible even for a
    // moment after the tap.
    state = ASK_WAITING_FOR_NETWORK;
    setShortcutsHidden(true);
    attemptCreateAskTask();
}

// One-shot task creation, no retry loop. The Network Worker refactor
// (network_worker.h) means askTask no longer performs any network I/O at
// all - it only records the microphone, submits the ASK job, and exits
// (see askWork()'s own comment). Its stack requirement is therefore small
// and does not depend on Bus/Calendar/Weather's own activity the way the
// old design did, so a creation failure here is a genuine anomaly, not an
// expected transient race to paper over with retries - report it once and
// return to idle, matching every other real failure path in this file.
static void attemptCreateAskTask()
{
    // 4096, measured: uxTaskGetStackHighWaterMark() on a real successful run
    // (recording + buildAskContext()'s full call chain) showed ~2872 bytes
    // peak usage of the previous 8192-byte allocation - 4096 keeps ~1224
    // bytes of headroom over that measured peak, not an arbitrary guess.
    BaseType_t created = xTaskCreatePinnedToCore(askTask, "askOrchestration", 4096, NULL, 1, &askTaskHandle, 0);
    if (created == pdPASS)
        return;
    Serial.printf("ask: xTaskCreatePinnedToCore FAILED (code %d) - free internal heap %u bytes\n",
                  (int)created, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    taskRunning = false;
    snprintf(statusError, sizeof(statusError), "Couldn't start - low memory");
    state = ASK_ERROR;
}

// ---- Shortcut questions (strips under the mic) ----------------------------
// Tapping one skips recording: questions go to the model as text (same
// spoken answer), simple commands run right here on the board.
struct AskShortcut
{
    const char *text;
    const char *localAction; // nullptr = ask the model
    uint32_t color;
};
static const AskShortcut SHORTCUTS[] = {
    // Order packs two per row on the 448px-wide tab (strips wrap to fit).
    // All go to the model; "Turn the screen off." makes it call screen_off
    // and say so before the board sleeps.
    {"Summarise my day.", nullptr, 0x3949AB},
    {"What's my next meeting?", nullptr, 0x7E57C2},
    {"Update me about bus arrivals.", nullptr, 0x43A047},
    {"Will it rain?", nullptr, 0x1E88E5},
    {"How hot is it today?", nullptr, 0xE53935},
    {"UV index now?", nullptr, 0xFB8C00},
    {"Sunset?", nullptr, 0xF4511E},
    {"Is the air quality okay?", nullptr, 0x6D4C41},
    {"Turn the screen off.", "sleep", 0x546E7A},
};
static lv_obj_t *shortcutBox = nullptr;
static lv_obj_t *answerBox = nullptr;
static lv_obj_t *backIcon = nullptr;
static lv_obj_t *forwardIcon = nullptr;
static lv_obj_t *wakeIcon = nullptr; // "Jarvis" wake word on/off

static void wakeIconClicked(lv_event_t *e)
{
    wakeWordSetEnabled(!wakeWordEnabled());
}
// True once there's something to go "forward" to (a recording in progress
// or an answer), so the forward icon can return from the strips to it.
static bool haveAnswerView = false;
// Strips are hidden from the moment a question starts (recording or a
// strip tap) until the back icon is tapped; the answer area shows meanwhile.
static bool shortcutsHidden = false;

static void setShortcutsHidden(bool hidden)
{
    shortcutsHidden = hidden;
    if (!shortcutBox)
        return;
    if (hidden)
    {
        haveAnswerView = true;
        lv_obj_add_flag(shortcutBox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(answerBox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(backIcon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(forwardIcon, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_clear_flag(shortcutBox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(answerBox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(backIcon, LV_OBJ_FLAG_HIDDEN);
        if (haveAnswerView)
            lv_obj_clear_flag(forwardIcon, LV_OBJ_FLAG_HIDDEN); // back to the answer/recording
    }
}

static void backIconClicked(lv_event_t *e)
{
    if (state == ASK_LISTENING)
    {
        recordingCancelled = true; // askWork() discards it instead of sending
        micRequestStop();
    }
    setShortcutsHidden(false);
}

static void forwardIconClicked(lv_event_t *e)
{
    setShortcutsHidden(true);
}

static void askTextQuestion(const char *question, bool sayOnly = false)
{
    if (taskRunning)
        return;
    if (WiFi.status() != WL_CONNECTED)
    {
        snprintf(statusError, sizeof(statusError), "Offline - connect to Wi-Fi first");
        state = ASK_ERROR;
        return;
    }
    if (strcmp(cfgOpenAiKey(), "SET_ME_OPENAI_API_KEY") == 0)
    {
        snprintf(statusError, sizeof(statusError), "No OpenAI API key set yet");
        state = ASK_ERROR;
        return;
    }
    taskRunning = true;
    taskStartedAtMs = millis();
    pendingAskRec = {nullptr, 0};
    strlcpy(pendingAskText, question, sizeof(pendingAskText));
    pendingSayOnly = sayOnly;
    buildAskContext(pendingAskContext, sizeof(pendingAskContext));
    pendingAskWantAudio = soundEnabled;
    pendingAskGeneration = askGeneration;
    state = ASK_THINKING;
    setLoadingSpinnerColor(ASK_TINT);
    setLoadingVisible(true);
    Serial.printf("ask: text question: %s\n", question);
    setShortcutsHidden(true);
    networkWorkerSubmit(NET_JOB_ASK, "ASK", askNetworkJob, false);
}

static void shortcutClicked(lv_event_t *e)
{
    const AskShortcut *sc = (const AskShortcut *)lv_event_get_user_data(e);
    if (sc->localAction && strcmp(sc->localAction, "calendar") == 0)
        uiShowTab(1);
    else if (sc->localAction && strcmp(sc->localAction, "bus") == 0)
        uiShowTab(2);
    else if (sc->localAction && strcmp(sc->localAction, "sleep") == 0)
    {
        // Say it, then sleep once the sentence has played (askTick()).
        askTextQuestion("Turning the screen off.", true);
        if (taskRunning)
            sleepAfterAnswer = true;
        else
            powerSleepNow(); // offline / no key - just sleep
    }
    else
        askTextQuestion(sc->text);
}

// Runs an action the model asked for (UI thread - see actionPending).
static void runPendingAction()
{
    JsonDocument args;
    deserializeJson(args, pendingActionArgs);
    if (strcmp(pendingActionName, "show_screen") == 0)
    {
        const char *screen = args["screen"] | "";
        static const char *tabs[] = {"profile", "calendar", "bus", "ask", "settings"};
        if (strcmp(screen, "weather") == 0)
            weatherShow();
        for (uint32_t i = 0; i < 5; i++)
            if (strcmp(screen, tabs[i]) == 0)
                uiShowTab(i);
    }
    else if (strcmp(pendingActionName, "screen_off") == 0)
        sleepAfterAnswer = true; // once the spoken confirmation is done
    else if (strcmp(pendingActionName, "set_volume") == 0)
        settingsSetVolumePct(args["percent"] | settingsGetVolumePct());
    else if (strcmp(pendingActionName, "set_mute") == 0)
        askSetSoundEnabled(!(args["muted"] | false));
    else if (strcmp(pendingActionName, "set_screen_mode") == 0)
    {
        const char *mode = args["mode"] | "auto";
        nightModeSetSetting(strcmp(mode, "night") == 0 ? NIGHT_MODE_NIGHT : strcmp(mode, "day") == 0 ? NIGHT_MODE_DAY
                                                                                                         : NIGHT_MODE_AUTO);
    }
    else if (strcmp(pendingActionName, "select_bus_stop") == 0)
    {
        busSelectStopByCode(args["code"] | "");
        uiShowTab(2);
    }
}


// True for the WHOLE Ask interaction - from the mic tap (taskRunning set
// true in startOrStopAsk()) until the entire flow truly finishes
// (taskRunning cleared in askNetworkJob(), after STT+answer+TTS+speaker
// playback all complete), NOT just while the temporary recording task
// (askTask) is alive. askTask exits almost immediately after submitting
// the ASK job, but the interaction it started is still very much in
// progress - callers (bus.cpp/today.cpp, avoiding CPU contention with
// real-time I2S audio) need "is Ask busy" to mean that whole span.
bool askIsBusy()
{
    return taskRunning;
}

static void askIconClicked(lv_event_t *e)
{
    startOrStopAsk();
}

void askSetSoundEnabled(bool on)
{
    soundEnabled = on;
    if (soundToggleIcon)
        lv_img_set_src(soundToggleIcon, soundEnabled ? (const void *)&ask_icon_sound_on : (const void *)&ask_icon_sound_off);
}

bool askSoundEnabled()
{
    return soundEnabled;
}

static void soundToggleClicked(lv_event_t *e)
{
    askSetSoundEnabled(!soundEnabled);
}

// Plays back the last answer's PCM (kept in PSRAM - see lastAnswerPcm's
// own comment) with no network call. Runs on its own task (not the LVGL/
// UI thread) since speakerPlayPcm() blocks for the full playback duration
// and acquires net_lock.h's guard, same reasoning as every other I2S/
// network operation in this file.
static lv_obj_t *replayIcon = nullptr;

static void replayTask(void *)
{
    if (!lastAnswerPcm || lastAnswerSampleCount == 0)
    {
        Serial.println("ask: replay - no answer spoken yet this session");
        vTaskDelete(NULL);
        return; // vTaskDelete(NULL) never actually returns, but guard the fall-through explicitly rather than relying on that
    }
    Serial.printf("ask: replaying %u samples at %uHz\n", (unsigned)lastAnswerSampleCount, (unsigned)lastAnswerSampleRate);
    speakerPlayPcm(lastAnswerPcm, lastAnswerSampleCount, lastAnswerSampleRate);
    vTaskDelete(NULL);
}

bool askHasReplay()
{
    return lastAnswerPcm && lastAnswerSampleCount > 0;
}

bool askReplayLast()
{
    if (!askHasReplay() || askIsBusy() || speakerIsBusy())
        return false;
    // Pinned to core 0, same as every other I2S/network task in this file -
    // playback must never run on the LVGL/UI thread (core 1).
    xTaskCreatePinnedToCore(replayTask, "askReplay", 8192, NULL, 1, NULL, 0);
    return true;
}

static void replayIconClicked(lv_event_t *e)
{
    // Real bug fixed here: replaying while a NEW question is in flight let
    // askWithAudio() free() and reassign lastAnswerPcm concurrently while
    // replayTask() was still reading it (use-after-free on the audio
    // buffer). askIsBusy() covers the entire in-flight window (recording
    // through the network job through speaking) on this same variable, so
    // blocking replay during it is the same invariant this flag already
    // protects elsewhere in this file.
    askReplayLast(); // also refuses while already playing (two tasks on one I2S port)
}

// ---- UI --------------------------------------------------------------

static void askRender()
{
    switch (state)
    {
    case ASK_IDLE:
        lv_label_set_text(statusLabel, "Tap to speak or select a question");
        break;
    case ASK_WAITING_FOR_NETWORK:
        lv_label_set_text(statusLabel, "Network busy - please wait...");
        break;
    case ASK_LISTENING:
        // Countdown text itself is kept current by askTick() - see
        // listenStartMs.
        lv_label_set_text_fmt(statusLabel, "Listening %ds - tap to send, back to cancel", MAX_RECORD_SECONDS);
        break;
    case ASK_THINKING:
        lv_label_set_text(statusLabel, "Thinking...");
        break;
    case ASK_SPEAKING:
        // Show the answer text now, not just on ASK_DONE - answerText is
        // already final by the time state reaches here (set in askWork()
        // before the speak step), so there's no reason to keep the screen
        // blank while it's being read aloud. Typed out character-by-
        // character (see askTick()) rather than dumped all at once.
        lv_label_set_text(statusLabel, "Speaking...");
        // Snapshot into a UI-thread-only buffer now, while state==SPEAKING
        // guarantees askWork() is done writing answerText for THIS question
        // (the one documented safe point) - the reveal loop below only ever
        // touches this snapshot afterward, never the shared answerText,
        // so a concurrent write for the NEXT question can't corrupt it.
        strlcpy(revealSnapshot, answerText, sizeof(revealSnapshot));
        {
            // Pace the typing to finish exactly when the real audio does,
            // not a fixed guessed rate - was drifting out of sync (typing
            // finished well before the audio did).
            size_t len = strlen(revealSnapshot);
            uint32_t audioMs = lastAnswerSampleCount > 0 && lastAnswerSampleRate > 0
                                    ? (uint32_t)((uint64_t)lastAnswerSampleCount * 1000 / lastAnswerSampleRate)
                                    : 0;
            revealMsPerChar = (len > 0 && audioMs > 0) ? (uint32_t)(audioMs / len) : 28;
        }
        revealChars = 0;
        revealLastMs = millis();
        revealActive = true;
        lv_label_set_text(answerLabel, "");
        break;
    case ASK_DONE:
        lv_label_set_text(statusLabel, "Tap to speak or select a question");
        // Only (re)start the reveal if it didn't already run during
        // ASK_SPEAKING (sound-off path goes straight to DONE with no
        // speaking step) - don't restart and re-type text that's already
        // fully on screen. Snapshot taken here too, same reasoning as
        // ASK_SPEAKING above - state==DONE is the other documented safe
        // point where answerText for THIS question is guaranteed final.
        if (!revealActive && revealChars == 0)
        {
            strlcpy(revealSnapshot, answerText, sizeof(revealSnapshot));
            revealChars = 0;
            revealLastMs = millis();
            revealActive = true;
            lv_label_set_text(answerLabel, "");
        }
        break;
    case ASK_ERROR:
        lv_label_set_text(statusLabel, statusError);
        break;
    }
}

void askInit(lv_obj_t *tab)
{
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(tab, 16, 0);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(tab, 4, 0);

    // Sound on/off toggle (top-left) and replay-last-answer (top-right),
    // both in their native PNG colors (no img_recolor), matching this
    // tab's existing no-recolor convention for the mic icons. Positioned
    // as absolute overlays (LV_OBJ_FLAG_IGNORE_LAYOUT), not as children
    // participating in the tab's column flex flow - real bug fixed here:
    // an earlier version added these as a normal flex-flow row ABOVE the
    // mic icon, which pushed the mic icon/"Tap to speak or select a question" text down from
    // their original position - explicitly reported and asked to be
    // reverted. This way they float over the top corners without
    // affecting where anything else in the column lands.
    soundToggleIcon = lv_img_create(tab);
    lv_img_set_src(soundToggleIcon, soundEnabled ? (const void *)&ask_icon_sound_on : (const void *)&ask_icon_sound_off);
    lv_obj_add_flag(soundToggleIcon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_event_cb(soundToggleIcon, soundToggleClicked, LV_EVENT_CLICKED, NULL);
    lv_obj_align(soundToggleIcon, LV_ALIGN_TOP_LEFT, 0, 0);

    backIcon = lv_img_create(tab);
    lv_img_set_src(backIcon, &ask_icon_back);
    lv_obj_add_flag(backIcon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(backIcon, backIconClicked, LV_EVENT_CLICKED, NULL);
    lv_obj_align_to(backIcon, soundToggleIcon, LV_ALIGN_OUT_RIGHT_MID, 28, 0);

    forwardIcon = lv_img_create(tab);
    lv_img_set_src(forwardIcon, &ask_icon_forward);
    lv_obj_add_flag(forwardIcon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(forwardIcon, forwardIconClicked, LV_EVENT_CLICKED, NULL);
    lv_obj_align_to(forwardIcon, soundToggleIcon, LV_ALIGN_OUT_RIGHT_MID, 28, 0); // same spot as backIcon - only one is ever shown

    replayIcon = lv_img_create(tab);
    lv_img_set_src(replayIcon, &ask_icon_replay_empty); // askTick() switches it once there's an answer to replay
    lv_obj_add_flag(replayIcon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_event_cb(replayIcon, replayIconClicked, LV_EVENT_CLICKED, NULL);
    lv_obj_align(replayIcon, LV_ALIGN_TOP_RIGHT, 0, 0);

    // Wake word ("Jarvis") on/off switch, left of replay (hidden if the model isn't flashed).
    wakeIcon = lv_img_create(tab);
    lv_img_set_src(wakeIcon, &ask_icon_wake_off);
    lv_obj_add_flag(wakeIcon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_event_cb(wakeIcon, wakeIconClicked, LV_EVENT_CLICKED, NULL);
    lv_obj_align_to(wakeIcon, replayIcon, LV_ALIGN_OUT_LEFT_MID, -28, 0);
    if (!wakeWordAvailable())
        lv_obj_add_flag(wakeIcon, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *micIcon = lv_img_create(tab);
    lv_img_set_src(micIcon, &ask_icon_mic);
    lv_obj_add_flag(micIcon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(micIcon, askIconClicked, LV_EVENT_CLICKED, NULL);

    // Single label doing double duty: "Tap to speak or select a question" at idle, and every
    // in-progress/error status ("Listening...", "Thinking...", etc.) in
    // its place - not two separate lines, per explicit request.
    statusLabel = lv_label_create(tab);
    lv_label_set_text(statusLabel, "Tap to speak or select a question");
    lv_obj_set_style_text_color(statusLabel, lv_color_hex(ASK_TINT), 0);
    lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_top(statusLabel, 2, 0);
    lv_obj_set_style_pad_bottom(statusLabel, 2, 0);
    lv_obj_add_flag(statusLabel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(statusLabel, askIconClicked, LV_EVENT_CLICKED, NULL);

    answerBox = lv_obj_create(tab);

    lv_obj_remove_style_all(answerBox);
    lv_obj_set_size(answerBox, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(answerBox, 1);
    lv_obj_add_flag(answerBox, LV_OBJ_FLAG_HIDDEN); // shown instead of the strips once a question starts
    lv_obj_set_style_pad_top(answerBox, 6, 0); // gap below the status line
    lv_obj_set_style_pad_left(answerBox, 8, 0);
    lv_obj_set_style_pad_right(answerBox, 8, 0);
    lv_obj_set_style_pad_bottom(answerBox, 8, 0);
    // remove_style_all() also wipes the scrollbar part's default styling -
    // same fix as bus.cpp's tableList/historyList and settings.cpp's
    // rightList (restyle it explicitly rather than relying on theme
    // defaults).
    lv_obj_set_scrollbar_mode(answerBox, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(answerBox, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(answerBox, lv_color_hex(0x9e9e9e), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(answerBox, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(answerBox, 2, LV_PART_SCROLLBAR);

    answerLabel = lv_label_create(answerBox);
    lv_label_set_long_mode(answerLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(answerLabel, LV_PCT(100));
    // AUTO (not CENTER) - a centered block re-centers itself on every
    // length change as text is revealed, so already-read words visibly
    // shift position each tick - reported as hard to follow. LVGL 8.3.6's
    // lv_label has no true justified (stretched-spacing) mode; AUTO picks
    // LEFT/RIGHT based on the text's base direction, which keeps already-
    // printed characters in place as more are revealed.
    lv_obj_set_style_text_align(answerLabel, LV_TEXT_ALIGN_AUTO, 0);
    lv_label_set_text(answerLabel, "");

    shortcutBox = lv_obj_create(tab);
    lv_obj_remove_style_all(shortcutBox);
    // Always visible, under the answer area.
    lv_obj_set_size(shortcutBox, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(shortcutBox, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(shortcutBox, 4, 0);
    lv_obj_set_style_pad_column(shortcutBox, 4, 0);
    lv_obj_set_style_pad_top(shortcutBox, 6, 0); // gap below the status line
    lv_obj_clear_flag(shortcutBox, LV_OBJ_FLAG_SCROLLABLE);
    for (const AskShortcut &sc : SHORTCUTS)
    {
        lv_obj_t *strip = lv_obj_create(shortcutBox);
        lv_obj_remove_style_all(strip);
        lv_obj_set_size(strip, LV_SIZE_CONTENT, 34); // ends where the text ends, square corners
        lv_obj_set_style_bg_color(strip, lv_color_mix(lv_color_white(), lv_color_hex(sc.color), LV_OPA_40), 0); // 40% lighter
        lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(strip, 10, 0);
        lv_obj_add_flag(strip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(strip, shortcutClicked, LV_EVENT_CLICKED, (void *)&sc);
        lv_obj_t *label = lv_label_create(strip);
        lv_label_set_text(label, sc.text);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    }

}

void askTick()
{

    wakeWordTick();
    // "Jarvis" heard: bring up the Ask tab and start recording.
    if (wakeWordTakeDetection() && !taskRunning)
    {
        powerNoteTouch();
        weatherNoteTouch();
        weatherHide();
        uiShowTab(3);
        startOrStopAsk();
    }
    // On/off icon, kept in step with the web dashboard's toggle too.
    static int shownWake = -1;
    if (wakeIcon && (int)wakeWordEnabled() != shownWake)
    {
        shownWake = wakeWordEnabled();
        lv_img_set_src(wakeIcon, shownWake ? &ask_icon_wake_on : &ask_icon_wake_off);
        // Off icon is solid black - tint it mid grey so it reads in day and night mode.
        lv_obj_set_style_img_recolor(wakeIcon, lv_color_hex(0x9E9E9E), 0);
        lv_obj_set_style_img_recolor_opa(wakeIcon, shownWake ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    }

    if (actionPending)
    {
        actionPending = false;
        runPendingAction();
    }
    if (sleepAfterAnswer && !taskRunning)
    {
        sleepAfterAnswer = false;
        powerSleepNow();
    }

    // Replay icon: empty version until there's an answer to replay.
    static bool replayIconFull = false;
    if (replayIcon && askHasReplay() != replayIconFull)
    {
        replayIconFull = !replayIconFull;
        lv_img_set_src(replayIcon, replayIconFull ? &ask_icon_replay : &ask_icon_replay_empty);
    }

    static AskState lastRenderedState = (AskState)-1; // force initial render
    // Recording auto-stops after MAX_RECORD_SECONDS, which the user can't
    // otherwise see - count it down in the status line while listening.
    static uint32_t listenStartMs = 0;
    static int shownSecondsLeft = -1;
    if (state != lastRenderedState)
    {
        lastRenderedState = state;
        if (state == ASK_LISTENING)
        {
            // Starting a new question - stop any leftover reveal from the
            // previous answer. Real bug fixed here: revealChars was left
            // at the previous answer's full length, so ASK_DONE's own
            // "only (re)start if revealChars==0" guard silently blocked
            // every sound-off answer after the first one from ever being
            // typed out - resetting both together, not just revealActive.
            revealActive = false;
            revealChars = 0;
            listenStartMs = millis();
            shownSecondsLeft = MAX_RECORD_SECONDS;
        }
        askRender();
    }

    if (state == ASK_LISTENING)
    {
        int left = MAX_RECORD_SECONDS - (int)((millis() - listenStartMs) / 1000);
        if (left < 0)
            left = 0;
        if (left != shownSecondsLeft)
        {
            shownSecondsLeft = left;
            lv_label_set_text_fmt(statusLabel, "Listening %ds - tap to send, back to cancel", left);
        }
    }

    if (revealActive)
    {
        size_t total = strlen(revealSnapshot); // snapshot, not the shared answerText[] - see its own comment
        uint32_t now = millis();
        // Catch up by however many chars the elapsed time covers (not just
        // one per tick) so a slow loop() iteration doesn't fall behind pace.
        while (revealChars < total && now - revealLastMs >= revealMsPerChar)
        {
            revealChars++;
            revealLastMs += revealMsPerChar;
        }
        if (revealChars >= total)
        {
            revealChars = total;
            revealActive = false;
        }
        char partial[sizeof(revealSnapshot)];
        memcpy(partial, revealSnapshot, revealChars);
        partial[revealChars] = '\0';
        lv_label_set_text(answerLabel, partial);
    }

    // Backstop watchdog. askTask itself (the recording/submit task) only
    // lives for a few seconds now - the real long-running work after that
    // is a job on the shared Network Worker task (network_worker.h), which
    // this file has no handle to and cannot vTaskDelete() directly. Real
    // bug fixed here: this watchdog used to gate its whole recovery
    // (WiFi.disconnect()/netLockForceRelease()) on askTaskHandle being
    // non-NULL, which is only true for that brief initial window - for
    // nearly this entire 240s budget askTaskHandle is already NULL, so a
    // genuinely wedged Network Worker job used to leave the recovery
    // completely skipped (no socket reclaim, no lock release) while still
    // clearing taskRunning, letting a new question start concurrently with
    // the stuck one. WiFi.disconnect()/netLockForceRelease() are safe to
    // call unconditionally (both already used to being called when nothing
    // is actually stuck), so recovery now always runs.
    if (taskRunning && millis() - taskStartedAtMs > 240000)
    {
        Serial.println("ask: task watchdog fired - force-recovering");
        if (askTaskHandle)
        {
            // Still relevant in the rare case the watchdog fires during the
            // brief recording/submit window itself, not just the network
            // job phase - vTaskDelete() on another task doesn't unwind its
            // C++ stack, so anything it was holding (a TLS socket, this
            // lock) would otherwise leak/stay held forever.
            vTaskDelete(askTaskHandle);
            askTaskHandle = NULL;
        }
        // Reclaims any leaked TLS socket from a killed task or a wedged
        // Network Worker job by tearing down the whole netif; main.cpp's
        // wifiTask already retries every 5s once disconnected.
        WiFi.disconnect();
        // Force the lock back open in case whatever's stuck was holding it -
        // otherwise every other tab's network calls would block forever too.
        netLockForceRelease();
        taskRunning = false;
        setLoadingVisible(false); // don't leave the spinner stuck on if the task got force-killed mid-fetch
        snprintf(statusError, sizeof(statusError), "Timed out");
        state = ASK_ERROR;
    }
}
