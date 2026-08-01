#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace skrtg::viewer
{
struct ProcessLaunchOptions
{
    std::filesystem::path Executable;
    std::vector<std::string> Arguments;
    std::filesystem::path WorkingDirectory;
    std::filesystem::path LogPath;
};

class ChildProcess
{
public:
    ChildProcess();
    ~ChildProcess();
    ChildProcess(ChildProcess&&) noexcept;
    ChildProcess& operator=(ChildProcess&&) noexcept;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool Start(const ProcessLaunchOptions& Options, std::string& OutError);
    bool Poll(bool& OutFinished, int& OutExitCode, std::string& OutError);
    bool Wait(int& OutExitCode, std::string& OutError);
    bool Terminate(std::string& OutError);
    bool IsActive() const;

private:
    struct Impl;
    std::unique_ptr<Impl> State;
};

struct ProcessRunResult
{
    bool Started = false;
    int ExitCode = -1;
    std::string Error;
};

ProcessRunResult RunProcessBlocking(const ProcessLaunchOptions& Options);

} // namespace skrtg::viewer
