// Git filter drivers (gitattributes `filter=<driver>`) for libgit2, which
// only has built-in filters and silently skips drivers. Commands configured
// as filter.<driver>.process, .clean and .smudge run the way Git runs them,
// so tools such as git-lfs do their own work: gg reimplements none of it.
#include "repository.hpp"

#include <git2/errors.h>
#include <git2/sys/errors.h>
#include <git2/sys/filter.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace gg::detail {
namespace {

// A child process with its standard input and output connected to us.
class Channel {
 public:
  Channel() = default;
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;
  ~Channel() { finish(); }

  // Runs `command` through the shell in `directory`, as Git does.
  bool start(const std::string& command, const std::string& directory) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE child_input = nullptr;
    HANDLE child_output = nullptr;
    if (!CreatePipe(&child_input, &input_, &attributes, 0)) return false;
    if (!CreatePipe(&output_, &child_output, &attributes, 0)) {
      CloseHandle(child_input);
      return false;
    }
    SetHandleInformation(input_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(output_, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_input;
    startup.hStdOutput = child_output;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    std::wstring line = wide(command);
    std::wstring cwd = wide(directory);
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(
        nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
        cwd.empty() ? nullptr : cwd.c_str(), &startup, &process);
    CloseHandle(child_input);
    CloseHandle(child_output);
    if (!started) return false;
    CloseHandle(process.hThread);
    process_ = process.hProcess;
    return true;
#else
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return false;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, sockets[0]);
    posix_spawn_file_actions_addclose(&actions, sockets[1]);
    const std::string script = "cd " + shell_quote(directory) + " && " + command;
    const char* argv[] = {"/bin/sh", "-c", script.c_str(), nullptr};
    const int spawned = posix_spawn(&process_, "/bin/sh", &actions, nullptr,
                                    const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(sockets[1]);
    if (spawned != 0) {
      close(sockets[0]);
      process_ = -1;
      return false;
    }
    socket_ = sockets[0];
    return true;
#endif
  }

  bool write(const char* data, std::size_t size) {
    while (size > 0) {
#ifdef _WIN32
      DWORD written = 0;
      const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1 << 20));
      if (input_ == nullptr || !WriteFile(input_, data, chunk, &written, nullptr)) {
        return false;
      }
#else
      // A driver that exits must not stop gg with SIGPIPE.
      const ssize_t written = send(socket_, data, size, MSG_NOSIGNAL);
      if (written <= 0) return false;
#endif
      data += written;
      size -= static_cast<std::size_t>(written);
    }
    return true;
  }

  // Reads exactly `size` bytes; false on end of output or failure.
  bool read(char* data, std::size_t size) {
    while (size > 0) {
      const std::ptrdiff_t got = read_some(data, size);
      if (got <= 0) return false;
      data += got;
      size -= static_cast<std::size_t>(got);
    }
    return true;
  }

  std::ptrdiff_t read_some(char* data, std::size_t size) {
#ifdef _WIN32
    DWORD got = 0;
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1 << 20));
    if (!ReadFile(output_, data, chunk, &got, nullptr)) return -1;
    return static_cast<std::ptrdiff_t>(got);
#else
    ssize_t got = 0;
    do {
      got = recv(socket_, data, size, 0);
    } while (got < 0 && errno == EINTR);
    return got;
#endif
  }

  // Signals end of input; the driver still writes its output.
  void close_input() {
#ifdef _WIN32
    if (input_ != nullptr) CloseHandle(input_);
    input_ = nullptr;
#else
    if (socket_ >= 0) shutdown(socket_, SHUT_WR);
#endif
  }

  // Closes both directions and waits for the process, as Git does on exit.
  // Returns whether it exited successfully.
  bool finish() {
    close_input();
#ifdef _WIN32
    if (output_ != nullptr) CloseHandle(output_);
    output_ = nullptr;
    if (process_ == nullptr) return false;
    WaitForSingleObject(process_, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process_, &code);
    CloseHandle(process_);
    process_ = nullptr;
    return code == 0;
#else
    if (socket_ >= 0) close(socket_);
    socket_ = -1;
    if (process_ <= 0) return false;
    int status = 0;
    while (waitpid(process_, &status, 0) < 0 && errno == EINTR) {
    }
    process_ = -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
  }

  static std::string shell_quote(std::string_view value) {
#ifdef _WIN32
    std::string result = "\"";
    for (const char character : value) {
      if (character == '"') result += '\\';
      result += character;
    }
    return result + "\"";
#else
    std::string result = "'";
    for (const char character : value) {
      if (character == '\'') {
        result += "'\\''";
      } else {
        result += character;
      }
    }
    return result + "'";
#endif
  }

 private:
