// Keeps winsock.h out of windows.h: the adapter list (iphlpapi.h) needs winsock2.h
#define WIN32_LEAN_AND_MEAN
#include "utils.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#include <SetupApi.h>
// The adapter list for the Wake-on-LAN check (iptypes.h declares IP_ADAPTER_ADDRESSES only after winsock2.h)
#include <winsock2.h>
#include <iphlpapi.h>
#include <powrprof.h>

#include "src/utility.h"
#include "src/logging.h"

std::wstring acpToUtf16(const std::string& origStr) {
	auto acp = GetACP();

	int utf16Len = MultiByteToWideChar(acp, 0, origStr.c_str(), origStr.size(), NULL, 0);
	if (utf16Len == 0) {
		return L"";
	}

	std::wstring utf16Str(utf16Len, L'\0');
	MultiByteToWideChar(acp, 0, origStr.c_str(), origStr.size(), &utf16Str[0], utf16Len);

	return utf16Str;
}

std::string utf16ToAcp(const std::wstring& utf16Str) {
	auto acp = GetACP();

	int codepageLen = WideCharToMultiByte(acp, 0, utf16Str.c_str(), utf16Str.size(), NULL, 0, NULL, NULL);
	if (codepageLen == 0) {
		return "";
	}

	std::string codepageStr(codepageLen, '\0');
	WideCharToMultiByte(acp, 0, utf16Str.c_str(), utf16Str.size(), &codepageStr[0], codepageLen, NULL, NULL);

	return codepageStr;
}

std::string utf8ToAcp(const std::string& utf8Str) {
	if (GetACP() == CP_UTF8) {
		return std::string(utf8Str);
	}

	int utf16Len = MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), utf8Str.size(), NULL, 0);
	if (utf16Len == 0) {
		return std::string(utf8Str);
	}

	std::wstring utf16Str(utf16Len, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), utf8Str.size(), &utf16Str[0], utf16Len);

	return utf16ToAcp(utf16Str);
}

std::string acpToUtf8(const std::string& origStr) {
	if (GetACP() == CP_UTF8) {
		return std::string(origStr);
	}

	auto utf16Str = acpToUtf16(origStr);

	int utf8Len = WideCharToMultiByte(CP_UTF8, 0, utf16Str.c_str(), utf16Str.size(), NULL, 0, NULL, NULL);
	if (utf8Len == 0) {
		return std::string(origStr);
	}

	std::string utf8Str(utf8Len, '\0');
	WideCharToMultiByte(CP_UTF8, 0, utf16Str.c_str(), utf16Str.size(), &utf8Str[0], utf8Len, NULL, NULL);

	return utf8Str;
}

std::string currentCodePageToCharset() {
    // Map of Windows code pages to their corresponding charset strings
    static const std::unordered_map<int, std::string> codePageCharsetMap = {
        {65001, "UTF-8"},
        {1200, "UTF-16LE"},
        {1201, "UTF-16BE"},
        {1250, "windows-1250"},
        {1251, "windows-1251"},
        {1252, "windows-1252"},
        {1253, "windows-1253"},
        {1254, "windows-1254"},
        {1255, "windows-1255"},
        {1256, "windows-1256"},
        {1257, "windows-1257"},
        {1258, "windows-1258"},
        {936,  "GBK"},       // Simplified Chinese
        {949,  "EUC-KR"},    // Korean
        {950,  "Big5"},      // Traditional Chinese
        {932,  "Shift_JIS"}  // Japanese
    };

    static std::string charsetStr;

    if (charsetStr.empty()) {
        // Get the active Windows code page
        int codePage = GetACP();
        // Find the charset in the map
        auto it = codePageCharsetMap.find(codePage);
        if (it != codePageCharsetMap.end()) {
            charsetStr = it->second;
        } else {
            // Fallback charset if code page is not found in the map
            charsetStr = "ISO-8859-1";
        }
    }

    return charsetStr;
}


bool is_korean_ui_language() {
	// shell.exe runs as SYSTEM, so the user's display language is not available here; the system
	// display language is. The tray uses ANSI APIs, so Korean also needs the Korean code page.
	static const bool korean = PRIMARYLANGID(GetSystemDefaultUILanguage()) == LANG_KOREAN && GetACP() == 949;
	return korean;
}

