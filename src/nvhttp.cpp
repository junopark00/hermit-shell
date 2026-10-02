/**
 * @file src/nvhttp.cpp
 * @brief Definitions for the nvhttp (GameStream) server.
 */
// macros
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <charconv>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

// lib includes
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <Simple-Web-Server/server_http.hpp>

// local includes
#include "config.h"
#include "display_device.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "stream.h"
#include "system_tray.h"
#include "utility.h"
#include "uuid.h"
#include "video.h"
#include "zwpad.h"

#ifdef _WIN32
  #include <pthread.h>  // pthread_gethandle: std::thread is a winpthreads thread under MinGW

  #include "platform/windows/clipboard.h"
  #include "platform/windows/misc.h"
  #include "platform/windows/utils.h"
  #include "platform/windows/virtual_display.h"
#endif

using namespace std::literals;

namespace nvhttp {

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  using p_named_cert_t = crypto::p_named_cert_t;
  using PERM = crypto::PERM;

  struct client_t {
    std::vector<p_named_cert_t> named_devices;
  };

  struct pair_session_t;

  crypto::cert_chain_t cert_chain;

  class ShellHTTPSServer: public SimpleWeb::ServerBase<ShellHTTPS> {
  public:
    ShellHTTPSServer(const std::string &certification_file, const std::string &private_key_file):
        ServerBase<ShellHTTPS>::ServerBase(443),
        context(boost::asio::ssl::context::tls_server) {
      // Disabling TLS 1.0 and 1.1 (see RFC 8996)
      context.set_options(boost::asio::ssl::context::no_tlsv1);
      context.set_options(boost::asio::ssl::context::no_tlsv1_1);
      context.use_certificate_chain_file(certification_file);
      context.use_private_key_file(private_key_file, boost::asio::ssl::context::pem);
    }

    std::function<bool(std::shared_ptr<Request>, SSL*)> verify;
    std::function<void(std::shared_ptr<Response>, std::shared_ptr<Request>)> on_verify_failed;

  protected:
    boost::asio::ssl::context context;

    void after_bind() override {
      if (verify) {
        context.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert | boost::asio::ssl::verify_client_once);
        context.set_verify_callback([](int verified, boost::asio::ssl::verify_context &ctx) {
          // To respond with an error message, a connection must be established
          return 1;
        });
      }
    }

