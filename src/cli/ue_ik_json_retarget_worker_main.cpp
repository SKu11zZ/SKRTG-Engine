#include "skrtg/fbx/ue_ik_json_retarget.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
bool WriteTextFile(
    const std::filesystem::path& Path,
    const std::string& Text)
{
    std::error_code Error;
    if (Path.has_parent_path())
        std::filesystem::create_directories(Path.parent_path(), Error);
    if (Error) return false;
    std::ofstream Output(Path, std::ios::binary);
    if (!Output) return false;
    Output << Text;
    return static_cast<bool>(Output);
}

bool ValidateMeshSelection(
    const skrtg::fbx::RetargetReviewMeshSelection& Selection,
    const char* Label,
    std::string& OutError)
{
    const bool HasActiveLod = Selection.ActiveLod >= 0;
    const bool HasPaths = !Selection.MeshNodePaths.empty();
    if (HasActiveLod != HasPaths)
    {
        OutError = std::string(Label) +
            " Mesh selection requires both an active LOD and exact node paths";
        return false;
    }
    if (!HasActiveLod) return true;
    if (Selection.ActiveLod > 255 ||
        Selection.MeshNodePaths.size() > 64)
    {
        OutError = std::string(Label) +
            " Mesh selection is outside the supported limits";
        return false;
    }
    std::set<std::string> Unique;
    for (const std::string& Path : Selection.MeshNodePaths)
    {
        if (Path.empty() || Path.size() > 4096 ||
            Path.front() == '/' || Path.back() == '/' ||
            Path.find("//") != std::string::npos ||
            Path.find('\\') != std::string::npos ||
            !Unique.insert(Path).second)
        {
            OutError = std::string(Label) +
                " Mesh node paths must be unique canonical relative FBX scene paths";
            return false;
        }
    }
    return true;
}

