/**
 * @file src/system_tray.h
 * @brief Declarations for the system tray icon and notification system.
 */
#pragma once

/**
 * @brief Handles the system tray icon and notification system.
 */
namespace system_tray {
  /**
   * @brief Callback for opening the UI from the system tray.
   * @param item The tray menu item.
   */
  void tray_open_ui_cb([[maybe_unused]] struct tray_menu *item);


  void tray_force_stop_cb(struct tray_menu *item);

  /**
   * @brief Callback for resetting display device configuration.
   * @param item The tray menu item.
   */
  void tray_reset_display_device_config_cb([[maybe_unused]] struct tray_menu *item);

  /**
   * @brief Callback for restarting Shell from the system tray.
   * @param item The tray menu item.
   */
  void tray_restart_cb([[maybe_unused]] struct tray_menu *item);

  /**
   * @brief Callback for exiting Shell from the system tray.
   * @param item The tray menu item.
   */
  void tray_quit_cb([[maybe_unused]] struct tray_menu *item);

  /**
   * @brief Initializes the system tray without starting a loop.
   * @return 0 if initialization was successful, non-zero otherwise.
   */
  int init_tray();

  /**
   * @brief Processes a single tray event iteration.
   * @return 0 if processing was successful, non-zero otherwise.
   */
  int process_tray_events();

  /**
   * @brief Exit the system tray.
   * @return 0 after exiting the system tray.
   */
  int end_tray();

  /**
   * @brief Sets the tray icon in playing mode and spawns the appropriate notification
   * @param app_name The started application name
   */
  void update_tray_playing(std::string app_name);

  /**
   * @brief Sets the tray icon in pausing mode (stream stopped but app running) and spawns the appropriate notification
   * @param app_name The paused application name
   */
  void update_tray_pausing(std::string app_name);

  /**
   * @brief Sets the tray icon in stopped mode (app and stream stopped) and spawns the appropriate notification
   * @param app_name The started application name
   */
  void update_tray_stopped(std::string app_name);

  void
  update_tray_launch_error(std::string app_name, int exit_code);

  /**
   * @brief Spawns a notification for PIN Pairing. Clicking it opens the PIN Web UI Page
   */
  void update_tray_require_pin();

  void update_tray_paired(std::string device_name);

  /**
   * @brief Shell: update_tray_require_pin() without waiting for the tray thread, which shows the
   * notice within about 50 ms. Repeated requests until then make one notice.
   */
  void queue_require_pin();

  /**
   * @brief Shell: update_tray_paired() without waiting for the tray thread.
   * @param device_name The name of the paired device.
   */
  void queue_paired(std::string device_name);

  void update_tray_client_connected(std::string client_name);

  /**
   * @brief Shell: say the virtual display was removed and the desktop is on the monitors again.
   * @param manual true when asked from the tray or the web UI, false when a monitor turned on.
   */
  void update_tray_vdisplay_released(bool manual);

  /**
   * @brief Shell: say a release asked from the host could not remove the virtual display.
   */
  void update_tray_vdisplay_release_failed();
  /**
   * @brief Initializes and runs the system tray in a separate thread.
   * @return 0 if initialization was successful, non-zero otherwise.
   */
  int init_tray_threaded();

  /**
   * @brief Stops the threaded system tray and waits for the thread to finish.
   * @return 0 after stopping the threaded tray.
   */
  int end_tray_threaded();
}  // namespace system_tray
