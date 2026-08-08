// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#pragma once
#include "cli.hpp"
#include "repository.hpp"

#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace gg::detail {

enum class OutputColorMode { plain, ansi, debug };
enum class OutputStyle {
  working_copy,
  change_id,
  change_id_prefix,
  change_id_rest,
  working_change_id,
  working_change_id_prefix,
  working_change_id_rest,
  commit_id,
  commit_id_prefix,
  commit_id_rest,
  working_commit_id,
  working_commit_id_prefix,
  working_commit_id_rest,
  bookmark,
  tag,
  operation_id,
  current_operation_id,
  timestamp,
  added,
  removed,
  modified,
  heading,
  hunk
};

void set_output_color_mode(std::ostream&, OutputColorMode);
std::string styled(std::ostream&, std::string_view, OutputStyle);
std::string styled_short_change_id(Repository&, std::ostream&,
                                   std::string_view, bool working = false);
std::string styled_short_commit_id(Repository&, std::ostream&, const git_oid&,
                                   bool working = false);
std::string render_template(
    std::string_view expression,
    const std::map<std::string, std::string>& values);
std::map<std::string, std::string> revision_template_values(
    Repository&, const git_oid&);

std::vector<git_oid> resolve_revision_arguments(
    Repository& repo, const std::vector<std::string>& revisions);
std::vector<git_oid> commit_parents(
    Repository& repo, const std::vector<std::string>& revisions);
git_oid combined_tree(Repository& repo, const std::vector<git_oid>& parents);
bool revision_matches_paths(Repository&, const git_oid&,
                            const std::vector<std::string>&,
                            const DiffFormatOptions&);
void render_revision_diff(Repository&, const git_oid&,
                          const std::vector<std::string>&,
                          const DiffFormatOptions&, std::ostream&);
void render_tree_diff(Repository&, const git_oid&, const git_oid&,
                      const std::vector<std::string>&,
                      const DiffFormatOptions&, std::ostream&);
git_oid select_diff_tree(Repository&, const git_oid&, const git_oid&,
                         const std::vector<std::string>&, std::string_view);
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
void command_util_gc(Repository&, const UtilGcCommand&, std::ostream&);
void command_util_snapshot(Repository&, std::ostream&);
void command_workspace(Repository&, const WorkspaceCommand&, std::ostream&);
void command_sparse(Repository&, const SparseCommand&, std::ostream&);
void command_move(Repository&, const MovementCommand&, std::ostream&);
void command_config(Repository&, const ConfigCommand&, std::ostream&);
std::optional<std::string> config_value(Repository&, std::string_view);
int clone_command(const GitCloneCommand&, std::ostream&);
int init_command(const GitInitCommand&, std::ostream&);

}  // namespace gg::detail
