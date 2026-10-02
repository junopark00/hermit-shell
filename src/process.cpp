/**
 * @file src/process.cpp
 * @brief Definitions for the startup and shutdown of the apps started by a streaming Session.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#ifndef BOOST_PROCESS_VERSION
 #define BOOST_PROCESS_VERSION 1
#endif
// standard includes
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/crc.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

// local includes
#include "config.h"
#include "crypto.h"
#include "display_device.h"
#include "file_handler.h"
#include "logging.h"
#include "platform/common.h"
#include "process.h"
#include "httpcommon.h"
#include "system_tray.h"
#include "utility.h"
#include "video.h"
#include "uuid.h"

#ifdef _WIN32
  // from_utf8() string conversion function
  #include "platform/windows/misc.h"
  #include "platform/windows/utils.h"

  // _SH constants for _wfsopen()
  #include <share.h>
#endif

#define DEFAULT_APP_IMAGE_PATH SHELL_ASSETS_DIR "/box.png"

namespace proc {
  using namespace std::literals;
  namespace pt = boost::property_tree;

  proc_t proc;

  int input_only_app_id = -1;
  std::string input_only_app_id_str;
  int terminate_app_id = -1;
  std::string terminate_app_id_str;

#ifdef _WIN32
  VDISPLAY::DRIVER_STATUS vDisplayDriverStatus = VDISPLAY::DRIVER_STATUS::UNKNOWN;

  void onVDisplayWatchdogFailed() {
    vDisplayDriverStatus = VDISPLAY::DRIVER_STATUS::WATCHDOG_FAILED;
    VDISPLAY::closeVDisplayDevice();
  }

  void initVDisplayDriver() {
    vDisplayDriverStatus = VDISPLAY::openVDisplayDevice();
    if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
      if (!VDISPLAY::startPingThread(onVDisplayWatchdogFailed)) {
        onVDisplayWatchdogFailed();
        return;
      }
    }
  }

  // Shell: handing the desktop back to the physical monitors.
  // Creating, removing and reading the virtual display state (display_name, virtual_display,
  // vdisplay_released) happen on the HTTP, tray, web UI and watcher threads; this lock keeps them
  // in turn. Recursive because terminate() runs inside execute() on failure.
  static std::recursive_mutex vdisplay_mutex;
  // No automatic release before this time (steady clock), so a client that is still connecting
  // keeps the display it is about to stream
  static std::atomic<std::chrono::steady_clock::time_point> vdisplay_quiet_until {std::chrono::steady_clock::time_point {}};
  static std::atomic<bool> vdisplay_release_requested {false};
  // Wakes the watcher once (the request itself can stay pending for a while)
  static std::atomic<bool> vdisplay_watch_kick {false};
  // The last release could not remove the display (shown in the web UI until the next request)
  static std::atomic<bool> vdisplay_release_failed {false};
  // Active displays other than the virtual one, as last seen by the watcher (-1 = not watching)
  static std::atomic<int> vdisplay_other_displays {-1};
  static std::atomic<bool> vdisplay_release_pending {false};

  static void hold_off_vdisplay_release(std::chrono::seconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    auto current = vdisplay_quiet_until.load();
    while (current < until && !vdisplay_quiet_until.compare_exchange_weak(current, until)) {
    }
  }

  /**
   * @brief Count the monitors connected and turned on, other than the display whose GDI name is
   * `exclude` (the virtual display).
   * @details A monitor counts once Windows sees it (target available), whether or not it is part
   * of the desktop yet: a monitor turned on next to a virtual display may stay inactive when the
   * saved layout for that set of displays shows only the virtual one.
   * @return The count, or -1 when Windows did not answer.
   */
  static int count_other_displays(const std::wstring &exclude) {
    for (int attempt = 0; attempt < 3; ++attempt) {
      UINT32 path_count = 0;
      UINT32 mode_count = 0;
      if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
        return -1;
      }
      std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
      std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
      const LONG result = QueryDisplayConfig(QDC_ALL_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
      if (result == ERROR_INSUFFICIENT_BUFFER) {
        continue;  // the layout changed in between
      }
      if (result != ERROR_SUCCESS) {
        return -1;
      }
      paths.resize(path_count);

      auto target_key = [](const DISPLAYCONFIG_PATH_INFO &path) {
        return (uint64_t(uint32_t(path.targetInfo.adapterId.HighPart)) << 32 | path.targetInfo.adapterId.LowPart) * 1000003u + path.targetInfo.id;
      };

      // The virtual display's target, found through its active path
      std::set<uint64_t> excluded;
      if (!exclude.empty()) {
        for (const auto &path : paths) {
          if (!(path.flags & DISPLAYCONFIG_PATH_ACTIVE)) {
            continue;
          }
          DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
          source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
          source.header.size = sizeof(source);
          source.header.adapterId = path.sourceInfo.adapterId;
          source.header.id = path.sourceInfo.id;
          if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS && _wcsicmp(source.viewGdiDeviceName, exclude.c_str()) == 0) {
            excluded.insert(target_key(path));
          }
        }
      }

      // Every source/target pair is listed, so count each available target once
      std::set<uint64_t> targets;
      for (const auto &path : paths) {
        if (path.targetInfo.targetAvailable && !excluded.count(target_key(path))) {
          targets.insert(target_key(path));
        }
      }
      return (int) targets.size();
    }
    return -1;
  }

  static std::mutex vdisplay_watch_lock;
  static std::condition_variable vdisplay_watch_cv;
  static bool vdisplay_watch_stop = false;
  static std::thread vdisplay_watch_thread;

  /**
   * @brief Watches for a physical monitor turning on while the virtual display is in place.
   * @details A PC that boots with every monitor off gets the virtual display as its only, and so
   * main, display. When a monitor is turned on later Windows keeps that layout, and windows stay on
   * the virtual screen. When the count of other displays goes from none to some, the virtual
   * display is removed as soon as nobody streams; Windows then moves everything to the monitor.
   */
  static void vdisplay_watch() {
    int last = -1;
    while (true) {
      {
        std::unique_lock<std::mutex> lk(vdisplay_watch_lock);
        vdisplay_watch_cv.wait_for(lk, 2s, [] { return vdisplay_watch_stop || vdisplay_watch_kick.load(); });
        vdisplay_watch_kick = false;
        if (vdisplay_watch_stop) {
          return;
        }
      }

      std::wstring vdisplay;
      {
        std::lock_guard<std::recursive_mutex> lg(vdisplay_mutex);
        if (proc.virtual_display && !proc.display_name.empty()) {
          vdisplay = platf::from_utf8(proc.display_name);
        }
      }
      if (vdisplay.empty()) {
        last = -1;
        vdisplay_other_displays = -1;
        vdisplay_release_pending = false;
        vdisplay_release_requested = false;
        continue;
      }

      const int others = count_other_displays(vdisplay);
      if (others < 0) {
        continue;
      }
      if (last == 0 && others > 0 && config::video.virtual_display_release_on_monitor) {
        BOOST_LOG(info) << "A monitor turned on while the Virtual Display is in use ("sv << others
                        << " other display(s) active); it will be released when nobody streams"sv;
        vdisplay_release_pending = true;
      } else if (others == 0) {
        vdisplay_release_pending = false;
      }
      last = others;
      vdisplay_other_displays = others;

      const bool manual = vdisplay_release_requested.load();
      if (!manual && !vdisplay_release_pending) {
        continue;
      }
      // Never call into the stream code while holding vdisplay_mutex: ending a session can run
      // terminate(), which takes that lock while the session list is locked
      if (rtsp_stream::session_count() > 0) {
        continue;
      }
      if (proc.release_virtual_display(manual)) {
        last = -1;
        vdisplay_release_pending = false;
        vdisplay_release_requested = false;
      }
    }
  }

  nlohmann::json virtual_display_status() {
    // The web UI polls this; while a display is being created or an app ends (seconds) it gets
    // the last state seen instead of waiting
    static std::mutex last_lock;
    static nlohmann::json last;
    std::unique_lock<std::recursive_mutex> lg(vdisplay_mutex, std::try_to_lock);
    if (!lg.owns_lock()) {
      std::lock_guard<std::mutex> ll(last_lock);
      if (!last.is_null()) {
        return last;
      }
      lg.lock();
    }
    nlohmann::json status;
    status["driver"] = vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK;
    status["active"] = proc.virtual_display && !proc.display_name.empty();
    status["released"] = proc.vdisplay_released;
    status["name"] = proc.virtual_display ? proc.display_name : std::string {};
    status["other_displays"] = vdisplay_other_displays.load();
    // Waiting for streams to end after a monitor turned on / asked from the host (in progress)
    status["release_pending"] = vdisplay_release_pending.load();
    status["release_requested"] = vdisplay_release_requested.load();
    status["release_failed"] = vdisplay_release_failed.load();
    status["auto_release"] = config::video.virtual_display_release_on_monitor;
    lg.unlock();
    std::lock_guard<std::mutex> ll(last_lock);
    last = status;
    return status;
  }

  int request_virtual_display_release() {
    {
      std::lock_guard<std::recursive_mutex> lg(vdisplay_mutex);
      if (!proc.virtual_display || proc.display_name.empty()) {
        return 0;
      }
      // Asked for now, so drop the hold-off of the last connection
      vdisplay_quiet_until = std::chrono::steady_clock::time_point {};
      vdisplay_release_requested = true;
      vdisplay_release_failed = false;
    }
    // Outside the lock (see vdisplay_watch()). The display is about to go away, so streaming
    // clients are disconnected first; the app keeps running.
    const bool streaming = rtsp_stream::session_count() > 0;
    BOOST_LOG(info) << "Virtual Display release requested"sv << (streaming ? "; disconnecting streaming clients first"sv : ""sv);
    if (streaming) {
      rtsp_stream::terminate_sessions("host");
    }
    {
      std::lock_guard<std::mutex> lk(vdisplay_watch_lock);
      vdisplay_watch_kick = true;
    }
    vdisplay_watch_cv.notify_all();
    return streaming ? 2 : 1;
  }
