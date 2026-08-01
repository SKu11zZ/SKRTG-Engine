#pragma once

#include <string>
#include <vector>

namespace skrtg::core::validation
{
enum class Severity
{
    Info,
    Warning,
    Error
};

struct Issue
{
    Severity Level = Severity::Info;
    std::string Label;
    std::string Message;
    std::string Action;
};

struct JobReport
{
    std::string JobName;
    std::string Status = "not_run";
    std::vector<Issue> Issues;

    bool HasErrors() const;
    void AddIssue(Severity Level, std::string Label, std::string Message, std::string Action);
};
} // namespace skrtg::core::validation
