// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "cli.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ostream>
#include <set>
#include <sstream>
#include <utility>

namespace gg::detail {
namespace {

std::vector<const CLI::App*> schema_children(const CLI::App& command) {
  return command.get_subcommands(
      std::function<bool(const CLI::App*)>{});
}

const std::map<std::string, std::string_view> kHelpKeywords{
    {"bookmarks",
     "# Bookmarks\n\nBookmarks are ordinary local Git branches. Use `gg bookmark` "
     "to create, move, list, rename, forget, or delete them.\n"},
    {"config",
     "# Configuration\n\n`gg config` reads and writes flat dotted keys in user, "
     "repository, or workspace layers.\n"},
    {"filesets",
     "# Filesets\n\ngg currently accepts literal repository-relative files and "
     "directories where Jujutsu accepts filesets.\n"},
    {"glossary",
     "# Glossary\n\nA change is a Git commit with a stable gg change ID; a bookmark "
     "is a Git branch; `@` is the working-copy change.\n"},
    {"revsets",
     "# Revision selection\n\ngg accepts `@`, `@-` chains, stable change-ID "
     "prefixes, bookmarks, and Git revision expressions. Set-valued revsets are "
     "not yet supported.\n"},
    {"templates",
     "# Templates\n\nTemplates support Jujutsu-shaped `++` concatenation, quoted "
     "string literals, command-specific keywords, and the string methods "
     "`.short([length])` and `.first_line()`.\n"},
    {"tutorial",
     "# Tutorial\n\nRun `gg new` to start a working change, edit files, inspect them "
     "with `gg status` and `gg diff`, then use `gg commit` or bookmarks to "
     "organize and publish the work.\n"},
};

std::string stable_help(const CLI::App& command, std::string_view path) {
  const std::size_t separator = path.rfind(' ');
  const std::string parent = separator == std::string_view::npos
                                 ? ""
                                 : std::string(path.substr(0, separator));
  std::istringstream raw(command.help(parent));
  std::ostringstream stable;
  std::string line;
  while (std::getline(raw, line)) {
    const std::size_t excludes = line.find(" Excludes:");
    if (excludes != std::string::npos) line.erase(excludes);
    stable << line << '\n';
  }
  return stable.str();
}

void write_markdown_command(const CLI::App& command,
                            const std::string& path,
                            std::ostream& output) {
  output << "## `" << path << "`\n\n" << command.get_description()
         << "\n\n```text\n" << stable_help(command, path) << "```\n\n";
  for (const CLI::App* child : schema_children(command)) {
    write_markdown_command(*child, path + " " + child->get_name(), output);
  }
}

void write_markdown_help(const CLI::App& app, std::ostream& output) {
  output << "# gg command reference\n\n";
  write_markdown_command(app, "gg", output);
}

std::string man_name(std::string_view path) {
  std::string result(path);
  std::ranges::replace(result, ' ', '-');
  return result;
}

void write_man_pages(const CLI::App& command,
                     const std::string& path,
                     const std::filesystem::path& directory) {
  const std::string name = man_name(path);
  std::string title = name;
  std::ranges::transform(title, title.begin(), [](unsigned char value) {
    return static_cast<char>(std::toupper(value));
  });
  std::ofstream page(directory / (name + ".1"));
  if (!page) throw std::runtime_error("cannot create man page");  // GG_COV_EXCL_BRANCH
  page << ".TH \"" << title << "\" \"1\"\n.SH NAME\n" << name
       << " \\- " << command.get_description()
       << "\n.SH SYNOPSIS\n.nf\n";
  std::istringstream help(stable_help(command, path));
  std::string line;
  while (std::getline(help, line)) page << "\\&" << line << '\n';
  page << ".fi\n";
  for (const CLI::App* child : schema_children(command)) {
    write_man_pages(*child, path + " " + child->get_name(), directory);
  }
}

void install_man_pages(const CLI::App& app,
                       const std::filesystem::path& destination) {
  const std::filesystem::path man1 = destination / "man1";
  std::filesystem::create_directories(man1);
  write_man_pages(app, "gg", man1);
}

void collect_completion_words(const CLI::App& command,
                              std::set<std::string>& words) {
  for (const CLI::Option* option : command.get_options()) {
    for (const std::string& name : option->get_lnames()) {
      words.insert("--" + name);
    }
    for (const std::string& name : option->get_snames()) {
      words.insert("-" + name);
    }
  }
  for (const CLI::App* child : schema_children(command)) {
    words.insert(child->get_name());
    words.insert(child->get_aliases().begin(), child->get_aliases().end());
    collect_completion_words(*child, words);
  }
}

std::string joined_words(const std::set<std::string>& words) {
  std::ostringstream output;
  for (const std::string& word : words) output << word << ' ';
  return output.str();
}

void write_completion(const CLI::App& app,
                      std::string_view shell,
                      std::ostream& output) {
  std::set<std::string> words;
  collect_completion_words(app, words);
  const std::string joined = joined_words(words);
  if (shell == "bash") {
    output << "_gg() {\n"
              "  local cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
              "  COMPREPLY=( $(compgen -W '"
           << joined << "' -- \"$cur\") )\n}\ncomplete -F _gg gg\n";
  } else if (shell == "elvish") {
    output << "edit:completion:arg-completer[gg] = {|@words|\n  put";
    for (const std::string& word : words) output << " '" << word << "'";
    output << "\n}\n";
  } else if (shell == "fish") {
    output << "complete -c gg -f -a '" << joined << "'\n";
  } else if (shell == "nushell") {
    output << "def \"nu-complete gg\" [] { [";
    for (const std::string& word : words) output << " '" << word << "'";
    output << " ] }\nexport extern \"gg\" [command?: string@\"nu-complete gg\", "
              "...args: string]\n";
  } else if (shell == "power-shell" || shell == "powershell") {
    output << "Register-ArgumentCompleter -Native -CommandName gg -ScriptBlock "
              "{ param($wordToComplete)\n  @(";
    for (const std::string& word : words) output << "'" << word << "',";
    output << ") | Where-Object { $_ -like \"$wordToComplete*\" }\n}\n";
  } else {
    output << "#compdef gg\n_gg() {\n  local -a candidates\n  candidates=(";
    for (const std::string& word : words) output << " '" << word << "'";
    output << " )\n  _describe 'gg command or option' candidates\n}\n"
              "compdef _gg gg\n";
  }
}

void write_config_schema(std::ostream& output) {
  output << R"JSON({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://gg-vcs.dev/schema/config.json",
  "title": "gg configuration",
  "description": "Flat dotted gg-native configuration assignments",
  "type": "object",
  "patternProperties": {
    "^[A-Za-z0-9_-]+(?:\\.[A-Za-z0-9_-]+)*$": {}
  },
  "additionalProperties": false
}
)JSON";
}

}  // namespace