#endif

  class deinit_t: public platf::deinit_t {
  public:
    ~deinit_t() {
#ifdef _WIN32
      {
        std::lock_guard<std::mutex> lk(vdisplay_watch_lock);
        vdisplay_watch_stop = true;
      }
      vdisplay_watch_cv.notify_all();
      if (vdisplay_watch_thread.joinable()) {
        vdisplay_watch_thread.join();
      }
#endif
      proc.terminate();
    }
  };

  std::unique_ptr<platf::deinit_t> init() {
#ifdef _WIN32
    vdisplay_watch_stop = false;
    vdisplay_watch_thread = std::thread(vdisplay_watch);
#endif
    return std::make_unique<deinit_t>();
  }

  void terminate_process_group(boost::process::v1::child &proc, boost::process::v1::group &group, std::chrono::seconds exit_timeout) {
    if (group.valid() && platf::process_group_running((std::uintptr_t) group.native_handle())) {
      if (exit_timeout.count() > 0) {
        // Request processes in the group to exit gracefully
        if (platf::request_process_group_exit((std::uintptr_t) group.native_handle())) {
          // If the request was successful, wait for a little while for them to exit.
          BOOST_LOG(info) << "Successfully requested the app to exit. Waiting up to "sv << exit_timeout.count() << " seconds for it to close."sv;

          // group::wait_for() and similar functions are broken and deprecated, so we use a simple polling loop
          while (platf::process_group_running((std::uintptr_t) group.native_handle()) && (--exit_timeout).count() >= 0) {
            std::this_thread::sleep_for(1s);
          }

          if (exit_timeout.count() < 0) {
            BOOST_LOG(warning) << "App did not fully exit within the timeout. Terminating the app's remaining processes."sv;
          } else {
            BOOST_LOG(info) << "All app processes have successfully exited."sv;
          }
        } else {
          BOOST_LOG(info) << "App did not respond to a graceful termination request. Forcefully terminating the app's processes."sv;
        }
      } else {
        BOOST_LOG(info) << "No graceful exit timeout was specified for this app. Forcefully terminating the app's processes."sv;
      }

      // We always call terminate() even if we waited successfully for all processes above.
      // This ensures the process group state is consistent with the OS in boost.
      std::error_code ec;
      group.terminate(ec);
      group.detach();
    }

    if (proc.valid()) {
      // avoid zombie process
      proc.detach();
    }
  }

  boost::filesystem::path find_working_directory(const std::string &cmd, const boost::process::v1::environment &env) {
    // Parse the raw command string into parts to get the actual command portion
#ifdef _WIN32
    auto parts = boost::program_options::split_winmain(cmd);
#else
    auto parts = boost::program_options::split_unix(cmd);
#endif
    if (parts.empty()) {
      BOOST_LOG(error) << "Unable to parse command: "sv << cmd;
      return boost::filesystem::path();
    }

    BOOST_LOG(debug) << "Parsed target ["sv << parts.at(0) << "] from command ["sv << cmd << ']';

    // If the target is a URL, don't parse any further here
    if (parts.at(0).find("://") != std::string::npos) {
      return boost::filesystem::path();
    }

    // If the cmd path is not an absolute path, resolve it using our PATH variable
    boost::filesystem::path cmd_path(parts.at(0));
    if (!cmd_path.is_absolute()) {
      cmd_path = boost::process::v1::search_path(parts.at(0));
      if (cmd_path.empty()) {
        BOOST_LOG(error) << "Unable to find executable ["sv << parts.at(0) << "]. Is it in your PATH?"sv;
        return boost::filesystem::path();
      }
    }

    BOOST_LOG(debug) << "Resolved target ["sv << parts.at(0) << "] to path ["sv << cmd_path << ']';

    // Now that we have a complete path, we can just use parent_path()
    return cmd_path.parent_path();
  }

  void proc_t::launch_input_only() {
    _app_id = input_only_app_id;
    _app_name = "Remote Input";
    _app.uuid = REMOTE_INPUT_UUID;
    _app.terminate_on_pause = true;
    allow_client_commands = false;
    placebo = true;

#if defined SHELL_TRAY && SHELL_TRAY >= 1
    system_tray::update_tray_playing(_app_name);
#endif
  }

  int proc_t::execute(const ctx_t& app, std::shared_ptr<rtsp_stream::launch_session_t> launch_session) {
    if (_app_id == input_only_app_id) {
      terminate(false, false);
      std::this_thread::sleep_for(1s);
    } else {
      // Ensure starting from a clean slate
      terminate(false, false);
    }

    _app = app;
    _app_id = util::from_view(app.id);
    _app_name = app.name;
    _launch_session = launch_session;
    allow_client_commands = app.allow_client_commands;

    uint32_t client_width = launch_session->width ? launch_session->width : 1920;
    uint32_t client_height = launch_session->height ? launch_session->height : 1080;

    uint32_t render_width = client_width;
    uint32_t render_height = client_height;

    int scale_factor = launch_session->scale_factor;
    if (_app.scale_factor != 100) {
      scale_factor = _app.scale_factor;
    }

    if (scale_factor != 100) {
      render_width *= ((float)scale_factor / 100);
      render_height *= ((float)scale_factor / 100);

      // Chop the last bit to ensure the scaled resolution is even numbered
      // Most odd resolutions won't work well
      render_width &= ~1;
      render_height &= ~1;
    }

    launch_session->width = render_width;
    launch_session->height = render_height;

    this->initial_display = config::video.output_name;
#ifdef _WIN32
    // Shell: a client is starting a stream, don't hand the display back to the monitors meanwhile
    hold_off_vdisplay_release(30s);
#endif
    // Executed when returning from function
    auto fg = util::fail_guard([&]() {
      // Restore to user defined output name
      config::video.output_name = this->initial_display;
      terminate();
      display_device::revert_configuration();
    });

    if (!app.gamepad.empty()) {
      _saved_input_config = std::make_shared<config::input_t>(config::input);
      if (app.gamepad == "disabled") {
        config::input.controller = false;
      } else {
        config::input.controller = true;
        config::input.gamepad = app.gamepad;
      }
    }

#ifdef _WIN32
    if (
      config::video.headless_mode        // Headless mode
      || launch_session->virtual_display // User requested virtual display
      || _app.virtual_display            // App is configured to use virtual display
      || !video::allow_encoder_probing() // No active display presents
    ) {
      if (vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
        // Try init driver again
        initVDisplayDriver();
      }

      if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
        std::lock_guard<std::recursive_mutex> vdisplay_lock(vdisplay_mutex);

        // Try set the render adapter matching the capture adapter if user has specified one
        if (!config::video.adapter_name.empty()) {
          VDISPLAY::setRenderAdapterByName(platf::from_utf8(config::video.adapter_name));
        }

        std::string device_name;
        std::string device_uuid_str;
        uuid_util::uuid_t device_uuid;

        if (_app.use_app_identity) {
          device_name = _app.name;
          if (_app.per_client_app_identity) {
            device_uuid = uuid_util::uuid_t::parse(launch_session->unique_id);
            auto app_uuid = uuid_util::uuid_t::parse(_app.uuid);

            // Use XOR to mix the two UUIDs
            device_uuid.b64[0] ^= app_uuid.b64[0];
            device_uuid.b64[1] ^= app_uuid.b64[1];

            device_uuid_str = device_uuid.string();
          } else {
            device_uuid_str = _app.uuid;
            device_uuid = uuid_util::uuid_t::parse(_app.uuid);
          }
        } else {
          device_name = launch_session->device_name;
          device_uuid_str = launch_session->unique_id;
          device_uuid = uuid_util::uuid_t::parse(launch_session->unique_id);
        }

        memcpy(&launch_session->display_guid, &device_uuid, sizeof(GUID));

        int target_fps = launch_session->fps ? launch_session->fps : 60000;

        if (target_fps < 1000) {
          target_fps *= 1000;
        }

        if (config::video.double_refreshrate) {
          target_fps *= 2;
        }

        std::wstring vdisplayName = VDISPLAY::createVirtualDisplay(
          device_uuid_str.c_str(),
          device_name.c_str(),
          render_width,
          render_height,
          target_fps,
          launch_session->display_guid
        );
        vdisplay_uuid_str = device_uuid_str;
        vdisplay_device_name = device_name;

        // No matter we get the display name or not, the virtual display might still be created.
        // We need to track it properly to remove the display when the session terminates.
        launch_session->virtual_display = true;

        if (!vdisplayName.empty()) {
          BOOST_LOG(info) << "Virtual Display created at " << vdisplayName;

          // A new virtual display is not usable immediately. Configuring or probing it too early
          // fails ("Failed to collect path source data") and the launch returns 503, most often
          // when no physical monitor is active. Wait until it is reported as an active device.
          const auto wait_start = std::chrono::steady_clock::now();
          const bool vdisplay_ready = display_device::wait_for_active_device(platf::to_utf8(vdisplayName), 5s);
          const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - wait_start).count();
          if (vdisplay_ready) {
            BOOST_LOG(info) << "Virtual Display became active after " << waited_ms << " ms";
          } else {
            BOOST_LOG(warning) << "Virtual Display was not active after " << waited_ms << " ms, continuing anyway";
          }

          // Don't change display settings when no params are given
          if (launch_session->width && launch_session->height && launch_session->fps) {
            // Apply display settings
            VDISPLAY::changeDisplaySettings(vdisplayName.c_str(), render_width, render_height, target_fps);
          }

          // Check the ISOLATED DISPLAY configuration setting and rearrange the displays
          if (config::video.isolated_virtual_display_option == true) {
            // Apply the isolated display settings
            VDISPLAY::changeDisplaySettings2(vdisplayName.c_str(), render_width, render_height, target_fps, true);
          }

          // Set virtual_display to true when everything went fine
          this->virtual_display = true;
          this->vdisplay_released = false;
          vdisplay_release_failed = false;
          this->display_name = platf::to_utf8(vdisplayName);

          // When using virtual display, we don't care which display user configured to use.
          // So we always set output_name to the newly created virtual display as a workaround for
          // empty name when probing graphics cards.

          config::video.output_name = display_device::map_display_name(this->display_name);
        } else {
          BOOST_LOG(warning) << "Virtual Display creation failed, or cannot get created display name in time!";
        }
      } else {
        // Driver isn't working so we don't need to track virtual display.
        launch_session->virtual_display = false;
      }
    }

    display_device::configure_display(config::video, *launch_session);

    // We should not preserve display state when using virtual display.
    // It is already handled by Windows properly.
    if (this->virtual_display) {
      display_device::reset_persistence();
    }

