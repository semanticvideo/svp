#pragma once

#include "svp/exec/artifact_ref.hpp"

#include <filesystem>
#include <map>
#include <string>

namespace svp::exec {

// A TaskSpec input after the executor has located its verified bytes (plan
// §4.3: inputs are BLAKE3 references resolved inside the worker's cache
// root). `path` names a local file whose content matches `ref`.
struct ResolvedInput {
  ArtifactRef ref;
  std::filesystem::path path;
};

// Keyed by TaskSpec input name.
using ResolvedInputs = std::map<std::string, ResolvedInput>;

}  // namespace svp::exec
