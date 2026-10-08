/**
 * @file src/platform/windows/clipboard_helper.cpp
 * @brief Shell: the clipboard helper process and the platf::clipboard entry points that use it.
 *
 * shell.exe runs as SYSTEM (ShellService starts it in the user's session) and then does not see a
 * copy made in File Explorer: Windows lists only OLE's "DataObject" format to it, so CF_HDROP looks
 * absent, while every process of the signed-in user sees all formats. Remote Desktop solves the
 * same problem by running rdpclip.exe as the user; this does the same with tools\shellclip.exe.
 *
 * The helper is started as the console user with the user's own token (never elevated), with no
 * window, and talks over one pipe it inherits as its standard input and output (the pipe has a
 * random name, is created with FILE_FLAG_FIRST_PIPE_INSTANCE and a DACL for SYSTEM and
 * Administrators only, and its single client end is opened by this process before the helper is
 * started, so nothing else can get on it). Every request has a deadline; when the helper does not
 * answer in time, the request is answered "busy" (clients try again) and the helper is restarted.
 * It is also started again when it exits (with a growing delay) and stopped when the console
 * session changes or Shell stops. While no helper runs, the clipboard is read in this process as
 * before, so text and images keep working.
 */
#include "clipboard_helper.h"

#include <Windows.h>
#include <sddl.h>
#include <UserEnv.h>
#include <WtsApi32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>

