// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

namespace gg::test {

TEST_F(RepositoryTest, RewritesAncestorsWhileEditingDescendants) {
  const Result first = invoke({"new", "-m", "first", "main"});
  ASSERT_EQ(first.code, 0) << first.error;
  const std::string first_id = token_after(first.output, "Working copy now at: ");
  write("one.txt", "one\n");
  write("two.txt", "two\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"new", "-m", "descendant"}).code, 0);

  ASSERT_EQ(invoke({"split", "-r", first_id, "-m", "selected", "one.txt"}).code,
            0);
  ASSERT_EQ(invoke({"squash", "-r", first_id, "-m", "combined"}).code, 0);
  EXPECT_NE(invoke({"log"}).output.find("descendant"), std::string::npos);
}

TEST_F(RepositoryTest, RewritesChangesOutsideTheCurrentLine) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0) << source.error;
  const std::string source_id = token_after(source.output, "Working copy now at: ");
  write("one.txt", "one\n");
  write("two.txt", "two\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const Result current = invoke({"new", "-m", "current", "main"});
  ASSERT_EQ(current.code, 0) << current.error;
  const std::string current_id_value =
      token_after(current.output, "Working copy now at: ");
  ASSERT_EQ(invoke({"split", "-r", source_id, "one.txt"}).code, 0);
  ASSERT_EQ(invoke({"describe", "-m", "described", source_id}).code, 0);
  EXPECT_EQ(current_id(), current_id_value.substr(0, 8));
}

TEST_F(RepositoryTest, SplitsDirectoryPaths) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  write("directory/one.txt", "one\n");
  write("directory-other.txt", "not in the directory\n");
  write("other.txt", "other\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const Result split = invoke({"split", "directory"});
  ASSERT_EQ(split.code, 0) << split.error;
  EXPECT_NE(invoke({"status"}).output.find("A other.txt"), std::string::npos);
}

TEST_F(RepositoryTest, SquashesAnAncestorOfTheWorkingCopy) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0) << source.error;
  const std::string source_id = token_after(source.output, "Working copy now at: ");
  const Result child = invoke({"new", "-m", "child"});
  ASSERT_EQ(child.code, 0) << child.error;
  const std::string child_id = token_after(child.output, "Working copy now at: ");
  ASSERT_EQ(invoke({"squash", "-r", source_id}).code, 0);
  EXPECT_EQ(current_id(), child_id.substr(0, 8));
}

TEST_F(RepositoryTest, RebasesWithoutConflicts) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0) << source.error;
  const std::string source_id = token_after(source.output, "Working copy now at: ");
  write("source.txt", "source\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const Result destination = invoke({"new", "-m", "destination", "main"});
  ASSERT_EQ(destination.code, 0) << destination.error;
  const std::string destination_id =
      token_after(destination.output, "Working copy now at: ");
  write("destination.txt", "destination\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const Result rebased =
      invoke({"rebase", "-s", source_id, "-d", destination_id});
  ASSERT_EQ(rebased.code, 0) << rebased.error;
  EXPECT_NE(rebased.output.find("Rebased"), std::string::npos);
}

TEST_F(RepositoryTest, RebasesAnAncestorOfTheWorkingCopy) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0) << source.error;
  const std::string source_id = token_after(source.output, "Working copy now at: ");
  write("source.txt", "source\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const Result child = invoke({"new", "-m", "child"});
  ASSERT_EQ(child.code, 0) << child.error;
  const std::string child_id = token_after(child.output, "Working copy now at: ");
  EXPECT_EQ(invoke({"rebase", "-s", source_id, "-d", child_id}).code, 2);

  const Result destination = invoke({"new", "-m", "destination", "main"});
  ASSERT_EQ(destination.code, 0) << destination.error;
  const std::string destination_id =
      token_after(destination.output, "Working copy now at: ");
  write("destination.txt", "destination\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"edit", child_id}).code, 0);

  const Result rebased =
      invoke({"rebase", "-s", source_id, "-d", destination_id});
  ASSERT_EQ(rebased.code, 0) << rebased.error;
  EXPECT_EQ(current_id(), child_id.substr(0, 8));
}

TEST_F(RepositoryTest, RejectsSplittingAllChanges) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  write("only.txt", "only\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  EXPECT_EQ(invoke({"split", "only.txt"}).code, 2);
}

TEST_F(RepositoryTest, SquashesAndAbandonsCurrentChanges) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  const Result source = invoke({"new", "-m", "source"});
  ASSERT_EQ(source.code, 0) << source.error;
  ASSERT_EQ(invoke({"squash"}).code, 0);

  ASSERT_EQ(invoke({"new", "-m", "discard"}).code, 0);
  ASSERT_EQ(invoke({"bookmark", "create", "discarded"}).code, 0);
  ASSERT_EQ(invoke({"abandon"}).code, 0);
  EXPECT_FALSE(has_ref("refs/heads/discarded"));
}

