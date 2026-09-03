// mei_key.h — local HWID-bound license gate (weekly / monthly / lifetime).
#pragma once
bool        mei_key_check();        // (re)read key.dat + validate against this device; returns licensed
bool        mei_key_licensed();     // cached result of the last check
int         mei_key_tier();         // 0=weekly, 1=monthly, 2=lifetime, -1=none
long        mei_key_days_left();    // days remaining (99999 = lifetime, -1 = none)
const char* mei_key_hwid();         // this device's hardware id (8 hex)
const char* mei_key_str();          // the active key, or ""
bool        mei_key_activate(const char* key);   // bind a key to THIS device; returns success
