/**
 * @file src/system_tray.cpp
 * @brief Definitions for the system tray icon and notification system.
 */
// macros
#if defined SHELL_TRAY && SHELL_TRAY >= 1

  #if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <accctrl.h>
    #include <aclapi.h>
    #include "platform/windows/utils.h"
    #define TRAY_ICON WEB_DIR "images/shell.ico"
    #define TRAY_ICON_PLAYING WEB_DIR "images/shell-playing.ico"
    #define TRAY_ICON_PAUSING WEB_DIR "images/shell-pausing.ico"
    #define TRAY_ICON_LOCKED WEB_DIR "images/shell-locked.ico"
  #elif defined(__linux__) || defined(linux) || defined(__linux)
    #define TRAY_ICON SHELL_TRAY_PREFIX "-tray"
    #define TRAY_ICON_PLAYING SHELL_TRAY_PREFIX "-playing"
    #define TRAY_ICON_PAUSING SHELL_TRAY_PREFIX "-pausing"
    #define TRAY_ICON_LOCKED SHELL_TRAY_PREFIX "-locked"
  #elif defined(__APPLE__) || defined(__MACH__)
    #define TRAY_ICON WEB_DIR "images/logo-shell-16.png"
    #define TRAY_ICON_PLAYING WEB_DIR "images/shell-playing-16.png"
    #define TRAY_ICON_PAUSING WEB_DIR "images/shell-pausing-16.png"
    #define TRAY_ICON_LOCKED WEB_DIR "images/shell-locked-16.png"
    #include <dispatch/dispatch.h>
  #endif

  // Shell: the tray is in English, or in Korean when Windows' system display language is Korean.
  #define TRAY_TOOLTIP "Shell"
  #define TRAY_MSG_NO_APP_RUNNING localized("Reload Apps", "앱 목록 다시 불러오기")

  #ifndef BOOST_PROCESS_VERSION
    #define BOOST_PROCESS_VERSION 1
  #endif

  // standard includes
  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <csignal>
  #include <map>
  #include <mutex>
  #include <format>
  #include <string>
  #include <thread>
  #include <vector>

  // lib includes
  #include <boost/filesystem.hpp>
  #include <boost/process/v1/environment.hpp>
  #include <tray/src/tray.h>

  // local includes
  #include "config.h"
  #include "confighttp.h"
  #include "display_device.h"
  #include "logging.h"
  #include "platform/common.h"
  #include "process.h"
  #include "network.h"
  #include "src/entry_handler.h"

using namespace std::literals;

// system_tray namespace
namespace system_tray {
  // Tray strings are UTF-8 in the source, but the Windows tray uses ANSI APIs, so they are
  // converted to the system code page once and kept for the lifetime of the process.
  static const char *tray_text(const char *utf8) {
  #ifdef _WIN32
    static std::mutex lock;
    static std::map<std::string, std::string> converted;
    std::lock_guard<std::mutex> guard(lock);
    auto it = converted.find(utf8);
    if (it == converted.end()) {
      it = converted.emplace(utf8, utf8ToAcp(utf8)).first;
    }
    return it->second.c_str();
  #else
    return utf8;
  #endif
  }

  // Korean when Windows' system display language is Korean, English otherwise (see is_korean_ui_language()).
  static bool korean_ui() {
  #ifdef _WIN32
    return is_korean_ui_language();
  #else
    return false;
  #endif
  }

  // A tray string in the display language, ready for the tray API.
  static const char *localized(const char *english, const char *korean) {
    return tray_text(korean_ui() ? korean : english);
  }

  static std::atomic tray_initialized = false;

  // Threading variables for all platforms
  static std::thread tray_thread;
  static std::atomic tray_thread_running = false;
  static std::atomic tray_thread_should_exit = false;