    // This is Server<HTTPS>::accept() with SSL validation support added
    void accept() override {
      auto connection = create_connection(*io_service, context);

      acceptor->async_accept(connection->socket->lowest_layer(), [this, connection](const SimpleWeb::error_code &ec) {
        auto lock = connection->handler_runner->continue_lock();
        if (!lock) {
          return;
        }

        if (ec != SimpleWeb::error::operation_aborted) {
          this->accept();
        }

        auto session = std::make_shared<Session>(config.max_request_streambuf_size, connection);

        if (!ec) {
          boost::asio::ip::tcp::no_delay option(true);
          SimpleWeb::error_code ec;
          session->connection->socket->lowest_layer().set_option(option, ec);

          session->connection->set_timeout(config.timeout_request);
          session->connection->socket->async_handshake(boost::asio::ssl::stream_base::server, [this, session](const SimpleWeb::error_code &ec) {
            session->connection->cancel_timeout();
            auto lock = session->connection->handler_runner->continue_lock();
            if (!lock) {
              return;
            }
            if (!ec) {
              if (verify && !verify(session->request, session->connection->socket->native_handle())) {
                this->write(session, on_verify_failed);
              } else {
                this->read(session);
              }
            } else if (this->on_error) {
              this->on_error(session->request, ec);
            }
          });
        } else if (this->on_error) {
          this->on_error(session->request, ec);
        }
      });
    }
  };

  using https_server_t = ShellHTTPSServer;
  using http_server_t = SimpleWeb::Server<SimpleWeb::HTTP>;

  struct conf_intern_t {
    std::string servercert;
    std::string pkey;
  } conf_intern;

  // uniqueID, session
  std::unordered_map<std::string, pair_session_t> map_id_sess;
  // Shell: guards map_id_sess, which the HTTP and HTTPS server threads (pair, unpair) and the web UI
  // thread (pin) all use. Only those entry points and start() take it; everything they call
  // (the pairing phases, fail_pair, remove_session, answer_waiting_request, drop_stale_sessions)
  // expects it held and never takes it, so nothing locks it twice.
  std::mutex map_id_sess_mutex;
  client_t client_root;
  std::atomic<uint32_t> session_id_counter;

  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<ShellHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<ShellHTTPS>::Request>;
  using resp_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>;
  using req_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request>;

  enum class op_e {
    ADD,  ///< Add certificate
    REMOVE  ///< Remove certificate
  };

  std::string get_arg(const args_t &args, const char *name, const char *default_value) {
    auto it = args.find(name);
    if (it == std::end(args)) {
      if (default_value != nullptr) {
        return std::string(default_value);
      }

      throw std::out_of_range(name);
    }
    return it->second;
  }

  // Helper function to extract command entries from a JSON object.
  cmd_list_t extract_command_entries(const nlohmann::json& j, const std::string& key) {
    cmd_list_t commands;

    // Check if the key exists in the JSON.
    if (j.contains(key)) {
      // Ensure that the value for the key is an array.
      try {
        for (const auto& item : j.at(key)) {
          try {
            // Extract "cmd" and "elevated" fields from the JSON object.
            std::string cmd = item.at("cmd").get<std::string>();
            bool elevated = util::get_non_string_json_value<bool>(item, "elevated", false);

            // Add the command entry to the list.
            commands.push_back({cmd, elevated});
          } catch (const std::exception& e) {
            BOOST_LOG(warning) << "Error parsing command entry: " << e.what();
          }
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Error retrieving key \"" << key << "\": " << e.what();
      }
    } else {
      BOOST_LOG(debug) << "Key \"" << key << "\" not found in the JSON.";
    }

    return commands;
  }

  void save_state() {
    nlohmann::json root = nlohmann::json::object();
    // If the state file exists, try to read it.
    if (fs::exists(config::nvhttp.file_state)) {
      try {
        std::ifstream in(config::nvhttp.file_state);
        in >> root;
      } catch (std::exception &e) {
        BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();
        return;
      }
    }

    // Erase any previous "root" key.
    root.erase("root");

    // Create a new "root" object and set the unique id.
    root["root"] = nlohmann::json::object();
    root["root"]["uniqueid"] = http::unique_id;

    client_t &client = client_root;
    nlohmann::json named_cert_nodes = nlohmann::json::array();

    std::unordered_set<std::string> unique_certs;
    std::unordered_map<std::string, int> name_counts;

    for (auto &named_cert_p : client.named_devices) {
      // Only add each unique certificate once.
      if (unique_certs.insert(named_cert_p->cert).second) {
        nlohmann::json named_cert_node = nlohmann::json::object();
        std::string base_name = named_cert_p->name;
        // Remove any pending id suffix (e.g., " (2)") if present.
        size_t pos = base_name.find(" (");
        if (pos != std::string::npos) {
          base_name = base_name.substr(0, pos);
        }
        int count = name_counts[base_name]++;
        std::string final_name = base_name;
        if (count > 0) {
          final_name += " (" + std::to_string(count + 1) + ")";
        }
        named_cert_node["name"] = final_name;
        named_cert_node["cert"] = named_cert_p->cert;
        named_cert_node["uuid"] = named_cert_p->uuid;
        named_cert_node["display_mode"] = named_cert_p->display_mode;
        named_cert_node["perm"] = static_cast<uint32_t>(named_cert_p->perm);
        named_cert_node["enable_legacy_ordering"] = named_cert_p->enable_legacy_ordering;
        named_cert_node["allow_client_commands"] = named_cert_p->allow_client_commands;
        named_cert_node["always_use_virtual_display"] = named_cert_p->always_use_virtual_display;

        // Add "do" commands if available.
        if (!named_cert_p->do_cmds.empty()) {
          nlohmann::json do_cmds_node = nlohmann::json::array();
          for (const auto &cmd : named_cert_p->do_cmds) {
            do_cmds_node.push_back(crypto::command_entry_t::serialize(cmd));
          }
          named_cert_node["do"] = do_cmds_node;
        }

        // Add "undo" commands if available.
        if (!named_cert_p->undo_cmds.empty()) {
          nlohmann::json undo_cmds_node = nlohmann::json::array();
          for (const auto &cmd : named_cert_p->undo_cmds) {
            undo_cmds_node.push_back(crypto::command_entry_t::serialize(cmd));
          }
          named_cert_node["undo"] = undo_cmds_node;
        }

        named_cert_nodes.push_back(named_cert_node);
      }
    }

    root["root"]["named_devices"] = named_cert_nodes;

    try {
      std::ofstream out(config::nvhttp.file_state);
      out << root.dump(4);  // Pretty-print with an indent of 4 spaces.
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't write "sv << config::nvhttp.file_state << ": "sv << e.what();
      return;
    }
  }

  void load_state() {
    if (!fs::exists(config::nvhttp.file_state)) {
      BOOST_LOG(info) << "File "sv << config::nvhttp.file_state << " doesn't exist"sv;
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }

    nlohmann::json tree;
    try {
      std::ifstream in(config::nvhttp.file_state);
      in >> tree;
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();
      return;
    }

    // Check that the file contains a "root.uniqueid" value.
    if (!tree.contains("root") || !tree["root"].contains("uniqueid")) {
      http::uuid = uuid_util::uuid_t::generate();
      http::unique_id = http::uuid.string();
      return;
    }

    std::string uid = tree["root"]["uniqueid"];
    http::uuid = uuid_util::uuid_t::parse(uid);
    http::unique_id = uid;

    nlohmann::json root = tree["root"];
    client_t client;  // Local client to load into

    // Import from the old format if available.
    if (root.contains("devices")) {
      for (auto &device_node : root["devices"]) {
        // For each device, if there is a "certs" array, add a named certificate.
        if (device_node.contains("certs")) {
          for (auto &el : device_node["certs"]) {
            auto named_cert_p = std::make_shared<crypto::named_cert_t>();
            named_cert_p->name = "";
            named_cert_p->cert = el.get<std::string>();
            named_cert_p->uuid = uuid_util::uuid_t::generate().string();
            named_cert_p->display_mode = "";
            named_cert_p->perm = PERM::_all;
            named_cert_p->enable_legacy_ordering = true;
            named_cert_p->allow_client_commands = true;
            named_cert_p->always_use_virtual_display = false;
            client.named_devices.emplace_back(named_cert_p);
          }
        }
      }
    }

    // Import from the new format.
    if (root.contains("named_devices")) {
      for (auto &el : root["named_devices"]) {
        auto named_cert_p = std::make_shared<crypto::named_cert_t>();
        named_cert_p->name = el.value("name", "");
        named_cert_p->cert = el.value("cert", "");
        named_cert_p->uuid = el.value("uuid", "");
        named_cert_p->display_mode = el.value("display_mode", "");
        named_cert_p->perm = (PERM)(util::get_non_string_json_value<uint32_t>(el, "perm", (uint32_t)PERM::_all)) & PERM::_all;
        named_cert_p->enable_legacy_ordering = el.value("enable_legacy_ordering", true);
        named_cert_p->allow_client_commands = el.value("allow_client_commands", true);
        named_cert_p->always_use_virtual_display = el.value("always_use_virtual_display", false);
        // Load command entries for "do" and "undo" keys.
        named_cert_p->do_cmds = extract_command_entries(el, "do");
        named_cert_p->undo_cmds = extract_command_entries(el, "undo");
        client.named_devices.emplace_back(named_cert_p);
      }
    }

    // Clear any existing certificate chain and add the imported certificates.
    cert_chain.clear();
    for (auto &named_cert : client.named_devices) {
      cert_chain.add(named_cert);
    }

    client_root = client;
  }

  /// Shell: called under map_id_sess_mutex (clientpairingsecret), which keeps two pairings that end
  /// at once on the HTTP and HTTPS threads from changing client_root together; it waits for no other
  /// thread, only writes the small state file. The tray is told by pair once the lock is released.
  void add_authorized_client(const p_named_cert_t& named_cert_p) {
    client_t &client = client_root;
    client.named_devices.push_back(named_cert_p);

    if (!config::shell.flags[config::flag::FRESH_STATE]) {
      save_state();
      load_state();
    }
  }

  std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session(bool host_audio, bool input_only, const args_t &args, const crypto::named_cert_t* named_cert_p) {
    auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();

    launch_session->id = ++session_id_counter;

    // If launched from client
    if (named_cert_p->uuid != http::unique_id) {
      auto rikey = util::from_hex_vec(get_arg(args, "rikey"), true);
      std::copy(rikey.cbegin(), rikey.cend(), std::back_inserter(launch_session->gcm_key));

      launch_session->host_audio = host_audio;

      // Encrypted RTSP is enabled with client reported corever >= 1
      auto corever = util::from_view(get_arg(args, "corever", "0"));
      if (corever >= 1) {
        launch_session->rtsp_cipher = crypto::cipher::gcm_t {
          launch_session->gcm_key, false
        };
        launch_session->rtsp_iv_counter = 0;
      }
      launch_session->rtsp_url_scheme = launch_session->rtsp_cipher ? "rtspenc://"s : "rtsp://"s;

      // Generate the unique identifiers for this connection that we will send later during RTSP handshake
      unsigned char raw_payload[8];
      RAND_bytes(raw_payload, sizeof(raw_payload));
      launch_session->av_ping_payload = util::hex_vec(raw_payload);
      RAND_bytes((unsigned char *) &launch_session->control_connect_data, sizeof(launch_session->control_connect_data));

      launch_session->iv.resize(16);
      uint32_t prepend_iv = util::endian::big<uint32_t>(util::from_view(get_arg(args, "rikeyid")));
      auto prepend_iv_p = (uint8_t *) &prepend_iv;
      std::copy(prepend_iv_p, prepend_iv_p + sizeof(prepend_iv), std::begin(launch_session->iv));
    }

    std::stringstream mode;
    if (named_cert_p->display_mode.empty()) {
      auto mode_str = get_arg(args, "mode", config::video.fallback_mode.c_str());
      mode = std::stringstream(mode_str);
      BOOST_LOG(info) << "Display mode for client ["sv << named_cert_p->name <<"] requested to ["sv << mode_str << ']';
    } else {
      mode = std::stringstream(named_cert_p->display_mode);
      BOOST_LOG(info) << "Display mode for client ["sv << named_cert_p->name <<"] overriden to ["sv << named_cert_p->display_mode << ']';
    }

    // Split mode by the char "x", to populate width/height/fps
    int x = 0;
    std::string segment;
    while (std::getline(mode, segment, 'x')) {
      if (x == 0) {
        launch_session->width = atoi(segment.c_str());
      }
      if (x == 1) {
        launch_session->height = atoi(segment.c_str());
      }
      if (x == 2) {
        auto fps = atof(segment.c_str());
        if (fps < 1000) {
          fps *= 1000;
        };
        launch_session->fps = (int)fps;
        break;
      }
      x++;
    }

    // Parsing have failed or missing components
    if (x != 2) {
      launch_session->width = 1920;
      launch_session->height = 1080;
      launch_session->fps = 60000; // 60fps * 1000 denominator
    }

    launch_session->device_name = named_cert_p->name.empty() ? "ShellDisplay"s : named_cert_p->name;
    launch_session->unique_id = named_cert_p->uuid;
    launch_session->perm = named_cert_p->perm;
    launch_session->enable_sops = util::from_view(get_arg(args, "sops", "0"));
    launch_session->surround_info = util::from_view(get_arg(args, "surroundAudioInfo", "196610"));
    launch_session->surround_params = (get_arg(args, "surroundParams", ""));
    launch_session->gcmap = util::from_view(get_arg(args, "gcmap", "0"));
    launch_session->enable_hdr = util::from_view(get_arg(args, "hdrMode", "0"));
    launch_session->virtual_display = util::from_view(get_arg(args, "virtualDisplay", "0")) || named_cert_p->always_use_virtual_display;
    launch_session->scale_factor = util::from_view(get_arg(args, "scaleFactor", "100"));

    launch_session->client_do_cmds = named_cert_p->do_cmds;
    launch_session->client_undo_cmds = named_cert_p->undo_cmds;

    launch_session->input_only = input_only;

    return launch_session;
  }

  /**
   * @brief Shell: answers the getservercert request a session keeps waiting for the PIN, if there is
   * one, with a failed pairing. A session dropped with its request unanswered left the client waiting
   * until the connection timed out, and it then reported a closed connection instead of the reason.
   * Expects map_id_sess_mutex held.
   */
  void answer_waiting_request(pair_session_t &sess, const std::string &why) {
    auto reply = [&](auto &response) {
      if (!response) {
        return;  // already answered (moved out by pin) or never waiting
      }
      pt::ptree tree;
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", why);
      std::ostringstream data;
      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
      response.reset();  // releasing it sends the reply
    };
    auto &response = sess.async_insert_pin.response;
    if (response.has_left()) {
      reply(response.left());
    } else if (response.has_right()) {
      reply(response.right());
    }
  }

  /// Shell: erases the session of uniqueid, answering its waiting request first. Expects
  /// map_id_sess_mutex held. Returns whether there was one.
  bool erase_session(const std::string &unique_id, const std::string &why) {
    auto it = map_id_sess.find(unique_id);
    if (it == std::end(map_id_sess)) {
      return false;
    }
    answer_waiting_request(it->second, why);
    map_id_sess.erase(it);
    return true;
  }

  /// Shell: how long a client waits for the PIN. Its getservercert request is cut by the HTTP
  /// server's timeout_content (set to this in start(); SimpleWeb's default), so a session older
  /// than this no longer has a client to answer.
  constexpr auto pair_pin_timeout = std::chrono::seconds(300);

  /// Shell: drops the sessions still waiting for a PIN after pair_pin_timeout; pin() would otherwise
  /// hand the PIN to a client that gave up. Expects map_id_sess_mutex held.
  void drop_stale_sessions() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = std::begin(map_id_sess); it != std::end(map_id_sess);) {
      if (it->second.last_phase == PAIR_PHASE::NONE && now - it->second.created >= pair_pin_timeout) {
        BOOST_LOG(info) << "Pairing request of [" << it->second.client.name << "] expired: no PIN was entered within 5 minutes";
        answer_waiting_request(it->second, "No PIN was entered in time");
        it = map_id_sess.erase(it);
      } else {
        ++it;
      }
    }
  }

  void remove_session(const pair_session_t &sess) {
    // Shell: a copy, since the key would otherwise live in the session being erased
    const auto unique_id = sess.client.uniqueID;
    erase_session(unique_id, "Pairing ended");
  }

  void fail_pair(pair_session_t &sess, pt::ptree &tree, const std::string status_msg) {
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", 400);
    tree.put("root.<xmlattr>.status_message", status_msg);
    // Security measure, delete the session when something went wrong and force a re-pair.
    // Shell: a request it still keeps waiting for the PIN gets the same reason.
    const auto unique_id = sess.client.uniqueID;
    erase_session(unique_id, status_msg);
    BOOST_LOG(warning) << "Pair attempt failed due to " << status_msg;
  }

  void getservercert(pair_session_t &sess, pt::ptree &tree, const std::string &pin) {
    if (sess.last_phase != PAIR_PHASE::NONE) {
      fail_pair(sess, tree, "Out of order call to getservercert");
      return;
    }
    sess.last_phase = PAIR_PHASE::GETSERVERCERT;

    if (sess.async_insert_pin.salt.size() < 32) {
      fail_pair(sess, tree, "Salt too short");
      return;
    }

    std::string_view salt_view {sess.async_insert_pin.salt.data(), 32};

    auto salt = util::from_hex<std::array<uint8_t, 16>>(salt_view, true);

    auto key = crypto::gen_aes_key(salt, pin);
    sess.cipher_key = std::make_unique<crypto::aes_t>(key);

    tree.put("root.paired", 1);
    tree.put("root.plaincert", util::hex_vec(conf_intern.servercert, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void clientchallenge(pair_session_t &sess, pt::ptree &tree, const std::string &challenge) {
    if (sess.last_phase != PAIR_PHASE::GETSERVERCERT) {
      fail_pair(sess, tree, "Out of order call to clientchallenge");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTCHALLENGE;

    if (!sess.cipher_key) {
      fail_pair(sess, tree, "Cipher key not set");
      return;
    }
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    std::vector<uint8_t> decrypted;
    cipher.decrypt(challenge, decrypted);

    auto x509 = crypto::x509(conf_intern.servercert);
    auto sign = crypto::signature(x509);
    auto serversecret = crypto::rand(16);

    decrypted.insert(std::end(decrypted), std::begin(sign), std::end(sign));
    decrypted.insert(std::end(decrypted), std::begin(serversecret), std::end(serversecret));

    auto hash = crypto::hash({(char *) decrypted.data(), decrypted.size()});
    auto serverchallenge = crypto::rand(16);

    std::string plaintext;
    plaintext.reserve(hash.size() + serverchallenge.size());

    plaintext.insert(std::end(plaintext), std::begin(hash), std::end(hash));
    plaintext.insert(std::end(plaintext), std::begin(serverchallenge), std::end(serverchallenge));

    std::vector<uint8_t> encrypted;
    cipher.encrypt(plaintext, encrypted);

    sess.serversecret = std::move(serversecret);
    sess.serverchallenge = std::move(serverchallenge);

    tree.put("root.paired", 1);
    tree.put("root.challengeresponse", util::hex_vec(encrypted, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void serverchallengeresp(pair_session_t &sess, pt::ptree &tree, const std::string &encrypted_response) {
    if (sess.last_phase != PAIR_PHASE::CLIENTCHALLENGE) {
      fail_pair(sess, tree, "Out of order call to serverchallengeresp");
      return;
    }
    sess.last_phase = PAIR_PHASE::SERVERCHALLENGERESP;

    if (!sess.cipher_key || sess.serversecret.empty()) {
      fail_pair(sess, tree, "Cipher key or serversecret not set");
      return;
    }

    std::vector<uint8_t> decrypted;
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    cipher.decrypt(encrypted_response, decrypted);

    sess.clienthash = std::move(decrypted);

    auto serversecret = sess.serversecret;
    auto sign = crypto::sign256(crypto::pkey(conf_intern.pkey), serversecret);

    serversecret.insert(std::end(serversecret), std::begin(sign), std::end(sign));

    tree.put("root.pairingsecret", util::hex_vec(serversecret, true));
    tree.put("root.paired", 1);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  /// Shell: paired_name gets the name of the device once it is paired, for the tray notification.
  void clientpairingsecret(pair_session_t &sess, pt::ptree &tree, const std::string &client_pairing_secret, std::optional<std::string> &paired_name) {
    if (sess.last_phase != PAIR_PHASE::SERVERCHALLENGERESP) {
      fail_pair(sess, tree, "Out of order call to clientpairingsecret");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTPAIRINGSECRET;

    auto &client = sess.client;

    if (client_pairing_secret.size() <= 16) {
      fail_pair(sess, tree, "Client pairing secret too short");
      return;
    }

    std::string_view secret {client_pairing_secret.data(), 16};
    std::string_view sign {client_pairing_secret.data() + secret.size(), client_pairing_secret.size() - secret.size()};

    auto x509 = crypto::x509(client.cert);
    if (!x509) {
      fail_pair(sess, tree, "Invalid client certificate");
      return;
    }
    auto x509_sign = crypto::signature(x509);

    std::string data;
    data.reserve(sess.serverchallenge.size() + x509_sign.size() + secret.size());

    data.insert(std::end(data), std::begin(sess.serverchallenge), std::end(sess.serverchallenge));
    data.insert(std::end(data), std::begin(x509_sign), std::end(x509_sign));
    data.insert(std::end(data), std::begin(secret), std::end(secret));

    auto hash = crypto::hash(data);

    // if hash not correct, probably MITM
    bool same_hash = hash.size() == sess.clienthash.size() && std::equal(hash.begin(), hash.end(), sess.clienthash.begin());
    auto verify = crypto::verify256(crypto::x509(client.cert), secret, sign);
    if (same_hash && verify) {
      tree.put("root.paired", 1);

      auto named_cert_p = std::make_shared<crypto::named_cert_t>();
      named_cert_p->name = client.name;
      for (char& c : named_cert_p->name) {
        if (c == '(') c = '[';
        else if (c == ')') c = ']';
      }
      named_cert_p->cert = std::move(client.cert);
      named_cert_p->uuid = uuid_util::uuid_t::generate().string();
      if (client.perm) {
        // Shell: the permissions chosen on the pairing page win, also for the first device.
        named_cert_p->perm = *client.perm;
      } else if (client_root.named_devices.empty()) {
        // If the device is the first one paired with the server, assign full permission.
        named_cert_p->perm = PERM::_all;
      } else {
        named_cert_p->perm = PERM::_default;
      }

      named_cert_p->enable_legacy_ordering = true;
      named_cert_p->allow_client_commands = true;
      named_cert_p->always_use_virtual_display = false;

      // Shell: the session is erased once, by remove_session below; erasing it here as well left
      // remove_session reading the uniqueid from the freed session.
      add_authorized_client(named_cert_p);
      paired_name = named_cert_p->name;
    } else {
      tree.put("root.paired", 0);
      BOOST_LOG(warning) << "Pair attempt failed due to same_hash: " << same_hash << ", verify: " << verify;
    }

    remove_session(sess);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  template<class T>
  struct tunnel;

  template<>
  struct tunnel<ShellHTTPS> {
    static auto constexpr to_string = "HTTPS"sv;
  };

  template<>
  struct tunnel<SimpleWeb::HTTP> {
    static auto constexpr to_string = "NONE"sv;
  };

  inline crypto::named_cert_t* get_verified_cert(req_https_t request) {
    return (crypto::named_cert_t*)request->userp.get();
  }

  template <class T>
  void print_req(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    BOOST_LOG(debug) << "TUNNEL :: "sv << tunnel<T>::to_string;

    BOOST_LOG(debug) << "METHOD :: "sv << request->method;
    BOOST_LOG(debug) << "DESTINATION :: "sv << request->path;

    for (auto &[name, val] : request->header) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;

    for (auto &[name, val] : request->parse_query_string()) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;
  }

  template<class T>
  void not_found(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 404);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(SimpleWeb::StatusCode::client_error_not_found, data.str());
    response->close_connection_after_response = true;
  }

  template <class T>
  void pair(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;

    auto fg = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    if (!config::shell.enable_pairing) {
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Pairing is disabled for this instance");

      return;
    }

    auto args = request->parse_query_string();
    if (args.find("uniqueid"s) == std::end(args)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing uniqueid parameter");

      return;
    }

    auto uniqID {get_arg(args, "uniqueid")};

    args_t::const_iterator it;
    if (it = args.find("phrase"); it != std::end(args)) {
      if (it->second == "getservercert"sv) {
        pair_session_t sess;

        auto deviceName { get_arg(args, "devicename") };

        if (deviceName == "roth"sv) {
          deviceName = "Legacy client";
        }

        sess.client.uniqueID = std::move(uniqID);
        sess.client.name = std::move(deviceName);
        sess.client.cert = util::from_hex_vec(get_arg(args, "clientcert"), true);
        sess.async_insert_pin.salt = get_arg(args, "salt");

        BOOST_LOG(debug) << sess.client.cert;

        // Shell: read before the sessions are locked, so a PIN typed slowly holds up nobody else
        const bool pin_stdin = config::shell.flags[config::flag::PIN_STDIN];
        std::string pin;
        if (pin_stdin) {
          std::cout << "Please insert pin: "sv;
          std::getline(std::cin, pin);
        }

        // Shell: released before fg writes the reply
        std::lock_guard lock {map_id_sess_mutex};
        drop_stale_sessions();
        // Shell: a new pairing attempt replaces any earlier session of this uniqueid (a wrong PIN
        // leaves one at SERVERCHALLENGERESP, a dropped handshake at GETSERVERCERT). Kept, it would
        // never be offered a PIN again, and stock clients all share one uniqueid. A request it still
        // keeps waiting for the PIN is told so, rather than left to time out.
        erase_session(sess.client.uniqueID, "Superseded by a newer pairing attempt");
        auto ptr = map_id_sess.emplace(sess.client.uniqueID, std::move(sess)).first;

        if (pin_stdin) {
          // Shell: answered here; the phase checks below are for the later requests
          getservercert(ptr->second, tree, pin);
          return;
        } else {
#if defined SHELL_TRAY && SHELL_TRAY >= 1
          // Shell: queued for the tray thread, which shows it; tray_update waits for that thread,
          // and neither this HTTP io thread nor the task pool (stream input) may wait on it
          system_tray::queue_require_pin();
#endif
          ptr->second.async_insert_pin.response = std::move(response);

          fg.disable();
          return;
        }
      } else if (it->second == "pairchallenge"sv) {
        tree.put("root.paired", 1);
        tree.put("root.<xmlattr>.status_code", 200);
        return;
      }
    }

    // Shell: released before fg writes the reply
    std::lock_guard lock {map_id_sess_mutex};
    auto sess_it = map_id_sess.find(uniqID);
    if (sess_it == std::end(map_id_sess)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Invalid uniqueid");

      return;
    }

    if (it = args.find("clientchallenge"); it != std::end(args)) {
      auto challenge = util::from_hex_vec(it->second, true);
      clientchallenge(sess_it->second, tree, challenge);
    } else if (it = args.find("serverchallengeresp"); it != std::end(args)) {
      auto encrypted_response = util::from_hex_vec(it->second, true);
      serverchallengeresp(sess_it->second, tree, encrypted_response);
    } else if (it = args.find("clientpairingsecret"); it != std::end(args)) {
      auto pairingsecret = util::from_hex_vec(it->second, true);
      std::optional<std::string> paired_name;
      clientpairingsecret(sess_it->second, tree, pairingsecret, paired_name);
#if defined SHELL_TRAY && SHELL_TRAY >= 1
      if (paired_name) {
        // Shell: queued for the tray thread (see getservercert above)
        system_tray::queue_paired(std::move(*paired_name));
      }
#endif
    } else {
      tree.put("root.<xmlattr>.status_code", 404);
      tree.put("root.<xmlattr>.status_message", "Invalid pairing request");
    }
  }

  /**
   * @brief Shell: GET /unpair?uniqueid=<id> over HTTP. Clients send it after a pairing failed on
   * their side (a wrong PIN), and from their own Unpair command. It drops only an unfinished pairing
   * of that uniqueid (answering a request it keeps waiting for the PIN) and then replies 200, which
   * lets the client report the wrong PIN. Paired devices are removed in the web UI only, so without
   * an unfinished pairing the reply is 400, and the client does not take the device for unpaired.
   */
  void unpair(resp_http_t response, req_http_t request) {
    print_req<SimpleWeb::HTTP>(request);

    pt::ptree tree;

    auto fg = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto args = request->parse_query_string();
    auto it = args.find("uniqueid"s);
    if (it == std::end(args)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing uniqueid parameter");
      return;
    }

    bool dropped;
    {
      std::lock_guard lock {map_id_sess_mutex};
      dropped = erase_session(it->second, "Pairing was cancelled");
    }

    if (dropped) {
      BOOST_LOG(info) << "Unfinished pairing dropped at the client's request"sv;
      tree.put("root.<xmlattr>.status_code", 200);
    } else {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Unpair this device in the host's web UI");
    }
  }

  pin_result_e pin(std::string pin, std::string name, std::optional<crypto::PERM> perm) {
    pt::ptree tree;
    // Shell: held while the session is chosen and updated, released before the client is answered
    std::unique_lock lock {map_id_sess_mutex};
    drop_stale_sessions();
    if (map_id_sess.empty()) {
      return pin_result_e::no_client;
    }

    // ensure pin is 4 digits
    if (pin.size() != 4) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put(
        "root.<xmlattr>.status_message",
        std::format("Pin must be 4 digits, {} provided", pin.size())
      );
      return pin_result_e::invalid_pin;
    }

    // ensure all pin characters are numeric
    if (!std::all_of(pin.begin(), pin.end(), ::isdigit)) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Pin must be numeric");
      return pin_result_e::invalid_pin;
    }

    // Shell: the session whose client is waiting for the PIN. A session already past getservercert
    // (Pair pressed twice while a client is mid-pairing) or without a request to answer is passed
    // over: getservercert would fail it, and fail_pair erases the session a reference points to.
    // Of several waiting clients the newest gets it: the one whose PIN was most likely just shown.
    auto has_response = [](const auto &response) {
      return (response.has_left() && response.left()) || (response.has_right() && response.right());
    };
    auto sess_it = std::end(map_id_sess);
    for (auto it = std::begin(map_id_sess); it != std::end(map_id_sess); ++it) {
      if (it->second.last_phase == PAIR_PHASE::NONE && has_response(it->second.async_insert_pin.response) &&
          (sess_it == std::end(map_id_sess) || it->second.created > sess_it->second.created)) {
        sess_it = it;
      }
    }
    if (sess_it == std::end(map_id_sess)) {
      BOOST_LOG(warning) << "No client is waiting for a pin";
      return pin_result_e::no_client;
    }
    auto &sess = sess_it->second;

    if (!name.empty()) {
      sess.client.name = name;
    }

    // Shell: kept on the session until clientpairingsecret stores the device
    if (perm) {
      sess.client.perm = *perm & PERM::_all;
    }

    // The client's request is answered either way, so take it out of the session first: when the
    // salt is bad, getservercert erases the session, and sess with it. Moving out leaves the null
    // pointer the session keeps between PINs. sess is not used after getservercert.
    auto async_response = std::move(sess.async_insert_pin.response);
    getservercert(sess, tree, pin);
    // Shell: 200 only means the client got the reply it checks the PIN with; a wrong PIN shows
    // when the client ends the pairing, after this returns.
    bool sent = tree.get<int>("root.<xmlattr>.status_code", 0) == 200;

    lock.unlock();

    // response to the request for pin
    std::ostringstream data;
    pt::write_xml(data, tree);

    if (async_response.has_left()) {
      async_response.left()->write(data.str());
    } else {
      async_response.right()->write(data.str());
    }

    // response to the current request
    return sent ? pin_result_e::sent : pin_result_e::failed;
  }

  template<class T>
  void serverinfo(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    int pair_status = 0;
    if constexpr (std::is_same_v<ShellHTTPS, T>) {
      auto args = request->parse_query_string();
      auto clientID = args.find("uniqueid"s);

      if (clientID != std::end(args)) {
        pair_status = 1;
      }
    }

    auto local_endpoint = request->local_endpoint();

    pt::ptree tree;

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.hostname", config::nvhttp.shell_name);

    tree.put("root.appversion", VERSION);
    tree.put("root.GfeVersion", GFE_VERSION);
    tree.put("root.uniqueid", http::unique_id);
    tree.put("root.HttpsPort", net::map_port(PORT_HTTPS));
    tree.put("root.ExternalPort", net::map_port(PORT_HTTP));
    tree.put("root.MaxLumaPixelsHEVC", video::active_hevc_mode > 1 ? "1869449984" : "0");

    // Only include the MAC address for requests sent from paired clients over HTTPS.
    // For HTTP requests, use a placeholder MAC address that Moonlight knows to ignore.
    if constexpr (std::is_same_v<ShellHTTPS, T>) {
      tree.put("root.mac", platf::get_mac_address(net::addr_to_normalized_string(local_endpoint.address())));

      auto named_cert_p = get_verified_cert(request);
      if (!!(named_cert_p->perm & PERM::server_cmd)) {
        pt::ptree& root_node = tree.get_child("root");

        if (config::shell.server_cmds.size() > 0) {
          // Broadcast server_cmds
          for (const auto& cmd : config::shell.server_cmds) {
            pt::ptree cmd_node;
            cmd_node.put_value(cmd.cmd_name);
            root_node.push_back(std::make_pair("ServerCommand", cmd_node));
          }
        }
      } else {
        BOOST_LOG(debug) << "Permission Get ServerCommand denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";
      }

      tree.put("root.Permission", std::to_string((uint32_t)named_cert_p->perm));

    #ifdef _WIN32
      tree.put("root.VirtualDisplayCapable", true);
      if (!!(named_cert_p->perm & PERM::_all_actions)) {
        tree.put("root.VirtualDisplayDriverReady", proc::vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK);
      } else {
        tree.put("root.VirtualDisplayDriverReady", true);
      }
    #endif
    } else {
      tree.put("root.mac", "00:00:00:00:00:00");
      tree.put("root.Permission", "0");
    }

    // Moonlight clients track LAN IPv6 addresses separately from LocalIP which is expected to
    // always be an IPv4 address. If we return that same IPv6 address here, it will clobber the
    // stored LAN IPv4 address. To avoid this, we need to return an IPv4 address in this field
    // when we get a request over IPv6.
    //
    // HACK: We should return the IPv4 address of local interface here, but we don't currently
    // have that implemented. For now, we will emulate the behavior of GFE+GS-IPv6-Forwarder,
    // which returns 127.0.0.1 as LocalIP for IPv6 connections. Moonlight clients with IPv6
    // support know to ignore this bogus address.
    if (local_endpoint.address().is_v6() && !local_endpoint.address().to_v6().is_v4_mapped()) {
      tree.put("root.LocalIP", "127.0.0.1");
    } else {
      tree.put("root.LocalIP", net::addr_to_normalized_string(local_endpoint.address()));
    }

    uint32_t codec_mode_flags = SCM_H264;
    if (video::last_encoder_probe_supported_yuv444_for_codec[0]) {
      codec_mode_flags |= SCM_H264_HIGH8_444;
    }
    if (video::active_hevc_mode >= 2) {
      codec_mode_flags |= SCM_HEVC;
      if (video::last_encoder_probe_supported_yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT8_444;
      }
    }
    if (video::active_hevc_mode >= 3) {
      codec_mode_flags |= SCM_HEVC_MAIN10;
      if (video::last_encoder_probe_supported_yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT10_444;
      }
    }
    if (video::active_av1_mode >= 2) {
      codec_mode_flags |= SCM_AV1_MAIN8;
      if (video::last_encoder_probe_supported_yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH8_444;
      }
    }
    if (video::active_av1_mode >= 3) {
      codec_mode_flags |= SCM_AV1_MAIN10;
      if (video::last_encoder_probe_supported_yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH10_444;
      }
    }
    tree.put("root.ServerCodecModeSupport", codec_mode_flags);

    tree.put("root.PairStatus", pair_status);

    if constexpr (std::is_same_v<ShellHTTPS, T>) {
      int current_appid = proc::proc.running();
      // When input only mode is enabled, the only resume method should be launching the same app again.
      if (config::input.enable_input_only_mode && current_appid != proc::input_only_app_id) {
        current_appid = 0;
      }
      tree.put("root.currentgame", current_appid);
      tree.put("root.currentgameuuid", proc::proc.get_running_app_uuid());
      // Protocol constants: clients parse these exact state strings, so they keep the upstream names
      tree.put("root.state", current_appid > 0 ? "SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE");
    } else {
      tree.put("root.currentgame", 0);
      tree.put("root.currentgameuuid", "");
      tree.put("root.state", "SUNSHINE_SERVER_FREE");
    }

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
  }

  nlohmann::json get_all_clients() {
    nlohmann::json named_cert_nodes = nlohmann::json::array();
    client_t &client = client_root;
    std::list<std::string> connected_uuids = rtsp_stream::get_all_session_uuids();

    for (auto &named_cert : client.named_devices) {
      nlohmann::json named_cert_node;
      named_cert_node["name"] = named_cert->name;
      named_cert_node["uuid"] = named_cert->uuid;
      named_cert_node["display_mode"] = named_cert->display_mode;
      named_cert_node["perm"] = static_cast<uint32_t>(named_cert->perm);
      named_cert_node["enable_legacy_ordering"] = named_cert->enable_legacy_ordering;
      named_cert_node["allow_client_commands"] = named_cert->allow_client_commands;
      named_cert_node["always_use_virtual_display"] = named_cert->always_use_virtual_display;

      // Add "do" commands if available
      if (!named_cert->do_cmds.empty()) {
        nlohmann::json do_cmds_node = nlohmann::json::array();
        for (const auto &cmd : named_cert->do_cmds) {
          do_cmds_node.push_back(crypto::command_entry_t::serialize(cmd));
        }
        named_cert_node["do"] = do_cmds_node;
      }

      // Add "undo" commands if available
      if (!named_cert->undo_cmds.empty()) {
        nlohmann::json undo_cmds_node = nlohmann::json::array();
        for (const auto &cmd : named_cert->undo_cmds) {
          undo_cmds_node.push_back(crypto::command_entry_t::serialize(cmd));
        }
        named_cert_node["undo"] = undo_cmds_node;
      }

      // Determine connection status
      bool connected = false;
      if (connected_uuids.empty()) {
        connected = false;
      } else {
        for (auto it = connected_uuids.begin(); it != connected_uuids.end(); ++it) {
          if (*it == named_cert->uuid) {
            connected = true;
            connected_uuids.erase(it);
            break;
          }
        }
      }
      named_cert_node["connected"] = connected;

      named_cert_nodes.push_back(named_cert_node);
    }

    return named_cert_nodes;
  }

  void applist(resp_https_t response, req_https_t request) {
    print_req<ShellHTTPS>(request);

    pt::ptree tree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto &apps = tree.add_child("root", pt::ptree {});

    apps.put("<xmlattr>.status_code", 200);

    auto named_cert_p = get_verified_cert(request);
    if (!!(named_cert_p->perm & PERM::_all_actions)) {
      auto current_appid = proc::proc.running();
      auto should_hide_inactive_apps = config::input.enable_input_only_mode && current_appid > 0 && current_appid != proc::input_only_app_id;

      auto app_list = proc::proc.get_apps();

      bool enable_legacy_ordering = config::shell.legacy_ordering && named_cert_p->enable_legacy_ordering;
      size_t bits;
      if (enable_legacy_ordering) {
        bits = zwpad::pad_width_for_count(app_list.size());
      }

      for (size_t i = 0; i < app_list.size(); i++) {
        auto& app = app_list[i];
        auto appid = util::from_view(app.id);
        if (should_hide_inactive_apps) {
          if (
            appid != current_appid
            && appid != proc::input_only_app_id
            && appid != proc::terminate_app_id
          ) {
            continue;
          }
        } else {
          if (appid == proc::terminate_app_id) {
            continue;
          }
        }

        std::string app_name;
        if (enable_legacy_ordering) {
          app_name = zwpad::pad_for_ordering(app.name, bits, i);
        } else {
          app_name = app.name;
        }

        pt::ptree app_node;

        app_node.put("IsHdrSupported"s, video::active_hevc_mode == 3 ? 1 : 0);
        app_node.put("AppTitle"s, app_name);
        app_node.put("UUID", app.uuid);
        app_node.put("IDX", app.idx);
        app_node.put("ID", app.id);

        apps.push_back(std::make_pair("App", std::move(app_node)));
      }
    } else {
      BOOST_LOG(debug) << "Permission ListApp denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      pt::ptree app_node;

      app_node.put("IsHdrSupported"s, 0);
      app_node.put("AppTitle"s, "Permission Denied");
      app_node.put("UUID", "");
      app_node.put("IDX", "0");
      app_node.put("ID", "114514");

      apps.push_back(std::make_pair("App", std::move(app_node)));

      return;
    }

  }

  void launch(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<ShellHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto args = request->parse_query_string();

    auto appid_str = get_arg(args, "appid", "0");
    auto appuuid_str = get_arg(args, "appuuid", "");
    auto appid = util::from_view(appid_str);
    auto current_appid = proc::proc.running();
    auto current_app_uuid = proc::proc.get_running_app_uuid();
    bool is_input_only = config::input.enable_input_only_mode && (appid == proc::input_only_app_id || (appuuid_str == REMOTE_INPUT_UUID));

    auto named_cert_p = get_verified_cert(request);
    auto perm = PERM::launch;

    BOOST_LOG(verbose) << "Launching app [" << appid_str << "] with UUID [" << appuuid_str << "]";
    // BOOST_LOG(verbose) << "QS: " << request->query_string;

    // If we have already launched an app, we should allow clients with view permission to join the input only or current app's session.
    if (
      current_appid > 0
      && (appuuid_str != TERMINATE_APP_UUID || appid != proc::terminate_app_id)
      && (is_input_only || appid == current_appid || (!appuuid_str.empty() && appuuid_str == current_app_uuid))
    ) {
      perm = PERM::_allow_view;
    }

    if (!(named_cert_p->perm & perm)) {
      BOOST_LOG(debug) << "Permission LaunchApp denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Permission denied");

      return;
    }
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args) ||
      args.find("localAudioPlayMode"s) == std::end(args) ||
      (args.find("appid"s) == std::end(args) && args.find("appuuid"s) == std::end(args))
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required launch parameter");

      return;
    }

    if (!is_input_only) {
      // Special handling for the "terminate" app
      if (
        (config::input.enable_input_only_mode && appid == proc::terminate_app_id)
        || appuuid_str == TERMINATE_APP_UUID
      ) {
        proc::proc.terminate();

        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 410);
        tree.put("root.<xmlattr>.status_message", "App terminated.");

        return;
      }

      if (
        current_appid > 0
        && current_appid != proc::input_only_app_id
        && (
          (appid > 0 && appid != current_appid)
          || (!appuuid_str.empty() && appuuid_str != current_app_uuid)
        )
      ) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "An app is already running on this host");

        return;
      }
    }

    host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    auto launch_session = make_launch_session(host_audio, is_input_only, args, named_cert_p);

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    bool no_active_sessions = rtsp_stream::session_count() == 0;

    if (is_input_only) {
      BOOST_LOG(info) << "Launching input only session..."sv;

      launch_session->client_do_cmds.clear();
      launch_session->client_undo_cmds.clear();

      // Still probe encoders once, if input only session is launched first
      // But we're ignoring if it's successful or not
      if (no_active_sessions && !proc::proc.virtual_display) {
        video::probe_encoders();
        if (current_appid == 0) {
          proc::proc.launch_input_only();
        }
      }
    } else if (appid > 0 || !appuuid_str.empty()) {
      if (appid == current_appid || (!appuuid_str.empty() && appuuid_str == current_app_uuid)) {
        // We're basically resuming the same app

        BOOST_LOG(debug) << "Resuming app [" << proc::proc.get_last_run_app_name() << "] from launch app path...";

        if (!proc::proc.allow_client_commands || !named_cert_p->allow_client_commands) {
          launch_session->client_do_cmds.clear();
          launch_session->client_undo_cmds.clear();
        }

        if (current_appid == proc::input_only_app_id) {
          launch_session->input_only = true;
        }

  #ifdef _WIN32
        if (no_active_sessions) {
          // Shell: a virtual display released while nobody streamed is created again, and one in
          // place is switched to the size this client asked for (as /resume does)
          proc::proc.restore_virtual_display(*launch_session);
          if (proc::proc.virtual_display) {
            proc::proc.apply_resume_display_mode(*launch_session);
          }
        }
  #endif

        if (no_active_sessions && !proc::proc.virtual_display) {
          display_device::configure_display(config::video, *launch_session);
          if (video::probe_encoders()) {
            tree.put("root.resume", 0);
            tree.put("root.<xmlattr>.status_code", 503);
            tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");

            return;
          }
        }
      } else {
        const auto& apps = proc::proc.get_apps();
        auto app_iter = std::find_if(apps.begin(), apps.end(), [&appid_str, &appuuid_str](const auto _app) {
          return _app.id == appid_str || _app.uuid == appuuid_str;
        });

        if (app_iter == apps.end()) {
          BOOST_LOG(error) << "Couldn't find app with ID ["sv << appid_str << "] or UUID ["sv << appuuid_str << ']';
          tree.put("root.<xmlattr>.status_code", 404);
          tree.put("root.<xmlattr>.status_message", "Cannot find requested application");
          tree.put("root.gamesession", 0);
          return;
        }

        if (!app_iter->allow_client_commands) {
          launch_session->client_do_cmds.clear();
          launch_session->client_undo_cmds.clear();
        }

        auto err = proc::proc.execute(*app_iter, launch_session);
        if (err) {
          tree.put("root.<xmlattr>.status_code", err);
          tree.put(
            "root.<xmlattr>.status_message",
            err == 503
            ? "Failed to initialize video capture/encoding. Is a display connected and turned on?"
            : "Failed to start the specified application");
          tree.put("root.gamesession", 0);

          return;
        }
      }
    } else {
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "How did you get here?");
      tree.put("root.gamesession", 0);
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.gamesession", 1);

    rtsp_stream::launch_session_raise(launch_session);
  }

  void resume(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<ShellHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto named_cert_p = get_verified_cert(request);
    if (!(named_cert_p->perm & PERM::_allow_view)) {
      BOOST_LOG(debug) << "Permission ViewApp denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Permission denied");

      return;
    }

    auto current_appid = proc::proc.running();
    if (current_appid == 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "No running app to resume");

      return;
    }

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required resume parameter");

      return;
    }

    // Newer Moonlight clients send localAudioPlayMode on /resume too,
    // so we should use it if it's present in the args and there are
    // no active sessions we could be interfering with.
    const bool no_active_sessions {rtsp_stream::session_count() == 0};
    if (no_active_sessions && args.find("localAudioPlayMode"s) != std::end(args)) {
      host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    }
    auto launch_session = make_launch_session(host_audio, false, args, named_cert_p);

    if (!proc::proc.allow_client_commands || !named_cert_p->allow_client_commands) {
      launch_session->client_do_cmds.clear();
      launch_session->client_undo_cmds.clear();
    }

    if (config::input.enable_input_only_mode && current_appid == proc::input_only_app_id) {
      launch_session->input_only = true;
    }

  #ifdef _WIN32
    if (no_active_sessions) {
      // Shell: a virtual display released while nobody streamed (a monitor turned on) is created
      // again for this client
      proc::proc.restore_virtual_display(*launch_session);
    }
  #endif

    if (no_active_sessions && proc::proc.virtual_display) {
      // Shell: the virtual display was created at the resolution of an earlier session. Switch it
      // to what this client asked for, or the old desktop is scaled into the new frame with bars.
      proc::proc.apply_resume_display_mode(*launch_session);
    }

    if (no_active_sessions && !proc::proc.virtual_display) {
      // We want to prepare display only if there are no active sessions
      // and the current session isn't virtual display at the moment.
      // This should be done before probing encoders as it could change the active displays.
      display_device::configure_display(config::video, *launch_session);

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      if (video::probe_encoders()) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.resume", 1);

    rtsp_stream::launch_session_raise(launch_session);

#if defined SHELL_TRAY && SHELL_TRAY >= 1
    system_tray::update_tray_client_connected(named_cert_p->name);
#endif
  }

  void cancel(resp_https_t response, req_https_t request) {
    print_req<ShellHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto named_cert_p = get_verified_cert(request);
    if (!(named_cert_p->perm & PERM::launch)) {
      BOOST_LOG(debug) << "Permission CancelApp denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Permission denied");

      return;
    }

    tree.put("root.cancel", 1);
    tree.put("root.<xmlattr>.status_code", 200);

    // The client asked to quit the app ("Quit app" in Moonlight and Hermit).
    rtsp_stream::terminate_sessions("client_quit");

    if (proc::proc.running() > 0) {
      proc::proc.terminate();
    }

    // The config needs to be reverted regardless of whether "proc::proc.terminate()" was called or not.
    display_device::revert_configuration();
  }

  void appasset(resp_https_t response, req_https_t request) {
    print_req<ShellHTTPS>(request);

    auto fg = util::fail_guard([&]() {
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error);
      response->close_connection_after_response = true;
    });

    auto named_cert_p = get_verified_cert(request);

    if (!(named_cert_p->perm & PERM::_all_actions)) {
      BOOST_LOG(debug) << "Permission Get AppAsset denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      fg.disable();
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    auto args = request->parse_query_string();
    auto app_image = proc::proc.get_app_image(util::from_view(get_arg(args, "appid")));

    fg.disable();

    std::ifstream in(app_image, std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    response->close_connection_after_response = true;
  }

#ifdef _WIN32
  /**
   * @brief Run fn as the logged-in console user when Shell runs as SYSTEM, so copied files are
   * read with the user's access rights and received files belong to the user.
   * @return False if there is no user session or impersonation failed; fn is not run then.
   */
  bool run_as_console_user(const std::function<void(HANDLE)> &fn) {
    if (!platf::is_running_as_system()) {
      fn(nullptr);
      return true;
    }
    HANDLE token = platf::retrieve_users_token(false);
    if (token == nullptr) {
      return false;
    }
    auto close_token = util::fail_guard([token]() {
      CloseHandle(token);  // also when fn throws
    });
    auto ec = platf::impersonate_current_user(token, [&]() {
      fn(token);
    });
    return !ec;
  }

  /// Over the size or item limits, when sending (archive_paths, list_paths) or receiving
  /// (decode_archive) files: 413.
  bool is_file_limit_error(const std::string &error) {
    return error == "files too large" || error == "too many files" || error == "archive too large" || error == "entry count out of range";
  }

  /// The copied files themselves cannot be sent or kept (a name the other side cannot create, two
  /// names that differ only in case, a file also used as a folder, or nothing left once links are
  /// skipped): 422, not a host failure.
  bool is_file_content_error(const std::string &error) {
    return !platf::clipboard::content_error_reason(error).empty();
  }

  /// Status for a failed type=files or type=filelist: 413 limits, 422 content, 500 everything else
  /// (no user session, I/O errors).
  SimpleWeb::StatusCode file_error_status(const std::string &error) {
    if (is_file_limit_error(error)) {
      return SimpleWeb::StatusCode::client_error_payload_too_large;
    }
    if (is_file_content_error(error)) {
      return SimpleWeb::StatusCode::client_error_unprocessable_entity;
    }
    return SimpleWeb::StatusCode::server_error_internal_server_error;
  }

  /// Body of a failed file reply: the error as plain text, after the machine-readable reason on a
  /// line of its own for a 422 (unsupported-name, duplicate-name, nothing-to-copy).
  std::string file_error_body(const std::string &error) {
    auto reason = platf::clipboard::content_error_reason(error);
    return reason.empty() ? error : std::string {reason} + "\n" + error;
  }

  /// Shell: 503 when another program kept the clipboard open through every retry. Clients keep
  /// their clipboard sequence number and try again later.
  constexpr auto clipboard_busy_body = "clipboard-busy\nanother program is using the clipboard; try again later"sv;

  /// Shell: 503 for a type=filedata request whose file is stalled in a read for an earlier one. Same
  /// reason line, so clients treat it as busy and try again after a short wait.
  constexpr auto clipboard_file_reading_body = "clipboard-busy\nthis file is stalled in a read for an earlier request; try again later"sv;

  void refuse_clipboard_busy(const resp_https_t &response, const std::string &client, std::string_view what) {
    BOOST_LOG(info) << "Clipboard " << what << " for [" << client << "] not done: another program is using the clipboard";
    response->write(SimpleWeb::StatusCode::server_error_service_unavailable, clipboard_busy_body);
    response->close_connection_after_response = true;
  }

  /// Status for an archive decode_archive rejected (POST type=files): 413 limits, 422 content,
  /// 400 everything else, which is a malformed archive rather than a host failure.
  SimpleWeb::StatusCode archive_error_status(const std::string &error) {
    auto status = file_error_status(error);
    return status == SimpleWeb::StatusCode::server_error_internal_server_error ? SimpleWeb::StatusCode::client_error_bad_request : status;
  }

  /// Shell: a file list served with GET /actions/clipboard?type=filelist, whose files are then
  /// fetched one by one with type=filedata.
  struct clipboard_snapshot_t {
    std::string id;
    std::vector<platf::clipboard::file_list_entry> entries;
    std::uint64_t stream_session = 0;  ///< the client's stream session it was made in (alloc order)
    DWORD console_session = 0;  ///< the Windows console session whose user listed the files
  };

  /// The client's current stream session (its alloc order), or 0 if it has none.
  std::uint64_t current_stream_session(const std::string &client_uuid) {
    auto session = rtsp_stream::find_session(client_uuid);
    return session ? stream::session::alloc_order(*session) : 0;
  }

  /// Newest two per client (cert uuid): a paste that is still copying from the previous list keeps
  /// working after the client fetched a newer one. Older lists are dropped (410 Gone), and so are
  /// lists from an earlier stream session of the client: a list lives only as long as its session.
  constexpr std::size_t clipboard_snapshots_per_client = 2;
  // Never destroyed, as the clipboard workers below (a listing job left behind by stop() stores here)
  std::mutex &clipboard_snapshots_mutex = *new std::mutex;
  auto &clipboard_snapshots = *new std::map<std::string, std::deque<std::shared_ptr<const clipboard_snapshot_t>>>;

  void store_clipboard_snapshot(const std::string &client_uuid, std::shared_ptr<const clipboard_snapshot_t> snapshot) {
    std::lock_guard lock {clipboard_snapshots_mutex};
    auto &list = clipboard_snapshots[client_uuid];
    std::erase_if(list, [&](const auto &stored) {
      return stored->stream_session != snapshot->stream_session;
    });
    list.push_front(std::move(snapshot));
    while (list.size() > clipboard_snapshots_per_client) {
      list.pop_back();
    }
  }

  std::shared_ptr<const clipboard_snapshot_t> find_clipboard_snapshot(const std::string &client_uuid, const std::string &id, std::uint64_t stream_session) {
    std::lock_guard lock {clipboard_snapshots_mutex};
    auto it = clipboard_snapshots.find(client_uuid);
    if (id.empty() || it == clipboard_snapshots.end()) {
      return nullptr;
    }
    std::erase_if(it->second, [&](const auto &stored) {
      return stored->stream_session != stream_session;
    });
    for (const auto &snapshot : it->second) {
      if (snapshot->id == id) {
        return snapshot;
      }
    }
    return nullptr;
  }

  /// Reply of a job run on the clipboard worker, written to the response on the io thread.
  struct file_reply_t {
    SimpleWeb::StatusCode code = SimpleWeb::StatusCode::success_ok;
    std::string body;
    std::string content_type;  ///< no Content-Type header when empty
  };

  /// Shell: threads for the slow clipboard file work (walking folders, reading or unpacking up to
  /// 256 MB, reading files that may stall), so the single HTTPS io thread keeps serving other
  /// requests meanwhile. Queued jobs are taken in order by the first free thread; with one thread
  /// they run one at a time.
  class clipboard_worker_t {
  public:
    void start(std::size_t count) {
      std::lock_guard lock {mutex};
      stopping = false;
      for (std::size_t i = 0; i < count; ++i) {
        ++running;
        threads.emplace_back([this]() {
          run();
          std::lock_guard lock {mutex};
          --running;
          done.notify_all();
        });
      }
    }

    /**
     * @brief Waits for the running jobs until deadline; queued jobs are dropped without a reply.
     * A job can be stuck in a call on a hung network share or a OneDrive placeholder, and Shell
     * must be down before its 10 s force shutdown. So after stop_grace the blocking file calls of
     * the threads are cancelled (CancelSynchronousIo reaches only a call under way, hence again
     * every stop_poll), and threads still stuck at the deadline are left behind, detached. Such a
     * thread uses its worker when its job ends, so the workers are never destroyed.
     * @return True if every thread ended, false if some were left behind.
     */
    bool stop(std::chrono::steady_clock::time_point deadline) {
      std::unique_lock lock {mutex};
      stopping = true;
      wake.notify_all();
      const auto cancel_from = std::min(std::chrono::steady_clock::now() + stop_grace, deadline);
      for (;;) {
        if (std::chrono::steady_clock::now() >= cancel_from) {
          for (auto &thread : threads) {
            CancelSynchronousIo(static_cast<HANDLE>(pthread_gethandle(thread.native_handle())));
          }
        }
        if (done.wait_for(lock, stop_poll, [this]() {
              return running == 0;
            })) {
          break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          break;
        }
      }
      const std::size_t stuck = running;
      std::deque<std::function<void()>> dropped;
      dropped.swap(jobs);
      lock.unlock();

      for (auto &thread : threads) {
        if (stuck == 0) {
          thread.join();
        } else {
          thread.detach();  // the threads that did end are only released
        }
      }
      threads.clear();
      if (stuck > 0) {
        BOOST_LOG(warning) << "Clipboard: " << stuck << " file job(s) still stuck in a file call at shutdown; left behind";
      }
      return stuck == 0;
    }

    void post(std::function<void()> job) {
      {
        std::lock_guard lock {mutex};
        if (stopping) {
          return;
        }
        jobs.push_back(std::move(job));
      }
      wake.notify_one();
    }

  private:
    void run() {
      for (;;) {
        std::function<void()> job;
        {
          std::unique_lock lock {mutex};
          wake.wait(lock, [this]() {
            return stopping || !jobs.empty();
          });
          if (stopping) {
            return;
          }
          job = std::move(jobs.front());
          jobs.pop_front();
        }
        job();
      }
    }

    static constexpr auto stop_grace = std::chrono::seconds(1);
    static constexpr auto stop_poll = std::chrono::milliseconds(100);

    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable done;  ///< a thread ended
    std::deque<std::function<void()>> jobs;
    bool stopping = false;
    std::size_t running = 0;  ///< threads started and not yet ended
    std::vector<std::thread> threads;
  };

  /// Shell: how long stopping waits for the clipboard jobs (see clipboard_worker_t::stop()).
  constexpr auto clipboard_stop_timeout = std::chrono::seconds(3);

  /// Listing and packing (type=filelist, GET type=files) and unpacking (POST type=files), one at a time.
  /// Never destroyed (see clipboard_worker_t::stop()).
  clipboard_worker_t &clipboard_worker = *new clipboard_worker_t;

  /// Opening and reading the files of type=filedata, apart from clipboard_worker so a long job
  /// there (a 256 MB archive) does not hold up downloads. Each read waits for the previous chunk of
  /// its file to be sent, so a file is read strictly in order and holds at most one thread; a file
  /// whose read stalls (a OneDrive placeholder being fetched, a slow network share) holds up only
  /// that thread, and the others keep serving the remaining downloads of every device.
  clipboard_worker_t &clipboard_file_reader = *new clipboard_worker_t;
  constexpr std::size_t clipboard_file_reader_threads = 4;

  /**
   * @brief Runs work on the clipboard worker, then writes its reply on the HTTPS io thread, the
   * only thread that touches the response. work must not use the response; the jobs keep it alive.
   */
  void run_file_job(const std::shared_ptr<SimpleWeb::io_context> &io, resp_https_t response, std::function<file_reply_t()> work) {
    clipboard_worker.post([io, response = std::move(response), work = std::move(work)]() mutable {
      auto reply = std::make_shared<file_reply_t>();
      try {
        *reply = work();
      } catch (const std::exception &e) {
        RevertToSelf();  // the job may have thrown while impersonating the console user
        BOOST_LOG(error) << "Clipboard file job failed: " << e.what();
        *reply = file_reply_t {SimpleWeb::StatusCode::server_error_internal_server_error, {}, {}};
      }
      work = nullptr;  // free the inputs (up to 256 MB) before the reply is copied into the response
      boost::asio::post(*io, [response = std::move(response), reply]() {
        SimpleWeb::CaseInsensitiveMultimap headers;
        if (!reply->content_type.empty()) {
          headers.emplace("Content-Type", reply->content_type);
        }
        response->write(reply->code, reply->body, headers);
        if (reply->code != SimpleWeb::StatusCode::success_ok) {
          response->close_connection_after_response = true;
        }
      });
    });
  }

  /// Shell: a listed file of a device: its cert uuid, the list id and the index in the list.
  using clipboard_file_key_t = std::tuple<std::string, std::string, std::uint64_t>;

  /// Shell: how long a read of a file may sit in one open or ReadFile call before a new request for
  /// the same file gets 503. Shorter overlaps (Explorer seeking, a resume right after a cut while a
  /// chunk is still being read) go through: two reads of one file for a moment are harmless.
  constexpr auto clipboard_file_stall_after = std::chrono::seconds(3);

  /// Shell: the reads of one file queued or running on the file reader threads, and when each call
  /// of theirs that is under way in opening or reading the file began.
  struct clipboard_file_reads_t {
    std::size_t count = 0;
    std::multiset<std::chrono::steady_clock::time_point> calls;
  };

  /// Shell: reads per file. A client that retries a file whose earlier read stalls gets 503 instead
  /// of a second stuck thread.
  /// Never destroyed, as the clipboard workers (a read job left behind by stop() ends its count here).
  std::mutex &clipboard_file_reads_mutex = *new std::mutex;
  auto &clipboard_file_reads = *new std::map<clipboard_file_key_t, clipboard_file_reads_t>;

  /// One read of a file, counted for as long as it lives. Only the reader job holds it, so the count
  /// drops when the job ends whichever way (read, failed, connection gone) and when stop() or post()
  /// drops the job unrun.
  class clipboard_file_read_t {
  public:
    /// The time one call of a read spends opening or reading the file (which can stall), marked from
    /// construction to destruction. Lives inside the job, so within the read it belongs to.
    class call_t {
    public:
      explicit call_t(const clipboard_file_read_t &read) {
        std::lock_guard lock {clipboard_file_reads_mutex};
        reads = clipboard_file_reads.find(read.key);  // there while the read counts in it
        call = reads->second.calls.insert(std::chrono::steady_clock::now());
      }

      ~call_t() {
        std::lock_guard lock {clipboard_file_reads_mutex};
        reads->second.calls.erase(call);
      }

      call_t(const call_t &) = delete;
      call_t &operator=(const call_t &) = delete;

    private:
      std::map<clipboard_file_key_t, clipboard_file_reads_t>::iterator reads;
      std::multiset<std::chrono::steady_clock::time_point>::iterator call;
    };

    /// A new read of key, or null if refuse_if_stalled and another read of it has been in one open
    /// or ReadFile call for clipboard_file_stall_after or longer.
    static std::shared_ptr<clipboard_file_read_t> begin(const clipboard_file_key_t &key, bool refuse_if_stalled) {
      std::lock_guard lock {clipboard_file_reads_mutex};
      auto &reads = clipboard_file_reads[key];
      // A call under way belongs to a read that is counted, so a refusal leaves no empty entry.
      if (refuse_if_stalled && !reads.calls.empty() && std::chrono::steady_clock::now() - *reads.calls.begin() >= clipboard_file_stall_after) {
        return nullptr;
      }
      ++reads.count;
      return std::shared_ptr<clipboard_file_read_t>(new clipboard_file_read_t(key));
    }

    ~clipboard_file_read_t() {
      std::lock_guard lock {clipboard_file_reads_mutex};
      auto it = clipboard_file_reads.find(key);
      if (it != clipboard_file_reads.end() && --it->second.count == 0) {
        clipboard_file_reads.erase(it);
      }
    }

    clipboard_file_read_t(const clipboard_file_read_t &) = delete;
    clipboard_file_read_t &operator=(const clipboard_file_read_t &) = delete;

  private:
    explicit clipboard_file_read_t(clipboard_file_key_t key):
        key(std::move(key)) {
    }

    clipboard_file_key_t key;
  };

  bool parse_u64(const std::string &text, std::uint64_t &value) {
    auto end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return !text.empty() && ec == std::errc {} && ptr == end;
  }

  /// One file being sent for GET type=filedata: read in chunks on the file reader threads, each chunk
  /// written on the io thread and the next one read only after it was written to the socket. The
  /// steps hand the stream from one thread to the other, so only one of them uses it at a time; the
  /// response is used, and released, on the io thread only.
  struct clipboard_file_stream_t {
    std::shared_ptr<SimpleWeb::io_context> io;  ///< the HTTPS io thread
    HANDLE file = INVALID_HANDLE_VALUE;
    std::weak_ptr<SimpleWeb::ServerBase<ShellHTTPS>::Response> response;
    std::string client;
    clipboard_file_key_t key;
    std::uint64_t index = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::uint64_t sent = 0;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::vector<char> buffer;

    ~clipboard_file_stream_t() {
      if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
      }
    }

    void log_end(const char *outcome) const {
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
      BOOST_LOG(info) << "Clipboard file " << index << " to [" << client << "] " << outcome << ": " << sent << " of " << length
                      << " bytes from offset " << offset << " in " << ms << " ms";
    }
  };

  // Small enough that a slow client holds back the host through TCP flow control instead of
  // making it buffer the file, large enough to keep a fast link busy.
  constexpr std::size_t clipboard_file_chunk_bytes = 256 * 1024;

  void send_clipboard_file_chunk(std::shared_ptr<clipboard_file_stream_t> stream, resp_https_t response) {
    if (stream->sent == stream->length) {
      stream->log_end("sent");
      return;  // releasing the response finishes it (and closes the connection)
    }
    // ReadFile can stall (a network share, a cloud placeholder being fetched), so it runs on the
    // file reader threads. The response travels along untouched, which keeps the connection open.
    // reading counts the read of this file until the job ends.
    clipboard_file_reader.post([stream, response = std::move(response), reading = clipboard_file_read_t::begin(stream->key, false)]() mutable {
      DWORD want = static_cast<DWORD>(std::min<std::uint64_t>(clipboard_file_chunk_bytes, stream->length - stream->sent));
      DWORD got = 0;
      bool read;
      {
        clipboard_file_read_t::call_t call {*reading};
        read = ReadFile(stream->file, stream->buffer.data(), want, &got, nullptr) && got == want;
      }
      boost::asio::post(*stream->io, [stream, response = std::move(response), got, read]() {
        if (!read) {
          // The file shrank or became unreadable: the client sees a body shorter than Content-Length.
          stream->log_end("failed (read error)");
          return;
        }
        response->write(stream->buffer.data(), got);
        stream->sent += got;
        response->send([stream](const SimpleWeb::error_code &ec) {
          // SimpleWeb keeps the response alive through the write and this callback.
          auto response = stream->response.lock();
          if (ec || !response) {
            stream->log_end(ec ? "failed (connection closed)" : "stopped");
            return;
          }
          send_clipboard_file_chunk(stream, std::move(response));
        });
      });
    });
  }

  void refuse_clipboard_file(const resp_https_t &response, const std::string &client, SimpleWeb::StatusCode code, const std::string &why) {
    BOOST_LOG(info) << "Clipboard file not sent to [" << client << "]: " << why;
    response->write(code, why);
    response->close_connection_after_response = true;
  }

  /**
   * @brief Shell: GET type=filedata&snapshot=<id>&index=<n>[&offset=<bytes>]: the bytes of one file from
   * the client's file list, from offset to the end (offset lets the client resume).
   * 400 bad arguments, 404 index is not a file, 409 the file is gone or changed since the list was
   * made, 410 unknown or dropped list (also when it was made in an earlier stream session or for
   * another signed-in user), 416 offset past the end, 500 the file cannot be opened, 503 an earlier
   * read of the file has been stalled in opening or reading it for clipboard_file_stall_after (the
   * client retries after a short wait).
   */
  void send_clipboard_file(resp_https_t response, const crypto::named_cert_t *named_cert_p, const args_t &args, const std::shared_ptr<SimpleWeb::io_context> &io) {
    const std::string &client = named_cert_p->name;
    std::uint64_t index = 0;
    std::uint64_t offset = 0;
    if (!parse_u64(get_arg(args, "index", ""), index) || !parse_u64(get_arg(args, "offset", "0"), offset)) {
      refuse_clipboard_file(response, client, SimpleWeb::StatusCode::client_error_bad_request, "bad index or offset");
      return;
    }
    auto snapshot = find_clipboard_snapshot(named_cert_p->uuid, get_arg(args, "snapshot", ""), current_stream_session(named_cert_p->uuid));
    if (!snapshot) {
      refuse_clipboard_file(response, client, SimpleWeb::StatusCode::client_error_gone, "unknown file list");
      return;
    }
    // The files were listed with the rights of the user signed in then; nobody else gets them.
    if (snapshot->console_session != WTSGetActiveConsoleSessionId()) {
      refuse_clipboard_file(response, client, SimpleWeb::StatusCode::client_error_gone, "the signed-in user changed since the list was made");
      return;
    }
    if (index >= snapshot->entries.size() || snapshot->entries[index].directory) {
      refuse_clipboard_file(response, client, SimpleWeb::StatusCode::client_error_not_found, "no such file");
      return;
    }
    const platf::clipboard::file_list_entry entry = snapshot->entries[index];  // a copy for the worker
    if (offset > entry.size) {
      refuse_clipboard_file(response, client, SimpleWeb::StatusCode::client_error_range_not_satisfiable, "offset past the end");
      return;
    }

    // Shell: a retry of a file whose earlier read is stalled would hold up a second reader thread,
    // and the four of them could all end up waiting for the same file. An earlier read that is only
    // queued or briefly busy (the client dropped it a moment ago) does not count.
    clipboard_file_key_t key {named_cert_p->uuid, snapshot->id, index};
    auto reading = clipboard_file_read_t::begin(key, true);
    if (!reading) {
      BOOST_LOG(info) << "Clipboard file " << index << " not sent to [" << client << "]: an earlier read of it is stalled";
      response->write(SimpleWeb::StatusCode::server_error_service_unavailable, clipboard_file_reading_body);
      response->close_connection_after_response = true;
      return;
    }

    auto stream = std::make_shared<clipboard_file_stream_t>();
    stream->io = io;
    stream->client = client;
    stream->key = std::move(key);
    stream->index = index;
    stream->offset = offset;
    stream->length = entry.size - offset;

    // Opening and checking the file can stall on a network share, so a file reader thread does it
    // and the io thread then answers. named_cert_p is not used off the io thread. reading counts the
    // read of this file until the job ends.
    clipboard_file_reader.post([stream, response = std::move(response), entry, reading = std::move(reading)]() mutable {
      bool changed = false;
      std::string why;
      {
        clipboard_file_read_t::call_t call {*reading};
        try {
          if (!run_as_console_user([&](HANDLE) {
                stream->file = platf::clipboard::open_listed_file(entry, changed, why);
              })) {
            why = "no user session";
          }
        } catch (const std::exception &e) {
          RevertToSelf();  // the job may have thrown while impersonating the console user
          BOOST_LOG(error) << "Clipboard file job failed: " << e.what();
          why = "cannot open file";
        }
        LARGE_INTEGER position;
        position.QuadPart = static_cast<LONGLONG>(stream->offset);
        if (stream->file != INVALID_HANDLE_VALUE && stream->offset > 0 && !SetFilePointerEx(stream->file, position, nullptr, FILE_BEGIN)) {
          CloseHandle(stream->file);
          stream->file = INVALID_HANDLE_VALUE;
          changed = false;
          why = "cannot seek";
        }
      }
      boost::asio::post(*stream->io, [stream, response = std::move(response), changed, why]() {
        if (stream->file == INVALID_HANDLE_VALUE) {
          refuse_clipboard_file(response, stream->client, changed ? SimpleWeb::StatusCode::client_error_conflict : SimpleWeb::StatusCode::server_error_internal_server_error, why);
          return;
        }
        stream->buffer.resize(static_cast<std::size_t>(std::min<std::uint64_t>(clipboard_file_chunk_bytes, std::max<std::uint64_t>(stream->length, 1))));
        stream->response = response;

        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/octet-stream");
        headers.emplace("Content-Length", std::to_string(stream->length));
        // One file per connection, so an aborted transfer never leaves a half-sent body on a reused one.
        response->close_connection_after_response = true;
        response->write(SimpleWeb::StatusCode::success_ok, headers);
        send_clipboard_file_chunk(stream, std::move(response));
      });
    });
  }
