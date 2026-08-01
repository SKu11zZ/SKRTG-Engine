#include "skrtg/viewer/retarget_bridge.h"

#include "cli_platform.h"

#include <iostream>
#include <string>

namespace
{
void PrintHelp()
{
    std::cout
        << "SKRTG Native Viewer Retarget Bridge N2.1\n\n"
        << "usage:\n"
        << "  skrtg_retarget_bridge --request <bridge_request.json>\n"
        << "  skrtg_retarget_bridge --validate <bridge_request.json>\n";
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

    skrtg::viewer::RetargetBridgeRequest Request;
    std::string Error;
    if (!skrtg::viewer::ReadRetargetBridgeRequest(
            argv[2], Request, Error))
    {
        std::cerr << Error << '\n';
        return 2;
    }
    if (std::string(argv[1]) == "--validate")
    {
        const skrtg::viewer::RetargetBridgePreflight Preflight =
            skrtg::viewer::PreflightRetargetBridge(Request);
        for (const std::string& Warning : Preflight.Warnings)
            std::cout << "warning: " << Warning << '\n';
        for (const std::string& Message : Preflight.Errors)
            std::cerr << "error: " << Message << '\n';
        std::cout << "bridge_preflight="
                  << (Preflight.Success ? "pass" : "fail") << '\n';
        return Preflight.Success ? 0 : 1;
    }

    const skrtg::viewer::RetargetBridgeRunResult Result =
        skrtg::viewer::RunRetargetBridge(Request);
    for (const std::string& Message : Result.Errors)
        std::cerr << "error: " << Message << '\n';
    std::cout << "retarget_bridge_success="
              << (Result.Success ? "true" : "false") << '\n'
              << "review_package="
              << skrtg::viewer::PathToUtf8(Result.ReviewPackage) << '\n'
              << "status_json="
              << skrtg::viewer::PathToUtf8(Result.StatusJson) << '\n';
    return Result.Success ? 0 : 1;
}