void PrintHelp()
{
    std::cout
        << "SKRTG UE IK JSON canonical-bridge retarget worker\n\n"
        << "Usage:\n"
        << "  skrtg_ueik_retarget_worker"
           " --source-rig-json <IK_Source.ikrig.json>"
           " --source-rig-sha256 <sha256>"
           " --target-rig-json <IK_Target.ikrig.json>"
           " --target-rig-sha256 <sha256>"
           " --source-alignment-rtg-json <RTG_Canonical_Source.json>"
           " --source-alignment-rtg-sha256 <sha256>"
           " --target-alignment-rtg-json <RTG_Canonical_Target.json>"
           " --target-alignment-rtg-sha256 <sha256>"
           " --source-rest-fbx <source_rest.fbx>"
           " --source-rest-sha256 <sha256>"
           " --source-animation-fbx <source_animation.fbx>"
           " --source-animation-sha256 <sha256>"
           " [--source-fbx-import-mode"
           " <fbx_body_basis_v7|ue5.8_exact_golden_v1>]"
           " [--rest-fbx-import-mode"
           " <reconciled_rest_v1|"
           "ue5.8_exported_y_reflection_v1>]"
           " [--source-animation-golden-json"
           " <animation.animgolden.json>]"
           " [--source-animation-golden-sha256 <sha256>]"
           " --target-rest-fbx <target_rest.fbx>"
           " --target-rest-sha256 <sha256>"
           " [--source-mesh-active-lod <index>"
           " --source-mesh-node-path <exact/fbx/path> ...]"
           " [--target-mesh-active-lod <index>"
           " --target-mesh-node-path <exact/fbx/path> ...]"
           " [--op-stack-json <candidate.skrtgops.json>]"
           " [--op-stack-sha256 <sha256>]"
           " --out-dir <artifact_root>"
           " [--animation-stack <exact_name>]"
           " [--clip-id <id>]"
           " [--clip-label <label>]"
           " [--sample-rate <fps>]"
           " [--foundation-export-fbx <file.fbx>]"
           " [--export-fbx <file.fbx>]\n\n"
        << "The worker reads exported UE JSON only. It does not read "
           "uasset files, infer mappings, or select/adopt a route. Optional "
           "Operation System v2 programs are exact-name, hash-bound, and "
           "candidate-only.\n";
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 &&
        (std::string(argv[1]) == "--help" ||
         std::string(argv[1]) == "-h"))
    {
        PrintHelp();
        return 0;
    }

    skrtg::fbx::UEIKJsonRetargetOptions Options;
    const std::vector<std::string> Args(argv + 1, argv + argc);
    auto RequireValue = [&](std::size_t& Index,
                            const std::string& Argument)
        -> const std::string*
    {
        if (Index + 1 >= Args.size())
        {
            std::cerr << Argument << " requires a value.\n";
            return nullptr;
        }
        return &Args[++Index];
    };

    for (std::size_t Index = 0; Index < Args.size(); ++Index)
    {
        const std::string& Argument = Args[Index];
        const std::string* Value = nullptr;
        if (Argument == "--source-rig-json")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.Route.SourceRigJson = *Value;
        }
        else if (Argument == "--source-rig-sha256")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.SourceRigJsonExpectedSha256 = *Value;
        }
        else if (Argument == "--target-rig-json")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.Route.TargetRigJson = *Value;
        }
        else if (Argument == "--target-rig-sha256")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.TargetRigJsonExpectedSha256 = *Value;
        }
        else if (Argument == "--source-alignment-rtg-json")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.Route.SourceAlignmentRetargeterJson = *Value;
        }
        else if (Argument == "--source-alignment-rtg-sha256")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.SourceAlignmentRetargeterJsonExpectedSha256 = *Value;
        }
        else if (Argument == "--target-alignment-rtg-json")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.Route.TargetAlignmentRetargeterJson = *Value;
        }
        else if (Argument == "--target-alignment-rtg-sha256")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.TargetAlignmentRetargeterJsonExpectedSha256 = *Value;
        }
        else if (Argument == "--source-rest-fbx")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.SourceRestFbxPath = *Value;
        }
        else if (Argument == "--source-rest-sha256")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.SourceRestFbxExpectedSha256 = *Value;
        }
        else if (Argument == "--source-animation-fbx")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.SourceAnimationFbxPath = *Value;
        }
        else if (Argument == "--source-animation-sha256")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.SourceAnimationFbxExpectedSha256 = *Value;
        }
        else if (Argument == "--source-fbx-import-mode")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            if (*Value == "fbx_body_basis_v7")
            {
                Options.SourceFbxImportMode =
                    skrtg::fbx::UEIKSourceFbxImportMode::
                        FbxBodyBasisV7;
            }
            else if (*Value == "ue5.8_exact_golden_v1")
            {
                Options.SourceFbxImportMode =
                    skrtg::fbx::UEIKSourceFbxImportMode::
                        UE58ExactGoldenV1;
            }
            else
            {
                std::cerr
                    << "Unsupported --source-fbx-import-mode: "
                    << *Value << "\n";
                return 2;
            }
        }
        else if (Argument == "--source-animation-golden-json")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.SourceAnimationGoldenJsonPath = *Value;
        }
        else if (Argument == "--source-animation-golden-sha256")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.SourceAnimationGoldenJsonExpectedSha256 =
                *Value;
        }
        else if (Argument == "--rest-fbx-import-mode")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            if (*Value == "reconciled_rest_v1")
            {
                Options.RestFbxImportMode =
                    skrtg::fbx::UEIKRestFbxImportMode::
                        ReconciledRestV1;
            }
            else if (*Value ==
                     "ue5.8_exported_y_reflection_v1")
            {
                Options.RestFbxImportMode =
                    skrtg::fbx::UEIKRestFbxImportMode::
                        UE58ExportedYReflectionV1;
            }
            else
            {
                std::cerr
                    << "Unsupported --rest-fbx-import-mode: "
                    << *Value << "\n";
                return 2;
            }
        }
        else if (Argument == "--target-rest-fbx")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.TargetRestFbxPath = *Value;
        }
        else if (Argument == "--target-rest-sha256")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.TargetRestFbxExpectedSha256 = *Value;
        }
        else if (Argument == "--source-mesh-active-lod" ||
                 Argument == "--target-mesh-active-lod")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            int ActiveLod = -1;
            try
            {
                std::size_t Parsed = 0;
                ActiveLod = std::stoi(*Value, &Parsed);
                if (Parsed != Value->size()) throw std::invalid_argument("");
            }
            catch (...)
            {
                std::cerr << Argument << " must be an integer.\n";
                return 2;
            }
            if (Argument == "--source-mesh-active-lod")
                Options.SourceMeshSelection.ActiveLod = ActiveLod;
            else
                Options.TargetMeshSelection.ActiveLod = ActiveLod;
        }
        else if (Argument == "--source-mesh-node-path" ||
                 Argument == "--target-mesh-node-path")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            if (Argument == "--source-mesh-node-path")
                Options.SourceMeshSelection.MeshNodePaths.push_back(*Value);
            else
                Options.TargetMeshSelection.MeshNodePaths.push_back(*Value);
        }
        else if (Argument == "--op-stack-json")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.OperationStackJsonPath = *Value;
        }
        else if (Argument == "--op-stack-sha256")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.OperationStackJsonExpectedSha256 = *Value;
        }
        else if (Argument == "--out-dir")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.OutputDirectory = *Value;
        }
        else if (Argument == "--animation-stack")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.AnimationStackName = *Value;
        }
        else if (Argument == "--clip-id")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.ClipId = *Value;
        }
        else if (Argument == "--clip-label")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.ClipLabel = *Value;
        }
        else if (Argument == "--sample-rate")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            try
            {
                Options.SampleRate = std::stod(*Value);
            }
            catch (...)
            {
                std::cerr << "--sample-rate is not a number.\n";
                return 2;
            }
        }
        else if (Argument == "--foundation-export-fbx")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.FoundationExportFbxFileName = *Value;
        }
        else if (Argument == "--export-fbx")
        {
            if ((Value = RequireValue(Index, Argument)) == nullptr)
                return 2;
            Options.ExportFbxFileName = *Value;
        }
        else
        {
            std::cerr << "Unknown argument: " << Argument << "\n";
            return 2;
        }
    }

    if (Options.Route.SourceRigJson.empty() ||
        Options.Route.TargetRigJson.empty() ||
        Options.Route.SourceAlignmentRetargeterJson.empty() ||
        Options.Route.TargetAlignmentRetargeterJson.empty() ||
        Options.SourceRigJsonExpectedSha256.empty() ||
        Options.TargetRigJsonExpectedSha256.empty() ||
        Options.SourceAlignmentRetargeterJsonExpectedSha256.empty() ||
        Options.TargetAlignmentRetargeterJsonExpectedSha256.empty() ||
        Options.SourceRestFbxPath.empty() ||
        Options.SourceRestFbxExpectedSha256.empty() ||
        Options.SourceAnimationFbxPath.empty() ||
        Options.SourceAnimationFbxExpectedSha256.empty() ||
        Options.TargetRestFbxPath.empty() ||
        Options.TargetRestFbxExpectedSha256.empty() ||
        Options.OutputDirectory.empty() ||
        (Options.OperationStackJsonPath.empty() !=
         Options.OperationStackJsonExpectedSha256.empty()) ||
        (Options.SourceFbxImportMode ==
             skrtg::fbx::UEIKSourceFbxImportMode::
                 UE58ExactGoldenV1 &&
         (Options.SourceAnimationGoldenJsonPath.empty() ||
          Options
              .SourceAnimationGoldenJsonExpectedSha256
              .empty())))
    {
        PrintHelp();
        return 2;
    }
    std::string MeshSelectionError;
    if (!ValidateMeshSelection(
            Options.SourceMeshSelection, "source",
            MeshSelectionError) ||
        !ValidateMeshSelection(
            Options.TargetMeshSelection, "target",
            MeshSelectionError))
    {
        std::cerr << MeshSelectionError << "\n";
        return 2;
    }

    const skrtg::fbx::UEIKJsonRetargetResult Result =
        skrtg::fbx::GenerateUEIKJsonRetargetReview(Options);
    if (!Result.ConsoleSummary.empty())
        std::cout << Result.ConsoleSummary << "\n";
    for (const std::string& Warning : Result.Warnings)
        std::cout << "warning: " << Warning << "\n";
    for (const std::string& Error : Result.Errors)
        std::cerr << "error: " << Error << "\n";
    if (!Result.Success) return 1;

    for (const skrtg::fbx::RetargetReviewPackageArtifact& Artifact :
         Result.Artifacts)
    {
        if (!WriteTextFile(Artifact.Path, Artifact.Text))
        {
            std::cerr << "error: failed to write artifact: "
                      << Artifact.Path.string() << "\n";
            return 1;
        }
        std::cout << "Artifact: " << Artifact.Path.string() << "\n";
    }
    std::cout << "Viewer: " << Result.ViewerPath.string() << "\n"
              << "Foundation FBX: "
              << Result.ExportedFoundationFbxPath.string() << "\n"
              << "Foundation SHA256: "
              << Result.ExportedFoundationFbxSha256 << "\n"
              << "Final FBX: "
              << Result.ExportedFbxPath.string() << "\n"
              << "Final SHA256: "
              << Result.ExportedFbxSha256 << "\n";
    return 0;
}
