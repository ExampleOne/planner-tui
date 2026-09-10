#pragma once

#include "planner.hpp"

#include <string>
#include <vector>

namespace planner {

std::vector<std::string> render(const State& state, int width, int height);
std::string render_text(const State& state, int width, int height);

}  // namespace planner

