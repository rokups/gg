// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace gg {

int run(std::span<const std::string_view> arguments,
        std::ostream& output,
        std::ostream& error);

}  // namespace gg