power_modes_t query_power_modes() {
	power_modes_t modes;
	SYSTEM_POWER_CAPABILITIES caps = {};
	if (!GetPwrCapabilities(&caps)) {
		BOOST_LOG(warning) << "GetPwrCapabilities failed: " << GetLastError();
		return modes;
	}
	// SetSuspendState(FALSE) enters the deepest of S1-S3 the PC has. A modern standby PC (S0 low-power
	// idle, "AoAc") reports no S3 and sleeps through the same call.
	POWER_PLATFORM_INFORMATION platform = {};
	const bool modern_standby = CallNtPowerInformation(PlatformInformation, nullptr, 0, &platform, sizeof(platform)) == 0 && platform.AoAc;  // 0: STATUS_SUCCESS
	modes.sleep = caps.SystemS1 || caps.SystemS2 || caps.SystemS3 || modern_standby;
	modes.hibernate_file = caps.HiberFilePresent;
	modes.hibernate = caps.SystemS4 && caps.HiberFilePresent;
	return modes;
}

namespace {
	std::wstring lowercase(std::wstring text) {
		std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) {
			return static_cast<wchar_t>(std::towlower(c));
		});
		return text;
	}

	// Reads a REG_SZ or REG_DWORD value of an open key as text; empty when absent.
	std::wstring read_registry_text(HKEY key, const wchar_t *name) {
		wchar_t buffer[256];
		DWORD size = sizeof(buffer);
		DWORD type = 0;
		if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(buffer), &size) != ERROR_SUCCESS) {
			return {};
		}
		if (type == REG_DWORD && size >= sizeof(DWORD)) {
			DWORD value;
			memcpy(&value, buffer, sizeof(value));
			return std::to_wstring(value);
		}
		if (type == REG_SZ || type == REG_EXPAND_SZ) {
			buffer[std::min<DWORD>(size / sizeof(wchar_t), 255)] = L'\0';
			return buffer;
		}
		return {};
	}

	// Network adapters' driver settings, by NetCfgInstanceId (the GUID GetAdaptersAddresses names
	// an adapter by): the standardized NDIS keyword *WakeOnMagicPacket, "1" when the adapter wakes the
	// PC on a magic packet. Adapters without the keyword (virtual ones, drivers that do not offer it)
	// are left out.
	std::unordered_map<std::wstring, bool> wake_on_magic_packet_settings() {
		std::unordered_map<std::wstring, bool> settings;
		HKEY class_key = nullptr;
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
		                  L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}",
		                  0, KEY_READ, &class_key) != ERROR_SUCCESS) {
			return settings;
		}
		for (DWORD i = 0;; ++i) {
			wchar_t sub_name[64];
			DWORD sub_size = 64;
			if (RegEnumKeyExW(class_key, i, sub_name, &sub_size, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
				break;
			}
			HKEY adapter_key = nullptr;
			if (RegOpenKeyExW(class_key, sub_name, 0, KEY_READ, &adapter_key) != ERROR_SUCCESS) {
				continue;
			}
			const auto instance = read_registry_text(adapter_key, L"NetCfgInstanceId");
			const auto wake = read_registry_text(adapter_key, L"*WakeOnMagicPacket");
			RegCloseKey(adapter_key);
			if (!instance.empty() && !wake.empty()) {
				settings[lowercase(instance)] = wake == L"1";
			}
		}
		RegCloseKey(class_key);
		return settings;
	}

	// Friendly names of the devices Windows lets wake the PC ("Allow this device to wake the
	// computer", what `powercfg /devicequery wake_armed` lists). known is false when the list could
	// not be read.
	std::set<std::wstring> wake_armed_devices(bool &known) {
		// DevicePowerOpen/Close are process-wide: one query at a time
		static std::mutex mutex;
		std::lock_guard lock(mutex);
		std::set<std::wstring> devices;
		known = false;
		if (!DevicePowerOpen(0)) {
			return devices;
		}
		known = true;
		for (ULONG i = 0;; ++i) {
			wchar_t name[512];
			ULONG size = sizeof(name);
			if (!DevicePowerEnumDevices(i, 0, DEVICEPOWER_FILTER_DEVICES_PRESENT | DEVICEPOWER_FILTER_WAKEENABLED,
			                            reinterpret_cast<PBYTE>(name), &size)) {
				break;
			}
			name[511] = L'\0';
			devices.insert(lowercase(name));
		}
		DevicePowerClose();
		return devices;
	}
}  // namespace

