#include "skrtg/viewer/batch_retarget.h"

#include "cli_platform.h"

#include <iostream>
#include <string>

namespace
{
void PrintHelp()
{
    std::cout
        << "SKRTG low-memory batch retarget coordinator\n\n"
        << "usage:\n"
        << "  skrtg_batch_retarget --request <batch_request.json>\n"
        << "  skrtg_batch_retarget --validate <batch_request.json>\n";
}
} // namespace

int main(int argc, char** argv)
{
    ConfigureNonInteractiveCli();
    if (argc == 2 &&
        (std::string(argv[1]) == "--help" ||
         std::string(argv[1]) == "-h"))
    {
        PrintHelp();
        return 0;
    }
    if (argc != 3 ||
        (std::string(argv[1]) != "--request" &&
         std::string(argv[1]) != "--validate"))
    {
        PrintHelp();
        return 2;
    }

    skrtg::viewer::BatchRetargetRequest Request;
    std::string Error;
    if (!skrtg::viewer::ReadBatchRetargetRequest(
            argv[2], Request, Error))
    {
        std::cerr << Error << '\n';
        return 2;
    }
    if (std::string(argv[1]) == "--validate")
    {
        const skrtg::viewer::BatchRetargetPlan Plan =
            skrtg::viewer::BuildBatchRetargetPlan(Request);
        for (const std::string& Warning : Plan.Warnings)
            std::cout << "warning: " << Warning << '\n';
        for (const std::string& Message : Plan.Errors)
            std::cerr << "error: " << Message << '\n';
        std::cout << "batch_preflight="
                  << (Plan.Success ? "pass" : "fail") << '\n'
                  << "job_count=" << Plan.Jobs.size() << '\n'
                  << "maximum_concurrent_jobs="
                  << Plan.MaximumConcurrentJobs << '\n';
        return Plan.Success ? 0 : 1;
    }

    const skrtg::viewer::BatchRetargetRunResult Result =
        skrtg::viewer::RunBatchRetarget(Request);
    for (const std::string& Message : Result.Errors)
        std::cerr << "error: " << Message << '\n';
    std::cout << "batch_success="
              << (Result.Success ? "true" : "false") << '\n'
              << "status_json="
              << skrtg::viewer::PathToUtf8(Result.StatusJson) << '\n'
              << "jobs_total=" << Result.Status.TotalJobs << '\n'
              << "jobs_succeeded=" << Result.Status.SucceededJobs << '\n'
              << "jobs_failed=" << Result.Status.FailedJobs << '\n'
              << "maximum_concurrent_jobs="
              << Result.Status.MaximumConcurrentJobs << '\n';
    return Result.Success ? 0 : 1;
}
