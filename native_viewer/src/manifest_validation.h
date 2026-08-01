#pragma once

#include "skrtg/viewer/skrv/package.h"

#include <string>
#include <string_view>
#include <vector>

namespace skrtg::viewer::skrv
{
bool ValidateManifestJson(
    std::string_view Text,
    const std::vector<IntegrityEntry>& Inventory,
    ManifestSummary& OutSummary,
    std::vector<std::string>& OutErrors);
} // namespace skrtg::viewer::skrv