#else

    display_device::configure_display(config::video, *launch_session);

#endif

    // Probe encoders again before streaming to ensure our chosen
    // encoder matches the active GPU (which could have changed
    // due to hotplugging, driver crash, primary monitor change,
    // or any number of other factors).
    if (rtsp_stream::session_count() == 0 && video::probe_encoders()) {
      if (config::video.ignore_encoder_probe_failure) {
        BOOST_LOG(warning) << "Encoder probe failed, but continuing due to user configuration.";
      } else {
        return 503;
      }
    }

    std::string fps_str;
    char fps_buf[8];
    snprintf(fps_buf, sizeof(fps_buf), "%.3f", (float)launch_session->fps / 1000.0f);
    fps_str = fps_buf;

    // Add Stream-specific environment variables
    // Compatibility: the SUNSHINE_* names used by scripts written for Sunshine-based hosts are set as well
    _env["SUNSHINE_APP_ID"] = _app.id;
    _env["SUNSHINE_APP_NAME"] = _app.name;
    _env["SUNSHINE_CLIENT_WIDTH"] = std::to_string(render_width);
    _env["SUNSHINE_CLIENT_HEIGHT"] = std::to_string(render_height);
    _env["SUNSHINE_CLIENT_FPS"] = config::shell.envvar_compatibility_mode ? std::to_string(std::round((float)launch_session->fps / 1000.0f)) : fps_str;
    _env["SUNSHINE_CLIENT_HDR"] = launch_session->enable_hdr ? "true" : "false";
    _env["SUNSHINE_CLIENT_GCMAP"] = std::to_string(launch_session->gcmap);
    _env["SUNSHINE_CLIENT_HOST_AUDIO"] = launch_session->host_audio ? "true" : "false";
    _env["SUNSHINE_CLIENT_ENABLE_SOPS"] = launch_session->enable_sops ? "true" : "false";

    _env["SHELL_APP_ID"] = _app.id;
    _env["SHELL_APP_NAME"] = _app.name;
    _env["SHELL_APP_UUID"] = _app.uuid;
    _env["SHELL_APP_STATUS"] = "STARTING";
    _env["SHELL_CLIENT_UUID"] = launch_session->unique_id;
    _env["SHELL_CLIENT_NAME"] = launch_session->device_name;
    _env["SHELL_CLIENT_WIDTH"] = std::to_string(render_width);
    _env["SHELL_CLIENT_HEIGHT"] = std::to_string(render_height);
    _env["SHELL_CLIENT_RENDER_WIDTH"] = std::to_string(launch_session->width);
    _env["SHELL_CLIENT_RENDER_HEIGHT"] = std::to_string(launch_session->height);
    _env["SHELL_CLIENT_SCALE_FACTOR"] = std::to_string(scale_factor);
    _env["SHELL_CLIENT_FPS"] = fps_str;
    _env["SHELL_CLIENT_HDR"] = launch_session->enable_hdr ? "true" : "false";
    _env["SHELL_CLIENT_GCMAP"] = std::to_string(launch_session->gcmap);
    _env["SHELL_CLIENT_HOST_AUDIO"] = launch_session->host_audio ? "true" : "false";
    _env["SHELL_CLIENT_ENABLE_SOPS"] = launch_session->enable_sops ? "true" : "false";

    int channelCount = launch_session->surround_info & 65535;
    switch (channelCount) {
      case 2:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "2.0";
        _env["SHELL_CLIENT_AUDIO_CONFIGURATION"] = "2.0";
        break;
      case 6:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "5.1";
        _env["SHELL_CLIENT_AUDIO_CONFIGURATION"] = "5.1";
        break;
      case 8:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "7.1";
        _env["SHELL_CLIENT_AUDIO_CONFIGURATION"] = "7.1";
        break;
    }
    _env["SUNSHINE_CLIENT_AUDIO_SURROUND_PARAMS"] = launch_session->surround_params;
    _env["SHELL_CLIENT_AUDIO_SURROUND_PARAMS"] = launch_session->surround_params;

    if (!_app.output.empty() && _app.output != "null"sv) {
#ifdef _WIN32
      // fopen() interprets the filename as an ANSI string on Windows, so we must convert it
      // to UTF-16 and use the wchar_t variants for proper Unicode log file path support.
      auto woutput = platf::from_utf8(_app.output);

      // Use _SH_DENYNO to allow us to open this log file again for writing even if it is
      // still open from a previous execution. This is required to handle the case of a
      // detached process executing again while the previous process is still running.
      _pipe.reset(_wfsopen(woutput.c_str(), L"a", _SH_DENYNO));
#else
      _pipe.reset(fopen(_app.output.c_str(), "a"));
#endif
    }

    std::error_code ec;
    _app_prep_begin = std::begin(_app.prep_cmds);
    _app_prep_it = _app_prep_begin;

    for (; _app_prep_it != std::end(_app.prep_cmds); ++_app_prep_it) {
      auto &cmd = *_app_prep_it;

      // Skip empty commands
      if (cmd.do_cmd.empty()) {
        continue;
      }

      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd.do_cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing Do Cmd: ["sv << cmd.do_cmd << "] elevated: " << cmd.elevated;
      auto child = platf::run_command(cmd.elevated, true, cmd.do_cmd, working_dir, _env, _pipe.get(), ec, nullptr);

      if (ec) {
        BOOST_LOG(error) << "Couldn't run ["sv << cmd.do_cmd << "]: System: "sv << ec.message();
        // We don't want any prep commands failing launch of the desktop.
        // This is to prevent the issue where users reboot their PC and need to log in with Shell.
        // permission_denied is typically returned when the user impersonation fails, which can happen when user is not signed in yet.
        if (!(_app.cmd.empty() && ec == std::errc::permission_denied)) {
          return -1;
        }
      }

      child.wait();
      auto ret = child.exit_code();
      if (ret != 0 && ec != std::errc::permission_denied) {
        BOOST_LOG(error) << '[' << cmd.do_cmd << "] failed with code ["sv << ret << ']';
        return -1;
      }
    }

    _env["SHELL_APP_STATUS"] = "RUNNING";

    for (auto &cmd : _app.detached) {
      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Spawning ["sv << cmd << "] in ["sv << working_dir << ']';
      auto child = platf::run_command(_app.elevated, true, cmd, working_dir, _env, _pipe.get(), ec, nullptr);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't spawn ["sv << cmd << "]: System: "sv << ec.message();
      } else {
        child.detach();
      }
    }

    if (_app.cmd.empty()) {
      BOOST_LOG(info) << "No commands configured, showing desktop..."sv;
      placebo = true;
    } else {
      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(_app.cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing: ["sv << _app.cmd << "] in ["sv << working_dir << ']';
      _process = platf::run_command(_app.elevated, true, _app.cmd, working_dir, _env, _pipe.get(), ec, &_process_group);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't run ["sv << _app.cmd << "]: System: "sv << ec.message();
        return -1;
      }
    }

    _app_launch_time = std::chrono::steady_clock::now();

  #ifdef _WIN32
    auto resetHDRThread = std::thread([this, enable_hdr = launch_session->enable_hdr]{
      // Windows doesn't seem to be able to set HDR correctly when a display is just connected / changed resolution,
      // so we have tooggle HDR for the virtual display manually after a delay.
      auto retryInterval = 200ms;
      while (is_changing_settings_going_to_fail()) {
        if (retryInterval > 2s) {
          BOOST_LOG(warning) << "Restoring HDR settings failed due to retry timeout!";
          return;
        }
        std::this_thread::sleep_for(retryInterval);
        retryInterval *= 2;
      }

      retryInterval = 200ms;
      while (this->display_name.empty()) {
        if (retryInterval > 2s) {
          BOOST_LOG(warning) << "Not getting current display in time! HDR will not be toggled.";
          return;
        }
        std::this_thread::sleep_for(retryInterval);
        retryInterval *= 2;
      }

      // We should have got the actual streaming display by now
      std::string currentDisplay = this->display_name;
      auto currentDisplayW = platf::from_utf8(currentDisplay);

      initial_hdr = VDISPLAY::getDisplayHDRByName(currentDisplayW.c_str());

      if (config::video.dd.hdr_option == config::video_t::dd_t::hdr_option_e::automatic) {
        mode_changed_display = currentDisplay;

        // Try turn off HDR whatever
        // As we always have to apply the workaround by turining off HDR first
        VDISPLAY::setDisplayHDRByName(currentDisplayW.c_str(), false);

        if (enable_hdr) {
          if (VDISPLAY::setDisplayHDRByName(currentDisplayW.c_str(), true)) {
            BOOST_LOG(info) << "HDR enabled for display " << currentDisplay;
          } else {
            BOOST_LOG(info) << "HDR enable failed for display " << currentDisplay;
          }
        }
      } else if (initial_hdr) {
        if (VDISPLAY::setDisplayHDRByName(currentDisplayW.c_str(), false) && VDISPLAY::setDisplayHDRByName(currentDisplayW.c_str(), true)) {
          BOOST_LOG(info) << "HDR toggled successfully for display " << currentDisplay;
        } else {
          BOOST_LOG(info) << "HDR toggle failed for display " << currentDisplay;
        }
      }
    });

    resetHDRThread.detach();
  #endif

    fg.disable();

#if defined SHELL_TRAY && SHELL_TRAY >= 1
    system_tray::update_tray_playing(_app.name);
#endif

    return 0;
  }

  int proc_t::running() {
#ifndef _WIN32
    // On POSIX OSes, we must periodically wait for our children to avoid
    // them becoming zombies. This must be synchronized carefully with
    // calls to bp::wait() and platf::process_group_running() which both
    // invoke waitpid() under the hood.
    auto reaper = util::fail_guard([]() {
      while (waitpid(-1, nullptr, WNOHANG) > 0);
    });
#endif

    if (placebo) {
      return _app_id;
    } else if (_app.wait_all && _process_group && platf::process_group_running((std::uintptr_t) _process_group.native_handle())) {
      // The app is still running if any process in the group is still running
      return _app_id;
    } else if (_process.running()) {
      // The app is still running only if the initial process launched is still running
      return _app_id;
    } else if (_app.auto_detach && std::chrono::steady_clock::now() - _app_launch_time < 5s) {
      BOOST_LOG(info) << "App exited with code ["sv << _process.native_exit_code() << "] within 5 seconds of launch. Treating the app as a detached command."sv;
      BOOST_LOG(info) << "Adjust this behavior in the Applications tab or apps.json if this is not what you want."sv;
      placebo = true;

    #if defined SHELL_TRAY && SHELL_TRAY >= 1
      if (_process.native_exit_code() != 0) {
        system_tray::update_tray_launch_error(proc::proc.get_last_run_app_name(), _process.native_exit_code());
      }
    #endif

      return _app_id;
    }

    // Perform cleanup actions now if needed
    if (_process) {
      terminate();
    }

    return 0;
  }