std::string wake_on_lan_state() {
	// Connected adapters, with the names used to look them up
	std::vector<BYTE> buffer(16 * 1024);
	ULONG size = static_cast<ULONG>(buffer.size());
	const ULONG flags = GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	ULONG result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &size);
	if (result == ERROR_BUFFER_OVERFLOW) {
		buffer.resize(size);
		result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &size);
	}
	if (result != NO_ERROR) {
		BOOST_LOG(warning) << "Wake-on-LAN check: GetAdaptersAddresses failed: " << result;
		return "unknown";
	}

	const auto settings = wake_on_magic_packet_settings();
	bool armed_known = false;
	const auto armed = wake_armed_devices(armed_known);

	bool any_off = false;
	bool any_unconfirmed = false;
	for (auto adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()); adapter; adapter = adapter->Next) {
		// Wired Ethernet with a link: a sleeping PC is woken through this one
		if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD || adapter->OperStatus != IfOperStatusUp || adapter->PhysicalAddressLength == 0) {
			continue;
		}
		const auto setting = settings.find(lowercase(acpToUtf16(adapter->AdapterName)));
		if (setting == settings.end()) {
			continue;  // no Wake on Magic Packet setting: a virtual adapter, or a driver without it
		}
		if (!setting->second) {
			any_off = true;
			continue;
		}
		// The setting is on; Windows must also let the device wake the PC
		if (armed_known && adapter->Description && armed.count(lowercase(adapter->Description))) {
			return "yes";
		}
		any_unconfirmed = true;
	}
	if (any_unconfirmed) {
		return "unknown";
	}
	return any_off ? "no" : "unknown";
}

bool enable_shutdown_privilege() {
	HANDLE token = nullptr;
	LUID luid;
	bool enabled = false;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token) &&
	    LookupPrivilegeValueA(nullptr, SE_SHUTDOWN_NAME, &luid)) {
		TOKEN_PRIVILEGES tp = {};
		tp.PrivilegeCount = 1;
		tp.Privileges[0].Luid = luid;
		tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
		// AdjustTokenPrivileges succeeds even when it enabled nothing: GetLastError tells
		enabled = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
	}
	if (token) {
		CloseHandle(token);
	}
	return enabled;
}

void suspend_pc_later(bool hibernate, DWORD delay_ms) {
	std::thread([hibernate, delay_ms]() {
		Sleep(delay_ms);
		if (!enable_shutdown_privilege()) {
			BOOST_LOG(warning) << "The shutdown privilege could not be enabled; trying to " << (hibernate ? "hibernate" : "sleep") << " anyway";
		}
		// Returns when the PC is running again (after hibernation: once it has been turned on again)
		if (!SetSuspendState(hibernate ? TRUE : FALSE, FALSE, FALSE)) {
			BOOST_LOG(error) << (hibernate ? "Hibernate" : "Sleep") << " failed: " << GetLastError();
			return;
		}
		BOOST_LOG(info) << "The PC is awake again after " << (hibernate ? "hibernation" : "sleep");
	}).detach();
}

// Modified from https://github.com/FrogTheFrog/Sunshine/blob/b6f8573d35eff7c55da6965dfa317dc9722bd4ef/src/platform/windows/display_device/windows_utils.cpp

std::string
get_error_string(LONG error_code) {
	std::stringstream error;
	error << "[code: ";
	switch (error_code) {
		case ERROR_INVALID_PARAMETER:
			error << "ERROR_INVALID_PARAMETER";
			break;
		case ERROR_NOT_SUPPORTED:
			error << "ERROR_NOT_SUPPORTED";
			break;
		case ERROR_ACCESS_DENIED:
			error << "ERROR_ACCESS_DENIED";
			break;
		case ERROR_INSUFFICIENT_BUFFER:
			error << "ERROR_INSUFFICIENT_BUFFER";
			break;
		case ERROR_GEN_FAILURE:
			error << "ERROR_GEN_FAILURE";
			break;
		case ERROR_SUCCESS:
			error << "ERROR_SUCCESS";
			break;
		default:
			error << error_code;
			break;
	}
	error << ", message: " << std::system_category().message(static_cast<int>(error_code)) << "]";
	return error.str();
}