#ifdef _WIN32
  static std::wstring wide(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
  }

  HANDLE input_ = nullptr;
  HANDLE output_ = nullptr;
  HANDLE process_ = nullptr;
#else
  int socket_ = -1;
  pid_t process_ = -1;
#endif
};

// pkt-line framing: four hex digits of total length, then data; "0000"
// is a flush packet.
constexpr std::size_t kMaxPacketData = 65516;

bool write_packet(Channel& channel, std::string_view data) {
  static constexpr char kDigits[] = "0123456789abcdef";
  const std::size_t length = data.size() + 4;  // At most kMaxPacketData + 4.
  const std::array<char, 4> header{kDigits[(length >> 12) & 0xf],
                                   kDigits[(length >> 8) & 0xf],
                                   kDigits[(length >> 4) & 0xf],
                                   kDigits[length & 0xf]};
  return channel.write(header.data(), header.size()) &&
         channel.write(data.data(), data.size());
}

bool write_flush(Channel& channel) { return channel.write("0000", 4); }

bool write_text(Channel& channel, std::string_view line) {
  return write_packet(channel, std::string(line) + "\n");
}

// Returns false on failure; `flush` is set for a flush packet.
bool read_packet(Channel& channel, std::string& data, bool& flush) {
  std::array<char, 4> header{};
  if (!channel.read(header.data(), header.size())) return false;
  std::size_t length = 0;
  for (const char digit : header) {
    length <<= 4;
    if (digit >= '0' && digit <= '9') {
      length |= static_cast<std::size_t>(digit - '0');
    } else if (digit >= 'a' && digit <= 'f') {
      length |= static_cast<std::size_t>(digit - 'a' + 10);
    } else if (digit >= 'A' && digit <= 'F') {
      length |= static_cast<std::size_t>(digit - 'A' + 10);
    } else {
      return false;
    }
  }
  flush = length == 0;
  data.clear();
  if (flush) return true;
  if (length < 4) return false;
  data.resize(length - 4);
  return channel.read(data.data(), data.size());
}

// Reads text packets up to a flush, without their trailing newlines.
bool read_list(Channel& channel, std::vector<std::string>& lines) {
  lines.clear();
  std::string data;
  bool flush = false;
  while (read_packet(channel, data, flush)) {
    if (flush) return true;
    if (!data.empty() && data.back() == '\n') data.pop_back();
    lines.push_back(data);
  }
  return false;
}

std::optional<std::string> status_of(const std::vector<std::string>& lines) {
  std::optional<std::string> status;
  for (const std::string& line : lines) {
    if (line.starts_with("status=")) status = line.substr(7);
  }
  return status;
}

// A filter.<driver>.process command, started on first use and shared by
// every file of its working tree.
class DriverProcess {
 public:
  std::mutex mutex;  // Held for a whole request.

  // Starts the process and negotiates capabilities if it is not running.
  bool ready(const std::string& command, const std::string& directory) {
    if (aborted_) return false;
    if (channel_ != nullptr) return true;
    auto channel = std::make_unique<Channel>();
    std::vector<std::string> lines;
    if (!channel->start(command, directory) ||
        !write_text(*channel, "git-filter-client") ||
        !write_text(*channel, "version=2") || !write_flush(*channel) ||
        !read_list(*channel, lines) ||
        std::ranges::find(lines, "git-filter-server") == lines.end() ||
        std::ranges::find(lines, "version=2") == lines.end() ||
        !write_text(*channel, "capability=clean") ||
        !write_text(*channel, "capability=smudge") || !write_flush(*channel) ||
        !read_list(*channel, lines)) {
      aborted_ = true;
      return false;
    }
    clean_ = std::ranges::find(lines, "capability=clean") != lines.end();
    smudge_ = std::ranges::find(lines, "capability=smudge") != lines.end();
    channel_ = std::move(channel);
    return true;
  }

  bool supports(git_filter_mode_t mode) const {
    return mode == GIT_FILTER_CLEAN ? clean_ : smudge_;
  }

  Channel& channel() { return *channel_; }

  // After a protocol failure the stream is out of step; restart next time.
  void reset() { channel_.reset(); }
  void abort() {
    aborted_ = true;
    channel_.reset();
  }

 private:
  std::unique_ptr<Channel> channel_;
  bool clean_ = false;
  bool smudge_ = false;
  bool aborted_ = false;
};

struct Registry {
  std::mutex mutex;
  std::map<std::string, std::shared_ptr<DriverProcess>> processes;
};

