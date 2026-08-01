#include "skrtg/viewer/process.h"

#include <cerrno>
#include <cstring>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace skrtg::viewer
{
namespace
{
#if defined(_WIN32)
std::wstring Utf8ToWide(const std::string& Value)
{
    if (Value.empty()) return {};
    const int Count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, Value.data(),
        static_cast<int>(Value.size()), nullptr, 0);
    if (Count <= 0) return {};
    std::wstring Result(static_cast<std::size_t>(Count), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, Value.data(),
            static_cast<int>(Value.size()), Result.data(), Count) != Count)
    {
        return {};
    }
    return Result;
}

std::wstring QuoteWindowsArgument(const std::wstring& Value)
{
    if (Value.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return Value;
    std::wstring Result = L"\"";
    std::size_t Backslashes = 0;
    for (const wchar_t Character : Value)
    {
        if (Character == L'\\')
        {
            ++Backslashes;
            continue;
        }
        if (Character == L'\"')
        {
            Result.append(Backslashes * 2 + 1, L'\\');
            Result.push_back(L'\"');
            Backslashes = 0;
            continue;
        }
        Result.append(Backslashes, L'\\');
        Backslashes = 0;
        Result.push_back(Character);
    }
    Result.append(Backslashes * 2, L'\\');
    Result.push_back(L'\"');
    return Result;
}

std::string WindowsError(const char* Prefix)
{
    return std::string(Prefix) + ": win32=" +
        std::to_string(GetLastError());
}
#endif
} // namespace

struct ChildProcess::Impl
{
#if defined(_WIN32)
    HANDLE Process = nullptr;
    HANDLE Thread = nullptr;
    HANDLE Job = nullptr;
#else
    pid_t Pid = -1;
#endif
    bool Active = false;
};

ChildProcess::ChildProcess() : State(std::make_unique<Impl>()) {}

ChildProcess::~ChildProcess()
{
    std::string Ignored;
    if (IsActive()) Terminate(Ignored);
}

ChildProcess::ChildProcess(ChildProcess&&) noexcept = default;
ChildProcess& ChildProcess::operator=(ChildProcess&& Other) noexcept
{
    if (this == &Other) return *this;
    std::string Ignored;
    if (IsActive()) Terminate(Ignored);
    State = std::move(Other.State);
    return *this;
}

