#pragma once

#include <set>
#include <string>

namespace skrtg::viewer::skrv::detail
{
class PackageInventory
{
public:
    bool Register(const std::string& PortableCaseInsensitiveKey,
                  const bool IsRegularFile)
    {
        if (!AllPaths.insert(PortableCaseInsensitiveKey).second)
            return false;
        if (IsRegularFile)
            RegularFiles.insert(PortableCaseInsensitiveKey);
        return true;
    }

    const std::set<std::string>& GetRegularFiles() const
    {
        return RegularFiles;
    }

private:
    std::set<std::string> AllPaths;
    std::set<std::string> RegularFiles;
};
} // namespace skrtg::viewer::skrv::detail
