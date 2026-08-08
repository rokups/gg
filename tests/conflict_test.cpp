// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

namespace gg::test {

TEST_F(RepositoryTest, PausesConflictingRebaseAndCanAbortOrContinue) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0) << source.error;
  const std::string source_id = token_after(source.output, "Working copy now at: ");
  write("tracked.txt", "source\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const Result destination = invoke({"new", "-m", "destination", "main"});
  ASSERT_EQ(destination.code, 0) << destination.error;
  const std::string destination_id =
      token_after(destination.output, "Working copy now at: ");
  write("tracked.txt", "destination\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const Result conflict =
      invoke({"rebase", "-s", source_id, "-d", destination_id});
  ASSERT_EQ(conflict.code, 1) << conflict.error;
  EXPECT_TRUE(has_ref("refs/gg/rewrite"));
  EXPECT_NE(file().find("<<<<<<<"), std::string::npos);
  EXPECT_NE(invoke({"status"}).output.find("paused with conflicts"),
            std::string::npos);
  EXPECT_EQ(invoke({"st"}).code, 0);
  EXPECT_EQ(invoke({"continue"}).code, 2);
  EXPECT_EQ(invoke({"log"}).code, 2);
  write("tracked.txt", "<<<<<<< partial marker\n");
  EXPECT_EQ(invoke({"continue"}).code, 2);
  write("tracked.txt", "======= partial marker\n");
  EXPECT_EQ(invoke({"continue"}).code, 2);
  write("tracked.txt", ">>>>>>> partial marker\n");
  EXPECT_EQ(invoke({"continue"}).code, 2);

  ASSERT_EQ(invoke({"abort"}).code, 0);
  EXPECT_FALSE(has_ref("refs/gg/rewrite"));
  EXPECT_EQ(file(), "destination\n");

  ASSERT_EQ(invoke({"rebase", "-s", source_id, "-d", destination_id}).code, 1);
  write("tracked.txt", "resolved\n");
  const Result continued = invoke({"continue"});
  ASSERT_EQ(continued.code, 0) << continued.error;
  EXPECT_FALSE(has_ref("refs/gg/rewrite"));
  EXPECT_EQ(file(), "destination\n");
  EXPECT_NE(invoke({"log", "-r", source_id}).output.find("source"),
            std::string::npos);
}

TEST_F(RepositoryTest, ReportsDeleteModifyConflicts) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0) << source.error;
  const std::string source_id = token_after(source.output, "Working copy now at: ");
  write("tracked.txt", "source\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const Result destination = invoke({"new", "-m", "destination", "main"});
  ASSERT_EQ(destination.code, 0) << destination.error;
  const std::string destination_id =
      token_after(destination.output, "Working copy now at: ");
  std::filesystem::remove(path_ / "tracked.txt");
  ASSERT_EQ(invoke({"status"}).code, 0);

  EXPECT_EQ(invoke({"rebase", "-s", source_id, "-d", destination_id}).code, 1);
  EXPECT_TRUE(has_ref("refs/gg/rewrite"));
  EXPECT_EQ(invoke({"status", "extra"}).code, 0);
  EXPECT_EQ(invoke({"abort", "extra"}).code, 2);
  EXPECT_EQ(invoke({"continue", "extra"}).code, 2);
  EXPECT_EQ(invoke({"abort"}).code, 0);
}

TEST_F(RepositoryTest, ContinuesThroughConsecutiveConflicts) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0) << source.error;
  const std::string source_id = token_after(source.output, "Working copy now at: ");
  write("tracked.txt", "source\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  ASSERT_EQ(invoke({"new", "-m", "child"}).code, 0);
  write("tracked.txt", "child\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const Result destination = invoke({"new", "-m", "destination", "main"});
  ASSERT_EQ(destination.code, 0) << destination.error;
  const std::string destination_id =
      token_after(destination.output, "Working copy now at: ");
  write("tracked.txt", "destination\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  ASSERT_EQ(invoke({"rebase", "-s", source_id, "-d", destination_id}).code, 1);
  write("tracked.txt", "resolved source\n");
  EXPECT_EQ(invoke({"continue"}).code, 1);
  EXPECT_TRUE(has_ref("refs/gg/rewrite"));
  write("tracked.txt", "resolved child\n");
  const Result continued = invoke({"continue"});
  ASSERT_EQ(continued.code, 0) << continued.error;
  EXPECT_FALSE(has_ref("refs/gg/rewrite"));
}

}  // namespace gg::test