#endif

  /**
   * @brief Shell: GET /actions/bitrate?kbps=N changes the bitrate of the caller's running stream
   *        without restarting it (Hermit's stream panel). Replies "bitrate=<encoding kbps>" once the
   *        request is queued for the encoder thread (a refusal by the encoder is only logged).
   *        403 if the caller is not streaming, 501 if the encoder cannot change it live.
   */
  void setBitrate(resp_https_t response, req_https_t request) {
    print_req<ShellHTTPS>(request);

    auto named_cert_p = get_verified_cert(request);
    if (!(named_cert_p->perm & PERM::_allow_view)) {
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    auto args = request->parse_query_string();
    int kbps = 0;
    try {
      kbps = std::stoi(std::string {get_arg(args, "kbps", "0")});
    } catch (...) {
      kbps = 0;
    }
    if (kbps < 500 || kbps > 1000000) {
      response->write(SimpleWeb::StatusCode::client_error_bad_request, "kbps out of range"sv);
      response->close_connection_after_response = true;
      return;
    }

    if (!video::live_bitrate_supported()) {
      response->write(SimpleWeb::StatusCode::server_error_not_implemented, "encoder cannot change the bitrate live"sv);
      response->close_connection_after_response = true;
      return;
    }

    auto session = rtsp_stream::find_session(named_cert_p->uuid);
    int encoder_kbps = session ? stream::session::change_bitrate(*session, kbps) : 0;
    if (!encoder_kbps) {
      response->write(SimpleWeb::StatusCode::client_error_forbidden, "not streaming"sv);
      response->close_connection_after_response = true;
      return;
    }

    response->write("bitrate=" + std::to_string(encoder_kbps) + "\n");
  }

  /**
   * @brief Shell: turn the host PC off or restart it from a client (Hermit's PC list).
   * @details `action=query` answers whether this client may do it and which other clients are
   * streaming right now, so the client can warn before anyone is cut off. `action=shutdown` turns
   * the PC off, `action=restart` restarts it; `force=1` also closes apps that would hold it back
   * (unsaved work is lost). Needs the launch permission, the same control as starting apps.
   */
  void power(resp_https_t response, req_https_t request) {
    print_req<ShellHTTPS>(request);

    auto named_cert_p = get_verified_cert(request);
    const bool allowed = !!(named_cert_p->perm & PERM::launch);

    auto args = request->parse_query_string();
    const auto action = get_arg(args, "action", "query");

    if (action == "query"sv) {
      std::string body;
#ifdef _WIN32
      body += "supported=1\n";
#else
      body += "supported=0\n";
#endif
      body += allowed ? "allowed=1\n" : "allowed=0\n";
      // Streams of other clients (this client is on its PC list, so not streaming itself)
      std::string clients;
      int streaming = 0;
      for (const auto &session : (allowed ? rtsp_stream::get_all_sessions() : decltype(rtsp_stream::get_all_sessions()) {})) {
        if (stream::session::state(*session) == stream::session::state_e::STOPPED) {
          continue;
        }
        auto info = nlohmann::json::parse(stream::session::live_info_json(*session), nullptr, false);
        if (!info.is_object() || info.value("uuid", std::string {}) == named_cert_p->uuid) {
          continue;
        }
        auto name = info.value("client", std::string {});
        name.erase(std::remove_if(name.begin(), name.end(), [](char c) {
          return c == '\r' || c == '\n';
        }), name.end());
        clients += "client=" + name + "\n";
        ++streaming;
      }
      body += "streaming=" + std::to_string(streaming) + "\n" + clients;
      response->write(body);
      return;
    }

    const bool restart = action == "restart"sv;
    if (action != "shutdown"sv && !restart) {
      response->write(SimpleWeb::StatusCode::client_error_bad_request, "unknown action"sv);
      response->close_connection_after_response = true;
      return;
    }
    if (!allowed) {
      BOOST_LOG(info) << (restart ? "Restart" : "Shutdown") << " from [" << named_cert_p->name << "] denied: no launch permission";
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

#ifdef _WIN32
    const bool force = get_arg(args, "force", "0") == "1"sv;
    BOOST_LOG(info) << (restart ? "Restarting" : "Shutting down") << " the PC, asked by [" << named_cert_p->name << "]" << (force ? " (closing apps that hold it back)" : "");

    HANDLE token = nullptr;
    LUID luid;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token) &&
        LookupPrivilegeValue(nullptr, SE_SHUTDOWN_NAME, &luid)) {
      TOKEN_PRIVILEGES tp = {};
      tp.PrivilegeCount = 1;
      tp.Privileges[0].Luid = luid;
      tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
      AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    }
    if (token) {
      CloseHandle(token);
    }

    // A few seconds of grace so this answer reaches the client before the network goes down
    DWORD flags = restart ? SHUTDOWN_RESTART : SHUTDOWN_POWEROFF;
    if (force) {
      flags |= SHUTDOWN_FORCE_SELF | SHUTDOWN_FORCE_OTHERS;
    }
    // The notice Windows shows before shutting down: English, or Korean when the system display language is Korean
    const std::wstring device = platf::from_utf8(named_cert_p->name);
    std::wstring message = is_korean_ui_language() ?
                             device + (restart ? L"에서 PC 다시 시작을 요청했습니다." : L"에서 PC 종료를 요청했습니다.") :
                             device + (restart ? L" requested a restart of this PC." : L" requested a shutdown of this PC.");
    const DWORD result = InitiateShutdownW(nullptr, message.data(), 5, flags,
                                           SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
    if (result != ERROR_SUCCESS) {
      BOOST_LOG(error) << (restart ? "Restart" : "Shutdown") << " failed: " << result;
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error, std::string(action) + " failed: " + std::to_string(result));
      response->close_connection_after_response = true;
      return;
    }
    response->write("ok\n"sv);
#else
    response->write(SimpleWeb::StatusCode::server_error_not_implemented, "not supported on this host"sv);
    response->close_connection_after_response = true;
#endif
  }

  /// io is the HTTPS server's io context: slow file work replies through it (see run_file_job).
  void getClipboard(resp_https_t response, req_https_t request, [[maybe_unused]] const std::shared_ptr<SimpleWeb::io_context> &io) {
    print_req<ShellHTTPS>(request);

    auto named_cert_p = get_verified_cert(request);

    if (
      !(named_cert_p->perm & PERM::_allow_view)
      || !(named_cert_p->perm & PERM::clipboard_read)
    ) {
      BOOST_LOG(debug) << "Permission Read Clipboard denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    auto args = request->parse_query_string();
    auto clipboard_type = get_arg(args, "type");
    // "info", "image", "files", "filelist" and "filedata" are Shell extensions; stock clients only use "text".
    bool type_supported = clipboard_type == "text"sv;
#ifdef _WIN32
    type_supported = type_supported || clipboard_type == "info"sv || clipboard_type == "image"sv || clipboard_type == "files"sv ||
                     clipboard_type == "filelist"sv || clipboard_type == "filedata"sv;
#endif
    if (!type_supported) {
      BOOST_LOG(debug) << "Clipboard type [" << clipboard_type << "] is not supported!";

      response->write(SimpleWeb::StatusCode::client_error_bad_request);
      response->close_connection_after_response = true;
      return;
    }

    std::list<std::string> connected_uuids = rtsp_stream::get_all_session_uuids();

    bool found = !connected_uuids.empty();

    if (found) {
      found = (std::find(connected_uuids.begin(), connected_uuids.end(), named_cert_p->uuid) != connected_uuids.end());
    }

    if (!found) {
      BOOST_LOG(debug) << "Client ["<< named_cert_p->name << "] trying to get clipboard is not connected to a stream";

      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      response->close_connection_after_response = true;
      return;
    }

#ifdef _WIN32
    if (clipboard_type == "info"sv) {
      // Sequence number first so a change between the two reads makes the client re-check later.
      auto seq = platf::clipboard::sequence();
      auto type = platf::clipboard::current_type();
      // "files=stream": type=filelist and type=filedata are available. Clients read the lines they
      // know by key, so older ones ignore it.
      response->write("seq=" + std::to_string(seq) + "\ntype=" + type + "\nfiles=stream\n");
      return;
    }
    if (clipboard_type == "image"sv) {
      // Empty body means there is no image; 413 that there is one, but over the size limit; 422 that
      // there is one that cannot be read or converted to PNG; 503 that the clipboard is busy.
      std::string png;
      auto status = platf::clipboard::get_image_png(png);
      if (status == platf::clipboard::status_e::busy) {
        refuse_clipboard_busy(response, named_cert_p->name, "image read"sv);
        return;
      }
      if (status == platf::clipboard::status_e::too_large) {
        response->write(SimpleWeb::StatusCode::client_error_payload_too_large);
        response->close_connection_after_response = true;
        return;
      }
      if (status != platf::clipboard::status_e::ok && status != platf::clipboard::status_e::none) {
        BOOST_LOG(info) << "Clipboard image not sent to [" << named_cert_p->name << "]: it cannot be converted to PNG";
        response->write(SimpleWeb::StatusCode::client_error_unprocessable_entity, std::string {platf::clipboard::reason_image_not_convertible} + "\nthe clipboard image cannot be converted to PNG");
        response->close_connection_after_response = true;
        return;
      }
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "image/png");
      response->write(SimpleWeb::StatusCode::success_ok, png, headers);
      return;
    }
    if (clipboard_type == "files"sv || clipboard_type == "filelist"sv || clipboard_type == "filedata"sv) {
      if (!(named_cert_p->perm & PERM::file_dwnload)) {
        BOOST_LOG(debug) << "Permission Download Files denied for [" << named_cert_p->name << "]";
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        response->close_connection_after_response = true;
        return;
      }
    }
    if (clipboard_type == "filedata"sv) {
      send_clipboard_file(response, named_cert_p, args, io);
      return;
    }
    if (clipboard_type == "filelist"sv) {
      // Sequence number first, as for type=info. Folders are expanded now, files are read only
      // when the client asks for them (type=filedata).
      auto seq = platf::clipboard::sequence();
      std::vector<std::filesystem::path> roots;
      if (platf::clipboard::get_file_drop_list(roots) == platf::clipboard::status_e::busy) {
        refuse_clipboard_busy(response, named_cert_p->name, "file list"sv);
        return;
      }
      if (roots.empty()) {
        response->write(SimpleWeb::StatusCode::success_ok, ""sv);  // no files on the clipboard
        return;
      }
      // The folder walk runs on the clipboard worker, off the io thread.
      auto snapshot = std::make_shared<clipboard_snapshot_t>();
      snapshot->stream_session = current_stream_session(named_cert_p->uuid);
      run_file_job(io, response, [roots = std::move(roots), seq, snapshot, client = named_cert_p->name, client_uuid = named_cert_p->uuid]() {
        snapshot->console_session = WTSGetActiveConsoleSessionId();
        std::string error;
        bool listed = false;
        if (!run_as_console_user([&](HANDLE) {
              listed = platf::clipboard::list_paths(roots, platf::clipboard::max_stream_files_bytes, snapshot->entries, error);
            })) {
          error = "no user session";
        }
        if (!listed) {
          BOOST_LOG(info) << "Clipboard file list not sent to [" << client << "]: " << error;
          return file_reply_t {file_error_status(error), file_error_body(error), {}};
        }
        snapshot->id = crypto::rand_alphabet(16, "0123456789abcdef"sv);
        std::uint64_t total = 0;
        for (const auto &entry : snapshot->entries) {
          total += entry.size;
        }
        auto manifest = platf::clipboard::format_file_list(seq, snapshot->id, snapshot->entries);
        BOOST_LOG(info) << "Clipboard file list sent to [" << client << "]: " << snapshot->entries.size() << " items, " << total << " bytes";
        store_clipboard_snapshot(client_uuid, snapshot);
        return file_reply_t {SimpleWeb::StatusCode::success_ok, std::move(manifest), "text/plain; charset=utf-8"};
      });
      return;
    }
    if (clipboard_type == "files"sv) {
      std::vector<std::filesystem::path> roots;
      if (platf::clipboard::get_file_drop_list(roots) == platf::clipboard::status_e::busy) {
        refuse_clipboard_busy(response, named_cert_p->name, "files"sv);
        return;
      }
      if (roots.empty()) {
        response->write(SimpleWeb::StatusCode::success_ok, ""sv);  // no files on the clipboard
        return;
      }
      // Reading up to 256 MB runs on the clipboard worker, off the io thread.
      run_file_job(io, response, [roots = std::move(roots), client = named_cert_p->name]() {
        std::string archive;
        std::string error;
        bool packed = false;
        if (!run_as_console_user([&](HANDLE) {
              packed = platf::clipboard::archive_paths(roots, archive, error);
            })) {
          error = "no user session";
        }
        if (!packed) {
          BOOST_LOG(info) << "Clipboard files not sent to [" << client << "]: " << error;
          return file_reply_t {file_error_status(error), file_error_body(error), {}};
        }
        return file_reply_t {SimpleWeb::StatusCode::success_ok, std::move(archive), "application/octet-stream"};
      });
      return;
    }

    // Busy (503) and unreadable (500) are told apart from an empty clipboard (empty 200), which is
    // all stock clients get when there is no text.
    std::string content;
    auto status = platf::clipboard::get_text(content);
    if (status == platf::clipboard::status_e::busy) {
      refuse_clipboard_busy(response, named_cert_p->name, "text read"sv);
      return;
    }
    if (status == platf::clipboard::status_e::failed) {
      BOOST_LOG(warning) << "Clipboard text not sent to [" << named_cert_p->name << "]: it cannot be read";
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "the clipboard text cannot be read"sv);
      response->close_connection_after_response = true;
      return;
    }
