/**
 * @file src/platform/windows/misc.h
 * @brief Miscellaneous declarations for Windows.
 */
#pragma once

// standard includes
#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>

// platform includes
#include <Windows.h>
#include <winnt.h>

namespace platf {
  void print_status(const std::string_view &prefix, HRESULT status);
  HDESK syncThreadDesktop();

  int64_t qpc_counter();

  std::chrono::nanoseconds qpc_time_difference(int64_t performance_counter1, int64_t performance_counter2);

  /**
   * @brief Convert a UTF-8 string into a UTF-16 wide string.
   * @param string The UTF-8 string.
   * @return The converted UTF-16 wide string.
   */
  std::wstring from_utf8(const std::string_view &string);

  /**
   * @brief Convert a UTF-16 wide string into a UTF-8 string.
   * @param string The UTF-16 wide string.
   * @return The converted UTF-8 string.
   */
  std::string to_utf8(const std::wstring_view &string);

  /**
   * @brief Whether this process runs as the SYSTEM account (the normal service setup).
   */
  bool is_running_as_system();

  /**
   * @brief Primary token of the user logged on to the active console session.
   * @param elevated Request the linked elevated token when the user is an administrator.
   * @return The token (close it with CloseHandle), or nullptr if there is no active user session.
   */
  HANDLE retrieve_users_token(bool elevated);

  /**
   * @brief Run the callback while impersonating the given user on the calling thread.
   * @return An error if impersonation could not start; the callback is not run in that case.
   */
  std::error_code impersonate_current_user(HANDLE user_token, std::function<void()> callback);

  /**
   * @brief Shell: start the new instance a standalone restart() asked for, if it has not started yet.
   * restart() registers this with atexit; main() calls it itself when it ends the process early.
   */
  void restart_if_requested();
}  // namespace platf