#ifdef _WIN32
  /**
   * @brief Shell: the virtual display mode for a client, the same way a fresh launch works it out
   * (see execute()).
   */
  static void virtual_display_mode_for(const rtsp_stream::launch_session_t &launch_session, int app_scale_factor,
                                       uint32_t &render_width, uint32_t &render_height, int &target_fps) {
    render_width = launch_session.width;
    render_height = launch_session.height;
    int scale_factor = app_scale_factor != 100 ? app_scale_factor : launch_session.scale_factor;
    if (scale_factor != 100 && scale_factor > 0) {
      render_width = (uint32_t) (render_width * ((float) scale_factor / 100)) & ~1u;
      render_height = (uint32_t) (render_height * ((float) scale_factor / 100)) & ~1u;
    }
    target_fps = launch_session.fps ? launch_session.fps : 60000;
    if (target_fps < 1000) {
      target_fps *= 1000;
    }
    if (config::video.double_refreshrate) {
      target_fps *= 2;
    }
  }
#endif

  void proc_t::apply_resume_display_mode(const rtsp_stream::launch_session_t &launch_session) {
#ifdef _WIN32
    std::lock_guard<std::recursive_mutex> vdisplay_lock(vdisplay_mutex);
    if (!virtual_display || display_name.empty() || !launch_session.width || !launch_session.height) {
      return;
    }

    uint32_t render_width;
    uint32_t render_height;
    int target_fps;
    virtual_display_mode_for(launch_session, _app.scale_factor, render_width, render_height, target_fps);

    const std::wstring name = platf::from_utf8(display_name);
    DEVMODEW current = {};
    const bool have_current = VDISPLAY::getDeviceSettings(name.c_str(), current);
    const int target_hz = (target_fps + 500) / 1000;
    if (have_current && current.dmPelsWidth == render_width && current.dmPelsHeight == render_height &&
        std::abs((int) current.dmDisplayFrequency - target_hz) <= 1) {
      return;  // already in the requested mode
    }

    BOOST_LOG(info) << "Virtual Display mode for the resumed session: ["sv
                    << (have_current ? std::to_string(current.dmPelsWidth) + "x" + std::to_string(current.dmPelsHeight) : std::string("?"))
                    << "] -> ["sv << render_width << 'x' << render_height << 'x' << target_hz << ']';
    VDISPLAY::changeDisplaySettings(name.c_str(), render_width, render_height, target_fps);
    // Same placement as a fresh launch when the isolated virtual display option is on
    if (config::video.isolated_virtual_display_option) {
      VDISPLAY::changeDisplaySettings2(name.c_str(), render_width, render_height, target_fps, true);
    }

    DEVMODEW after = {};
    if (!VDISPLAY::getDeviceSettings(name.c_str(), after) ||
        (after.dmPelsWidth == render_width && after.dmPelsHeight == render_height)) {
      return;
    }

    // The virtual display driver offers only the modes of the size a display was created with
    // (a display created at 2560x1440 has no 720x1280 mode), so create it again at the new size,
    // with the same identity. Windows of the running app move over to it.
    if (!_launch_session || vdisplay_uuid_str.empty()) {
      BOOST_LOG(warning) << "Virtual Display stayed at ["sv << after.dmPelsWidth << 'x' << after.dmPelsHeight
                         << "]; the picture will be scaled into the client's frame. Quit the app to start at the new resolution."sv;
      return;
    }
    BOOST_LOG(info) << "Virtual Display has no ["sv << render_width << 'x' << render_height
                    << "] mode; creating it again at that size"sv;
    VDISPLAY::removeVirtualDisplay(_launch_session->display_guid);
    const std::wstring new_name = VDISPLAY::createVirtualDisplay(
      vdisplay_uuid_str.c_str(),
      vdisplay_device_name.c_str(),
      render_width,
      render_height,
      target_fps,
      _launch_session->display_guid
    );
    if (new_name.empty()) {
      BOOST_LOG(error) << "Virtual Display could not be created again; quit the app and start it again"sv;
      return;
    }
    if (!display_device::wait_for_active_device(platf::to_utf8(new_name), 5s)) {
      BOOST_LOG(warning) << "Virtual Display was not active after 5 s, continuing anyway"sv;
    }
    VDISPLAY::changeDisplaySettings(new_name.c_str(), render_width, render_height, target_fps);
    if (config::video.isolated_virtual_display_option) {
      VDISPLAY::changeDisplaySettings2(new_name.c_str(), render_width, render_height, target_fps, true);
    }
    display_name = platf::to_utf8(new_name);
    config::video.output_name = display_device::map_display_name(display_name);
    apply_virtual_display_hdr(launch_session.enable_hdr);

    DEVMODEW recreated = {};
    if (VDISPLAY::getDeviceSettings(new_name.c_str(), recreated)) {
      BOOST_LOG(info) << "Virtual Display created again at "sv << display_name << " ["sv
                      << recreated.dmPelsWidth << 'x' << recreated.dmPelsHeight << ']';
    }
#endif
  }

#ifdef _WIN32
  void proc_t::apply_virtual_display_hdr(bool enable_hdr) {
    // A display created again starts with HDR off; do what a fresh launch does (see execute())
    if (config::video.dd.hdr_option != config::video_t::dd_t::hdr_option_e::automatic || display_name.empty()) {
      return;
    }
    const auto name = platf::from_utf8(display_name);
    mode_changed_display = display_name;
    VDISPLAY::setDisplayHDRByName(name.c_str(), false);
    if (enable_hdr) {
      BOOST_LOG(info) << "HDR "sv << (VDISPLAY::setDisplayHDRByName(name.c_str(), true) ? "enabled"sv : "enable failed"sv)
                      << " for display "sv << display_name;
    }
  }
#endif

  bool proc_t::release_virtual_display(bool manual) {
#ifdef _WIN32
    int result;
    {
      std::lock_guard<std::recursive_mutex> vdisplay_lock(vdisplay_mutex);
      result = release_virtual_display_locked(manual);
    }
    // Outside the lock: a tray update waits for the tray thread, whose menu callbacks take it
  #if defined SHELL_TRAY && SHELL_TRAY >= 1
    if (result > 0) {
      system_tray::update_tray_vdisplay_released(manual);
    } else if (result < 0 && manual) {
      system_tray::update_tray_vdisplay_release_failed();
    }
  #endif
    return result > 0;
#else
    return false;
#endif
  }