Registry& registry() {
  static Registry value;
  return value;
}

std::shared_ptr<DriverProcess> process_for(const std::string& key) {
  Registry& value = registry();
  std::lock_guard lock(value.mutex);
  std::shared_ptr<DriverProcess>& process = value.processes[key];
  if (process == nullptr) process = std::make_shared<DriverProcess>();
  return process;
}

struct Driver {
  std::string name;
  std::string process;
  std::string command;  // filter.<driver>.clean or .smudge
  bool required = false;
  std::string directory;
  std::string path;
  git_filter_mode_t mode = GIT_FILTER_SMUDGE;
};

std::string config_string(git_config* config, const std::string& key) {
  git_buf value = GIT_BUF_INIT;
  std::string result;
  if (git_config_get_string_buf(&value, config, key.c_str()) == 0) {
    result.assign(value.ptr, value.size);
  }
  git_buf_dispose(&value);
  git_error_clear();
  return result;
}

int filter_error(const Driver& driver, std::string_view action) {
  const std::string message = "filter driver '" + driver.name + "' failed to " +
                              std::string(action) + " '" + driver.path + "'";
  git_error_set_str(GIT_ERROR_FILTER, message.c_str());
  return -1;
}

int driver_check(git_filter*, void** payload, const git_filter_source* source,
                 const char** attributes) {
  const char* name = attributes[0];
  git_repository* repository = git_filter_source_repo(source);
  const char* workdir = git_repository_workdir(repository);
  if (name == nullptr || workdir == nullptr) return GIT_PASSTHROUGH;
  git_config* config = nullptr;
  if (git_repository_config_snapshot(&config, repository) != 0) return -1;
  auto driver = std::make_unique<Driver>();
  driver->name = name;
  const std::string prefix = "filter." + driver->name + ".";
  driver->mode = git_filter_source_mode(source);
  driver->process = config_string(config, prefix + "process");
  driver->command = config_string(
      config, prefix + (driver->mode == GIT_FILTER_CLEAN ? "clean" : "smudge"));
  int required = 0;
  if (git_config_get_bool(&required, config, (prefix + "required").c_str()) != 0) {
    git_error_clear();
  }
  git_config_free(config);
  driver->required = required != 0;
  driver->directory = workdir;
  driver->path = git_filter_source_path(source) == nullptr
                     ? ""
                     : git_filter_source_path(source);
  // Git leaves content unfiltered when no command is configured, unless the
  // driver is required.
  if (driver->process.empty() && driver->command.empty()) {
    if (driver->required) return filter_error(*driver, "find a command to");
    return GIT_PASSTHROUGH;
  }
  *payload = driver.release();
  return 0;
}

void driver_cleanup(git_filter*, void* payload) {
  delete static_cast<Driver*>(payload);
}

// Content arrives in chunks while libgit2 reads it. A required driver's
// failure fails the operation, so its content streams straight through;
// an optional driver's content is kept to fall back on, as Git does.
struct DriverStream {
  git_writestream base{};
  git_writestream* next = nullptr;
  const Driver* driver = nullptr;
  bool started = false;
  bool failed = false;
  std::string original;

  // Long-running process.
  std::shared_ptr<DriverProcess> process;
  std::unique_lock<std::mutex> lock;

  // Single-shot command: output is read while input is still written.
  std::unique_ptr<Channel> single;
  std::thread reader;
  std::string single_output;

  bool use_process() {
    if (driver->process.empty()) return false;
    process = process_for(driver->directory + '\n' + driver->process);
    lock = std::unique_lock(process->mutex);
    if (process->ready(driver->process, driver->directory) &&
        process->supports(driver->mode)) {
      return true;
    }
    lock.unlock();
    process.reset();
    return false;
  }

  bool begin() {
    started = true;
    if (use_process()) {
      Channel& channel = process->channel();
      return write_text(channel, std::string("command=") +
                                     (driver->mode == GIT_FILTER_CLEAN ? "clean"
                                                                       : "smudge")) &&
             write_text(channel, "pathname=" + driver->path) &&
             write_flush(channel);
    }
    if (driver->command.empty()) return false;
    std::string command = driver->command;
    for (std::size_t position = command.find("%f");
         position != std::string::npos; position = command.find("%f", position)) {
      const std::string quoted = Channel::shell_quote(driver->path);
      command.replace(position, 2, quoted);
      position += quoted.size();
    }
    single = std::make_unique<Channel>();
    if (!single->start(command, driver->directory)) return false;
    reader = std::thread([this] {
      std::array<char, 64 * 1024> buffer{};
      std::ptrdiff_t got = 0;
      while ((got = single->read_some(buffer.data(), buffer.size())) > 0) {
        single_output.append(buffer.data(), static_cast<std::size_t>(got));
      }
    });
    return true;
  }

