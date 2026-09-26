#pragma once

// API keys / URLs that can be changed from the web settings page
// (web_config.cpp) without reflashing. Each getter returns the value saved
// in NVS if one was set, otherwise the compile-time default from secrets.h.
// Values only change at boot - the web page restarts the board after saving
// any of these, so the network worker never sees a string mid-rewrite.

void appConfigLoad(); // call once from setup(), before anything fetches

const char *cfgLtaKey();
const char *cfgOpenAiKey();
const char *cfgIcsUrl();
const char *cfgGithubToken();
const char *cfgWebPassword();

// Field ids for appConfigSave(): "lta", "openai", "ics", "gh", "webpw".
// An empty value clears the override (falls back to secrets.h).
void appConfigSave(const char *field, const char *value);
bool appConfigIsOverridden(const char *field);