#include "clipboard.h"
#include "clipboard_access.h"
#include "clipboard_protocol.h"
#include "misc.h"
#include "src/logging.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf::clipboard {
  namespace proto = protocol;

  namespace helper {
    namespace {
      /// How long a request may take, including the helper's clipboard retries (about 0.3 s) and,
      /// for images, the PNG conversion.
      constexpr auto request_timeout = 4s;
      constexpr auto image_timeout = 12s;

      constexpr DWORD pipe_buffer_bytes = 64 * 1024;
      constexpr auto backoff_min = 1s;
      constexpr auto backoff_max = 30s;
      /// A helper that ran this long before exiting gets a fresh (short) restart delay.
      constexpr auto stable_run = 60s;

      /// One helper process and its pipe. The handles close with the last reference, so a request
      /// still using the pipe when the manager drops the connection keeps a valid handle.
      struct connection_t {
        HANDLE pipe = nullptr;  ///< the server end, opened for overlapped I/O
        HANDLE process = nullptr;
        DWORD pid = 0;
        DWORD session = 0;  ///< the console session it was started for
        std::chrono::steady_clock::time_point started;
        std::atomic<bool> broken {false};  ///< a request failed on it: the manager restarts the helper

        ~connection_t() {
          if (pipe) {
            CloseHandle(pipe);
          }
          if (process) {
            CloseHandle(process);
          }
        }
      };

      struct state_t {
        std::mutex mutex;  ///< guards current and status
        std::shared_ptr<connection_t> current;
        helper_status_t status {"stopped", {}, 0, 0};
        std::thread thread;
        HANDLE stop_event = nullptr;  ///< manual reset: Shell is stopping
        HANDLE wake_event = nullptr;  ///< auto reset: a request found the connection broken
      };

      // Never destroyed: a request on another thread may still look at it while Shell exits.
      state_t &state = *new state_t;

      /// Requests go one at a time over the pipe.
      std::mutex &io_mutex = *new std::mutex;

      std::shared_ptr<connection_t> current_connection() {
        std::lock_guard lock(state.mutex);
        return state.current;
      }

      void set_status(const std::string &new_state, const std::string &detail, DWORD pid = 0) {
        std::lock_guard lock(state.mutex);
        state.status.state = new_state;
        state.status.detail = detail;
        state.status.pid = pid;
      }

      std::string error_text(DWORD error) {
        return "error " + std::to_string(error);
      }

      std::filesystem::path helper_path() {
        wchar_t module[MAX_PATH + 1] {};
        DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH + 1);
        if (length == 0 || length > MAX_PATH) {
          return {};
        }
        return std::filesystem::path(std::wstring(module, length)).parent_path() / L"tools" / L"shellclip.exe";
      }

      std::wstring random_pipe_name() {
        std::random_device device;
        wchar_t suffix[33] {};
        for (int i = 0; i < 32; ++i) {
          suffix[i] = L"0123456789abcdef"[device() % 16];
        }
        return L"\\\\.\\pipe\\Shell-clipboard-" + std::to_wstring(GetCurrentProcessId()) + L"-" + suffix;
      }

      /**
       * @brief Starts the helper as the signed-in console user.
       * @param no_user Set when the launch failed because nobody is signed in.
       * @return The connection, or nullptr with error set.
       */
      std::shared_ptr<connection_t> launch(DWORD session, std::string &error, bool &no_user) {
        no_user = false;
        auto exe = helper_path();
        std::error_code ec;
        if (exe.empty() || !std::filesystem::is_regular_file(exe, ec)) {
          error = "tools\\shellclip.exe is missing next to shell.exe";
          return nullptr;
        }

        HANDLE token = platf::retrieve_users_token(false);
        if (token == nullptr) {
          no_user = true;
          error = "no user is signed in at the console";
          return nullptr;
        }
        auto close_token = util::fail_guard([token]() {
          CloseHandle(token);
        });

        // The pipe: SYSTEM and Administrators only; the helper only ever inherits a handle to it.
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr)) {
          error = "cannot build the pipe's security descriptor (" + error_text(GetLastError()) + ")";
          return nullptr;
        }
        auto free_descriptor = util::fail_guard([descriptor]() {
          LocalFree(descriptor);
        });
        SECURITY_ATTRIBUTES pipe_security {sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};

        std::wstring name = random_pipe_name();
        HANDLE server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, pipe_buffer_bytes, pipe_buffer_bytes, 0, &pipe_security);
        if (server == INVALID_HANDLE_VALUE) {
          error = "cannot create the pipe (" + error_text(GetLastError()) + ")";
          return nullptr;
        }
        auto connection = std::make_shared<connection_t>();
        connection->pipe = server;

        // The one client end, opened here (as SYSTEM) and handed to the helper by inheritance.
        SECURITY_ATTRIBUTES inheritable {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, &inheritable, OPEN_EXISTING, 0, nullptr);
        if (client == INVALID_HANDLE_VALUE) {
          error = "cannot open the pipe (" + error_text(GetLastError()) + ")";
          return nullptr;
        }
        auto close_client = util::fail_guard([client]() {
          CloseHandle(client);  // the helper has its own copy once started
        });
        ULONG client_pid = 0;
        if (!GetNamedPipeClientProcessId(server, &client_pid) || client_pid != GetCurrentProcessId()) {
          error = "the pipe was opened by another process";
          return nullptr;
        }

        PVOID environment = nullptr;
        if (!CreateEnvironmentBlock(&environment, token, FALSE)) {
          error = "cannot read the user's environment (" + error_text(GetLastError()) + ")";
          return nullptr;
        }
        auto free_environment = util::fail_guard([environment]() {
          DestroyEnvironmentBlock(environment);
        });

        // Only the pipe is inherited, as the helper's standard input and output.
        SIZE_T attribute_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
        auto attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, attribute_size));
        if (attributes == nullptr || !InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) {
          if (attributes) {
            HeapFree(GetProcessHeap(), 0, attributes);
          }
          error = "cannot prepare the process attributes (" + error_text(GetLastError()) + ")";
          return nullptr;
        }
        auto free_attributes = util::fail_guard([attributes]() {
          DeleteProcThreadAttributeList(attributes);
          HeapFree(GetProcessHeap(), 0, attributes);
        });
        HANDLE inherited[] = {client};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) {
          error = "cannot set the inherited handles (" + error_text(GetLastError()) + ")";
          return nullptr;
        }

        STARTUPINFOEXW startup {};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = client;
        startup.StartupInfo.hStdOutput = client;
        startup.StartupInfo.hStdError = nullptr;
        startup.StartupInfo.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
        startup.lpAttributeList = attributes;

        std::wstring command = L"\"" + exe.wstring() + L"\"";
        std::wstring directory = exe.parent_path().wstring();
        PROCESS_INFORMATION process {};
        // The user's own token: as elevated as the user is at the console, never more.
        if (!CreateProcessAsUserW(token, exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, environment, directory.c_str(), &startup.StartupInfo, &process)) {
          error = "cannot start tools\\shellclip.exe as the user (" + error_text(GetLastError()) + ")";
          return nullptr;
        }
        CloseHandle(process.hThread);
        connection->process = process.hProcess;
        connection->pid = process.dwProcessId;
        connection->session = session;
        connection->started = std::chrono::steady_clock::now();
        return connection;
      }

      enum class io_e {
        ok,
        timeout,
        failed,
      };

      /// Reads or writes the whole buffer with the deadline, on the overlapped pipe handle.
      io_e transfer(HANDLE pipe, HANDLE event, bool write, char *data, std::size_t size, std::chrono::steady_clock::time_point deadline, DWORD &error) {
        error = 0;
        std::size_t done = 0;
        while (done < size) {
          auto chunk = static_cast<DWORD>(std::min<std::size_t>(size - done, 1u << 20));
          OVERLAPPED overlapped {};
          overlapped.hEvent = event;
          ResetEvent(event);
          DWORD transferred = 0;
          BOOL ok = write ? WriteFile(pipe, data + done, chunk, &transferred, &overlapped) : ReadFile(pipe, data + done, chunk, &transferred, &overlapped);
          if (!ok && GetLastError() != ERROR_IO_PENDING) {
            error = GetLastError();
            return io_e::failed;
          }
          if (!ok) {
            auto now = std::chrono::steady_clock::now();
            DWORD wait = now >= deadline ? 0 : static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            if (WaitForSingleObject(event, wait) != WAIT_OBJECT_0) {
              CancelIoEx(pipe, &overlapped);
              GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);  // the cancel has to finish before overlapped goes away
              return io_e::timeout;
            }
          }
          // Also after a call that finished at once: the count is only reliable from here.
          if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
            error = GetLastError();
            return io_e::failed;
          }
          if (transferred == 0) {
            error = ERROR_BROKEN_PIPE;
            return io_e::failed;
          }
          done += transferred;
        }
        return io_e::ok;
      }

      /// Marks the connection unusable; the manager thread stops that helper and starts a new one.
      void drop(const std::shared_ptr<connection_t> &connection, const std::string &why) {
        if (!connection->broken.exchange(true)) {
          BOOST_LOG(warning) << "Clipboard helper (pid " << connection->pid << ") " << why << "; starting it again";
          SetEvent(state.wake_event);
        }
      }

      /**
       * @brief Sends one request to the helper and waits for its reply.
       * @return False when no helper is available (the caller does the work in this process).
       * True otherwise, with status the helper's answer; busy when it did not answer in time or
       * the connection failed (the helper is then restarted).
       */
      bool request(proto::op_e op, std::string_view payload, status_e &status, std::string &reply) {
        reply.clear();
        auto connection = current_connection();
        if (!connection || connection->broken) {
          return false;
        }
        std::lock_guard io_lock(io_mutex);
        if (connection->broken) {
          return false;
        }
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event == nullptr) {
          status = status_e::failed;
          return true;
        }
        auto close_event = util::fail_guard([event]() {
          CloseHandle(event);
        });
        auto deadline = std::chrono::steady_clock::now() + ((op == proto::op_e::get_image || op == proto::op_e::set_image) ? image_timeout : request_timeout);

        auto fail = [&](const char *what, io_e result, DWORD code) {
          drop(connection, std::string(what) + (result == io_e::timeout ? " in time" : " (" + error_text(code) + ")"));
          status = status_e::busy;
          return true;
        };

        std::string frame;
        proto::encode_frame(frame, static_cast<std::uint8_t>(op), payload);
        DWORD io_error = 0;
        auto result = transfer(connection->pipe, event, true, frame.data(), frame.size(), deadline, io_error);
        if (result != io_e::ok) {
          return fail("did not take the request", result, io_error);
        }
        char header[proto::header_size] {};
        result = transfer(connection->pipe, event, false, header, sizeof(header), deadline, io_error);
        if (result != io_e::ok) {
          return fail("did not answer", result, io_error);
        }
        std::uint8_t code = 0;
        std::uint32_t length = 0;
        proto::decode_header(std::string_view(header, sizeof(header)), code, length);
        if (!proto::is_reply(code) || length > proto::max_reply_payload(op)) {
          return fail("sent a malformed reply", io_e::failed, ERROR_INVALID_DATA);
        }
        reply.resize(length);
        if (length > 0) {
          result = transfer(connection->pipe, event, false, reply.data(), reply.size(), deadline, io_error);
          if (result != io_e::ok) {
            return fail("did not finish its reply", result, io_error);
          }
        }
        auto reply_code = static_cast<proto::reply_e>(code);
        if (reply_code == proto::reply_e::bad_request) {
          BOOST_LOG(error) << "Clipboard helper rejected a request (op " << static_cast<int>(op) << "): " << reply;
          reply.clear();
        }
        status = to_status(reply_code);
        return true;
      }

      /// A reply that does not decode: the helper is out of step, start it again.
      void bad_reply(const char *what) {
        if (auto connection = current_connection()) {
          drop(connection, std::string("sent a reply that does not decode (") + what + ")");
        }
      }

      /// Logged once per reason: the request was done in this process and why. Requests come from
      /// the HTTPS thread and the file worker, so the last reason is kept under state.mutex.
      void note_fallback() {
        static std::string logged;
        std::string reason;
        {
          std::lock_guard lock(state.mutex);
          if (state.status.state == "not-needed") {
            return;
          }
          reason = state.status.state + (state.status.detail.empty() ? "" : ": " + state.status.detail);
          if (reason == logged) {
            return;
          }
          logged = reason;
        }
        BOOST_LOG(info) << "Clipboard read in Shell's own process (clipboard helper " << reason << ")";
      }

      /// Ends the helper: the pipe is disconnected first so it exits on its own, then it is killed
      /// if it does not.
      void end(const std::shared_ptr<connection_t> &connection) {
        connection->broken = true;
        DisconnectNamedPipe(connection->pipe);
        if (WaitForSingleObject(connection->process, 1500) != WAIT_OBJECT_0) {
          TerminateProcess(connection->process, 1);
          WaitForSingleObject(connection->process, 1500);
        }
      }

      void run() {
        auto backoff = backoff_min;
        auto next_try = std::chrono::steady_clock::now();
        std::string last_error;
        std::uint32_t launches = 0;
        set_status("starting", "");

        while (true) {
          auto now = std::chrono::steady_clock::now();
          auto connection = current_connection();
          DWORD session = WTSGetActiveConsoleSessionId();

          if (connection) {
            bool exited = WaitForSingleObject(connection->process, 0) == WAIT_OBJECT_0;
            bool session_changed = session != connection->session;
            if (exited || session_changed || connection->broken) {
              {
                std::lock_guard lock(state.mutex);
                state.current.reset();
              }
              if (now - connection->started >= stable_run) {
                backoff = backoff_min;
              }
              if (exited) {
                DWORD code = 0;
                GetExitCodeProcess(connection->process, &code);
                BOOST_LOG(warning) << "Clipboard helper (pid " << connection->pid << ") exited with code " << code << "; starting it again in " << std::chrono::duration_cast<std::chrono::seconds>(backoff).count() << " s";
              } else {
                if (session_changed) {
                  BOOST_LOG(info) << "Console session changed; stopping the clipboard helper (pid " << connection->pid << ")";
                }
                end(connection);
              }
              set_status("starting", "");
              next_try = now + backoff;
              if (!session_changed) {
                backoff = std::min(backoff * 2, backoff_max);  // a crash loop slows down; a session change does not
              }
              connection.reset();
            }
          }

          if (!connection && now >= next_try) {
            if (session == 0xFFFFFFFF) {
              set_status("no-user", "no console session");
              next_try = now + 2s;
            } else {
              std::string error;
              bool no_user = false;
              connection = launch(session, error, no_user);
              if (connection) {
                {
                  std::lock_guard lock(state.mutex);
                  state.current = connection;
                  state.status.state = "running";
                  state.status.detail.clear();
                  state.status.pid = connection->pid;
                  state.status.restarts = launches++;
                }
                BOOST_LOG(info) << "Clipboard helper started as the signed-in user (pid " << connection->pid << ")";
                last_error.clear();
              } else {
                set_status(no_user ? "no-user" : "failed", error);
                if (error != last_error) {
                  last_error = error;
                  if (no_user) {
                    BOOST_LOG(info) << "Clipboard helper not started: " << error << "; the clipboard is read in Shell's own process until someone signs in";
                  } else {
                    BOOST_LOG(warning) << "Clipboard helper not started: " << error << "; the clipboard is read in Shell's own process (copied files are not seen)";
                  }
                }
                next_try = now + (no_user ? 2s : backoff);
                if (!no_user) {
                  backoff = std::min(backoff * 2, backoff_max);
                }
              }
            }
          }

          HANDLE handles[3] = {state.stop_event, state.wake_event, connection ? connection->process : nullptr};
          DWORD count = connection ? 3 : 2;
          DWORD wait = 1000;
          if (!connection) {
            now = std::chrono::steady_clock::now();
            wait = now >= next_try ? 100 : static_cast<DWORD>(std::clamp<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(next_try - now).count(), 100, 2000));
          }
          if (WaitForMultipleObjects(count, handles, FALSE, wait) == WAIT_OBJECT_0) {
            break;  // Shell is stopping
          }
        }

        std::shared_ptr<connection_t> connection;
        {
          std::lock_guard lock(state.mutex);
          connection = std::move(state.current);
          state.current.reset();
          state.status.state = "stopped";
          state.status.detail.clear();
          state.status.pid = 0;
        }
        if (connection) {
          end(connection);
          BOOST_LOG(info) << "Clipboard helper stopped";
        }
      }

      class guard_t: public platf::deinit_t {
      public:
        ~guard_t() override {
          if (state.stop_event) {
            SetEvent(state.stop_event);
          }
          if (state.thread.joinable()) {
            state.thread.join();
          }
        }
      };
    }  // namespace

    std::unique_ptr<platf::deinit_t> start() {
      if (!platf::is_running_as_system()) {
        set_status("not-needed", "Shell runs as the signed-in user and sees the clipboard itself");
        return std::make_unique<platf::deinit_t>();
      }
      state.stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      state.wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (state.stop_event == nullptr || state.wake_event == nullptr) {
        set_status("failed", "cannot create the events (" + error_text(GetLastError()) + ")");
        BOOST_LOG(error) << "Clipboard helper not started: cannot create the events";
        return std::make_unique<platf::deinit_t>();
      }
      state.thread = std::thread(run);
      return std::make_unique<guard_t>();
    }
  }  // namespace helper

  helper_status_t helper_status() {
    std::lock_guard lock(helper::state.mutex);
    return helper::state.status;
  }

  // ---- The platf::clipboard entry points: the helper when it runs, this process otherwise ----

  status_e snapshot(std::uint32_t &seq, std::string &type) {
    status_e status = status_e::failed;
    std::string reply;
    if (helper::request(proto::op_e::snapshot, {}, status, reply)) {
      if (status == status_e::ok && !proto::decode_snapshot(reply, seq, type)) {
        helper::bad_reply("snapshot");
        return status_e::busy;
      }
      return status;
    }
    helper::note_fallback();
    return in_process::snapshot(seq, type);
  }

  std::string describe_for_log() {
    status_e status = status_e::failed;
    std::string reply;
    if (helper::request(proto::op_e::describe, {}, status, reply) && status == status_e::ok) {
      return "helper (pid " + std::to_string(helper_status().pid) + "): " + reply;
    }
    auto state = helper_status();
    return "in-process (helper " + state.state + (state.detail.empty() ? "" : ": " + state.detail) + "): " + in_process::describe_for_log();
  }

  status_e get_text(std::string &text) {
    status_e status = status_e::failed;
    if (helper::request(proto::op_e::get_text, {}, status, text)) {
      if (status != status_e::ok) {
        text.clear();
      }
      return status;
    }
    helper::note_fallback();
    return in_process::get_text(text);
  }

  status_e set_text(const std::string &text) {
    status_e status = status_e::failed;
    std::string reply;
    if (text.size() <= proto::max_text_payload && helper::request(proto::op_e::set_text, text, status, reply)) {
      return status;
    }
    helper::note_fallback();
    return in_process::set_text(text);
  }

  status_e get_image_png(std::string &png) {
    status_e status = status_e::failed;
    if (helper::request(proto::op_e::get_image, {}, status, png)) {
      if (status != status_e::ok) {
        png.clear();
      }
      return status;
    }
    helper::note_fallback();
    return in_process::get_image_png(png);
  }

  status_e set_image_png(const std::string &png) {
    status_e status = status_e::failed;
    std::string reply;
    if (png.size() <= proto::max_image_payload && helper::request(proto::op_e::set_image, png, status, reply)) {
      return status;
    }
    helper::note_fallback();
    return in_process::set_image_png(png);
  }

  status_e get_file_drop_list(std::vector<std::filesystem::path> &paths) {
    paths.clear();
    status_e status = status_e::failed;
    std::string reply;
    if (helper::request(proto::op_e::get_files, {}, status, reply)) {
      if (status != status_e::ok) {
        return status;
      }
      std::vector<std::string> listed;
      if (!proto::decode_paths(reply, listed)) {
        helper::bad_reply("file list");
        return status_e::busy;
      }
      for (const auto &item : listed) {
        std::wstring wide = widen(item);
        if (wide.empty()) {
          helper::bad_reply("file list path");
          paths.clear();
          return status_e::busy;
        }
        paths.emplace_back(std::move(wide));
      }
      return status_e::ok;
    }
    helper::note_fallback();
    return in_process::get_file_drop_list(paths);
  }

  status_e set_file_drop_list(const std::vector<std::filesystem::path> &paths) {
    std::vector<std::string> listed;
    listed.reserve(paths.size());
    for (const auto &p : paths) {
      listed.push_back(narrow(p.wstring()));
    }
    std::string payload = proto::encode_paths(listed);
    status_e status = status_e::failed;
    std::string reply;
    if (!payload.empty() && helper::request(proto::op_e::set_files, payload, status, reply)) {
      return status;
    }
    helper::note_fallback();
    return in_process::set_file_drop_list(paths);
  }
}  // namespace platf::clipboard