  int write(const char* data, std::size_t size) {
    if (!driver->required) original.append(data, size);
    if (failed) return 0;
    if (!started && !begin()) {
      failed = true;
      return 0;
    }
    if (process != nullptr) {
      while (size > 0 && !failed) {
        const std::size_t take = std::min(size, kMaxPacketData);
        failed = !write_packet(process->channel(), std::string_view(data, take));
        data += take;
        size -= take;
      }
    } else if (!single->write(data, size)) {
      failed = true;
    }
    return 0;
  }

  enum class Outcome { Success, Error, Broken };

  // Sends the response from the long-running process to `next`, or to
  // `buffer` when an optional driver's output must be checked first. Error
  // means the driver reported a failure and the stream is still in step.
  Outcome finish_process(std::string* buffer) {
    Channel& channel = process->channel();
    std::vector<std::string> lines;
    if (!write_flush(channel) || !read_list(channel, lines)) return Outcome::Broken;
    std::optional<std::string> status = status_of(lines);
    if (status == "abort") {
      process->abort();
      return Outcome::Error;
    }
    if (status == "error") return Outcome::Error;
    if (status != "success") return Outcome::Broken;
    std::string data;
    bool flush = false;
    while (true) {
      if (!read_packet(channel, data, flush)) return Outcome::Broken;
      if (flush) break;
      if (buffer != nullptr) {
        buffer->append(data);
      } else if (next->write(next, data.data(), data.size()) < 0) {
        return Outcome::Broken;
      }
    }
    if (!read_list(channel, lines)) return Outcome::Broken;
    status = status_of(lines).value_or("success");
    if (status == "abort") process->abort();
    return status == "success" ? Outcome::Success : Outcome::Error;
  }

  int close() {
    if (!started && !failed && !begin()) failed = true;
    bool succeeded = !failed;
    bool streamed = false;
    std::string output;
    const bool buffered = !driver->required;
    if (process != nullptr) {
      const Outcome outcome =
          failed ? Outcome::Broken : finish_process(buffered ? &output : nullptr);
      if (outcome == Outcome::Broken) process->reset();
      succeeded = outcome == Outcome::Success;
      streamed = succeeded && !buffered;
      lock.unlock();
    } else if (single != nullptr) {
      single->close_input();
      reader.join();
      succeeded = single->finish() && succeeded;
      output = std::move(single_output);
    }
    if (!succeeded) {
      if (driver->required) return filter_error(*driver, "filter");
      output = std::move(original);
    }
    if (!streamed && next->write(next, output.data(), output.size()) < 0) {
      return -1;
    }
    return next->close(next);
  }

  ~DriverStream() {
    if (reader.joinable()) {
      single->finish();
      reader.join();
    }
  }
};

int stream_write(git_writestream* stream, const char* data, std::size_t size) {
  return reinterpret_cast<DriverStream*>(stream)->write(data, size);
}

int stream_close(git_writestream* stream) {
  return reinterpret_cast<DriverStream*>(stream)->close();
}

void stream_free(git_writestream* stream) {
  delete reinterpret_cast<DriverStream*>(stream);
}

int driver_stream(git_writestream** out, git_filter*, void** payload,
                  const git_filter_source*, git_writestream* next) {
  auto* stream = new DriverStream();
  stream->base = {stream_write, stream_close, stream_free};
  stream->next = next;
  stream->driver = static_cast<const Driver*>(*payload);
  *out = &stream->base;
  return 0;
}

// Git closes its filter processes when it exits; libgit2 shutting down is
// the equivalent here.
void driver_shutdown(git_filter*) {
  Registry& value = registry();
  std::lock_guard lock(value.mutex);
  value.processes.clear();
}

git_filter make_driver_filter() {
  git_filter filter{};
  git_filter_init(&filter, GIT_FILTER_VERSION);
  filter.attributes = "filter=*";
  filter.check = driver_check;
  filter.stream = driver_stream;
  filter.cleanup = driver_cleanup;
  filter.shutdown = driver_shutdown;
  return filter;
}

}  // namespace

int register_filter_drivers() {
  // On checkout after the built-in CRLF and ident filters, and before them
  // when committing, which is the order Git applies a driver.
  static git_filter filter = make_driver_filter();
  const int result = git_filter_register("gg-driver", &filter, 200);
  if (result == GIT_EEXISTS) {
    git_error_clear();
    return 0;
  }
  return result;
}

}  // namespace gg::detail
