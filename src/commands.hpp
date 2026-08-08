// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#pragma once
#include "cli.hpp"
#include "repository.hpp"

#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace gg::detail {

std::vector<git_oid> commit_parents(
    Repository& repo, const std::vector<std::string>& revisions);
git_oid combined_tree(Repository& repo, const std::vector<git_oid>& parents);
void finish_workspace(Repository& repo, const git_oid& workspace,
                      std::map<std::string, git_oid> updates,
                      std::set<std::string> deletes,
                      std::string_view operation);

int credentials(git_credential** output, const char* url, const char* username,
                unsigned int allowed, void* payload);

void command_new(Repository&, const NewCommand&, std::ostream&);
void command_status(Repository&, std::ostream&);
void command_log(Repository&, const LogCommand&, std::ostream&);
void command_edit(Repository&, const EditCommand&, std::ostream&);
void command_describe(Repository&, const DescribeCommand&, std::ostream&);
void command_rebase(Repository&, const RebaseCommand&, std::ostream&);
void command_split(Repository&, const SplitCommand&, std::ostream&);
void command_squash(Repository&, const SquashCommand&, std::ostream&);
void command_abandon(Repository&, const AbandonCommand&, std::ostream&);
void command_commit(Repository&, const CommitCommand&, std::ostream&);
void command_file(Repository&, const FileCommand&, std::ostream&);
void command_diff(Repository&, const DiffCommand&, std::ostream&);
void command_show(Repository&, const ShowCommand&, std::ostream&);
void command_bookmark(Repository&, const BookmarkCommand&, std::ostream&);
void command_fetch(Repository&, const GitFetchCommand&, std::ostream&);
void command_push(Repository&, const GitPushCommand&, std::ostream&);
void command_undo(Repository&, std::ostream&);
void command_redo(Repository&, std::ostream&);
void command_operation_log(Repository&, std::ostream&);
void command_operation_restore(Repository&, const OperationRestoreCommand&,
                               std::ostream&);
void command_util_snapshot(Repository&, std::ostream&);
void command_move(Repository&, const MovementCommand&, std::ostream&);
int clone_command(const GitCloneCommand&, std::ostream&);
int init_command(const GitInitCommand&, std::ostream&);

}  // namespace gg::detail