#else
    std::string content = platf::get_clipboard();
#endif
    response->write(content);
    return;
  }

  void
  setClipboard(resp_https_t response, req_https_t request, [[maybe_unused]] const std::shared_ptr<SimpleWeb::io_context> &io) {
    print_req<ShellHTTPS>(request);

    auto named_cert_p = get_verified_cert(request);

    if (
      !(named_cert_p->perm & PERM::_allow_view)
      || !(named_cert_p->perm & PERM::clipboard_set)
    ) {
      BOOST_LOG(debug) << "Permission Write Clipboard denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    auto args = request->parse_query_string();
    auto clipboard_type = get_arg(args, "type");
    bool type_supported = clipboard_type == "text"sv;
#ifdef _WIN32
    type_supported = type_supported || clipboard_type == "image"sv || clipboard_type == "files"sv;
#endif
    if (!type_supported) {
      BOOST_LOG(debug) << "Clipboard type [" << clipboard_type << "] is not supported!";

      response->write(SimpleWeb::StatusCode::client_error_bad_request);
      response->close_connection_after_response = true;
      return;
    }

    std::list<std::string> connected_uuids = rtsp_stream::get_all_session_uuids();

    bool found = !connected_uuids.empty();

    if (found) {
      found = (std::find(connected_uuids.begin(), connected_uuids.end(), named_cert_p->uuid) != connected_uuids.end());
    }

    if (!found) {
      BOOST_LOG(debug) << "Client ["<< named_cert_p->name << "] trying to set clipboard is not connected to a stream";

      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      response->close_connection_after_response = true;
      return;
    }

    std::string content = request->content.string();

    bool success = false;
#ifdef _WIN32
    // Shell: ok, or why the clipboard was not set (busy is answered with 503)
    auto status = platf::clipboard::status_e::failed;
    if (clipboard_type == "image"sv) {
      // 413 for an image over the byte or pixel limit, so clients can say it is too large
      std::uint32_t width = 0, height = 0;
      if (content.size() > platf::clipboard::max_image_bytes ||
          (platf::clipboard::png_size(content, width, height) && static_cast<std::uint64_t>(width) * height > platf::clipboard::max_image_pixels)) {
        response->write(SimpleWeb::StatusCode::client_error_payload_too_large);
        response->close_connection_after_response = true;
        return;
      }
      status = platf::clipboard::set_image_png(content);
      if (status == platf::clipboard::status_e::not_convertible) {
        // Not a PNG at all: the client's mistake, not a host failure
        response->write(SimpleWeb::StatusCode::client_error_unprocessable_entity, std::string {platf::clipboard::reason_image_not_convertible} + "\nthe data is not a PNG image");
        response->close_connection_after_response = true;
        return;
      }
    } else if (clipboard_type == "files"sv) {
      if (!(named_cert_p->perm & PERM::file_upload)) {
        BOOST_LOG(debug) << "Permission Upload Files denied for [" << named_cert_p->name << "]";
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        response->close_connection_after_response = true;
        return;
      }
      if (content.size() > platf::clipboard::max_files_archive_bytes) {
        response->write(SimpleWeb::StatusCode::client_error_payload_too_large);
        response->close_connection_after_response = true;
        return;
      }
      // Checking and unpacking up to 256 MB runs on the clipboard worker, off the io thread.
      run_file_job(io, response, [content = std::move(content), client = named_cert_p->name]() mutable {
        std::vector<platf::clipboard::archive_entry> entries;
        std::string error;
        if (!platf::clipboard::decode_archive(content, entries, error)) {
          BOOST_LOG(info) << "Clipboard files from [" << client << "] rejected: " << error;
          return file_reply_t {archive_error_status(error), file_error_body(error), {}};
        }
        content.clear();
        content.shrink_to_fit();

        std::vector<std::filesystem::path> top_level;
        bool extracted = false;
        if (!run_as_console_user([&](HANDLE token) {
              extracted = platf::clipboard::extract_archive(entries, platf::clipboard::staging_root(token), top_level, error);
            })) {
          error = "no user session";
        }
        if (!extracted) {
          BOOST_LOG(warning) << "Clipboard files from [" << client << "] not saved: " << error;
        }
        auto placed = extracted ? platf::clipboard::set_file_drop_list(top_level) : platf::clipboard::status_e::failed;
        if (placed == platf::clipboard::status_e::busy) {
          BOOST_LOG(info) << "Clipboard files from [" << client << "] not placed: another program is using the clipboard";
          return file_reply_t {SimpleWeb::StatusCode::server_error_service_unavailable, std::string {clipboard_busy_body}, {}};
        }
        if (placed != platf::clipboard::status_e::ok) {
          BOOST_LOG(debug) << "Setting clipboard failed!";
          return file_reply_t {SimpleWeb::StatusCode::server_error_internal_server_error, {}, {}};
        }
        // Lets the client recognise its own write and not fetch it back later.
        return file_reply_t {SimpleWeb::StatusCode::success_ok, "seq=" + std::to_string(platf::clipboard::sequence()) + "\n", {}};
      });
      return;
    } else {
      status = platf::clipboard::set_text(content);
    }
    if (status == platf::clipboard::status_e::busy) {
      refuse_clipboard_busy(response, named_cert_p->name, clipboard_type == "image"sv ? "image write"sv : "text write"sv);
      return;
    }
    success = status == platf::clipboard::status_e::ok;
#else
    success = platf::set_clipboard(content);
#endif

    if (!success) {
      BOOST_LOG(debug) << "Setting clipboard failed!";

      response->write(SimpleWeb::StatusCode::server_error_internal_server_error);
      response->close_connection_after_response = true;
      return;
    }

#ifdef _WIN32
    // Lets the client recognise its own write and not fetch it back later.
    response->write("seq=" + std::to_string(platf::clipboard::sequence()) + "\n");
#else
    response->write();
#endif
    return;
  }

  void setup(const std::string &pkey, const std::string &cert) {
    conf_intern.pkey = pkey;
    conf_intern.servercert = cert;
  }

  void start() {
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    auto port_http = net::map_port(PORT_HTTP);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::shell.address_family);

    bool clean_slate = config::shell.flags[config::flag::FRESH_STATE];

    if (!clean_slate) {
      load_state();
    }

    auto pkey = file_handler::read_file(config::nvhttp.pkey.c_str());
    auto cert = file_handler::read_file(config::nvhttp.cert.c_str());
    setup(pkey, cert);

    // resume doesn't always get the parameter "localAudioPlayMode"
    // launch will store it in host_audio
    bool host_audio {};

    https_server_t https_server {config::nvhttp.cert, config::nvhttp.pkey};
    http_server_t http_server;

    // Verify certificates after establishing connection
    https_server.verify = [](req_https_t req, SSL *ssl) {
      crypto::x509_t x509 {
#if OPENSSL_VERSION_MAJOR >= 3
        SSL_get1_peer_certificate(ssl)
#else
        SSL_get_peer_certificate(ssl)
#endif
      };
      if (!x509) {
        BOOST_LOG(info) << "unknown -- denied"sv;
        return false;
      }

      bool verified = false;
      p_named_cert_t named_cert_p;

      auto fg = util::fail_guard([&]() {
        char subject_name[256];

        X509_NAME_oneline(X509_get_subject_name(x509.get()), subject_name, sizeof(subject_name));

        if (verified) {
          BOOST_LOG(debug) << subject_name << " -- "sv << "verified, device name: "sv << named_cert_p->name;
        } else {
          BOOST_LOG(debug) << subject_name << " -- "sv << "denied"sv;
        }

      });

      auto err_str = cert_chain.verify(x509.get(), named_cert_p);
      if (err_str) {
        BOOST_LOG(warning) << "SSL Verification error :: "sv << err_str;
        return verified;
      }

      verified = true;
      req->userp = named_cert_p;

      return true;
    };

    https_server.on_verify_failed = [](resp_https_t resp, req_https_t req) {
      pt::ptree tree;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_xml(data, tree);
        resp->write(data.str());
        resp->close_connection_after_response = true;
      });

      tree.put("root.<xmlattr>.status_code"s, 401);
      tree.put("root.<xmlattr>.query"s, req->path);
      tree.put("root.<xmlattr>.status_message"s, "The client is not authorized. Certificate verification failed."s);
    };

    https_server.default_resource["GET"] = not_found<ShellHTTPS>;
    https_server.resource["^/serverinfo$"]["GET"] = serverinfo<ShellHTTPS>;
    https_server.resource["^/pair$"]["GET"] = pair<ShellHTTPS>;
    https_server.resource["^/applist$"]["GET"] = applist;
    https_server.resource["^/appasset$"]["GET"] = appasset;
    https_server.resource["^/launch$"]["GET"] = [&host_audio](auto resp, auto req) {
      launch(host_audio, resp, req);
    };
    https_server.resource["^/resume$"]["GET"] = [&host_audio](auto resp, auto req) {
      resume(host_audio, resp, req);
    };
    https_server.resource["^/cancel$"]["GET"] = cancel;
    https_server.resource["^/actions/clipboard$"]["GET"] = [&https_server](auto resp, auto req) {
      getClipboard(resp, req, https_server.io_service);
    };
    https_server.resource["^/actions/bitrate$"]["GET"] = setBitrate;
    https_server.resource["^/actions/power$"]["GET"] = power;
    https_server.resource["^/actions/clipboard$"]["POST"] = [&https_server](auto resp, auto req) {
      setClipboard(resp, req, https_server.io_service);
    };

    https_server.config.reuse_address = true;
    https_server.config.address = net::af_to_any_address_string(address_family);
    https_server.config.port = port_https;
    // Shell: SimpleWeb ends a request whose body or response takes longer than timeout_content
    // (300 s by default). 256 MB type=files transfers need more on slow links; type=filedata
    // downloads are still cut after this and resume with offset.
    https_server.config.timeout_content = 1800;

    http_server.default_resource["GET"] = not_found<SimpleWeb::HTTP>;
    http_server.resource["^/serverinfo$"]["GET"] = serverinfo<SimpleWeb::HTTP>;
    http_server.resource["^/pair$"]["GET"] = pair<SimpleWeb::HTTP>;
    // Shell: HTTP only, where clients send it. Over HTTPS a client means its own paired device,
    // which is removed in the web UI, so that keeps answering 404.
    http_server.resource["^/unpair$"]["GET"] = unpair;

    http_server.config.reuse_address = true;
    http_server.config.address = net::af_to_any_address_string(address_family);
    http_server.config.port = port_http;
    // Shell: SimpleWeb's default, stated because it is how long a client waits for the PIN
    // (pair_pin_timeout): its getservercert request stays open until the PIN is entered.
    http_server.config.timeout_content = static_cast<long>(pair_pin_timeout.count());

    auto accept_and_run = [&](auto *http_server) {
      try {
        http_server->start();
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling http_server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }

        BOOST_LOG(fatal) << "Couldn't start http server on ports ["sv << port_https << ", "sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
#ifdef _WIN32
    clipboard_worker.start(1);
    clipboard_file_reader.start(clipboard_file_reader_threads);
#endif
    std::thread ssl {accept_and_run, &https_server};
    std::thread tcp {accept_and_run, &http_server};

    // Wait for any event
    shutdown_event->view();

    {
      std::lock_guard lock {map_id_sess_mutex};
      for (auto &[unique_id, sess] : map_id_sess) {
        answer_waiting_request(sess, "The host is shutting down");
      }
      map_id_sess.clear();
    }

    https_server.stop();
    http_server.stop();

    ssl.join();
    tcp.join();
#ifdef _WIN32
    const auto clipboard_deadline = std::chrono::steady_clock::now() + clipboard_stop_timeout;
    bool clipboard_ended = clipboard_worker.stop(clipboard_deadline);
    clipboard_ended = clipboard_file_reader.stop(clipboard_deadline) && clipboard_ended;
    if (!clipboard_ended) {
      clipboard_threads_left_behind = true;
      // Shell: a job left behind posts its reply, with the response, to the stopped HTTPS io
      // context. Kept alive, the context never runs nor destroys that reply, so no response is
      // finished (its deleter uses https_server) or destroyed after this returns.
      new std::shared_ptr<SimpleWeb::io_context>(https_server.io_service);
    }
#endif
  }

  std::atomic_bool clipboard_threads_left_behind {false};

  void
  erase_all_clients() {
    client_t client;
    client_root = client;
    cert_chain.clear();
    save_state();
    load_state();
  }

  void stop_session(stream::session_t& session, bool graceful) {
    stream::session::set_end_reason(session, "host");
    if (graceful) {
      stream::session::graceful_stop(session);
    } else {
      stream::session::stop(session);
    }
  }

  bool find_and_stop_session(const std::string& uuid, bool graceful) {
    // Shell: every session of the device (a reconnected one can briefly have two)
    auto sessions = rtsp_stream::find_sessions(uuid);
    for (auto &session : sessions) {
      stop_session(*session, graceful);
    }
    return !sessions.empty();
  }

  void update_session_info(stream::session_t& session, const std::string& name, const crypto::PERM newPerm) {
    stream::session::update_device_info(session, name, newPerm);
  }

  bool find_and_udpate_session_info(const std::string& uuid, const std::string& name, const crypto::PERM newPerm) {
    auto sessions = rtsp_stream::find_sessions(uuid);
    for (auto &session : sessions) {
      update_session_info(*session, name, newPerm);
    }
    return !sessions.empty();
  }

  bool update_device_info(
    const std::string& uuid,
    const std::string& name,
    const std::string& display_mode,
    const cmd_list_t& do_cmds,
    const cmd_list_t& undo_cmds,
    const crypto::PERM newPerm,
    const bool enable_legacy_ordering,
    const bool allow_client_commands,
    const bool always_use_virtual_display
  ) {
    find_and_udpate_session_info(uuid, name, newPerm);

    client_t &client = client_root;
    auto it = client.named_devices.begin();
    for (; it != client.named_devices.end(); ++it) {
      auto named_cert_p = *it;
      if (named_cert_p->uuid == uuid) {
        named_cert_p->name = name;
        named_cert_p->display_mode = display_mode;
        named_cert_p->perm = newPerm;
        named_cert_p->do_cmds = do_cmds;
        named_cert_p->undo_cmds = undo_cmds;
        named_cert_p->enable_legacy_ordering = enable_legacy_ordering;
        named_cert_p->allow_client_commands = allow_client_commands;
        named_cert_p->always_use_virtual_display = always_use_virtual_display;
        save_state();
        return true;
      }
    }

    return false;
  }

  bool unpair_client(const std::string_view uuid) {
    bool removed = false;
    client_t &client = client_root;
    for (auto it = client.named_devices.begin(); it != client.named_devices.end();) {
      if ((*it)->uuid == uuid) {
        it = client.named_devices.erase(it);
        removed = true;
      } else {
        ++it;
      }
    }

    save_state();
    load_state();

    if (removed) {
      for (auto &session : rtsp_stream::find_sessions(uuid)) {
        stop_session(*session, true);
      }

      if (client.named_devices.empty()) {
        proc::proc.terminate();
      }
    }

    return removed;
  }
}  // namespace nvhttp
