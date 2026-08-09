// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gg::detail {

char** process_environment();

std::optional<int> run_process(
    const std::vector<std::string>& arguments,
    char* const* environment = nullptr,
    const std::optional<std::filesystem::path>& standard_output = std::nullopt);

}  // namespace gg::detail
