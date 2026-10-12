#pragma once

#include <string>
#include <vector>

#include <windows.h>
#include <wtsapi32.h>

std::wstring acpToUtf16(const std::string& origStr);
std::string utf16ToAcp(const std::wstring& utf16Str);
std::string utf8ToAcp(const std::string& utf8Str);
std::string acpToUtf8(const std::string& currentStr);
std::string currentCodePageToCharset();

// True when Windows' system display language is Korean (and the ANSI code page is 949, which the
// tray's ANSI APIs need); host-side messages (tray, shutdown notice) are shown in Korean then and
// in English otherwise.
bool is_korean_ui_language();

// Shell: remote sleep and hibernation (nvhttp's /actions/power). Read-only checks of what this PC
// can do, so the client only offers what works and can warn when the PC may not come back.
struct power_modes_t {
  bool sleep = false;  // S1-S3, or modern standby (S0 low-power idle, no S3): SetSuspendState(FALSE) works
  bool hibernate = false;  // S4 and the hibernation file is present
  bool hibernate_file = false;  // hiberfil.sys exists (S4 without it: hibernation is turned off)
};
power_modes_t query_power_modes();

// Whether a connected wired Ethernet adapter wakes this PC on a magic packet: "yes" (the adapter's
// Wake on Magic Packet setting is on and Windows lets the device wake the PC), "no" (the setting is
// off on every such adapter) or "unknown" (nothing could be read: no wired adapter with the setting,
// or the wake permission could not be confirmed).
std::string wake_on_lan_state();

// Gives this process the shutdown privilege (SE_SHUTDOWN_NAME), needed to shut down, restart,
// sleep or hibernate the PC. False if it could not be enabled.
bool enable_shutdown_privilege();

// Puts the PC to sleep (hibernate = false) or hibernates it (SetSuspendState) after delay_ms, from
// its own thread, so the HTTP answer reaches the client first. The outcome is logged.
void suspend_pc_later(bool hibernate, DWORD delay_ms);

std::string get_error_string(LONG error_code);

bool query_display_config(std::vector<DISPLAYCONFIG_PATH_INFO>& paths, std::vector<DISPLAYCONFIG_MODE_INFO>& modes, bool active_only);

bool is_user_session_locked();

bool test_no_access_to_ccd_api();

bool is_changing_settings_going_to_fail();
