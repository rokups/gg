// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "process.hpp"

#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <string_view>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace gg::detail {
namespace {

#ifdef _WIN32
std::wstring wide(std::string_view value) {
  if (value.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       value.data(), value.size(), nullptr, 0);
  if (size == 0) return {};
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          value.size(), result.data(), size) == 0) {
    return {};
  }
  return result;
}

std::wstring quote_argument(std::string_view value) {
  const std::wstring argument = wide(value);
  if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") ==
                               std::wstring::npos) {
    return argument;
  }
  std::wstring result(1, L'\"');
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
    } else {
      if (character == L'\"') result.append(backslashes, L'\\');
      result.append(backslashes, L'\\');
      backslashes = 0;
      result.push_back(character);
    }
  }
  result.append(backslashes * 2, L'\\');
  result.push_back(L'\"');
  return result;
}

std::vector<wchar_t> environment_block(char* const* environment) {
  std::vector<std::wstring> entries;
  for (char* const* entry = environment; *entry != nullptr; ++entry) {
    entries.push_back(wide(*entry));
  }
  std::ranges::sort(entries, {}, [](const std::wstring& entry) {
    return entry.substr(0, entry.find(L'='));
  });
  std::vector<wchar_t> block;
  for (const std::wstring& entry : entries) {
    block.insert(block.end(), entry.begin(), entry.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}
#endif

}  // namespace

char** process_environment() {
#ifdef _WIN32
  return _environ;
#else
  return environ;
#endif
}

std::optional<int> run_process(
    const std::vector<std::string>& arguments,
    char* const* environment,
    const std::optional<std::filesystem::path>& standard_output) {
  if (arguments.empty()) return std::nullopt;
#ifdef _WIN32
  std::wstring command_line;
  for (const std::string& argument : arguments) {
    if (!command_line.empty()) command_line.push_back(L' ');
    command_line += quote_argument(argument);
  }
  std::vector<wchar_t> command(command_line.begin(), command_line.end());
  command.push_back(L'\0');

  SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
  HANDLE output = INVALID_HANDLE_VALUE;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  if (standard_output.has_value()) {
    output = CreateFileW(standard_output->c_str(), GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return std::nullopt;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = output;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  }

  std::vector<wchar_t> block;
  if (environment != nullptr) block = environment_block(environment);
  PROCESS_INFORMATION process{};
  const BOOL started = CreateProcessW(
      nullptr, command.data(), nullptr, nullptr, TRUE,
      environment == nullptr ? 0 : CREATE_UNICODE_ENVIRONMENT,
      environment == nullptr ? nullptr : block.data(), nullptr, &startup,
      &process);
  if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
  if (started == FALSE) return std::nullopt;
  CloseHandle(process.hThread);
  if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0) {
    CloseHandle(process.hProcess);
    return std::nullopt;
  }
  DWORD exit_code = 0;
  const BOOL read_exit_code = GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hProcess);
  if (read_exit_code == FALSE) return std::nullopt;
  return static_cast<int>(exit_code);
#else
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (const std::string& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_t* action_pointer = nullptr;
  if (standard_output.has_value()) {
    if (posix_spawn_file_actions_init(&actions) != 0) return std::nullopt;
    const std::string output_path = standard_output->string();
    if (posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                         output_path.c_str(),
                                         O_WRONLY | O_CREAT | O_TRUNC,
                                         0600) != 0) {
      posix_spawn_file_actions_destroy(&actions);
      return std::nullopt;
    }
    action_pointer = &actions;
  }
  pid_t process = 0;
  const int spawned =
      posix_spawnp(&process, arguments.front().c_str(), action_pointer, nullptr,
                   argv.data(), environment == nullptr ? environ : environment);
  if (action_pointer != nullptr) posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0) return std::nullopt;
  int status = 0;
  if (waitpid(process, &status, 0) < 0 || !WIFEXITED(status)) {
    return std::nullopt;
  }
  return WEXITSTATUS(status);
#endif
}

}  // namespace gg::detail
