// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#pragma once

#include "cli.hpp"

#include <git2.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace gg::test {

struct Result {
  int code;
  std::string output;
  std::string error;
};

inline Result run(std::vector<std::string> arguments) {
  std::vector<std::string_view> views;
  views.reserve(arguments.size());
  for (const std::string& argument : arguments) {
    views.push_back(argument);
  }
  std::ostringstream output;
  std::ostringstream error;
  const int code = gg::detail::run_cli(views, output, error);
  return {code, output.str(), error.str()};
}

class RepositoryTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_GT(git_libgit2_init(), 0);
    static unsigned long counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("gg-test-" + std::to_string(getpid()) + "-" +
             std::to_string(++counter));
    std::filesystem::remove_all(path_);
    git_repository* repository = nullptr;
    ASSERT_EQ(git_repository_init(&repository, path_.string().c_str(), 0), 0);
    repository_.reset(repository);
    ASSERT_EQ(git_repository_set_head(repository_.get(), "refs/heads/main"), 0);
    git_config* config = nullptr;
    ASSERT_EQ(git_repository_config(&config, repository_.get()), 0);
    ASSERT_EQ(git_config_set_string(config, "user.name", "GG Test"), 0);
    ASSERT_EQ(git_config_set_string(config, "user.email", "gg@example.test"), 0);
    git_config_free(config);
    write("tracked.txt", "base\n");
    commit("base");
  }

  void TearDown() override {
    repository_.reset();
    std::filesystem::remove_all(path_);
    git_libgit2_shutdown();
  }

  void write(std::string_view relative, std::string_view content) {
    const auto target = path_ / relative;
    std::filesystem::create_directories(target.parent_path());
    std::ofstream(target) << content;
  }

  void commit(std::string_view message) {
    git_index* index = nullptr;
    ASSERT_EQ(git_repository_index(&index, repository_.get()), 0);
    ASSERT_EQ(git_index_add_bypath(index, "tracked.txt"), 0);
    ASSERT_EQ(git_index_write(index), 0);
    git_oid tree_oid{};
    ASSERT_EQ(git_index_write_tree(&tree_oid, index), 0);
    git_index_free(index);
    git_tree* tree = nullptr;
    ASSERT_EQ(git_tree_lookup(&tree, repository_.get(), &tree_oid), 0);
    git_signature* signature = nullptr;
    ASSERT_EQ(git_signature_now(&signature, "GG Test", "gg@example.test"), 0);
    git_oid commit_oid{};
    const std::string owned_message(message);
    ASSERT_EQ(git_commit_create(&commit_oid, repository_.get(), "HEAD", signature,
                                signature, nullptr, owned_message.c_str(), tree, 0,
                                nullptr),
              0);
    git_signature_free(signature);
    git_tree_free(tree);
  }

  Result invoke(std::vector<std::string> arguments) {
    return invoke_at(path_, std::move(arguments));
  }

  Result invoke_at(const std::filesystem::path& repository,
                   std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), repository.string());
    arguments.insert(arguments.begin(), "-R");
    return run(std::move(arguments));
  }

  Result invoke_git(const std::vector<std::string>& arguments) {
    return invoke_git_at(path_, arguments);
  }

  Result invoke_git_at(const std::filesystem::path& repository,
                       const std::vector<std::string>& arguments) {
    static unsigned long counter = 0;
    const auto output_path =
        path_ / ".git" / ("gg-test-git-output-" + std::to_string(++counter));
    const auto error_path = output_path.string() + ".error";
    std::string command = "git -C " + shell_quote(repository.string());
    for (const std::string& argument : arguments) {
      command += " " + shell_quote(argument);
    }
    command += " >" + shell_quote(output_path.string()) + " 2>" +
               shell_quote(error_path);
    const int raw_code = std::system(command.c_str());
    const int code = raw_code != -1 && WIFEXITED(raw_code)
                         ? WEXITSTATUS(raw_code)
                         : 128;
    const std::string output = read_path(output_path);
    const std::string error = read_path(error_path);
    std::filesystem::remove(output_path);
    std::filesystem::remove(error_path);
    return {code, output, error};
  }

  void expect_workspace_coherent() {
    ASSERT_TRUE(has_ref("refs/gg/workspaces/default"));
    const git_oid workspace = ref("refs/gg/workspaces/default");
    git_commit* commit = nullptr;
    ASSERT_EQ(git_commit_lookup(&commit, repository_.get(), &workspace), 0);
    ASSERT_GT(git_commit_parentcount(commit), 0U);
    const git_oid workspace_tree = *git_commit_tree_id(commit);
    const git_oid parent = *git_commit_parent_id(commit, 0);
    git_commit_free(commit);

    const git_oid head = ref("HEAD");
    EXPECT_NE(git_oid_equal(&head, &parent), 0);

    git_index* index = nullptr;
    ASSERT_EQ(git_repository_index(&index, repository_.get()), 0);
    ASSERT_EQ(git_index_read(index, 1), 0);
    git_oid index_tree{};
    ASSERT_EQ(git_index_write_tree_to(&index_tree, index, repository_.get()), 0);
    git_index_free(index);
    EXPECT_NE(git_oid_equal(&index_tree, &workspace_tree), 0);

    const Result unstaged = invoke_git({"diff", "--quiet"});
    EXPECT_EQ(unstaged.code, 0) << unstaged.error;
  }

  git_oid ref(std::string_view name) {
    git_oid oid{};
    const std::string owned_name(name);
    if (git_reference_name_to_id(&oid, repository_.get(), owned_name.c_str()) != 0) {
      throw std::runtime_error("reference not found: " + owned_name);
    }
    return oid;
  }

  bool has_ref(std::string_view name) {
    git_reference* reference = nullptr;
    const std::string owned_name(name);
    const int result =
        git_reference_lookup(&reference, repository_.get(), owned_name.c_str());
    git_reference_free(reference);
    return result == 0;
  }

  void set_ref(std::string_view name, const git_oid& oid) {
    git_reference* reference = nullptr;
    const std::string owned_name(name);
    ASSERT_EQ(git_reference_create(&reference, repository_.get(),
                                   owned_name.c_str(), &oid, 1, "test"),
              0);
    git_reference_free(reference);
  }

  git_filemode_t file_mode(std::string_view revision,
                           std::string_view path) {
    const git_oid oid = ref(revision);
    git_commit* commit = nullptr;
    if (git_commit_lookup(&commit, repository_.get(), &oid) != 0) {
      throw std::runtime_error("commit not found");
    }
    git_tree* tree = nullptr;
    if (git_commit_tree(&tree, commit) != 0) {
      git_commit_free(commit);
      throw std::runtime_error("tree not found");
    }
    git_tree_entry* entry = nullptr;
    const std::string owned_path(path);
    if (git_tree_entry_bypath(&entry, tree, owned_path.c_str()) != 0) {
      git_tree_free(tree);
      git_commit_free(commit);
      throw std::runtime_error("tree entry not found");
    }
    const git_filemode_t result = git_tree_entry_filemode(entry);
    git_tree_entry_free(entry);
    git_tree_free(tree);
    git_commit_free(commit);
    return result;
  }

  git_oid raw_commit(std::string_view message,
                     const std::vector<git_oid>& parent_oids = {}) {
    const git_oid head = ref("HEAD");
    git_commit* head_commit = nullptr;
    EXPECT_EQ(git_commit_lookup(&head_commit, repository_.get(), &head), 0);
    const git_oid tree_oid = *git_commit_tree_id(head_commit);
    git_commit_free(head_commit);

    git_tree* tree = nullptr;
    EXPECT_EQ(git_tree_lookup(&tree, repository_.get(), &tree_oid), 0);
    git_signature* signature = nullptr;
    EXPECT_EQ(git_signature_now(&signature, "GG Test", "gg@example.test"), 0);
    std::vector<git_commit*> owned_parents;
    std::vector<const git_commit*> parents;
    for (const git_oid& parent_oid : parent_oids) {
      git_commit* parent = nullptr;
      EXPECT_EQ(git_commit_lookup(&parent, repository_.get(), &parent_oid), 0);
      owned_parents.push_back(parent);
      parents.push_back(parent);
    }
    git_oid result{};
    const std::string owned_message(message);
    EXPECT_EQ(git_commit_create(&result, repository_.get(), nullptr, signature,
                                signature, nullptr, owned_message.c_str(), tree,
                                parents.size(), parents.data()),
              0);
    for (git_commit* parent : owned_parents) {
      git_commit_free(parent);
    }
    git_signature_free(signature);
    git_tree_free(tree);
    return result;
  }

  git_oid commit_parent(const git_oid& oid, unsigned int index = 0) {
    git_commit* value = nullptr;
    if (git_commit_lookup(&value, repository_.get(), &oid) != 0 ||
        git_commit_parentcount(value) <= index) {
      git_commit_free(value);
      throw std::runtime_error("commit parent not found");
    }
    const git_oid result = *git_commit_parent_id(value, index);
    git_commit_free(value);
    return result;
  }

  std::string token_after(std::string_view output, std::string_view prefix) {
    const std::size_t start = output.find(prefix);
    if (start == std::string_view::npos) {
      throw std::runtime_error("output prefix not found");
    }
    const std::size_t value = start + prefix.size();
    return std::string(output.substr(value, output.find(' ', value) - value));
  }

  std::string current_id() {
    const Result log = invoke({"log", "-r", "@"});
    if (log.code != 0 || log.output.size() < 10) {
      throw std::runtime_error(log.error);
    }
    return log.output.substr(3, 8);
  }

  std::string file() {
    std::ifstream input(path_ / "tracked.txt");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  static std::string shell_quote(std::string_view value) {
    std::string result{"'"};
    for (const char character : value) {
      if (character == '\'') {
        result += "'\\''";
      } else {
        result += character;
      }
    }
    result += '\'';
    return result;
  }

  static std::string read_path(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
  }

  struct RepositoryDeleter {
    void operator()(git_repository* repository) const {
      git_repository_free(repository);
    }
  };

  std::filesystem::path path_;
  std::unique_ptr<git_repository, RepositoryDeleter> repository_;
};

}  // namespace gg::test
