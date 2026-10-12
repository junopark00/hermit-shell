/**
 * @file src/webhook.h
 * @brief Shell: webhook notifications (docs/features.md "Webhooks"). Off unless `webhook_url` is
 * set; events are queued here and sent by one background thread, so a slow or unreachable receiver
 * never holds up pairing, streaming or the web UI.
 */
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace webhook {
  /**
   * @brief Read the webhook settings and the secret, validate the URL and start the delivery
   * thread when a URL is configured. Called once after the configuration is parsed. An invalid URL
   * is logged once and leaves webhooks off.
   */
  void start();

  /**
   * @brief Stop the delivery thread; an event being sent is given a few seconds to finish, the
   * rest of the queue is dropped.
   */
  void stop();

  /**
   * @brief Queue an event. Returns at once; nothing is sent from the caller's thread. Events whose
   * group is not in `webhook_events` and all events while webhooks are off are dropped silently.
   * @param event `pairing.succeeded`, `stream.ended`, `test`, ...
   * @param fields the event's own fields, added after `event`, `time` and `host`
   * @param client_address the client's IP; added as `client_ip` only with `webhook_include_client_ip`
   */
  void enqueue(const std::string &event, nlohmann::ordered_json fields, const std::string &client_address = {});

  void pairing_succeeded(const std::string &client_name, const std::string &client_address);
  void pairing_failed(const std::string &client_name, const std::string &reason, const std::string &client_address);
  void app_started(const std::string &app_name);
  void app_ended(const std::string &app_name);

  /**
   * @brief Store the shared secret (`webhook_secret` in the credentials folder) and use it from the
   * next event on. An empty secret removes the file and turns the signature off.
   * @return an empty string on success, else the reason
   */
  std::string save_secret(const std::string &secret);

  /**
   * @brief Queue a `test` event. @return an empty string when queued, else why not (webhooks off).
   */
  std::string send_test();

  /**
   * @brief State for the web UI: enabled, the URL problem if any, whether a secret is set, queue
   * counters and the last delivery result.
   */
  nlohmann::json status();
}  // namespace webhook