#ifdef _WIN32
  int proc_t::release_virtual_display_locked(bool manual) {
    if (!virtual_display || !_launch_session || !_launch_session->virtual_display) {
      return 0;
    }
    // A client started connecting after the watcher looked
    if (std::chrono::steady_clock::now() < vdisplay_quiet_until.load()) {
      return 0;
    }

    BOOST_LOG(info) << "Releasing Virtual Display "sv << display_name << " ("sv << (manual ? "asked from the host"sv : "a monitor turned on"sv)
                    << "); the app keeps running and the display is created again for the next client"sv;
    if (!VDISPLAY::removeVirtualDisplay(_launch_session->display_guid)) {
      // Still there: keep it as it is (terminate() removes it), and don't try again every poll
      BOOST_LOG(error) << "Virtual Display remove failed; it stays until the app quits"sv;
      vdisplay_release_pending = false;
      vdisplay_release_requested = false;
      vdisplay_release_failed = true;
      return -1;
    }
    // Same as the end of a virtual display session (see terminate())
    display_device::reset_persistence();

    // The display is gone, there is no HDR state of it to put back
    if (mode_changed_display == display_name) {
      mode_changed_display.clear();
    }
    config::video.output_name = initial_display;
    display_name.clear();
    virtual_display = false;
    vdisplay_released = true;
    vdisplay_release_failed = false;
    return 1;
  }
