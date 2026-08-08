// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "gg/app.hpp"

#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
  std::vector<std::string_view> arguments(argv + 1, argv + argc);
  return gg::run(arguments, std::cout, std::cerr);
}
