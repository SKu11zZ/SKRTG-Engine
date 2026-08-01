#include "skrtg/core/validation/report.h"

#include <utility>

namespace skrtg::core::validation
{
bool JobReport::HasErrors() const
{
    for (const Issue& Entry : Issues)
    {
        if (Entry.Level == Severity::Error)
        {
            return true;
        }
    }
    return false;
}

void JobReport::AddIssue(Severity Level, std::string Label, std::string Message, std::string Action)
{
    Issues.push_back({Level, std::move(Label), std::move(Message), std::move(Action)});
}
} // namespace skrtg::core::validation