bool
query_display_config(std::vector<DISPLAYCONFIG_PATH_INFO>& paths, std::vector<DISPLAYCONFIG_MODE_INFO>& modes, bool active_only) {
	LONG result = ERROR_SUCCESS;

	// When we want to enable/disable displays, we need to get all paths as they will not be active.
	// This will require some additional filtering of duplicate and otherwise useless paths.
	UINT32 flags = active_only ? QDC_ONLY_ACTIVE_PATHS : QDC_ALL_PATHS;
	flags |= QDC_VIRTUAL_MODE_AWARE;  // supported from W10 onwards

	do {
		UINT32 path_count { 0 };
		UINT32 mode_count { 0 };

		result = GetDisplayConfigBufferSizes(flags, &path_count, &mode_count);
		if (result != ERROR_SUCCESS) {
			BOOST_LOG(error) << get_error_string(result) << " failed to get display paths and modes!";
			return false;
		}

		paths.resize(path_count);
		modes.resize(mode_count);
		result = QueryDisplayConfig(flags, &path_count, paths.data(), &mode_count, modes.data(), nullptr);

		// The function may have returned fewer paths/modes than estimated
		paths.resize(path_count);
		modes.resize(mode_count);

		// It's possible that between the call to GetDisplayConfigBufferSizes and QueryDisplayConfig
		// that the display state changed, so loop on the case of ERROR_INSUFFICIENT_BUFFER.
	} while (result == ERROR_INSUFFICIENT_BUFFER);

	if (result != ERROR_SUCCESS) {
		BOOST_LOG(error) << get_error_string(result) << " failed to query display paths and modes!";
		return false;
	}

	return true;
}

bool
is_user_session_locked() {
	LPWSTR buffer { nullptr };
	const auto cleanup_guard {
		util::fail_guard([&buffer]() {
			if (buffer) {
				WTSFreeMemory(buffer);
			}
		})
	};

	DWORD buffer_size_in_bytes { 0 };
	if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTSGetActiveConsoleSessionId(), WTSSessionInfoEx, &buffer, &buffer_size_in_bytes)) {
		if (buffer_size_in_bytes > 0) {
			const auto wts_info { reinterpret_cast<const WTSINFOEXW *>(buffer) };
			if (wts_info && wts_info->Level == 1) {
				const bool is_locked { wts_info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK };
				BOOST_LOG(debug) << "is_user_session_locked: " << is_locked;
				return is_locked;
			}
		}

		BOOST_LOG(warning) << "Failed to get session info in is_user_session_locked.";
	}
	else {
		BOOST_LOG(error) << get_error_string(GetLastError()) << " failed while calling WTSQuerySessionInformationW!";
	}

	return false;
}

bool
test_no_access_to_ccd_api() {
	std::vector<DISPLAYCONFIG_PATH_INFO> paths;
	std::vector<DISPLAYCONFIG_MODE_INFO> modes;
	if (!query_display_config(paths, modes, true)) {
		BOOST_LOG(debug) << "test_no_access_to_ccd_api failed in query_display_config.";
		return true;
	}

	// Here we are supplying the retrieved display data back to SetDisplayConfig (with VALIDATE flag only, so that we make no actual changes).
	// Unless something is really broken on Windows, this call should never fail under normal circumstances - the configuration is 100% correct, since it was
	// provided by Windows.
	const UINT32 flags { SDC_VALIDATE | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VIRTUAL_MODE_AWARE };
	const LONG result { SetDisplayConfig(paths.size(), paths.data(), modes.size(), modes.data(), flags) };

	BOOST_LOG(debug) << "test_no_access_to_ccd_api result: " << get_error_string(result);
	return result == ERROR_ACCESS_DENIED;
}

bool
is_changing_settings_going_to_fail() {
	return is_user_session_locked() || test_no_access_to_ccd_api();
}
