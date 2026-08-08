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
bool revision_matches_paths(Repository&, const git_oid&,
                            const std::vector<std::string>&,
                            const DiffFormatOptions&);
void render_revision_diff(Repository&, const git_oid&,
                          const std::vector<std::string>&,
                          const DiffFormatOptions&, std::ostream&);
void finish_workspace(Repository& repo, const git_oid& workspace,
                      std::map<std::string, git_oid> updates,
                      std::set<std::string> deletes,
                      std::string_view operation);
void edit_file_with_editor(const std::filesystem::path&);
std::string edit_text(std::string_view);

int credentials(git_credential** output, const char* url, const char* username,
                unsigned int allowed, void* payload);

void command_new(Repository&, const NewCommand&, std::ostream&);
void command_status(Repository&, const StatusCommand&, std::ostream&);
void command_log(Repository&, const LogCommand&, std::ostream&);
void command_edit(Repository&, const EditCommand&, std::ostream&);
void command_metaedit(Repository&, const MetaeditCommand&, std::ostream&);
void command_describe(Repository&, const DescribeCommand&, std::ostream&);
void command_rebase(Repository&, const RebaseCommand&, std::ostream&);
void command_split(Repository&, const SplitCommand&, std::ostream&);
void command_squash(Repository&, const SquashCommand&, std::ostream&);
void command_abandon(Repository&, const AbandonCommand&, std::ostream&);
void command_commit(Repository&, const CommitCommand&, std::ostream&);
void command_restore(Repository&, const RestoreCommand&, std::ostream&);
void command_simplify_parents(Repository&, const SimplifyParentsCommand&,
                              std::ostream&);
void command_file(Repository&, const FileCommand&, std::ostream&);
void command_diff(Repository&, const DiffCommand&, std::ostream&);
void command_show(Repository&, const ShowCommand&, std::ostream&);
void command_bookmark(Repository&, const BookmarkCommand&, std::ostream&);
void command_tag(Repository&, const TagCommand&, std::ostream&);
void command_fetch(Repository&, const GitFetchCommand&, std::ostream&);
void command_push(Repository&, const GitPushCommand&, std::ostream&);
void command_undo(Repository&, std::ostream&);
void command_redo(Repository&, std::ostream&);
void command_operation_log(Repository&, const OperationLogCommand&,
                           std::ostream&);
void command_operation_restore(Repository&, const OperationRestoreCommand&,
                               std::ostream&);
int command_util_exec(const UtilExecCommand&, const std::filesystem::path&);
void command_util_snapshot(Repository&, std::ostream&);
void command_workspace(Repository&, const WorkspaceCommand&, std::ostream&);
void command_move(Repository&, const MovementCommand&, std::ostream&);
void command_config(Repository&, const ConfigCommand&, std::ostream&);
int clone_command(const GitCloneCommand&, std::ostream&);
int init_command(const GitInitCommand&, std::ostream&);

}  // namespace gg::detail