  void tray_open_ui_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Opening UI from system tray"sv;
    launch_ui();
  }

  void
  tray_force_stop_cb(struct tray_menu *item) {
    BOOST_LOG(info) << "Force stop from system tray"sv;
    proc::proc.terminate();
  }

  void tray_reset_display_device_config_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Resetting display device config from system tray"sv;

    std::ignore = display_device::reset_persistence();
  }

  // A plain notification (defined below the menu)
  static void notify(const char *title, const char *text);

  // Shell: notices asked by threads that must not wait for the tray thread (see queue_require_pin()),
  // shown by process_tray_events(). Each kind is pending once at most, so a burst of pairing
  // requests makes one notice; they are shown in the order they were first asked.
  enum class pending_notice_e {
    require_pin,
    paired,
  };

  static std::mutex pending_notices_lock;
  static std::vector<pending_notice_e> pending_notices;
  static std::string pending_paired_name;
  static void show_pending_notices();

  #ifdef _WIN32
  // Shell: hand the desktop back to the monitors without quitting the app
  static void tray_release_vdisplay_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Releasing the virtual display from system tray"sv;
    if (proc::request_virtual_display_release() == 0) {
      notify(localized("Virtual Display", "가상 디스플레이"), localized("No virtual display is in use right now.", "지금은 가상 디스플레이를 쓰고 있지 않습니다."));
    }
  }
  #endif

  void tray_restart_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Restarting from system tray"sv;

    proc::proc.terminate();
    platf::restart();
  }

  void tray_quit_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Quitting from system tray"sv;

    proc::proc.terminate();

  #ifdef _WIN32
    // If we're running in a service, return a special status to
    // tell it to terminate too, otherwise it will just respawn us.
    if (GetConsoleWindow() == nullptr) {
      lifetime::exit_shell(ERROR_SHUTDOWN_IN_PROGRESS, true);
      return;
    }
  #endif

    lifetime::exit_shell(0, true);
  }

  // Tray menu
  static struct tray tray = {
    .icon = TRAY_ICON,
    .tooltip = TRAY_TOOLTIP,
    .menu =
      (struct tray_menu[]) {
        // todo - use boost/locale to translate menu strings
        { .text = "Open Shell", .cb = tray_open_ui_cb },
        { .text = "-" },
        // { .text = "-" },
        // { .text = "Donate",
        //   .submenu =
        //     (struct tray_menu[]) {
        //       { .text = "GitHub Sponsors", .cb = tray_donate_github_cb },
        //       { .text = "MEE6", .cb = tray_donate_mee6_cb },
        //       { .text = "Patreon", .cb = tray_donate_patreon_cb },
        //       { .text = "PayPal", .cb = tray_donate_paypal_cb },
        //       { .text = nullptr } } },
        // { .text = "-" },
        { .text = "Reload Apps", .cb = tray_force_stop_cb },
  // Currently display device settings are only supported on Windows
  #ifdef _WIN32
        {.text = "Release Virtual Display", .cb = tray_release_vdisplay_cb},
        {.text = "Reset Display Device Config", .cb = tray_reset_display_device_config_cb},
  #endif
        {.text = "Restart", .cb = tray_restart_cb},
        {.text = "Quit", .cb = tray_quit_cb},
        {.text = nullptr}
      },
    .iconPathCount = 4,
    .allIconPaths = {TRAY_ICON, TRAY_ICON_LOCKED, TRAY_ICON_PLAYING, TRAY_ICON_PAUSING},
  };

  int init_tray() {
  #ifdef _WIN32
    // If we're running as SYSTEM, Explorer.exe will not have permission to open our thread handle
    // to monitor for thread termination. If Explorer fails to open our thread, our tray icon
    // will persist forever if we terminate unexpectedly. To avoid this, we will modify our thread
    // DACL to add an ACE that allows SYNCHRONIZE access to Everyone.
    {
      PACL old_dacl;
      PSECURITY_DESCRIPTOR sd;
      auto error = GetSecurityInfo(GetCurrentThread(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_dacl, nullptr, &sd);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "GetSecurityInfo() failed: "sv << error;
        return 1;
      }

      auto free_sd = util::fail_guard([sd]() {
        LocalFree(sd);
      });

      SID_IDENTIFIER_AUTHORITY sid_authority = SECURITY_WORLD_SID_AUTHORITY;
      PSID world_sid;
      if (!AllocateAndInitializeSid(&sid_authority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &world_sid)) {
        error = GetLastError();
        BOOST_LOG(warning) << "AllocateAndInitializeSid() failed: "sv << error;
        return 1;
      }

      auto free_sid = util::fail_guard([world_sid]() {
        FreeSid(world_sid);
      });

      EXPLICIT_ACCESS ea {};
      ea.grfAccessPermissions = SYNCHRONIZE;
      ea.grfAccessMode = GRANT_ACCESS;
      ea.grfInheritance = NO_INHERITANCE;
      ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
      ea.Trustee.ptstrName = (LPSTR) world_sid;

      PACL new_dacl;
      error = SetEntriesInAcl(1, &ea, old_dacl, &new_dacl);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "SetEntriesInAcl() failed: "sv << error;
        return 1;
      }

      auto free_new_dacl = util::fail_guard([new_dacl]() {
        LocalFree(new_dacl);
      });

      error = SetSecurityInfo(GetCurrentThread(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl, nullptr);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "SetSecurityInfo() failed: "sv << error;
        return 1;
      }
    }

    // Wait for the shell to be initialized before registering the tray icon.
    // This ensures the tray icon works reliably after a logoff/logon cycle.
    // Shell: the service starts at boot, often long before anyone signs in, so this can take
    // minutes or hours; stop waiting only when Shell exits.
    while (GetShellWindow() == nullptr) {
      if (tray_thread_should_exit) {
        return 1;
      }
      Sleep(1000);
    }
  #endif

    if (tray_init(&tray) < 0) {
      BOOST_LOG(warning) << "Failed to create system tray"sv;
      return 1;
    }

    BOOST_LOG(info) << "System tray created"sv;
    tray_initialized = true;
    return 0;
  }

  int process_tray_events() {
    if (!tray_initialized) {
      return 1;
    }

    // Process one iteration of the tray loop with non-blocking mode (0)
    if (const int result = tray_loop(0); result != 0) {
      BOOST_LOG(warning) << "System tray loop failed"sv;
      return result;
    }

    // Shell: the queued notices, on the thread that owns the tray
    show_pending_notices();

    return 0;
  }

  int end_tray() {
    if (tray_initialized) {
      tray_initialized = false;
      tray_exit();
    }
    return 0;
  }

  void update_tray_playing(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON_PLAYING;

    tray_update(&tray);
    tray.icon = TRAY_ICON_PLAYING;
    tray.notification_title = localized("App Launched", "앱 실행됨");
    char msg[256];
    static char force_close_msg[256];
    snprintf(msg, std::size(msg), korean_ui() ? "%s 실행됨" : "%s launched", app_name.c_str());
    snprintf(force_close_msg, std::size(force_close_msg), korean_ui() ? "강제 종료 [%s]" : "Force Stop [%s]", app_name.c_str());
  #ifdef _WIN32
    strncpy(msg, utf8ToAcp(msg).c_str(), std::size(msg) - 1);
    strncpy(force_close_msg, utf8ToAcp(force_close_msg).c_str(), std::size(force_close_msg) - 1);
  #endif
    tray.notification_text = msg;
    tray.notification_icon = TRAY_ICON_PLAYING;
    tray.tooltip = TRAY_TOOLTIP;
    tray.menu[2].text = force_close_msg;
    tray_update(&tray);
  }

  void update_tray_pausing(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON_PAUSING;
    tray_update(&tray);
    char msg[256];
    snprintf(msg, std::size(msg), korean_ui() ? "%s 스트리밍을 일시 정지했습니다. 앱은 계속 실행됩니다." : "Streaming of %s paused. The app keeps running.", app_name.c_str());
  #ifdef _WIN32
    strncpy(msg, utf8ToAcp(msg).c_str(), std::size(msg) - 1);
  #endif
    tray.icon = TRAY_ICON_PAUSING;
    tray.notification_title = localized("Stream Paused", "스트리밍 일시 정지");
    tray.notification_text = msg;
    tray.notification_icon = TRAY_ICON_PAUSING;
    tray.tooltip = TRAY_TOOLTIP;
    tray_update(&tray);
  }

  void update_tray_stopped(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON;
    tray_update(&tray);
    char msg[256];
    snprintf(msg, std::size(msg), korean_ui() ? "%s 앱을 종료했습니다" : "%s has been stopped", app_name.c_str());
  #ifdef _WIN32
    strncpy(msg, utf8ToAcp(msg).c_str(), std::size(msg) - 1);
  #endif
    tray.icon = TRAY_ICON;
    tray.notification_icon = TRAY_ICON;
    tray.notification_title = localized("App Stopped", "앱 종료됨");
    tray.notification_text = msg;
    tray.tooltip = TRAY_TOOLTIP;
    tray.menu[2].text = TRAY_MSG_NO_APP_RUNNING;
    tray_update(&tray);
  }

  void
  update_tray_launch_error(std::string app_name, int exit_code) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = NULL;
    tray.notification_text = NULL;
    tray.notification_cb = NULL;
    tray.notification_icon = NULL;
    tray.icon = TRAY_ICON;
    tray_update(&tray);
    char msg[256];
    snprintf(msg, std::size(msg), korean_ui() ? "%s 앱이 코드 %d로 너무 빨리 종료됐습니다. 여기를 누르면 스트림을 끝냅니다." : "%s exited too quickly with code %d. Click here to end the stream.", app_name.c_str(), exit_code);
  #ifdef _WIN32
    strncpy(msg, utf8ToAcp(msg).c_str(), std::size(msg) - 1);
  #endif
    tray.icon = TRAY_ICON;
    tray.notification_icon = TRAY_ICON;
    tray.notification_title = localized("Launch Error", "실행 오류");
    tray.notification_text = msg;
    tray.notification_cb = []() {
      BOOST_LOG(info) << "Force stop from notification"sv;
      proc::proc.terminate();
    };
    tray.tooltip = TRAY_TOOLTIP;
    tray_update(&tray);
  }

  void update_tray_require_pin() {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON;
    tray_update(&tray);
    tray.icon = TRAY_ICON;
    tray.notification_title = localized("Pairing Request", "페어링 요청");
    tray.notification_text = localized("Click here to enter the PIN and finish pairing", "여기를 눌러 PIN을 입력하고 페어링을 마치세요");
    tray.notification_icon = TRAY_ICON_LOCKED;
    tray.tooltip = TRAY_TOOLTIP;
    tray.notification_cb = []() {
      launch_ui("/pin#PIN");
    };
    tray_update(&tray);
  }

  void
  update_tray_paired(std::string device_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = NULL;
    tray.notification_text = NULL;
    tray.notification_cb = NULL;
    tray.notification_icon = NULL;
    tray_update(&tray);
    char msg[256];
    snprintf(msg, std::size(msg), korean_ui() ? "%s 기기가 페어링됐습니다. 본인이 쓰는 기기인지 확인하세요." : "%s has been paired. Make sure it is a device you own.", device_name.c_str());
  #ifdef _WIN32
    strncpy(msg, utf8ToAcp(msg).c_str(), std::size(msg) - 1);
  #endif
    tray.notification_title = localized("Device Paired", "기기 페어링 완료");
    tray.notification_text = msg;
    tray.notification_icon = TRAY_ICON;
    tray.notification_cb = []() {
      launch_ui("/pin");
    };
    tray.tooltip = TRAY_TOOLTIP;
    tray_update(&tray);
  }

  static void show_pending_notices() {
    std::vector<pending_notice_e> notices;
    std::string paired_name;
    {
      std::lock_guard lock {pending_notices_lock};
      notices.swap(pending_notices);
      paired_name = std::move(pending_paired_name);
      pending_paired_name.clear();
    }
    for (const auto notice : notices) {
      switch (notice) {
        case pending_notice_e::require_pin:
          update_tray_require_pin();
          break;
        case pending_notice_e::paired:
          update_tray_paired(paired_name);
          break;
      }
    }
  }

  static void queue_notice(pending_notice_e notice, std::string paired_name = {}) {
    // As the update_tray_*() calls: nothing is kept for a tray that is not there (yet)
    if (!tray_initialized) {
      return;
    }

    std::lock_guard lock {pending_notices_lock};
    if (notice == pending_notice_e::paired) {
      pending_paired_name = std::move(paired_name);
    }
    if (std::find(pending_notices.begin(), pending_notices.end(), notice) == pending_notices.end()) {
      pending_notices.push_back(notice);
    }
  }

  void queue_require_pin() {
    queue_notice(pending_notice_e::require_pin);
  }

  void queue_paired(std::string device_name) {
    queue_notice(pending_notice_e::paired, std::move(device_name));
  }

  void
  update_tray_client_connected(std::string client_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = NULL;
    tray.notification_text = NULL;
    tray.notification_cb = NULL;
    tray.notification_icon = NULL;
    tray.icon = TRAY_ICON;
    tray_update(&tray);
    char msg[256];
    snprintf(msg, std::size(msg), korean_ui() ? "%s 기기가 세션에 연결됐습니다" : "%s connected to the session", client_name.c_str());
  #ifdef _WIN32
    strncpy(msg, utf8ToAcp(msg).c_str(), std::size(msg) - 1);
  #endif
    tray.notification_title = localized("Device Connected", "기기 연결됨");
    tray.notification_text = msg;
    tray.notification_icon = TRAY_ICON;
    tray.tooltip = TRAY_TOOLTIP;
    tray_update(&tray);
  }

  static void notify(const char *title, const char *text) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray_update(&tray);
    tray.notification_title = title;
    tray.notification_text = text;
    tray.notification_icon = TRAY_ICON;
    tray_update(&tray);
  }

  void update_tray_vdisplay_released(bool manual) {
    notify(localized("Virtual Display Released", "가상 디스플레이 해제"), manual ?
      localized("The desktop is back on the monitors. The app keeps running; the next connection creates the virtual display again.",
           "화면을 모니터로 돌려놓았습니다. 앱은 계속 실행되고, 다음에 접속하면 가상 디스플레이를 다시 만듭니다.") :
      localized("A monitor turned on, so the desktop is back on the monitors. The app keeps running; the next connection creates the virtual display again.",
           "모니터가 켜져서 화면을 모니터로 돌려놓았습니다. 앱은 계속 실행되고, 다음에 접속하면 가상 디스플레이를 다시 만듭니다."));
  }

  void update_tray_vdisplay_release_failed() {
    notify(localized("Virtual Display Not Released", "가상 디스플레이 해제 실패"),
      localized("The virtual display could not be removed. It is removed when the app quits.",
           "가상 디스플레이를 제거하지 못했습니다. 앱을 종료하면 함께 제거됩니다."));
  }

  // Threading functions available on all platforms
  static void tray_thread_worker() {
    BOOST_LOG(info) << "System tray thread started"sv;

    // Initialize the tray in this thread
    if (init_tray() != 0) {
      BOOST_LOG(error) << "Failed to initialize tray in thread"sv;
      tray_thread_running = false;
      return;
    }

    tray_thread_running = true;

    // Main tray event loop
    while (!tray_thread_should_exit) {
      if (process_tray_events() != 0) {
        BOOST_LOG(warning) << "Tray event processing failed in thread"sv;
        break;
      }

      // Sleep to avoid busy waiting
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Clean up the tray
    end_tray();
    tray_thread_running = false;
    BOOST_LOG(info) << "System tray thread ended"sv;
  }

  int init_tray_threaded() {
    if (tray_thread_running) {
      BOOST_LOG(warning) << "Tray thread is already running"sv;
      return 1;
    }

    const std::string open_str = std::string(korean_ui() ? "Shell 열기 (" : "Open Shell (") + config::nvhttp.shell_name + ":" + std::to_string(net::map_port(confighttp::PORT_HTTPS)) + ")";
  #ifdef _WIN32
    static const std::string title_str = utf8ToAcp(open_str);
  #else
    static const std::string title_str = open_str;
  #endif
    tray.menu[0].text = title_str.c_str();

    // Menu labels in the display language, set here because the static menu above must use constant strings.
    for (struct tray_menu *item = tray.menu; item->text != nullptr; ++item) {
      if (item->cb == tray_force_stop_cb) {
        item->text = TRAY_MSG_NO_APP_RUNNING;
      } else if (item->cb == tray_restart_cb) {
        item->text = localized("Restart", "다시 시작");
      } else if (item->cb == tray_quit_cb) {
        item->text = localized("Quit", "종료");
      }
  #ifdef _WIN32
      else if (item->cb == tray_reset_display_device_config_cb) {
        item->text = localized("Reset Display Device Config", "디스플레이 설정 초기화");
      } else if (item->cb == tray_release_vdisplay_cb) {
        item->text = localized("Release Virtual Display (back to the monitors)", "가상 디스플레이 해제 (모니터로 돌아가기)");
      }
  #endif
    }

    if (config::shell.hide_tray_controls) {
      tray.menu[1].text = nullptr;
    }

    tray_thread_should_exit = false;

    try {
      tray_thread = std::thread(tray_thread_worker);

      // Wait for the thread to start and initialize
      const auto start_time = std::chrono::steady_clock::now();
      while (!tray_thread_running && !tray_thread_should_exit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Shell: the desktop is not there yet (the service started before anyone signed in).
        // Upstream gave up after 10 s, so the icon never appeared after a reboot; the thread
        // keeps waiting and adds the icon once the user signs in.
        if (std::chrono::steady_clock::now() - start_time > std::chrono::seconds(10)) {
          BOOST_LOG(info) << "System tray waits for the desktop; the icon appears once a user signs in"sv;
          return 0;
        }
      }

      if (!tray_thread_running) {
        BOOST_LOG(error) << "Tray thread failed to start"sv;
        if (tray_thread.joinable()) {
          tray_thread.join();
        }
        return 1;
      }

      BOOST_LOG(info) << "System tray thread initialized successfully"sv;
      return 0;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Failed to create tray thread: " << e.what();
      return 1;
    }
  }

  int end_tray_threaded() {
    // Also a thread still waiting for the desktop (see init_tray_threaded())
    if (!tray_thread.joinable()) {
      return 0;
    }

    BOOST_LOG(info) << "Stopping system tray thread"sv;
    tray_thread_should_exit = true;

    if (tray_thread.joinable()) {
      tray_thread.join();
    }

    BOOST_LOG(info) << "System tray thread stopped"sv;
    return 0;
  }

}  // namespace system_tray

  #ifdef BOOST_PROCESS_VERSION
    #undef BOOST_PROCESS_VERSION 1
  #endif

#endif