TEST_F(RepositoryTest, RejectsInvalidRewriteShapes) {
  const git_oid base = ref("HEAD");
  ASSERT_EQ(git_reference_remove(repository_.get(), "refs/heads/main"), 0);
  std::filesystem::remove(path_ / "tracked.txt");
  const Result root = invoke({"new", "-m", "root"});
  ASSERT_EQ(root.code, 0) << root.error;
  const std::string root_id = token_after(root.output, "Working copy now at: ");
  EXPECT_EQ(invoke({"rebase", "-s", root_id, "-d",
                    std::string(git_oid_tostr_s(&base))})
                .code,
            2);
  EXPECT_EQ(invoke({"split", "-r", root_id, "anything"}).code, 2);
  EXPECT_EQ(invoke({"squash", "-r", root_id}).code, 2);
  EXPECT_EQ(invoke({"abandon", root_id}).code, 2);

}

TEST_F(RepositoryTest, RejectsSquashingIntoANonParent) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0) << source.error;
  const std::string source_id = token_after(source.output, "Working copy now at: ");
  const Result other = invoke({"new", "-m", "other", "main"});
  ASSERT_EQ(other.code, 0) << other.error;
  const std::string other_id = token_after(other.output, "Working copy now at: ");
  EXPECT_EQ(invoke({"squash", "--from", source_id, "--into", other_id}).code, 2);
}

TEST_F(RepositoryTest, SquashesAndAbandonsUnrelatedChanges) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0) << source.error;
  const std::string source_id = token_after(source.output, "Working copy now at: ");
  ASSERT_EQ(invoke({"new", "-m", "current", "main"}).code, 0);
  ASSERT_EQ(invoke({"squash", "-r", source_id}).code, 0);

  const Result discarded = invoke({"new", "-m", "discarded", "main"});
  ASSERT_EQ(discarded.code, 0) << discarded.error;
  const std::string discarded_id =
      token_after(discarded.output, "Working copy now at: ");
  ASSERT_EQ(invoke({"new", "-m", "still current", "main"}).code, 0);
  ASSERT_EQ(invoke({"abandon", discarded_id}).code, 0);
}

TEST_F(RepositoryTest, AbandonCanRetainBookmarksAndDescendantContents) {
  const Result parent = invoke({"new", "-m", "parent", "main"});
  ASSERT_EQ(parent.code, 0) << parent.error;
  const std::string parent_id = token_after(parent.output, "Working copy now at: ");
  write("parent.txt", "parent\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"bookmark", "create", "kept"}).code, 0);
  ASSERT_EQ(invoke({"new", "-m", "child"}).code, 0);
  write("child.txt", "child\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  ASSERT_EQ(invoke({"abandon", "--retain-bookmarks",
                    "--restore-descendants", parent_id})
                .code,
            0);
  EXPECT_TRUE(has_ref("refs/heads/kept"));
  EXPECT_EQ(invoke({"file", "show", "parent.txt"}).output, "parent\n");
  EXPECT_EQ(invoke({"file", "show", "child.txt"}).output, "child\n");
}


}  // namespace gg::test
