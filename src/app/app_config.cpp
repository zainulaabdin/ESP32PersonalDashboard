#include "app_config.h"
#include "secrets_select.h"
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#define APP_CONFIG_NVS_NAMESPACE "appcfg"

struct ConfigField
{
    const char *key;      // NVS key, also the web form field name
    const char *fallback; // secrets.h default
    char *value;          // NVS override, "" if none
    size_t size;
};

static char ltaKey[80] = "";
static char openAiKey[256] = "";
static char icsUrl[512] = "";
static char githubToken[160] = "";
static char webPassword[64] = "";

static ConfigField fields[] = {
    {"lta", LTA_API_KEY, ltaKey, sizeof(ltaKey)},
    {"openai", OPENAI_API_KEY, openAiKey, sizeof(openAiKey)},
    {"ics", OUTLOOK_ICS_URL, icsUrl, sizeof(icsUrl)},
    {"gh", GITHUB_OTA_TOKEN, githubToken, sizeof(githubToken)},
    {"webpw", WEB_CONFIG_PASSWORD, webPassword, sizeof(webPassword)},
};

// Template placeholders (secrets.h.example) - never worth saving.
static bool isRealValue(const char *v)
{
    return v[0] && strncmp(v, "YOUR_", 5) != 0 && strncmp(v, "SET_ME", 6) != 0 && strcmp(v, "changeme") != 0;
}

static ConfigField *findField(const char *key)
{
    for (auto &f : fields)
        if (strcmp(f.key, key) == 0)
            return &f;
    return nullptr;
}

static const char *get(const char *key)
{
    ConfigField *f = findField(key);
    return f->value[0] ? f->value : f->fallback;
}

void appConfigLoad()
{
    Preferences prefs;
    prefs.begin(APP_CONFIG_NVS_NAMESPACE, false);
    for (auto &f : fields)
    {
        prefs.getString(f.key, f.value, f.size);
        // A local build with a filled secrets.h saves its values once, so
        // later key-free release builds (OTA) keep working on this board.
        if (!f.value[0] && isRealValue(f.fallback))
        {
            prefs.putString(f.key, f.fallback);
            strlcpy(f.value, f.fallback, f.size);
        }
    }
    prefs.end();
}

const char *cfgLtaKey() { return get("lta"); }
const char *cfgOpenAiKey() { return get("openai"); }
const char *cfgIcsUrl() { return get("ics"); }
const char *cfgGithubToken() { return get("gh"); }
const char *cfgWebPassword() { return get("webpw"); }

void appConfigSave(const char *field, const char *value)
{
    ConfigField *f = findField(field);
    if (!f)
        return;
    Preferences prefs;
    prefs.begin(APP_CONFIG_NVS_NAMESPACE, false);
    if (value[0])
        prefs.putString(f->key, value);
    else
        prefs.remove(f->key);
    prefs.end();
}

bool appConfigIsOverridden(const char *field)
{
    ConfigField *f = findField(field);
    return f && f->value[0];
}
