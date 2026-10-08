/**
 * @file src/platform/windows/clipboard_helper.h
 * @brief Shell: runs tools\shellclip.exe as the signed-in user and sends it the clipboard requests.
 */
#pragma once

#include <memory>

#include "src/platform/common.h"

namespace platf::clipboard::helper {
  /**
   * @brief Starts the thread that keeps the helper running while a user is signed in at the console
   * (started again, with a growing delay, when it exits; stopped and started again when the console
   * session changes). Does nothing but record "not-needed" when Shell does not run as SYSTEM.
   * @return A guard that stops the helper and the thread when it is destroyed.
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> start();
}  // namespace platf::clipboard::helper
