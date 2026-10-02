/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <string>
#include <utility>
#include <vector>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "audio.h"
#include "crypto.h"
#include "video.h"

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9;
  constexpr auto CONTROL_PORT = 10;
  constexpr auto AUDIO_STREAM_PORT = 11;

  struct session_t;

  struct config_t {
    audio::config_t audio;
    video::config_t monitor;

    int packetsize;
    int minRequiredFecPackets;
    int mlFeatureFlags;
    int controlProtocolType;
    int audioQosType;
    int videoQosType;

    uint32_t encryptionFlagsEnabled;

    std::optional<int> gcmap;

    // Shell: rate (Mbps) this session's video frames are paced at, and whether it was
    // derived from the session bitrate (video_pacing_mbps = 0) rather than configured.
    int videoPacingMbps;
    bool videoPacingAuto;

    // Shell: client-requested bitrate after max_bitrate (and warp), for the session history.
    int streamBitrateKbps;

    // Shell: warp factor applied to the bitrate at setup (1 without warp), for live changes.
    int bitrateWarpFactor = 1;

    // Shell: encoding bitrate / stream bitrate at setup (FEC, audio and packet overhead taken
    // off), applied to live bitrate changes. Fixed for the session so changes do not drift.
    double bitrateEncodeRatio = 1.0;

    // videoPacingMbps and streamBitrateKbps can change during the stream (live bitrate) while
    // other threads read them: access them through std::atomic_ref.
  };

  namespace session {
    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session);
    std::string uuid(const session_t& session);
    std::uint64_t alloc_order(const session_t &session);  // higher: allocated later
    bool uuid_match(const session_t& session, const std::string_view& uuid);
    bool update_device_info(session_t& session, const std::string& name, const crypto::PERM& newPerm);
    int start(session_t &session, const std::string &addr_string);
    void stop(session_t &session);
    void graceful_stop(session_t& session);
    void join(session_t &session);
    state_e state(session_t &session);
    inline bool send(session_t& session, const std::string_view &payload);

    /**
     * @brief Note why a session ends, for the session history. The first reason set wins.
     * @param reason A string literal: "disconnect", "timeout", "host" or "app_exit".
     */
    void set_end_reason(session_t &session, const char *reason);

    /**
     * @brief Shell: change the bitrate of a running stream without restarting it.
     * @param client_kbps Bitrate the client asked for (before FEC and overhead adjustment).
     * @return The new encoding bitrate in kbps, or 0 if the session is not streaming.
     */
    int change_bitrate(session_t &session, int client_kbps);

    /**
     * @brief Shell: live figures of a session for the web dashboard, as a JSON object.
     */
    std::string live_info_json(const session_t &session);
  }  // namespace session

  // Shell: every finished session is appended as one JSON object per line to
  // session_history.jsonl in the config folder (next to shell_state.json).
  namespace session_history {
    /**
     * @brief Read the session history, oldest line first. Never throws; errors give an empty list.
     */
    std::vector<std::string> read_lines();
  }  // namespace session_history
}  // namespace stream
