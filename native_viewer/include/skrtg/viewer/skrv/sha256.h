#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <span>
#include <string>

namespace skrtg::viewer::skrv
{
std::string Sha256(std::span<const std::byte> Bytes);

bool Sha256File(
    const std::filesystem::path& Path,
    std::string& OutUpperHexDigest,
    std::string& OutError);

bool Sha256StreamRange(
    std::istream& Input,
    std::uint64_t ByteCount,
    std::string& OutUpperHexDigest,
    std::string& OutError);
} // namespace skrtg::viewer::skrv