ParseResult parse_cli(std::span<const std::string_view> arguments,
                      std::ostream& output,
                      std::ostream& error) {
  CLI::App app{"A JJ-shaped interface over ordinary Git repositories", "gg"};
  app.require_subcommand(1);
  app.set_version_flag("--version", "gg 0.1.0");

  std::string repository = ".";
  app.add_option("-R,--repository", repository, "Repository path");
  bool ignore_working_copy = false;
  app.add_flag("--ignore-working-copy", ignore_working_copy,
               "Do not snapshot or update the working copy");
  std::string at_operation;
  app.add_option("--at-operation,--at-op", at_operation,
                 "Load the repository at an operation");
  bool ignore_immutable = false;
  app.add_flag("--ignore-immutable", ignore_immutable,
               "Allow rewriting any non-root revision");
  bool no_pager = false;
  app.add_flag("--no-pager", no_pager, "Disable the pager");
  bool debug = false;
  app.add_flag("--debug", debug, "Enable debug logging");
  bool quiet = false;
  app.add_flag("--quiet", quiet, "Silence non-primary command output");
  std::string color = "auto";
  app.add_option("--color", color, "When to colorize output")
      ->check(CLI::IsMember({"always", "never", "debug", "auto"}));
  std::vector<std::string> config_values;
  app.add_option("--config", config_values, "Additional NAME=VALUE configuration")
      ->type_size(1)
      ->allow_extra_args(false);
  std::vector<std::filesystem::path> config_files;
  app.add_option("--config-file", config_files, "Additional configuration file")
      ->type_size(1)
      ->allow_extra_args(false);

  StatusCommand status_value;
  auto* status = app.add_subcommand("status", "Show the working-copy change");
  status->alias("st");
  status->add_option("filesets", status_value.paths,
                     "Repository-relative paths");

  LogCommand log_value;
  auto* log = app.add_subcommand("log", "Show revision history");
  log->add_option("filesets", log_value.paths, "Repository-relative paths");
  log->add_option("-r,--revision,--revisions", log_value.revision,
                  "Starting revision");
  log->add_option("-n,--limit", log_value.limit, "Maximum number of revisions")
      ->check(CLI::NonNegativeNumber);
  log->add_flag("--reversed", log_value.reversed,
                "Show older revisions first");
  log->add_flag("-G,--no-graph", log_value.no_graph,
                "Do not show graph markers");
  log->add_option("-T,--template", log_value.template_value,
                  "Revision template");
  log->add_flag("-p,--patch", log_value.patch, "Show patches");
  log->add_flag("--count", log_value.count, "Print only the revision count");
  log->add_flag("-s,--summary", log_value.format.summary,
                "Show changed paths and statuses");
  log->add_flag("--stat", log_value.format.stat, "Show a diffstat");
  log->add_flag("--types", log_value.format.types, "Show file types");
  log->add_flag("--name-only", log_value.format.name_only,
                "Show changed paths only");
  log->add_flag("--git", log_value.format.git, "Show Git patch output");
  log->add_flag("--color-words", log_value.format.color_words,
                "Show inline word changes");
  log->add_option("--tool", log_value.format.tool, "Diff tool");
  log->add_option("--context", log_value.format.context, "Context lines")
      ->check(CLI::NonNegativeNumber);
  log->add_flag("-w,--ignore-all-space", log_value.format.ignore_all_space,
                "Ignore whitespace when comparing lines");
  log->add_flag("-b,--ignore-space-change",
                log_value.format.ignore_space_change,
                "Ignore changes in whitespace amount");

  NewCommand new_value;
  auto* make_new = app.add_subcommand("new", "Create and edit an empty change");
  make_new->add_option("-m,--message", new_value.message, "Description");
  CLI::Option* new_parents =
      make_new->add_option("parents", new_value.parents, "Parent revisions");
  CLI::Option* new_after = make_new->add_option(
      "-A,--insert-after,--after", new_value.insert_after,
      "Insert after revisions");
  CLI::Option* new_before = make_new->add_option(
      "-B,--insert-before,--before", new_value.insert_before,
      "Insert before revisions");
  new_after->excludes(new_parents);
  new_before->excludes(new_parents);
  make_new->add_flag("--no-edit", new_value.no_edit,
                     "Create the change without editing it");

  DescribeCommand describe_value;
  auto* describe = app.add_subcommand("describe", "Set a change description");
  CLI::Option* describe_message =
      describe->add_option("-m,--message", describe_value.message, "Description");
  CLI::Option* describe_stdin = describe->add_flag(
      "--stdin", describe_value.stdin_value, "Read the description from stdin");
  describe->add_flag("--editor", describe_value.editor,
                     "Edit the description in an editor");
  describe_message->excludes(describe_stdin);
  describe->add_option("revisions", describe_value.revisions, "Revisions");
  describe->add_option("-r,--revision", describe_value.revision_options,
                       "Revisions");

  EditCommand edit_value;
  std::string edit_positional;
  auto* edit = app.add_subcommand("edit", "Edit an existing change");
  CLI::Option* edit_revision =
      edit->add_option("target", edit_positional, "Revision")->expected(0, 1);
  CLI::Option* edit_revision_option =
      edit->add_option("-r,--revision", edit_value.revision, "Revision");
  edit_revision->excludes(edit_revision_option);
  edit->require_option(1);

  MetaeditCommand metaedit_value;
  auto* metaedit = app.add_subcommand("metaedit", "Modify revision metadata");
  metaedit->add_option("revisions", metaedit_value.revisions, "Revisions");
  metaedit
      ->add_option("-r,--revision", metaedit_value.revision_options,
                   "Revision")
      ->expected(1);
  metaedit->add_flag("--update-change-id", metaedit_value.update_change_id,
                     "Generate a new change ID");
  CLI::Option* metaedit_message = metaedit->add_option(
      "-m,--message", metaedit_value.message, "Description");
  CLI::Option* update_author_timestamp = metaedit->add_flag(
      "--update-author-timestamp", metaedit_value.update_author_timestamp,
      "Set the author timestamp to now");
  CLI::Option* update_author = metaedit->add_flag(
      "--update-author", metaedit_value.update_author,
      "Use the configured author identity");
  CLI::Option* author = metaedit->add_option(
      "--author", metaedit_value.author, "Author as 'Name <email>'");
  author->excludes(update_author);
  CLI::Option* author_timestamp = metaedit->add_option(
      "--author-timestamp", metaedit_value.author_timestamp,
      "Author timestamp in RFC 3339 or RFC 2822 form");
  author_timestamp->excludes(update_author_timestamp);
  metaedit->add_flag("--force-rewrite", metaedit_value.force_rewrite,
                     "Rewrite even if metadata is unchanged");

  RebaseCommand rebase_value;
  auto* rebase = app.add_subcommand("rebase", "Move a change and descendants");
  rebase->add_option("-s,--source", rebase_value.source, "Source revision")
      ->required();
  rebase
      ->add_option("-d,--destination", rebase_value.destination,
                   "Destination revision")
      ->required();

  SplitCommand split_value;
  auto* split = app.add_subcommand("split", "Split selected paths");
  split->add_option("-r,--revision", split_value.revision, "Revision");
  split->add_option("-m,--message", split_value.message, "Description");
  split->add_option("paths", split_value.paths, "Repository-relative paths")
      ->required();

  SquashCommand squash_value;
  auto* squash = app.add_subcommand("squash", "Move a change into its parent");
  CLI::Option* squash_revision =
      squash->add_option("-r,--revision", squash_value.revision, "Revision");
  CLI::Option* squash_source =
      squash->add_option("-f,--from", squash_value.source, "Source revision");
  CLI::Option* squash_destination = squash->add_option(
      "-t,--into", squash_value.destination, "Destination revision");
  squash_revision->excludes(squash_source)->excludes(squash_destination);
  squash->add_option("-m,--message", squash_value.message, "Description");

  AbandonCommand abandon_value;
  auto* abandon = app.add_subcommand("abandon", "Abandon a change");
  abandon->add_option("revisions", abandon_value.revisions, "Revisions");
  abandon->add_option("-r,--revision", abandon_value.revision_options,
                      "Revisions");
  abandon->add_flag("--retain-bookmarks", abandon_value.retain_bookmarks,
                    "Move bookmarks to the abandoned revision's parent");
  abandon->add_flag("--restore-descendants",
                    abandon_value.restore_descendants,
                    "Preserve descendant contents while restacking");

  CommitCommand commit_value;
  auto* commit = app.add_subcommand(
      "commit", "Describe the working change and create a new one");
  commit->alias("ci");
  commit->add_option("filesets", commit_value.paths,
                     "Repository-relative paths");
  commit->add_flag("-i,--interactive", commit_value.interactive,
                   "Interactively select changes");
  commit->add_option("--tool", commit_value.tool, "Diff editor");
  CLI::Option* commit_message =
      commit->add_option("-m,--message", commit_value.message, "Description");
  commit->add_flag("--editor", commit_value.editor,
                   "Edit the description in an editor");

  RestoreCommand restore_value;
  auto* restore = app.add_subcommand("restore", "Restore paths from a revision");
  restore->add_option("filesets", restore_value.paths,
                      "Repository-relative paths");
  CLI::Option* restore_from =
      restore->add_option("-f,--from", restore_value.from, "Source revision");
  CLI::Option* restore_into = restore->add_option(
      "-t,--into,--to", restore_value.into, "Destination revision");
  CLI::Option* restore_changes = restore->add_option(
      "-c,--changes-in", restore_value.changes_in, "Revision whose changes to undo");
  restore_changes->excludes(restore_from)->excludes(restore_into);
  restore->add_flag("-i,--interactive", restore_value.interactive,
                    "Interactively select changes");
  restore->add_option("--tool", restore_value.tool, "Diff editor");
  restore->add_flag("--restore-descendants",
                    restore_value.restore_descendants,
                    "Preserve descendant contents while restacking");

  SimplifyParentsCommand simplify_value;
  auto* simplify = app.add_subcommand(
      "simplify-parents", "Remove redundant parent edges");
  simplify->add_option("-s,--source", simplify_value.sources,
                       "Revision and its descendants");
  simplify->add_option("-r,--revision,--revisions", simplify_value.revisions,
                       "Revision to simplify");

  auto* file = app.add_subcommand("file", "Inspect and modify files");
  file->require_subcommand(1);
  FileCommand file_list_value;
  auto* file_list = file->add_subcommand("list", "List files in a revision");
  file_list->add_option("filesets", file_list_value.paths,
                        "Repository-relative paths");
  file_list->add_option("-r,--revision", file_list_value.revision, "Revision");
  file_list->add_option("-T,--template", file_list_value.template_value,
                        "File template");
  FileCommand file_show_value;
  file_show_value.action = FileAction::show;
  auto* file_show =
      file->add_subcommand("show", "Print file contents from a revision");
  file_show->add_option("filesets", file_show_value.paths,
                        "Repository-relative paths")
      ->required();
  file_show->add_option("-r,--revision", file_show_value.revision, "Revision");
  file_show->add_option("-T,--template", file_show_value.template_value,
                        "File template");
  FileCommand file_search_value;
  file_search_value.action = FileAction::search;
  auto* file_search = file->add_subcommand("search", "Search file contents");
  file_search->add_option("filesets", file_search_value.paths,
                          "Repository-relative paths");
  file_search
      ->add_option("-r,--revision", file_search_value.revision, "Revision");
  file_search
      ->add_option("-p,--pattern", file_search_value.pattern, "Search pattern")
      ->required();
  CLI::Option* file_search_names = file_search->add_flag(
      "--name-only", file_search_value.name_only, "Print matching paths only");
  CLI::Option* file_search_lines = file_search->add_flag(
      "-n,--line-number", file_search_value.line_number, "Print line numbers");
  file_search_names->excludes(file_search_lines);
  FileCommand file_chmod_value;
  file_chmod_value.action = FileAction::chmod;
  auto* file_chmod =
      file->add_subcommand("chmod", "Set or remove executable bits");
  file_chmod
      ->add_option("mode", file_chmod_value.mode,
                   "Mode: n, normal, x, or executable")
      ->required()
      ->check(CLI::IsMember({"n", "normal", "x", "executable"}));
  file_chmod->add_option("filesets", file_chmod_value.paths,
                         "Repository-relative paths")
      ->required();
  file_chmod
      ->add_option("-r,--revision", file_chmod_value.revision, "Revision");
  FileCommand file_track_value;
  file_track_value.action = FileAction::track;
  auto* file_track = file->add_subcommand(
      "track", "Start tracking paths in working-copy snapshots");
  file_track->add_option("filesets", file_track_value.paths,
                         "Repository-relative paths")
      ->required();
  file_track->add_flag("--include-ignored", file_track_value.include_ignored,
                       "Track paths even when ignored by Git");
  FileCommand file_untrack_value;
  file_untrack_value.action = FileAction::untrack;
  auto* file_untrack = file->add_subcommand(
      "untrack", "Stop tracking paths in working-copy snapshots");
  file_untrack->add_option("filesets", file_untrack_value.paths,
                           "Repository-relative paths")
      ->required();

  auto add_diff_format = [](CLI::App* command, DiffFormatOptions& value) {
    std::vector<CLI::Option*> options;
    options.push_back(command->add_flag("-s,--summary", value.summary,
                                        "Show changed paths and statuses"));
    options.push_back(
        command->add_flag("--stat", value.stat, "Show a diffstat"));
    options.push_back(
        command->add_flag("--types", value.types, "Show file types"));
    options.push_back(command->add_flag("--name-only", value.name_only,
                                        "Show changed paths only"));
    options.push_back(
        command->add_flag("--git", value.git, "Show Git patch output"));
    options.push_back(command->add_flag(
        "--color-words", value.color_words, "Show inline word changes"));
    options.push_back(command->add_option("--tool", value.tool, "Diff tool"));
    options.push_back(
        command
            ->add_option("--context", value.context, "Context lines")
            ->check(CLI::NonNegativeNumber));
    options.push_back(command->add_flag(
        "-w,--ignore-all-space", value.ignore_all_space,
        "Ignore whitespace when comparing lines"));
    options.push_back(command->add_flag(
        "-b,--ignore-space-change", value.ignore_space_change,
        "Ignore changes in whitespace amount"));
    return options;
  };

  DiffCommand diff_value;
  auto* diff = app.add_subcommand("diff", "Compare revision contents");
  diff->add_option("filesets", diff_value.paths, "Repository-relative paths");
  CLI::Option* diff_revisions =
      diff->add_option("-r,--revisions,--revision", diff_value.revisions,
                       "Revision to compare with its parents");
  CLI::Option* diff_from =
      diff->add_option("-f,--from", diff_value.from, "Source revision");
  CLI::Option* diff_to =
      diff->add_option("-t,--to", diff_value.to, "Target revision");
  diff_revisions->excludes(diff_from)->excludes(diff_to);
  CLI::Option* diff_template = diff->add_option(
      "-T,--template", diff_value.template_value, "Diff entry template");
  for (CLI::Option* format : add_diff_format(diff, diff_value.format)) {
    diff_template->excludes(format);
  }

  ShowCommand show_value;
  auto* show = app.add_subcommand("show", "Show revisions and their changes");
  show->add_option("revisions", show_value.revisions, "Revisions");
  show->add_option("-r", show_value.revision_options, "Revisions");
  show->add_flag("--reversed", show_value.reversed,
                 "Show revisions in reverse order");
  show->add_option("-T,--template", show_value.template_value,
                   "Revision template");
  const std::vector<CLI::Option*> show_formats =
      add_diff_format(show, show_value.format);
  CLI::Option* show_no_patch =
      show->add_flag("--no-patch", show_value.no_patch, "Do not show patches");
  for (CLI::Option* option : show_formats) {
    show_no_patch->excludes(option);
  }

  auto* bookmark = app.add_subcommand("bookmark", "Manage Git-backed bookmarks");
  bookmark->require_subcommand(0, 1);
  BookmarkCommand bookmark_advance;
  bookmark_advance.action = BookmarkAction::advance;
  auto* advance =
      bookmark->add_subcommand("advance", "Advance the closest bookmarks");
  advance->add_option("names", bookmark_advance.names, "Bookmark names");
  advance->add_option("-t,--to", bookmark_advance.revision,
                      "Target revision");
  BookmarkCommand bookmark_create;
  bookmark_create.action = BookmarkAction::create;
  auto* create = bookmark->add_subcommand("create", "Create a bookmark");
  create->add_option("names", bookmark_create.names, "Bookmark names")
      ->required();
  create->add_option("-r,--revision,--to", bookmark_create.revision,
                     "Revision");
  BookmarkCommand bookmark_set;
  bookmark_set.action = BookmarkAction::set;
  auto* set = bookmark->add_subcommand("set", "Set a bookmark");
  set->add_option("names", bookmark_set.names, "Bookmark names")->required();
  set->add_option("-r,--revision,--to", bookmark_set.revision, "Revision");
  set->add_flag("-B,--allow-backwards", bookmark_set.allow_backwards,
                "Allow moving bookmarks backwards");
  BookmarkCommand bookmark_move;
  bookmark_move.action = BookmarkAction::move;
  auto* move = bookmark->add_subcommand("move", "Move existing bookmarks");
  move->add_option("names", bookmark_move.names, "Bookmark names");
  move->add_option("-f,--from", bookmark_move.from, "Source revisions");
  move->add_option("-t,--to", bookmark_move.revision, "Target revision");
  move->add_flag("-B,--allow-backwards", bookmark_move.allow_backwards,
                 "Allow moving bookmarks backwards or sideways");
  BookmarkCommand bookmark_delete;
  bookmark_delete.action = BookmarkAction::erase;
  auto* erase = bookmark->add_subcommand("delete", "Delete bookmarks");
  erase->add_option("names", bookmark_delete.names, "Bookmark names")
      ->required();
  BookmarkCommand bookmark_forget;
  bookmark_forget.action = BookmarkAction::forget;
  auto* forget = bookmark->add_subcommand("forget", "Forget bookmarks");
  forget->add_option("names", bookmark_forget.names, "Bookmark names")
      ->required();
  forget->add_flag("--include-remotes", bookmark_forget.include_remotes,
                   "Also forget matching remote bookmarks");
  BookmarkCommand bookmark_rename;
  bookmark_rename.action = BookmarkAction::rename;
  auto* rename = bookmark->add_subcommand("rename", "Rename a bookmark");
  rename->add_option("names", bookmark_rename.names, "Old and new names")
      ->required()
      ->expected(2);
  rename->add_flag("--overwrite-existing", bookmark_rename.overwrite_existing,
                   "Overwrite the destination bookmark");
  BookmarkCommand bookmark_track;
  bookmark_track.action = BookmarkAction::track;
  auto* track = bookmark->add_subcommand("track", "Track remote bookmarks");
  track->add_option("names", bookmark_track.names, "Bookmark patterns or symbols")
      ->required();
  track->add_option("--remote", bookmark_track.remotes, "Remote pattern");
  BookmarkCommand bookmark_untrack;
  bookmark_untrack.action = BookmarkAction::untrack;
  auto* untrack =
      bookmark->add_subcommand("untrack", "Stop tracking remote bookmarks");
  untrack
      ->add_option("names", bookmark_untrack.names,
                   "Bookmark patterns or symbols")
      ->required();
  untrack->add_option("--remote", bookmark_untrack.remotes,
                      "Remote pattern");
  BookmarkCommand bookmark_list;
  auto* list = bookmark->add_subcommand("list", "List bookmarks");
  list->add_option("names", bookmark_list.names, "Bookmark names");
  CLI::Option* bookmark_list_all =
      list->add_flag("-a,--all-remotes", bookmark_list.all_remotes,
                     "Include all remote bookmarks");
  CLI::Option* bookmark_list_remotes =
      list->add_option("--remote", bookmark_list.remotes, "Remote name");
  CLI::Option* bookmark_list_tracked =
      list->add_flag("-t,--tracked", bookmark_list.tracked,
                     "Show tracked remote bookmarks only");
  CLI::Option* bookmark_list_conflicted =
      list->add_flag("-c,--conflicted", bookmark_list.conflicted,
                     "Show conflicted bookmarks only");
  bookmark_list_all->excludes(bookmark_list_remotes)
      ->excludes(bookmark_list_tracked)
      ->excludes(bookmark_list_conflicted);
  list->add_option("-r,--revision,--revisions", bookmark_list.revisions,
                   "Revision containing a bookmark target");
  list->add_option("-T,--template", bookmark_list.template_value,
                   "Bookmark template");
  const std::vector<std::string> ref_sort_keys{
      "name",            "name-",           "author-name",
      "author-name-",    "author-email",    "author-email-",
      "author-date",     "author-date-",    "committer-name",
      "committer-name-", "committer-email", "committer-email-",
      "committer-date",  "committer-date-"};
  list->add_option("--sort", bookmark_list.sort, "Sort key")
      ->delimiter(',')
      ->check(CLI::IsMember(ref_sort_keys));

  auto* tag = app.add_subcommand("tag", "Manage Git-backed tags");
  tag->require_subcommand(1);
  TagCommand tag_set_value;
  tag_set_value.action = TagAction::set;
  auto* tag_set = tag->add_subcommand("set", "Set tags");
  tag_set->add_option("names", tag_set_value.names, "Tag names")->required();
  tag_set->add_option("-r,--revision,--to", tag_set_value.revision,
                      "Revision");
  tag_set->add_flag("--allow-move", tag_set_value.allow_move,
                    "Allow moving existing tags");
  TagCommand tag_delete_value;
  tag_delete_value.action = TagAction::erase;
  auto* tag_delete = tag->add_subcommand("delete", "Delete tags");
  tag_delete->add_option("names", tag_delete_value.names, "Tag names")
      ->required();
  TagCommand tag_track_value;
  tag_track_value.action = TagAction::track;
  auto* tag_track = tag->add_subcommand("track", "Track remote tags");
  tag_track->add_option("names", tag_track_value.names, "Tag patterns or symbols")
      ->required();
  tag_track->add_option("--remote", tag_track_value.remotes, "Remote pattern");
  TagCommand tag_untrack_value;
  tag_untrack_value.action = TagAction::untrack;
  auto* tag_untrack = tag->add_subcommand("untrack", "Stop tracking remote tags");
  tag_untrack
      ->add_option("names", tag_untrack_value.names, "Tag patterns or symbols")
      ->required();
  tag_untrack->add_option("--remote", tag_untrack_value.remotes,
                          "Remote pattern");
  TagCommand tag_list_value;
  auto* tag_list = tag->add_subcommand("list", "List tags");
  tag_list->add_option("names", tag_list_value.names, "Tag names");
  CLI::Option* tag_list_all =
      tag_list->add_flag("-a,--all-remotes", tag_list_value.all_remotes,
                         "Include remote tags");
  CLI::Option* tag_list_remotes =
      tag_list->add_option("--remote", tag_list_value.remotes, "Remote name");
  CLI::Option* tag_list_tracked =
      tag_list->add_flag("-t,--tracked", tag_list_value.tracked,
                         "Show tracked tags only");
  CLI::Option* tag_list_conflicted =
      tag_list->add_flag("-c,--conflicted", tag_list_value.conflicted,
                         "Show conflicted tags only");
  tag_list_all->excludes(tag_list_remotes)
      ->excludes(tag_list_tracked)
      ->excludes(tag_list_conflicted);
  tag_list->add_option("-r,--revision,--revisions", tag_list_value.revisions,
                       "Revision containing a tag target");
  tag_list->add_option("-T,--template", tag_list_value.template_value,
                       "Tag template");
  tag_list->add_option("--sort", tag_list_value.sort, "Sort key")
      ->delimiter(',')
      ->check(CLI::IsMember(ref_sort_keys));

  GitCloneCommand clone_value;
  auto* clone = app.add_subcommand("clone", "Clone a Git repository");
  clone->add_option("url", clone_value.url, "Repository URL")->required();
  clone->add_option("destination", clone_value.destination, "Destination")
      ->expected(0, 1);
  clone->add_option("--remote", clone_value.remote, "Remote name");
  clone->add_option("--depth", clone_value.depth, "Shallow clone depth")
      ->check(CLI::PositiveNumber);
  clone->add_option("-b,--branch,--bookmark", clone_value.branches,
                    "Branch name")
      ->type_size(1)
      ->allow_extra_args(false);
  clone->add_option("-t,--tag", clone_value.tags, "Tag name")
      ->type_size(1)
      ->allow_extra_args(false);
  clone->add_option("--object-hash", clone_value.object_hash, "Object hash")
      ->check(CLI::IsMember({"sha1", "sha256"}));
  GitInitCommand init_value;
  auto* init = app.add_subcommand("init", "Initialize a gg repository");
  init->add_option("destination", init_value.destination, "Destination")
      ->expected(0, 1);
  init->add_option("--object-hash", init_value.object_hash, "Object hash")
      ->check(CLI::IsMember({"sha1", "sha256"}));
  GitFetchCommand fetch_value;
  auto* fetch = app.add_subcommand("fetch", "Fetch a Git remote");
  CLI::Option* fetch_branches =
      fetch->add_option("-b,--branch,--bookmark", fetch_value.branches,
                        "Branch name");
  CLI::Option* fetch_tags =
      fetch->add_option("-t,--tag", fetch_value.tags, "Tag name");
  CLI::Option* fetch_tracked =
      fetch->add_flag("--tracked", fetch_value.tracked,
                      "Fetch tracked branches and tags only");
  fetch_tracked->excludes(fetch_branches)->excludes(fetch_tags);
  CLI::Option* fetch_remotes =
      fetch->add_option("--remote", fetch_value.remotes, "Remote name");
  fetch->add_flag("--all-remotes", fetch_value.all_remotes,
                  "Fetch every remote")
      ->excludes(fetch_remotes);
  GitPushCommand push_value;
  auto* push = app.add_subcommand("push", "Push bookmarks and tags");
  CLI::Option* push_bookmarks =
      push->add_option("-b,--bookmark,--branch", push_value.bookmarks,
                       "Bookmark name");
  CLI::Option* push_tags =
      push->add_option("-t,--tag", push_value.tags, "Tag name");
  CLI::Option* push_revisions =
      push->add_option("-r,--revision,--revisions", push_value.revisions,
                       "Revision containing refs to push");
  CLI::Option* push_changes =
      push->add_option("-c,--change", push_value.changes,
                       "Revision to push under a generated bookmark");
  CLI::Option* push_named =
      push->add_option("--named", push_value.named, "Bookmark=revision");
  push->add_flag("--all", push_value.all, "Push all bookmarks and tags");
  push->add_flag("--tracked", push_value.tracked,
                 "Push refs known on the remote");
  push->add_flag("--deleted", push_value.deleted,
                 "Delete remote bookmarks and tags deleted locally")
      ->excludes(push_bookmarks)
      ->excludes(push_tags)
      ->excludes(push_revisions)
      ->excludes(push_changes)
      ->excludes(push_named);
  push->add_option("--remote", push_value.remote, "Remote name");
  push->add_flag("--allow-empty-description",
                 push_value.allow_empty_description,
                 "Allow empty commit descriptions");
  push->add_flag("--allow-private", push_value.allow_private,
                 "Allow private commits");
  push->add_flag("--allow-conflicts", push_value.allow_conflicts,
                 "Allow conflicted commits");
  push->add_flag("--dry-run", push_value.dry_run,
                 "Show updates without pushing");
  push->add_option("-o,--option", push_value.options, "Git push option");

  auto* continue_rewrite =
      app.add_subcommand("continue", "Continue a paused rewrite");
  auto* abort_rewrite = app.add_subcommand("abort", "Abort a paused rewrite");
  auto* undo = app.add_subcommand("undo", "Restore the previous operation");
  auto* redo = app.add_subcommand("redo", "Redo the most recently undone operation");
  auto* operation = app.add_subcommand("operation", "Manage operation history");
  operation->alias("op");
  operation->require_subcommand(1);
  OperationLogCommand operation_log_value;
  auto* operation_log =
      operation->add_subcommand("log", "Show the operation log");
  operation_log
      ->add_option("-n,--limit", operation_log_value.limit,
                   "Maximum number of operations")
      ->check(CLI::NonNegativeNumber);
  operation_log->add_flag("--reversed", operation_log_value.reversed,
                          "Show older operations first");
  operation_log->add_flag("-G,--no-graph", operation_log_value.no_graph,
                          "Do not show the operation graph");
  operation_log->add_flag("-d,--op-diff", operation_log_value.op_diff,
                          "Show repository-state changes");
  operation_log->add_option("-T,--template",
                            operation_log_value.template_value,
                            "Operation template");
  OperationRestoreCommand operation_restore_value;
  auto* operation_restore = operation->add_subcommand(
      "restore", "Restore the repository to an earlier operation");
  operation_restore
      ->add_option("operation", operation_restore_value.operation, "Operation ID")
      ->required();
  operation_restore
      ->add_option("--what", operation_restore_value.what,
                   "State to restore: repo or remote-tracking")
      ->check(CLI::IsMember({"repo", "remote-tracking"}));
  auto* util = app.add_subcommand("util", "Utility commands");
  util->require_subcommand(1);
  UtilExecCommand util_exec_value;
  auto* util_exec =
      util->add_subcommand("exec", "Execute an external command");
  util_exec->add_option("command", util_exec_value.command, "Command")
      ->required();
  util_exec->add_option("args", util_exec_value.arguments, "Command arguments");
  UtilGcCommand util_gc_value;
  auto* util_gc = util->add_subcommand("gc", "Run Git garbage collection");
  util_gc->add_option("--expire", util_gc_value.expire, "Expiration threshold")
      ->check(CLI::IsMember({"now"}));
  std::string util_completion_shell;
  auto* util_completion =
      util->add_subcommand("completion", "Print a shell completion script");
  util_completion
      ->add_option("shell", util_completion_shell, "Target shell")
      ->required()
      ->check(CLI::IsMember({"bash", "elvish", "fish", "nushell",
                             "power-shell", "powershell", "zsh"}));
  auto* util_config_schema =
      util->add_subcommand("config-schema", "Print the configuration schema");
  std::string util_man_path;
  auto* util_install_man =
      util->add_subcommand("install-man-pages", "Install generated man pages");
  util_install_man->add_option("path", util_man_path, "Installation root")
      ->required();
  auto* util_markdown =
      util->add_subcommand("markdown-help", "Print Markdown command help");
  auto* util_snapshot =
      util->add_subcommand("snapshot", "Snapshot the working copy");
  auto* workspace = app.add_subcommand("workspace", "Inspect workspaces");
  workspace->require_subcommand(1);
  WorkspaceCommand workspace_list_value;
  auto* workspace_list =
      workspace->add_subcommand("list", "List known workspaces");
  workspace_list->add_option("-T,--template",
                             workspace_list_value.template_value,
                             "Workspace template");
  WorkspaceCommand workspace_root_value;
  workspace_root_value.action = WorkspaceAction::root;
  auto* workspace_root =
      workspace->add_subcommand("root", "Show the workspace root");
  workspace_root->add_option("--name", workspace_root_value.name,
                             "Workspace name");
  WorkspaceCommand workspace_forget_value;
  workspace_forget_value.action = WorkspaceAction::forget;
  auto* workspace_forget =
      workspace->add_subcommand("forget", "Stop tracking workspaces");
  workspace_forget->add_option("workspaces", workspace_forget_value.names,
                               "Workspace names");
  WorkspaceCommand workspace_rename_value;
  workspace_rename_value.action = WorkspaceAction::rename;
  auto* workspace_rename =
      workspace->add_subcommand("rename", "Rename the current workspace");
  workspace_rename
      ->add_option("name", workspace_rename_value.name, "New workspace name")
      ->required();
  auto* sparse = app.add_subcommand("sparse", "Manage sparse working copies");
  sparse->require_subcommand(1);
  SparseCommand sparse_list_value;
  auto* sparse_list =
      sparse->add_subcommand("list", "List working-copy patterns");
  SparseCommand sparse_reset_value;
  sparse_reset_value.action = SparseAction::reset;
  auto* sparse_reset =
      sparse->add_subcommand("reset", "Include all working-copy files");
  MovementCommand next_value;
  auto* next = app.add_subcommand("next", "Move to a child revision");
  CLI::Option* next_offset =
      next->add_option("offset", next_value.offset, "Number of revisions")
          ->default_val(1)
          ->check(CLI::PositiveNumber);
  CLI::Option* next_edit =
      next->add_flag("-e,--edit", next_value.edit, "Edit the target revision");
  CLI::Option* next_no_edit = next->add_flag(
      "-n,--no-edit", next_value.no_edit, "Create a new working-copy revision");
  next_edit->excludes(next_no_edit);
  next->add_flag("--conflict", next_value.conflict,
                 "Jump to the next conflicted descendant")
      ->excludes(next_offset);
  MovementCommand previous_value;
  previous_value.direction = MovementDirection::previous;
  auto* previous = app.add_subcommand("prev", "Move to an ancestor revision");
  CLI::Option* previous_offset =
      previous
          ->add_option("offset", previous_value.offset, "Number of revisions")
          ->default_val(1)
          ->check(CLI::PositiveNumber);
  CLI::Option* previous_edit = previous->add_flag(
      "-e,--edit", previous_value.edit, "Edit the target revision");
  CLI::Option* previous_no_edit = previous->add_flag(
      "-n,--no-edit", previous_value.no_edit,
      "Create a new working-copy revision");
  previous_edit->excludes(previous_no_edit);
  previous->add_flag("--conflict", previous_value.conflict,
                     "Jump to the previous conflicted ancestor")
      ->excludes(previous_offset);
  auto* config = app.add_subcommand("config", "Manage gg configuration");
  config->require_subcommand(1);
  const auto add_config_scopes = [](CLI::App* command, ConfigCommand& value) {
    CLI::Option* user =
        command->add_flag("--user", value.user, "Use user configuration");
    CLI::Option* repository = command->add_flag(
        "--repo", value.repository, "Use repository configuration");
    CLI::Option* workspace = command->add_flag(
        "--workspace", value.workspace, "Use workspace configuration");
    user->excludes(repository)->excludes(workspace);
    repository->excludes(workspace);
  };
  ConfigCommand config_edit_value;
  config_edit_value.action = ConfigAction::edit;
  auto* config_edit = config->add_subcommand("edit", "Edit configuration");
  add_config_scopes(config_edit, config_edit_value);
  ConfigCommand config_get_value;
  config_get_value.action = ConfigAction::get;
  auto* config_get = config->add_subcommand("get", "Get a configuration value");
  config_get->add_option("name", config_get_value.name, "Configuration key")
      ->required();
  ConfigCommand config_list_value;
  auto* config_list = config->add_subcommand("list", "List configuration");
  config_list->add_option("name", config_list_value.name, "Key prefix")
      ->expected(0, 1);
  config_list->add_flag("--include-defaults",
                        config_list_value.include_defaults,
                        "Include built-in defaults");
  config_list->add_flag("--include-overridden",
                        config_list_value.include_overridden,
                        "Include overridden values");
  config_list->add_option("-T,--template", config_list_value.template_value,
                          "Configuration template");
  add_config_scopes(config_list, config_list_value);
  ConfigCommand config_path_value;
  config_path_value.action = ConfigAction::path;
  auto* config_path = config->add_subcommand("path", "Print a configuration path");
  add_config_scopes(config_path, config_path_value);
  ConfigCommand config_set_value;
  config_set_value.action = ConfigAction::set;
  auto* config_set = config->add_subcommand("set", "Set a configuration value");
  config_set->add_option("name", config_set_value.name, "Configuration key")
      ->required();
  config_set->add_option("value", config_set_value.value, "TOML value")
      ->required();
  add_config_scopes(config_set, config_set_value);
  ConfigCommand config_unset_value;
  config_unset_value.action = ConfigAction::unset;
  auto* config_unset =
      config->add_subcommand("unset", "Unset a configuration value");
  config_unset->add_option("name", config_unset_value.name, "Configuration key")
      ->required();
  add_config_scopes(config_unset, config_unset_value);
  std::vector<std::string> help_commands;
  std::string help_keyword;
  auto* help = app.add_subcommand("help", "Print help");
  CLI::Option* help_command =
      help->add_option("commands", help_commands, "Command path");
  help->add_option("-k,--keyword", help_keyword, "Help keyword")
      ->check(CLI::IsMember({"bookmarks", "config", "filesets", "glossary",
                            "revsets", "templates", "tutorial"}))
      ->excludes(help_command);
  auto* version = app.add_subcommand("version", "Print version");

  if (arguments.empty()) {
    output << app.help();
    return {0, std::monostate{}};
  }

  std::vector<std::string> storage{"gg"};
  storage.reserve(arguments.size() + 1);
  for (std::string_view argument : arguments) {
    storage.emplace_back(argument);
  }
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& argument : storage) {
    argv.push_back(argument.data());
  }

  try {
    app.parse(static_cast<int>(argv.size()), argv.data());
  } catch (const CLI::CallForHelp&) {  // GG_COV_EXCL_BRANCH
    output << app.help();
    return {0, std::monostate{}};
  } catch (const CLI::CallForVersion&) {
    output << "gg 0.1.0\n";
    return {0, std::monostate{}};
  } catch (const CLI::ParseError& exception) {
    error << "error: " << exception.what() << '\n';
    return {2, std::monostate{}};
  }

  if (util_completion->parsed()) {
    const std::string shell = util_completion_shell;
    app.clear();
    write_completion(app, shell, output);
    return {0, std::monostate{}};
  }
  if (util_config_schema->parsed()) {
    write_config_schema(output);
    return {0, std::monostate{}};
  }
  if (util_markdown->parsed()) {
    app.clear();
    write_markdown_help(app, output);
    return {0, std::monostate{}};
  }
  if (util_install_man->parsed()) {
    const std::string path = util_man_path;
    app.clear();
    install_man_pages(app, path);
    return {0, std::monostate{}};
  }
  if (help->parsed()) {
    if (!help_keyword.empty()) {
      output << kHelpKeywords.at(help_keyword);
      return {0, std::monostate{}};
    }
    CLI::App* target = &app;
    std::string parent = "gg";
    for (std::size_t index = 0; index < help_commands.size(); ++index) {
      const std::string& name = help_commands[index];
      target = target->get_subcommand_no_throw(name);
      if (target == nullptr) {
        error << "error: command not found: " << name << '\n';
        return {2, std::monostate{}};
      }
      if (index + 1 < help_commands.size()) {
        parent += " " + name;
      }
    }
    output << (help_commands.empty() ? app.help() : target->help(parent));
    return {0, std::monostate{}};
  }
  if (version->parsed()) {
    output << "gg 0.1.0\n";
    return {0, std::monostate{}};
  }

  Command command = RepositoryCommand{status_value};
  if (status->parsed()) {
    command = RepositoryCommand{status_value};
  } else if (log->parsed()) {
    command = RepositoryCommand{std::move(log_value)};
  } else if (make_new->parsed()) {
    command = RepositoryCommand{std::move(new_value)};
  } else if (describe->parsed()) {
    describe_value.message_provided = describe_message->count() != 0;
    command = RepositoryCommand{std::move(describe_value)};
  } else if (edit->parsed()) {
    if (edit_value.revision.empty()) {
      edit_value.revision = std::move(edit_positional);
    }
    command = RepositoryCommand{std::move(edit_value)};
  } else if (metaedit->parsed()) {
    metaedit_value.message_provided = metaedit_message->count() != 0;
    metaedit_value.author_provided = author->count() != 0;
    metaedit_value.author_timestamp_provided = author_timestamp->count() != 0;
    command = RepositoryCommand{std::move(metaedit_value)};
  } else if (rebase->parsed()) {
    command = RepositoryCommand{std::move(rebase_value)};
  } else if (split->parsed()) {
    command = RepositoryCommand{std::move(split_value)};
  } else if (squash->parsed()) {
    command = RepositoryCommand{std::move(squash_value)};
  } else if (abandon->parsed()) {
    command = RepositoryCommand{std::move(abandon_value)};
  } else if (commit->parsed()) {
    commit_value.message_provided = commit_message->count() != 0;
    command = RepositoryCommand{std::move(commit_value)};
  } else if (restore->parsed()) {
    command = RepositoryCommand{std::move(restore_value)};
  } else if (simplify->parsed()) {
    command = RepositoryCommand{std::move(simplify_value)};
  } else if (file_list->parsed()) {
    command = RepositoryCommand{std::move(file_list_value)};
  } else if (file_show->parsed()) {
    command = RepositoryCommand{std::move(file_show_value)};
  } else if (file_search->parsed()) {
    command = RepositoryCommand{std::move(file_search_value)};
  } else if (file_chmod->parsed()) {
    command = RepositoryCommand{std::move(file_chmod_value)};
  } else if (file_track->parsed()) {
    command = RepositoryCommand{std::move(file_track_value)};
  } else if (file_untrack->parsed()) {
    command = RepositoryCommand{std::move(file_untrack_value)};
  } else if (diff->parsed()) {
    command = RepositoryCommand{std::move(diff_value)};
  } else if (show->parsed()) {
    command = RepositoryCommand{std::move(show_value)};
  } else if (advance->parsed()) {
    command = RepositoryCommand{std::move(bookmark_advance)};
  } else if (create->parsed()) {
    command = RepositoryCommand{std::move(bookmark_create)};
  } else if (set->parsed()) {
    command = RepositoryCommand{std::move(bookmark_set)};
  } else if (move->parsed()) {
    command = RepositoryCommand{std::move(bookmark_move)};
  } else if (erase->parsed()) {
    command = RepositoryCommand{std::move(bookmark_delete)};
  } else if (forget->parsed()) {
    command = RepositoryCommand{std::move(bookmark_forget)};
  } else if (rename->parsed()) {
    command = RepositoryCommand{std::move(bookmark_rename)};
  } else if (track->parsed()) {
    command = RepositoryCommand{std::move(bookmark_track)};
  } else if (untrack->parsed()) {
    command = RepositoryCommand{std::move(bookmark_untrack)};
  } else if (list->parsed() || bookmark->parsed()) {
    command = RepositoryCommand{std::move(bookmark_list)};
  } else if (tag_set->parsed()) {
    command = RepositoryCommand{std::move(tag_set_value)};
  } else if (tag_delete->parsed()) {
    command = RepositoryCommand{std::move(tag_delete_value)};
  } else if (tag_track->parsed()) {
    command = RepositoryCommand{std::move(tag_track_value)};
  } else if (tag_untrack->parsed()) {
    command = RepositoryCommand{std::move(tag_untrack_value)};
  } else if (tag_list->parsed()) {
    command = RepositoryCommand{std::move(tag_list_value)};
  } else if (clone->parsed()) {
    command = std::move(clone_value);
  } else if (init->parsed()) {
    command = std::move(init_value);
  } else if (fetch->parsed()) {
    command = RepositoryCommand{std::move(fetch_value)};
  } else if (push->parsed()) {
    command = RepositoryCommand{std::move(push_value)};
  } else if (continue_rewrite->parsed()) {
    command = ContinueCommand{};
  } else if (abort_rewrite->parsed()) {
    command = AbortCommand{};
  } else if (undo->parsed()) {  // GG_COV_EXCL_BRANCH
    command = RepositoryCommand{UndoCommand{}};
  } else if (redo->parsed()) {
    command = RepositoryCommand{RedoCommand{}};
  } else if (operation_log->parsed()) {  // GG_COV_EXCL_BRANCH
    command = RepositoryCommand{std::move(operation_log_value)};
  } else if (operation_restore->parsed()) {  // GG_COV_EXCL_BRANCH
    command = RepositoryCommand{std::move(operation_restore_value)};
  } else if (util_exec->parsed()) {
    command = std::move(util_exec_value);
  } else if (util_gc->parsed()) {
    command = RepositoryCommand{std::move(util_gc_value)};
  } else if (util_snapshot->parsed()) {  // GG_COV_EXCL_BRANCH
    command = RepositoryCommand{UtilSnapshotCommand{}};
  } else if (workspace_list->parsed()) {
    command = RepositoryCommand{std::move(workspace_list_value)};
  } else if (workspace_root->parsed()) {
    command = RepositoryCommand{std::move(workspace_root_value)};
  } else if (workspace_forget->parsed()) {
    command = RepositoryCommand{std::move(workspace_forget_value)};
  } else if (workspace_rename->parsed()) {
    command = RepositoryCommand{std::move(workspace_rename_value)};
  } else if (sparse_list->parsed()) {
    command = RepositoryCommand{sparse_list_value};
  } else if (sparse_reset->parsed()) {
    command = RepositoryCommand{sparse_reset_value};
  } else if (next->parsed()) {
    command = RepositoryCommand{next_value};
  } else if (previous->parsed()) {  // GG_COV_EXCL_BRANCH
    command = RepositoryCommand{previous_value};
  } else if (config_edit->parsed()) {
    command = RepositoryCommand{std::move(config_edit_value)};
  } else if (config_get->parsed()) {
    command = RepositoryCommand{std::move(config_get_value)};
  } else if (config_list->parsed()) {
    command = RepositoryCommand{std::move(config_list_value)};
  } else if (config_path->parsed()) {
    command = RepositoryCommand{std::move(config_path_value)};
  } else if (config_set->parsed()) {
    command = RepositoryCommand{std::move(config_set_value)};
  } else if (config_unset->parsed()) {  // GG_COV_EXCL_BRANCH
    command = RepositoryCommand{std::move(config_unset_value)};
  }

  Invocation invocation{repository, std::move(command), {},
                        std::move(config_values), std::move(config_files),
                        std::move(color), std::move(at_operation),
                        ignore_working_copy, debug, quiet};
  invocation.replay_arguments = replay_arguments(invocation.command);
  return {-1, std::move(invocation)};
}

}  // namespace gg::detail