#endif

  void proc_t::restore_virtual_display(const rtsp_stream::launch_session_t &launch_session) {
#ifdef _WIN32
    std::lock_guard<std::recursive_mutex> vdisplay_lock(vdisplay_mutex);
    // A client is about to stream: the display it gets must stay
    hold_off_vdisplay_release(30s);
    if (!vdisplay_released || !_launch_session || vdisplay_uuid_str.empty() || vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
      return;
    }

    // With a monitor on, stream the monitor like any host does, unless the virtual display is
    // asked for (client option, app setting or headless mode)
    const int others = count_other_displays(L"");
    const bool wanted = launch_session.virtual_display || _app.virtual_display || config::video.headless_mode;
    if (!wanted && others > 0) {
      BOOST_LOG(info) << "Virtual Display stays released: "sv << others << " monitor(s) on, streaming them"sv;
      return;
    }

    // A client that asked for no size gets the size of the session that created the display
    const auto &mode_source = launch_session.width && launch_session.height ? launch_session : *_launch_session;
    uint32_t render_width;
    uint32_t render_height;
    int target_fps;
    virtual_display_mode_for(mode_source, _app.scale_factor, render_width, render_height, target_fps);
    if (&mode_source == _launch_session.get()) {
      // execute() stored the size with the scale factor applied already
      render_width = _launch_session->width;
      render_height = _launch_session->height;
    }

    // A client may have streamed the monitors meanwhile and changed their mode; put them back
    // now (not after the revert delay), as reset_persistence() below forgets what they were
    display_device::revert_configuration_now();

    if (!config::video.adapter_name.empty()) {
      VDISPLAY::setRenderAdapterByName(platf::from_utf8(config::video.adapter_name));
    }
    const std::wstring name = VDISPLAY::createVirtualDisplay(
      vdisplay_uuid_str.c_str(),
      vdisplay_device_name.c_str(),
      render_width,
      render_height,
      target_fps,
      _launch_session->display_guid
    );
    if (name.empty()) {
      BOOST_LOG(error) << "Virtual Display could not be created again; streaming the monitors instead"sv;
      // It might exist without a name yet (see execute())
      VDISPLAY::removeVirtualDisplay(_launch_session->display_guid);
      return;
    }
    if (!display_device::wait_for_active_device(platf::to_utf8(name), 5s)) {
      BOOST_LOG(warning) << "Virtual Display was not active after 5 s, continuing anyway"sv;
    }
    if (render_width && render_height) {
      VDISPLAY::changeDisplaySettings(name.c_str(), render_width, render_height, target_fps);
    }
    if (config::video.isolated_virtual_display_option) {
      VDISPLAY::changeDisplaySettings2(name.c_str(), render_width, render_height, target_fps, true);
    }

    virtual_display = true;
    vdisplay_released = false;
    display_name = platf::to_utf8(name);
    config::video.output_name = display_device::map_display_name(display_name);
    BOOST_LOG(info) << "Virtual Display created again at "sv << display_name << " ["sv << render_width << 'x' << render_height << ']';

    // Same as a fresh launch (see execute())
    display_device::configure_display(config::video, launch_session);
    display_device::reset_persistence();
    apply_virtual_display_hdr(launch_session.enable_hdr);
#endif
  }

  void proc_t::resume() {
    BOOST_LOG(info) << "Session resuming for app [" << _app_name << "].";

    if (!_app.state_cmds.empty()) {
      auto exec_thread = std::thread([cmd_list = _app.state_cmds, app_working_dir = _app.working_dir, _env = _env]() mutable {

        _env["SHELL_APP_STATUS"] = "RESUMING";

        std::error_code ec;
        auto _state_resume_it = std::begin(cmd_list);

        for (; _state_resume_it != std::end(cmd_list); ++_state_resume_it) {
          auto &cmd = *_state_resume_it;

          // Skip empty commands
          if (cmd.do_cmd.empty()) {
            continue;
          }

          boost::filesystem::path working_dir = app_working_dir.empty() ?
                                                  find_working_directory(cmd.do_cmd, _env) :
                                                  boost::filesystem::path(app_working_dir);
          BOOST_LOG(info) << "Executing Resume Cmd: ["sv << cmd.do_cmd << "] elevated: " << cmd.elevated;
          auto child = platf::run_command(cmd.elevated, true, cmd.do_cmd, working_dir, _env, nullptr, ec, nullptr);

          if (ec) {
            BOOST_LOG(error) << "Couldn't run ["sv << cmd.do_cmd << "]: System: "sv << ec.message();
            break;
          }

          child.wait();

          auto ret = child.exit_code();
          if (ret != 0 && ec != std::errc::permission_denied) {
            BOOST_LOG(error) << '[' << cmd.do_cmd << "] failed with code ["sv << ret << ']';
            break;
          }
        }
      });

      exec_thread.detach();
    }
  }

  void proc_t::pause() {
    if (!running()) {
      BOOST_LOG(info) << "Session already stopped, do not run pause commands.";
      return;
    }

    if (_app.terminate_on_pause) {
      BOOST_LOG(info) << "Terminating app [" << _app_name << "] when all clients are disconnected. Pause commands are skipped.";
      terminate();
      return;
    }

    BOOST_LOG(info) << "Session pausing for app [" << _app_name << "].";

    if (!_app.state_cmds.empty()) {
      auto exec_thread = std::thread([cmd_list = _app.state_cmds, app_working_dir = _app.working_dir, _env = _env]() mutable {
        _env["SHELL_APP_STATUS"] = "PAUSING";

        std::error_code ec;
        auto _state_pause_it = std::begin(cmd_list);

        for (; _state_pause_it != std::end(cmd_list); ++_state_pause_it) {
          auto &cmd = *_state_pause_it;

          // Skip empty commands
          if (cmd.undo_cmd.empty()) {
            continue;
          }

          boost::filesystem::path working_dir = app_working_dir.empty() ?
                                                  find_working_directory(cmd.undo_cmd, _env) :
                                                  boost::filesystem::path(app_working_dir);
          BOOST_LOG(info) << "Executing Pause Cmd: ["sv << cmd.undo_cmd << "] elevated: " << cmd.elevated;
          auto child = platf::run_command(cmd.elevated, true, cmd.undo_cmd, working_dir, _env, nullptr, ec, nullptr);

          if (ec) {
            BOOST_LOG(error) << "Couldn't run ["sv << cmd.undo_cmd << "]: System: "sv << ec.message();
            break;
          }

          child.wait();

          auto ret = child.exit_code();
          if (ret != 0 && ec != std::errc::permission_denied) {
            BOOST_LOG(error) << '[' << cmd.undo_cmd << "] failed with code ["sv << ret << ']';
            break;
          }
        }
      });

      exec_thread.detach();
    }

#if defined SHELL_TRAY && SHELL_TRAY >= 1
    system_tray::update_tray_pausing(proc::proc.get_last_run_app_name());
#endif
  }

  void proc_t::terminate(bool immediate, bool needs_refresh) {
    std::error_code ec;
    placebo = false;

    if (!immediate) {
      terminate_process_group(_process, _process_group, _app.exit_timeout);
    }

    _process = boost::process::v1::child();
    _process_group = boost::process::v1::group();

    _env["SHELL_APP_STATUS"] = "TERMINATING";

    for (; _app_prep_it != _app_prep_begin; --_app_prep_it) {
      auto &cmd = *(_app_prep_it - 1);

      if (cmd.undo_cmd.empty()) {
        continue;
      }

      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd.undo_cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing Undo Cmd: ["sv << cmd.undo_cmd << ']';
      auto child = platf::run_command(cmd.elevated, true, cmd.undo_cmd, working_dir, _env, _pipe.get(), ec, nullptr);

      if (ec) {
        BOOST_LOG(warning) << "System: "sv << ec.message();
      }

      child.wait();
      auto ret = child.exit_code();

      if (ret != 0) {
        BOOST_LOG(warning) << "Return code ["sv << ret << ']';
      }
    }

    _pipe.reset();

    bool has_run = _app_id > 0;

    // Tray notice sent at the end, outside vdisplay_mutex (see release_virtual_display())
    std::string stopped_app;

#ifdef _WIN32
    std::unique_lock<std::recursive_mutex> vdisplay_lock(vdisplay_mutex);

    // Revert HDR state
    if (has_run && !mode_changed_display.empty()) {
      auto displayNameW = platf::from_utf8(mode_changed_display);
      if (VDISPLAY::setDisplayHDRByName(displayNameW.c_str(), initial_hdr)) {
        BOOST_LOG(info) << "HDR reverted for display " << mode_changed_display;
      } else {
        BOOST_LOG(info) << "HDR revert failed for display " << mode_changed_display;
      }
    }

    bool used_virtual_display = vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK && _launch_session && _launch_session->virtual_display;
    if (used_virtual_display && vdisplay_released) {
      BOOST_LOG(info) << "Virtual Display was already released"sv;
    } else if (used_virtual_display) {
      if (VDISPLAY::removeVirtualDisplay(_launch_session->display_guid)) {
        BOOST_LOG(info) << "Virtual Display removed successfully";
      } else if (this->virtual_display) {
        BOOST_LOG(warning) << "Virtual Display remove failed";
      } else {
        BOOST_LOG(warning) << "Virtual Display remove failed, but it seems it was not created correctly either.";
      }
    }

    // Only show the Stopped notification if we actually have an app to stop
    // Since terminate() is always run when a new app has started
    if (proc::proc.get_last_run_app_name().length() > 0 && has_run) {
      // After a release the monitors may have been set up for a client (configure_display), so
      // put them back like a session without a virtual display
      if (used_virtual_display && !vdisplay_released) {
        display_device::reset_persistence();
      } else {
        display_device::revert_configuration();
      }
#else
    if (proc::proc.get_last_run_app_name().length() > 0 && has_run) {
      display_device::revert_configuration();
#endif

      stopped_app = proc::proc.get_last_run_app_name();
    }

    // Load the configured output_name first
    // to prevent the value being write to empty when the initial terminate happens
    if (!has_run && initial_display.empty()) {
      initial_display = config::video.output_name;
    } else {
      // Restore output name to its original value
      config::video.output_name = initial_display;
    }

    _app_id = -1;
    _app_name.clear();
    _app = {};
    display_name.clear();
    initial_display.clear();
    mode_changed_display.clear();
    _launch_session.reset();
    virtual_display = false;
    vdisplay_released = false;
#ifdef _WIN32
    vdisplay_release_failed = false;
#endif
    allow_client_commands = false;

    if (_saved_input_config) {
      config::input = *_saved_input_config;
      _saved_input_config.reset();
    }

    if (needs_refresh) {
      refresh(config::stream.file_apps, false);
    }

#ifdef _WIN32
    vdisplay_lock.unlock();
#endif
#if defined SHELL_TRAY && SHELL_TRAY >= 1
    if (!stopped_app.empty()) {
      system_tray::update_tray_stopped(stopped_app);
    }
#endif
  }

  const std::vector<ctx_t> &proc_t::get_apps() const {
    return _apps;
  }

  std::vector<ctx_t> &proc_t::get_apps() {
    return _apps;
  }

  // Gets application image from application list.
  // Returns image from assets directory if found there.
  // Returns default image if image configuration is not set.
  // Returns http content-type header compatible image type.
  std::string proc_t::get_app_image(int app_id) {
    auto iter = std::find_if(_apps.begin(), _apps.end(), [&app_id](const auto app) {
      return app.id == std::to_string(app_id);
    });
    auto app_image_path = iter == _apps.end() ? std::string() : iter->image_path;

    return validate_app_image_path(app_image_path);
  }

  std::string proc_t::get_last_run_app_name() {
    return _app_name;
  }

  std::string proc_t::get_running_app_uuid() {
    return _app.uuid;
  }

  boost::process::environment proc_t::get_env() {
    return _env;
  }

  proc_t::~proc_t() {
    // It's not safe to call terminate() here because our proc_t is a static variable
    // that may be destroyed after the Boost loggers have been destroyed. Instead,
    // we return a deinit_t to main() to handle termination when we're exiting.
    // Once we reach this point here, termination must have already happened.
    assert(!placebo);
    assert(!_process.running());
  }

  std::string_view::iterator find_match(std::string_view::iterator begin, std::string_view::iterator end) {
    int stack = 0;

    --begin;
    do {
      ++begin;
      switch (*begin) {
        case '(':
          ++stack;
          break;
        case ')':
          --stack;
      }
    } while (begin != end && stack != 0);

    if (begin == end) {
      throw std::out_of_range("Missing closing bracket \')\'");
    }
    return begin;
  }

  std::string parse_env_val(boost::process::v1::native_environment &env, const std::string_view &val_raw) {
    auto pos = std::begin(val_raw);
    auto dollar = std::find(pos, std::end(val_raw), '$');

    std::stringstream ss;

    while (dollar != std::end(val_raw)) {
      auto next = dollar + 1;
      if (next != std::end(val_raw)) {
        switch (*next) {
          case '(':
            {
              ss.write(pos, (dollar - pos));
              auto var_begin = next + 1;
              auto var_end = find_match(next, std::end(val_raw));
              auto var_name = std::string {var_begin, var_end};

#ifdef _WIN32
              // Windows treats environment variable names in a case-insensitive manner,
              // so we look for a case-insensitive match here. This is critical for
              // correctly appending to PATH on Windows.
              auto itr = std::find_if(env.cbegin(), env.cend(), [&](const auto &e) {
                return boost::iequals(e.get_name(), var_name);
              });
              if (itr != env.cend()) {
                // Use an existing case-insensitive match
                var_name = itr->get_name();
              }
#endif

              ss << env[var_name].to_string();

              pos = var_end + 1;
              next = var_end;

              break;
            }
          case '$':
            ss.write(pos, (next - pos));
            pos = next + 1;
            ++next;
            break;
        }

        dollar = std::find(next, std::end(val_raw), '$');
      } else {
        dollar = next;
      }
    }

    ss.write(pos, (dollar - pos));

    return ss.str();
  }

  std::string validate_app_image_path(std::string app_image_path) {
    if (app_image_path.empty()) {
      return DEFAULT_APP_IMAGE_PATH;
    }

    // get the image extension and convert it to lowercase
    auto image_extension = std::filesystem::path(app_image_path).extension().string();
    boost::to_lower(image_extension);

    // return the default box image if extension is not "png"
    if (image_extension != ".png") {
      return DEFAULT_APP_IMAGE_PATH;
    }

    // check if image is in assets directory
    auto full_image_path = std::filesystem::path(SHELL_ASSETS_DIR) / app_image_path;
    if (std::filesystem::exists(full_image_path)) {
      return full_image_path.string();
    } else if (app_image_path == "./assets/steam.png") {
      // handle old default steam image definition
      return SHELL_ASSETS_DIR "/steam.png";
    }

    // check if specified image exists
    std::error_code code;
    if (!std::filesystem::exists(app_image_path, code)) {
      // return default box image if image does not exist
      BOOST_LOG(warning) << "Couldn't find app image at path ["sv << app_image_path << ']';
      return DEFAULT_APP_IMAGE_PATH;
    }

    // image is a png, and not in assets directory
    // return only "content-type" http header compatible image type
    return app_image_path;
  }

  std::optional<std::string> calculate_sha256(const std::string &filename) {
    crypto::md_ctx_t ctx {EVP_MD_CTX_create()};
    if (!ctx) {
      return std::nullopt;
    }

    if (!EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr)) {
      return std::nullopt;
    }

    // Read file and update calculated SHA
    char buf[1024 * 16];
    std::ifstream file(filename, std::ifstream::binary);
    while (file.good()) {
      file.read(buf, sizeof(buf));
      if (!EVP_DigestUpdate(ctx.get(), buf, file.gcount())) {
        return std::nullopt;
      }
    }
    file.close();

    unsigned char result[SHA256_DIGEST_LENGTH];
    if (!EVP_DigestFinal_ex(ctx.get(), result, nullptr)) {
      return std::nullopt;
    }

    // Transform byte-array to string
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (const auto &byte : result) {
      ss << std::setw(2) << (int) byte;
    }
    return ss.str();
  }

  uint32_t calculate_crc32(const std::string &input) {
    boost::crc_32_type result;
    result.process_bytes(input.data(), input.length());
    return result.checksum();
  }

  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index) {
    // Generate id by hashing name with image data if present
    std::vector<std::string> to_hash;
    to_hash.push_back(app_name);
    auto file_path = validate_app_image_path(app_image_path);
    if (file_path != DEFAULT_APP_IMAGE_PATH) {
      auto file_hash = calculate_sha256(file_path);
      if (file_hash) {
        to_hash.push_back(file_hash.value());
      } else {
        // Fallback to just hashing image path
        to_hash.push_back(file_path);
      }
    }

    // Create combined strings for hash
    std::stringstream ss;
    for_each(to_hash.begin(), to_hash.end(), [&ss](const std::string &s) {
      ss << s;
    });
    auto input_no_index = ss.str();
    ss << index;
    auto input_with_index = ss.str();

    // CRC32 then truncate to signed 32-bit range due to client limitations
    auto id_no_index = std::to_string(abs((int32_t) calculate_crc32(input_no_index)));
    auto id_with_index = std::to_string(abs((int32_t) calculate_crc32(input_with_index)));

    return std::make_tuple(id_no_index, id_with_index);
  }

  /**
   * @brief Migrate the applications stored in the file tree by merging in a new app.
   *
   * This function updates the application entries in *fileTree_p* using the data in *inputTree_p*.
   * If an app in the file tree does not have a UUID, one is generated and inserted.
   * If an app with the same UUID as the new app is found, it is replaced.
   * Additionally, empty keys (such as "prep-cmd" or "detached") and keys no longer needed ("launching", "index")
   * are removed from the input.
   *
   * Legacy versions stored boolean and integer values as strings.
   * The following keys are converted:
   *   - Boolean keys: "exclude-global-prep-cmd", "elevated", "auto-detach", "wait-all",
   *                     "use-app-identity", "per-client-app-identity", "virtual-display"
   *   - Integer keys: "exit-timeout"
   *
   * A migration version is stored in the file tree (under "version") so that future changes can be applied.
   *
   * @param fileTree_p Pointer to the JSON object representing the file tree.
   * @param inputTree_p Pointer to the JSON object representing the new app.
   */
  void migrate_apps(nlohmann::json* fileTree_p, nlohmann::json* inputTree_p) {
    std::string new_app_uuid;

    if (inputTree_p) {
      // If the input contains a non-empty "uuid", use it; otherwise generate one.
      if (inputTree_p->contains("uuid") && !(*inputTree_p)["uuid"].get<std::string>().empty()) {
        new_app_uuid = (*inputTree_p)["uuid"].get<std::string>();
      } else {
        new_app_uuid = uuid_util::uuid_t::generate().string();
        (*inputTree_p)["uuid"] = new_app_uuid;
      }

      // Remove "prep-cmd" if empty.
      if (inputTree_p->contains("prep-cmd") && (*inputTree_p)["prep-cmd"].empty()) {
        inputTree_p->erase("prep-cmd");
      }

      // Remove "detached" if empty.
      if (inputTree_p->contains("detached") && (*inputTree_p)["detached"].empty()) {
        inputTree_p->erase("detached");
      }

      // Remove keys that are no longer needed.
      inputTree_p->erase("launching");
      inputTree_p->erase("index");
    }

    // Get the current apps array; if it doesn't exist, create one.
    nlohmann::json newApps = nlohmann::json::array();
    if (fileTree_p->contains("apps") && (*fileTree_p)["apps"].is_array()) {
      for (auto &app : (*fileTree_p)["apps"]) {
        // For apps without a UUID, generate one and remove "launching".
        if (!app.contains("uuid") || app["uuid"].get<std::string>().empty()) {
          app["uuid"] = uuid_util::uuid_t::generate().string();
          app.erase("launching");
          newApps.push_back(std::move(app));
        } else {
          // If an app with the same UUID as the new app is found, replace it.
          if (!new_app_uuid.empty() && app["uuid"].get<std::string>() == new_app_uuid) {
            newApps.push_back(*inputTree_p);
            new_app_uuid.clear();
          } else {
            newApps.push_back(std::move(app));
          }
        }
      }
    }
    // If the new app's UUID has not been merged yet, add it.
    if (!new_app_uuid.empty() && inputTree_p) {
      newApps.push_back(*inputTree_p);
    }
    (*fileTree_p)["apps"] = newApps;
  }

  void migration_v2(nlohmann::json& fileTree) {
    static const int this_version = 2;
    // Determine the current migration version (default to 1 if not present).
    int file_version = 1;
    if (fileTree.contains("version")) {
      try {
        file_version = fileTree["version"].get<int>();
      } catch (const std::exception& e) {
        BOOST_LOG(info) << "Cannot parse apps.json version, treating as v1: " << e.what();
      }
    }

    // If the version is less than this_version, perform legacy conversion.
    if (file_version < this_version) {
      BOOST_LOG(info) << "Migrating app list from v1 to v2...";
      migrate_apps(&fileTree, nullptr);

      // List of keys to convert to booleans.
      std::vector<std::string> boolean_keys = {
        "allow-client-commands",
        "exclude-global-prep-cmd",
        "elevated",
        "auto-detach",
        "wait-all",
        "use-app-identity",
        "per-client-app-identity",
        "virtual-display"
      };

      // List of keys to convert to integers.
      std::vector<std::string> integer_keys = {
        "exit-timeout",
        "scale-factor"
      };

      // Walk through each app and convert legacy string values.
      for (auto &app : fileTree["apps"]) {
        for (const auto &key : boolean_keys) {
          if (app.contains(key)) {
            auto& _key = app[key];
            if (_key.is_string()) {
              std::string s = _key.get<std::string>();
              std::transform(s.begin(), s.end(), s.begin(), ::tolower);  // Normalize to lowercase for comparison
              _key = (s == "true" || s == "on" || s == "yes");
            } else if (_key.is_array()) {
              // Check if the array contains at least one item and interpret the first element
              if (!_key.empty() && _key[0].is_string()) {
                std::string first = _key[0].get<std::string>();
                std::transform(first.begin(), first.end(), first.begin(), ::tolower);  // Normalize
                if (first == "on" || first == "true" || first == "yes") {
                  _key = true;
                } else if (first == "off" || first == "false" || first == "no") {
                  _key = false;
                } else {
                  _key = false;  // Default for unknown values
                }
              } else {
                _key = false;  // Treat empty arrays or non-string first elements as false
              }
            } else {
              // Fallback: Treat truthy/falsey cases
              if (_key.is_boolean()) {
                // Leave booleans as they are
              } else if (_key.is_number()) {
                _key = (_key.get<double>() != 0);  // Non-zero numbers are truthy
              } else if (_key.is_null()) {
                _key = false;  // Null is false
              } else {
                _key = !_key.empty();  // Non-empty objects/arrays are truthy, empty ones are falsey
              }
            }
          }
        }

        for (const auto &key : integer_keys) {
          if (app.contains(key) && app[key].is_string()) {
            std::string s = app[key].get<std::string>();
            app[key] = std::stoi(s);
          }
        }

        // For each entry in the "prep-cmd" array, convert "elevated" if necessary.
        if (app.contains("prep-cmd") && app["prep-cmd"].is_array()) {
          for (auto &prep : app["prep-cmd"]) {
            if (prep.contains("elevated") && prep["elevated"].is_string()) {
              std::string s = prep["elevated"].get<std::string>();
              prep["elevated"] = (s == "true");
            }
          }
        }
      }

      // Update migration version to this_version.
      fileTree["version"] = this_version;

      BOOST_LOG(info) << "Migrated app list from v1 to v2.";
    }
  }

  void migrate(nlohmann::json& fileTree, const std::string& fileName) {
    int last_version = 2;

    int file_version = 0;
    if (fileTree.contains("version")) {
      file_version = fileTree["version"].get<int>();
    }

    if (file_version < last_version) {
      migration_v2(fileTree);
      file_handler::write_file(fileName.c_str(), fileTree.dump(4));
    }
  }

  std::optional<proc::proc_t> parse(const std::string &file_name) {

    // Prepare environment variables.
    auto this_env = boost::this_process::environment();

    std::set<std::string> ids;
    std::vector<proc::ctx_t> apps;
    int i = 0;

    size_t fail_count = 0;
    do {
      // Read the JSON file into a tree.
      nlohmann::json tree;
      try {
        std::string content = file_handler::read_file(file_name.c_str());
        tree = nlohmann::json::parse(content);
      } catch (const std::exception& e) {
        BOOST_LOG(warning) << "Couldn't read apps.json properly! Apps will not be loaded."sv;
        break;
      }

      try {
        migrate(tree, file_name);

        if (tree.contains("env") && tree["env"].is_object()) {
          for (auto &item : tree["env"].items()) {
            this_env[item.key()] = parse_env_val(this_env, item.value().get<std::string>());
          }
        }

        // Ensure the "apps" array exists.
        if (!tree.contains("apps") || !tree["apps"].is_array()) {
          BOOST_LOG(warning) << "No apps were defined in apps.json!!!"sv;
          break;
        }

        // Iterate over each application in the "apps" array.
        for (auto &app_node : tree["apps"]) {
          proc::ctx_t ctx;
          ctx.idx = std::to_string(i);
          ctx.uuid = app_node.at("uuid");

          // Build the list of preparation commands.
          std::vector<proc::cmd_t> prep_cmds;
          bool exclude_global_prep = app_node.value("exclude-global-prep-cmd", false);
          if (!exclude_global_prep) {
            prep_cmds.reserve(config::shell.prep_cmds.size());
            for (auto &prep_cmd : config::shell.prep_cmds) {
              auto do_cmd = parse_env_val(this_env, prep_cmd.do_cmd);
              auto undo_cmd = parse_env_val(this_env, prep_cmd.undo_cmd);
              prep_cmds.emplace_back(
                std::move(do_cmd),
                std::move(undo_cmd),
                std::move(prep_cmd.elevated)
              );
            }
          }
          if (app_node.contains("prep-cmd") && app_node["prep-cmd"].is_array()) {
            for (auto &prep_node : app_node["prep-cmd"]) {
              std::string do_cmd = parse_env_val(this_env, prep_node.value("do", ""));
              std::string undo_cmd = parse_env_val(this_env, prep_node.value("undo", ""));
              bool elevated = prep_node.value("elevated", false);
              prep_cmds.emplace_back(
                std::move(do_cmd),
                std::move(undo_cmd),
                std::move(elevated)
              );
            }
          }

          // Build the list of pause/resume commands.
          std::vector<proc::cmd_t> state_cmds;
          bool exclude_global_state_cmds = app_node.value("exclude-global-state-cmd", false);
          if (!exclude_global_state_cmds) {
            state_cmds.reserve(config::shell.state_cmds.size());
            for (auto &state_cmd : config::shell.state_cmds) {
              auto do_cmd = parse_env_val(this_env, state_cmd.do_cmd);
              auto undo_cmd = parse_env_val(this_env, state_cmd.undo_cmd);
              state_cmds.emplace_back(
                std::move(do_cmd),
                std::move(undo_cmd),
                std::move(state_cmd.elevated)
              );
            }
          }
          if (app_node.contains("state-cmd") && app_node["state-cmd"].is_array()) {
            for (auto &prep_node : app_node["state-cmd"]) {
              std::string do_cmd = parse_env_val(this_env, prep_node.value("do", ""));
              std::string undo_cmd = parse_env_val(this_env, prep_node.value("undo", ""));
              bool elevated = prep_node.value("elevated", false);
              state_cmds.emplace_back(
                std::move(do_cmd),
                std::move(undo_cmd),
                std::move(elevated)
              );
            }
          }

          // Build the list of detached commands.
          std::vector<std::string> detached;
          if (app_node.contains("detached") && app_node["detached"].is_array()) {
            for (auto &detached_val : app_node["detached"]) {
              detached.emplace_back(parse_env_val(this_env, detached_val.get<std::string>()));
            }
          }

          // Process other fields.
          if (app_node.contains("output"))
            ctx.output = parse_env_val(this_env, app_node.value("output", ""));
          std::string name = parse_env_val(this_env, app_node.value("name", ""));
          if (app_node.contains("cmd"))
            ctx.cmd = parse_env_val(this_env, app_node.value("cmd", ""));
          if (app_node.contains("working-dir")) {
            ctx.working_dir = parse_env_val(this_env, app_node.value("working-dir", ""));
    #ifdef _WIN32
            // The working directory, unlike the command itself, should not be quoted.
            boost::erase_all(ctx.working_dir, "\"");
            ctx.working_dir += '\\';
    #endif
          }
          if (app_node.contains("image-path"))
            ctx.image_path = parse_env_val(this_env, app_node.value("image-path", ""));

          ctx.elevated = app_node.value("elevated", false);
          ctx.auto_detach = app_node.value("auto-detach", true);
          ctx.wait_all = app_node.value("wait-all", true);
          ctx.exit_timeout = std::chrono::seconds { app_node.value("exit-timeout", 5) };
          ctx.virtual_display = app_node.value("virtual-display", false);
          ctx.scale_factor = app_node.value("scale-factor", 100);
          ctx.use_app_identity = app_node.value("use-app-identity", false);
          ctx.per_client_app_identity = app_node.value("per-client-app-identity", false);
          ctx.allow_client_commands = app_node.value("allow-client-commands", true);
          ctx.terminate_on_pause = app_node.value("terminate-on-pause", false);
          ctx.gamepad = app_node.value("gamepad", "");

          // Calculate a unique application id.
          auto possible_ids = calculate_app_id(name, ctx.image_path, i++);
          if (ids.count(std::get<0>(possible_ids)) == 0) {
            ctx.id = std::get<0>(possible_ids);
          } else {
            ctx.id = std::get<1>(possible_ids);
          }
          ids.insert(ctx.id);

          ctx.name = std::move(name);
          ctx.prep_cmds = std::move(prep_cmds);
          ctx.state_cmds = std::move(state_cmds);
          ctx.detached = std::move(detached);

          apps.emplace_back(std::move(ctx));
        }

        fail_count = 0;
      } catch (std::exception &e) {
        BOOST_LOG(error) << "Error happened during app loading: "sv << e.what();

        fail_count += 1;

        if (fail_count >= 3) {
          // No hope for recovering
          BOOST_LOG(warning) << "Couldn't parse/migrate apps.json properly! Apps will not be loaded."sv;
          break;
        }

        BOOST_LOG(warning) << "App format is still invalid! Trying to re-migrate the app list..."sv;

        // Always try migrating from scratch when error happened
        tree["version"] = 0;

        try {
          migrate(tree, file_name);
        } catch (std::exception &e) {
          BOOST_LOG(error) << "Error happened during migration: "sv << e.what();
          break;
        }

        this_env = boost::this_process::environment();
        ids.clear();
        apps.clear();
        i = 0;

        continue;
      }

      break;
    } while (fail_count < 3);

    if (fail_count > 0) {
      BOOST_LOG(warning) << "No applications configured, adding fallback Desktop entry.";
      proc::ctx_t ctx;
      ctx.idx = std::to_string(i);
      ctx.uuid = FALLBACK_DESKTOP_UUID; // Placeholder UUID
      ctx.name = "Desktop (fallback)";
      ctx.image_path = parse_env_val(this_env, "desktop-alt.png");
      ctx.virtual_display = false;
      ctx.scale_factor = 100;
      ctx.use_app_identity = false;
      ctx.per_client_app_identity = false;
      ctx.allow_client_commands = false;
      ctx.terminate_on_pause = false;

      ctx.elevated = false;
      ctx.auto_detach = true;
      ctx.wait_all = false; // Desktop doesn't have a specific command to wait for
      ctx.exit_timeout = 5s;

      // Calculate unique ID
      auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
      if (ids.count(std::get<0>(possible_ids)) == 0) {
        // Avoid using index to generate id if possible
        ctx.id = std::get<0>(possible_ids);
      } else {
        // Fallback to include index on collision
        ctx.id = std::get<1>(possible_ids);
      }
      ids.insert(ctx.id);

      apps.emplace_back(std::move(ctx));
    }

    // Virtual Display entry
  #ifdef _WIN32
    if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
      proc::ctx_t ctx;
      ctx.idx = std::to_string(i);
      ctx.uuid = VIRTUAL_DISPLAY_UUID;
      ctx.name = "Virtual Display";
      ctx.image_path = parse_env_val(this_env, "virtual_desktop.png");
      ctx.virtual_display = true;
      ctx.scale_factor = 100;
      ctx.use_app_identity = false;
      ctx.per_client_app_identity = false;
      ctx.allow_client_commands = false;
      ctx.terminate_on_pause = false;

      ctx.elevated = false;
      ctx.auto_detach = true;
      ctx.wait_all = false;
      ctx.exit_timeout = 5s;

      auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
      if (ids.count(std::get<0>(possible_ids)) == 0) {
        // Avoid using index to generate id if possible
        ctx.id = std::get<0>(possible_ids);
      }
      else {
        // Fallback to include index on collision
        ctx.id = std::get<1>(possible_ids);
      }
      ids.insert(ctx.id);

      apps.emplace_back(std::move(ctx));
    }
  #endif

    if (config::input.enable_input_only_mode) {
      // Input Only entry
      {
        proc::ctx_t ctx;
        ctx.idx = std::to_string(i);
        ctx.uuid = REMOTE_INPUT_UUID;
        ctx.name = "Remote Input";
        ctx.image_path = parse_env_val(this_env, "input_only.png");
        ctx.virtual_display = false;
        ctx.scale_factor = 100;
        ctx.use_app_identity = false;
        ctx.per_client_app_identity = false;
        ctx.allow_client_commands = false;
        ctx.terminate_on_pause = true; // There's no need to keep an active input only session ongoing

        ctx.elevated = false;
        ctx.auto_detach = true;
        ctx.wait_all = true;
        ctx.exit_timeout = 5s;

        auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
        if (ids.count(std::get<0>(possible_ids)) == 0) {
          // Avoid using index to generate id if possible
          ctx.id = std::get<0>(possible_ids);
        }
        else {
          // Fallback to include index on collision
          ctx.id = std::get<1>(possible_ids);
        }
        ids.insert(ctx.id);

        input_only_app_id_str = ctx.id;
        input_only_app_id = util::from_view(ctx.id);

        apps.emplace_back(std::move(ctx));
      }

      // Terminate entry
      {
        proc::ctx_t ctx;
        ctx.idx = std::to_string(i);
        ctx.uuid = TERMINATE_APP_UUID;
        ctx.name = "Terminate";
        ctx.image_path = parse_env_val(this_env, "terminate.png");
        ctx.virtual_display = false;
        ctx.scale_factor = 100;
        ctx.use_app_identity = false;
        ctx.per_client_app_identity = false;
        ctx.allow_client_commands = false;
        ctx.terminate_on_pause = false;

        ctx.elevated = false;
        ctx.auto_detach = true;
        ctx.wait_all = true;
        ctx.exit_timeout = 5s;

        auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
        if (ids.count(std::get<0>(possible_ids)) == 0) {
          // Avoid using index to generate id if possible
          ctx.id = std::get<0>(possible_ids);
        }
        else {
          // Fallback to include index on collision
          ctx.id = std::get<1>(possible_ids);
        }
        // ids.insert(ctx.id);

        terminate_app_id_str = ctx.id;
        terminate_app_id = util::from_view(ctx.id);

        apps.emplace_back(std::move(ctx));
      }
    }

    return proc::proc_t {
      std::move(this_env),
      std::move(apps)
    };
  }

  void refresh(const std::string &file_name, bool needs_terminate) {
    if (needs_terminate) {
      proc.terminate(false, false);
    }

  #ifdef _WIN32
    size_t fail_count = 0;
    while (fail_count < 5 && vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
      initVDisplayDriver();
      if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
        break;
      }

      fail_count += 1;
      std::this_thread::sleep_for(1s);
    }
  #endif

    auto proc_opt = proc::parse(file_name);

    if (proc_opt) {
  #ifdef _WIN32
      std::lock_guard<std::recursive_mutex> vdisplay_lock(vdisplay_mutex);
  #endif
      proc = std::move(*proc_opt);
    }
  }
}  // namespace proc
