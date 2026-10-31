#pragma once

#include <string>
#include <vector>
#include "providers/provider.h"

namespace pf {

// Human-readable SC suggestions for the local diagnostic endpoint.
std::string BuildBingResponse(const std::string& query, const std::vector<std::string>& words);
std::string BuildNativeBingResponse(const std::string& query, const std::string& pinyin, const std::vector<Candidate>& candidates);

}  // namespace pf