bool ChildProcess::Start(
    const ProcessLaunchOptions& Options,
    std::string& OutError)
{
    OutError.clear();
    if (!State) State = std::make_unique<Impl>();
    if (State->Active)
    {
        OutError = "a child process is already active";
        return false;
    }
    if (Options.Executable.empty() || Options.LogPath.empty())
    {
        OutError = "process executable and log path are required";
        return false;
    }
    std::error_code FileError;
    if (Options.LogPath.has_parent_path())
    {
        std::filesystem::create_directories(
            Options.LogPath.parent_path(), FileError);
        if (FileError)
        {
            OutError = "failed to create process log directory";
            return false;
        }
    }
#if defined(_WIN32)
    HANDLE Log = CreateFileW(
        Options.LogPath.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (Log == INVALID_HANDLE_VALUE)
    {
        OutError = WindowsError("failed to open process log");
        return false;
    }
    SetHandleInformation(Log, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    HANDLE Job = CreateJobObjectW(nullptr, nullptr);
    if (Job == nullptr)
    {
        CloseHandle(Log);
        OutError = WindowsError("failed to create child process job");
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION JobLimits{};
    JobLimits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            Job, JobObjectExtendedLimitInformation,
            &JobLimits, sizeof(JobLimits)))
    {
        CloseHandle(Job);
        CloseHandle(Log);
        OutError = WindowsError("failed to configure child process job");
        return false;
    }

    std::wstring CommandLine =
        QuoteWindowsArgument(Options.Executable.wstring());
    for (const std::string& Argument : Options.Arguments)
    {
        const std::wstring Wide = Utf8ToWide(Argument);
        if (!Argument.empty() && Wide.empty())
        {
            CloseHandle(Job);
            CloseHandle(Log);
            OutError = "process argument is not valid UTF-8";
            return false;
        }
        CommandLine.push_back(L' ');
        CommandLine += QuoteWindowsArgument(Wide);
    }
    std::vector<wchar_t> Mutable(
        CommandLine.begin(), CommandLine.end());
    Mutable.push_back(L'\0');

    STARTUPINFOW Startup{};
    Startup.cb = sizeof(Startup);
    Startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    Startup.wShowWindow = SW_HIDE;
    Startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    Startup.hStdOutput = Log;
    Startup.hStdError = Log;
    PROCESS_INFORMATION Information{};
    const std::wstring Working = Options.WorkingDirectory.empty()
        ? std::wstring()
        : Options.WorkingDirectory.wstring();
    const BOOL Created = CreateProcessW(
        nullptr, Mutable.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
        Working.empty() ? nullptr : Working.c_str(),
        &Startup, &Information);
    CloseHandle(Log);
    if (!Created)
    {
        CloseHandle(Job);
        OutError = WindowsError("failed to create child process");
        return false;
    }
    if (!AssignProcessToJobObject(Job, Information.hProcess))
    {
        const std::string Error = WindowsError(
            "failed to assign child process job");
        TerminateProcess(Information.hProcess, 1);
        WaitForSingleObject(Information.hProcess, 5000);
        CloseHandle(Information.hThread);
        CloseHandle(Information.hProcess);
        CloseHandle(Job);
        OutError = Error;
        return false;
    }
    if (ResumeThread(Information.hThread) == static_cast<DWORD>(-1))
    {
        const std::string Error = WindowsError(
            "failed to resume child process");
        CloseHandle(Job);
        WaitForSingleObject(Information.hProcess, 5000);
        CloseHandle(Information.hThread);
        CloseHandle(Information.hProcess);
        OutError = Error;
        return false;
    }
    State->Process = Information.hProcess;
    State->Thread = Information.hThread;
    State->Job = Job;
    State->Active = true;
    return true;
#else
    const int Log = open(
        Options.LogPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (Log < 0)
    {
        OutError = std::string("failed to open process log: ") +
            std::strerror(errno);
        return false;
    }
    const pid_t Pid = fork();
    if (Pid < 0)
    {
        close(Log);
        OutError = std::string("failed to fork child process: ") +
            std::strerror(errno);
        return false;
    }
    if (Pid == 0)
    {
        setpgid(0, 0);
        if (!Options.WorkingDirectory.empty() &&
            chdir(Options.WorkingDirectory.c_str()) != 0)
            _exit(126);
        dup2(Log, STDOUT_FILENO);
        dup2(Log, STDERR_FILENO);
        close(Log);
        std::vector<std::string> Storage;
        Storage.reserve(Options.Arguments.size() + 1);
        Storage.push_back(Options.Executable.string());
        Storage.insert(
            Storage.end(), Options.Arguments.begin(), Options.Arguments.end());
        std::vector<char*> Arguments;
        Arguments.reserve(Storage.size() + 1);
        for (std::string& Value : Storage)
            Arguments.push_back(Value.data());
        Arguments.push_back(nullptr);
        execvp(Arguments[0], Arguments.data());
        _exit(127);
    }
    close(Log);
    setpgid(Pid, Pid);
    State->Pid = Pid;
    State->Active = true;
    return true;
#endif
}

bool ChildProcess::Poll(
    bool& OutFinished,
    int& OutExitCode,
    std::string& OutError)
{
    OutFinished = false;
    OutExitCode = -1;
    OutError.clear();
    if (!State || !State->Active)
    {
        OutError = "no child process is active";
        return false;
    }
#if defined(_WIN32)
    const DWORD WaitResult = WaitForSingleObject(State->Process, 0);
    if (WaitResult == WAIT_TIMEOUT) return true;
    if (WaitResult != WAIT_OBJECT_0)
    {
        OutError = WindowsError("failed to poll child process");
        return false;
    }
    DWORD ExitCode = 0;
    if (!GetExitCodeProcess(State->Process, &ExitCode))
    {
        OutError = WindowsError("failed to obtain child exit code");
        return false;
    }
    OutFinished = true;
    OutExitCode = static_cast<int>(ExitCode);
    CloseHandle(State->Thread);
    CloseHandle(State->Process);
    CloseHandle(State->Job);
    State->Thread = nullptr;
    State->Process = nullptr;
    State->Job = nullptr;
#else
    int Status = 0;
    const pid_t Result = waitpid(State->Pid, &Status, WNOHANG);
    if (Result == 0) return true;
    if (Result < 0)
    {
        OutError = std::string("failed to poll child process: ") +
            std::strerror(errno);
        return false;
    }
    OutFinished = true;
    OutExitCode = WIFEXITED(Status)
        ? WEXITSTATUS(Status)
        : 128 + (WIFSIGNALED(Status) ? WTERMSIG(Status) : 0);
    State->Pid = -1;
#endif
    State->Active = false;
    return true;
}

bool ChildProcess::Wait(int& OutExitCode, std::string& OutError)
{
    OutExitCode = -1;
    OutError.clear();
    if (!State || !State->Active)
    {
        OutError = "no child process is active";
        return false;
    }
#if defined(_WIN32)
    if (WaitForSingleObject(State->Process, INFINITE) != WAIT_OBJECT_0)
    {
        OutError = WindowsError("failed to wait for child process");
        return false;
    }
    DWORD ExitCode = 0;
    if (!GetExitCodeProcess(State->Process, &ExitCode))
    {
        OutError = WindowsError("failed to obtain child exit code");
        return false;
    }
    OutExitCode = static_cast<int>(ExitCode);
    CloseHandle(State->Thread);
    CloseHandle(State->Process);
    CloseHandle(State->Job);
    State->Thread = nullptr;
    State->Process = nullptr;
    State->Job = nullptr;
#else
    int Status = 0;
    if (waitpid(State->Pid, &Status, 0) < 0)
    {
        OutError = std::string("failed to wait for child process: ") +
            std::strerror(errno);
        return false;
    }
    OutExitCode = WIFEXITED(Status)
        ? WEXITSTATUS(Status)
        : 128 + (WIFSIGNALED(Status) ? WTERMSIG(Status) : 0);
    State->Pid = -1;
#endif
    State->Active = false;
    return true;
}

bool ChildProcess::Terminate(std::string& OutError)
{
    OutError.clear();
    if (!State || !State->Active)
    {
        OutError = "no child process is active";
        return false;
    }
#if defined(_WIN32)
    bool Success = true;
    if (State->Job != nullptr &&
        !TerminateJobObject(State->Job, 130U))
    {
        OutError = WindowsError("failed to terminate child process job");
        Success = false;
    }
    DWORD WaitResult = State->Process != nullptr
        ? WaitForSingleObject(State->Process, 5000)
        : WAIT_OBJECT_0;
    if (WaitResult == WAIT_TIMEOUT && State->Job != nullptr)
    {
        // Closing a kill-on-close job is the final tree-wide fallback. Keep
        // the process handle open and wait once more so success means the root
        // process really did signal before the Viewer records cancellation.
        CloseHandle(State->Job);
        State->Job = nullptr;
        WaitResult = State->Process != nullptr
            ? WaitForSingleObject(State->Process, 5000)
            : WAIT_OBJECT_0;
    }
    if (WaitResult != WAIT_OBJECT_0)
    {
        if (OutError.empty() && WaitResult == WAIT_TIMEOUT)
            OutError = "timed out waiting for terminated child process tree";
        else if (OutError.empty())
            OutError = WindowsError("failed to wait for terminated child");
        Success = false;
    }
    if (State->Thread != nullptr) CloseHandle(State->Thread);
    if (State->Process != nullptr) CloseHandle(State->Process);
    if (State->Job != nullptr) CloseHandle(State->Job);
    State->Thread = nullptr;
    State->Process = nullptr;
    State->Job = nullptr;
#else
    bool Success = true;
    if (State->Pid > 0 && kill(-State->Pid, SIGKILL) != 0 &&
        errno != ESRCH)
    {
        OutError = std::string("failed to terminate child process group: ") +
            std::strerror(errno);
        Success = false;
    }
    int Status = 0;
    if (State->Pid > 0 && waitpid(State->Pid, &Status, 0) < 0 &&
        errno != ECHILD)
    {
        if (OutError.empty())
        {
            OutError = std::string("failed to wait for terminated child: ") +
                std::strerror(errno);
        }
        Success = false;
    }
    State->Pid = -1;
#endif
    State->Active = false;
    return Success;
}

bool ChildProcess::IsActive() const
{
    return State != nullptr && State->Active;
}

ProcessRunResult RunProcessBlocking(const ProcessLaunchOptions& Options)
{
    ProcessRunResult Result;
    ChildProcess Process;
    if (!Process.Start(Options, Result.Error)) return Result;
    Result.Started = true;
    if (!Process.Wait(Result.ExitCode, Result.Error))
        Result.ExitCode = -1;
    return Result;
}

} // namespace skrtg::viewer
