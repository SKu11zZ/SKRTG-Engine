#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>

namespace skrtg::viewer::skrv
{
std::string Sha256(std::span<const std::byte> Bytes);

bool Sha256File(
    const std::filesystem::path& Path,
    std::string& OutUpperHexDigest,
    std::string& OutError);
} // namespace skrtg::viewer::skrv
