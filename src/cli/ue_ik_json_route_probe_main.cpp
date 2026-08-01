#include "skrtg/retarget/ue_ik_json_route.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace
{
void PrintHelp()
{
    std::cout
        << "SKRTG UE IK JSON canonical bridge route probe\n\n"
        << "usage:\n"
        << "  skrtg_ueik_route_probe"
           " --source-rig-json <source.ikrig.json>"
           " --target-rig-json <target.ikrig.json>"
           " --source-alignment-rtg-json <source.ikretargeter.json>"
           " --target-alignment-rtg-json <target.ikretargeter.json>\n";
}
} // namespace

int main(int argc, char** argv)
{
    skrtg::retarget::UEIKJsonCanonicalBridgeLoadOptions Options;
    const std::vector<std::string> Args(argv + 1, argv + argc);
    if (Args.size() == 1 &&
        (Args[0] == "--help" || Args[0] == "-h"))
    {
        PrintHelp();
        return 0;
    }
    auto RequireValue = [&](std::size_t& Index) -> const std::string*
    {
        if (Index + 1 >= Args.size()) return nullptr;
        return &Args[++Index];
    };
    for (std::size_t Index = 0; Index < Args.size(); ++Index)
    {
        const std::string& Arg = Args[Index];
        const std::string* Value = nullptr;
        if (Arg == "--source-rig-json")
        {
            if ((Value = RequireValue(Index)) == nullptr) return 2;
            Options.SourceRigJson = *Value;
        }
        else if (Arg == "--target-rig-json")
        {
            if ((Value = RequireValue(Index)) == nullptr) return 2;
            Options.TargetRigJson = *Value;
        }
        else if (Arg == "--source-alignment-rtg-json")
        {
            if ((Value = RequireValue(Index)) == nullptr) return 2;
            Options.SourceAlignmentRetargeterJson = *Value;
        }
        else if (Arg == "--target-alignment-rtg-json")
        {
            if ((Value = RequireValue(Index)) == nullptr) return 2;
            Options.TargetAlignmentRetargeterJson = *Value;
        }
        else
        {
            std::cerr << "unknown argument: " << Arg << "\n";
            return 2;
        }
    }
    if (Options.SourceRigJson.empty() ||
        Options.TargetRigJson.empty() ||
        Options.SourceAlignmentRetargeterJson.empty() ||
        Options.TargetAlignmentRetargeterJson.empty())
    {
        PrintHelp();
        return 2;
    }

    const auto Result =
        skrtg::retarget::LoadUEIKJsonCanonicalBridgeRoute(Options);
    for (const std::string& Warning : Result.Warnings)
        std::cout << "warning: " << Warning << "\n";
    for (const std::string& Error : Result.Errors)
        std::cerr << "error: " << Error << "\n";
    if (!Result.Success) return 1;

    int FkCount = 0;
    int IkCount = 0;
    std::vector<skrtg::core::math::TransformRT> SourceRebuilt(
        Result.Route.SourceRig.Bones.size());
    std::vector<skrtg::core::math::TransformRT> TargetRebuilt(
        Result.Route.TargetRig.Bones.size());
    for (std::size_t Index = 0;
         Index < SourceRebuilt.size(); ++Index)
    {
        const auto& Bone = Result.Route.SourceRig.Bones[Index];
        SourceRebuilt[Index] = Bone.ParentIndex < 0
            ? Bone.RetargetLocal
            : skrtg::core::math::Compose(
                SourceRebuilt[
                    static_cast<std::size_t>(Bone.ParentIndex)],
                Bone.RetargetLocal);
    }
    for (std::size_t Index = 0;
         Index < TargetRebuilt.size(); ++Index)
    {
        const auto& Bone = Result.Route.TargetRig.Bones[Index];
        TargetRebuilt[Index] = Bone.ParentIndex < 0
            ? Bone.RetargetLocal
            : skrtg::core::math::Compose(
                TargetRebuilt[
                    static_cast<std::size_t>(Bone.ParentIndex)],
                Bone.RetargetLocal);
    }
    for (const auto& Pair : Result.Route.ChainPairs)
    {
        if (Pair.EnableFk) ++FkCount;
        if (Pair.EnableIk) ++IkCount;
        if (Pair.EnableIk)
        {
            const auto ChainLength = [](
                const std::vector<int>& Indices,
                const std::vector<skrtg::core::math::TransformRT>& Pose)
            {
                double Sum = 0.0;
                for (std::size_t Index = 1;
                     Index < Indices.size(); ++Index)
                {
                    Sum += skrtg::core::math::Length(
                        skrtg::core::math::Subtract(
                            Pose[static_cast<std::size_t>(
                                Indices[Index])].TranslationCm,
                            Pose[static_cast<std::size_t>(
                                Indices[Index - 1])].TranslationCm));
                }
                return Sum;
            };
            std::vector<skrtg::core::math::TransformRT>
                SourceExported;
            SourceExported.reserve(
                Result.Route.SourceRig.Bones.size());
            for (const auto& Bone : Result.Route.SourceRig.Bones)
                SourceExported.push_back(Bone.RetargetModel);
            std::vector<skrtg::core::math::TransformRT>
                TargetExported;
            TargetExported.reserve(
                Result.Route.TargetRig.Bones.size());
            for (const auto& Bone : Result.Route.TargetRig.Bones)
                TargetExported.push_back(Bone.RetargetModel);
            std::cout
                << "ik_chain=" << Pair.CanonicalChainName
                << " source_exported_length="
                << ChainLength(
                    Pair.SourceBoneIndices, SourceExported)
                << " source_rebuilt_length="
                << ChainLength(
                    Pair.SourceBoneIndices, SourceRebuilt)
                << " target_exported_length="
                << ChainLength(
                    Pair.TargetBoneIndices, TargetExported)
                << " target_rebuilt_length="
                << ChainLength(
                    Pair.TargetBoneIndices, TargetRebuilt)
                << " stored_scale=" << Pair.LengthScale
                << "\n";
        }
    }
    std::vector<skrtg::core::math::TransformRT>
        SourceRetargetLocal;
    SourceRetargetLocal.reserve(
        Result.Route.SourceRig.Bones.size());
    for (const auto& Bone : Result.Route.SourceRig.Bones)
        SourceRetargetLocal.push_back(Bone.RetargetLocal);
    const auto Baseline =
        skrtg::retarget::SolveUEIKJsonRouteFrame(
            Result.Route, SourceRetargetLocal);
    if (!Baseline.Success)
    {
        for (const std::string& Error : Baseline.Errors)
            std::cerr << "baseline_error=" << Error << "\n";
        return 1;
    }
    double MaximumFkBaselinePositionErrorCm = 0.0;
    double MaximumFoundationBaselinePositionErrorCm = 0.0;
    for (std::size_t Index = 0;
         Index < TargetRebuilt.size(); ++Index)
    {
        MaximumFkBaselinePositionErrorCm = std::max(
            MaximumFkBaselinePositionErrorCm,
            skrtg::core::math::Length(
                skrtg::core::math::Subtract(
                    Baseline.TargetFkModelPose[Index]
                        .TranslationCm,
                    TargetRebuilt[Index].TranslationCm)));
        MaximumFoundationBaselinePositionErrorCm =
            std::max(
                MaximumFoundationBaselinePositionErrorCm,
                skrtg::core::math::Length(
                    skrtg::core::math::Subtract(
                        Baseline.TargetFoundationModelPose[Index]
                            .TranslationCm,
                        TargetRebuilt[Index].TranslationCm)));
    }
    std::cout
        << "baseline_fk_max_position_error_cm="
        << MaximumFkBaselinePositionErrorCm << "\n"
        << "baseline_foundation_max_position_error_cm="
        << MaximumFoundationBaselinePositionErrorCm << "\n"
        << "route_ready=true\n"
        << "route_id=" << Result.Route.RouteId << "\n"
        << "canonical_rig="
        << Result.Route.CanonicalRigObjectPath << "\n"
        << "source_rig="
        << Result.Route.SourceRig.AssetObjectPath << "\n"
        << "target_rig="
        << Result.Route.TargetRig.AssetObjectPath << "\n"
        << "source_pose=" << Result.Route.SourcePoseName << "\n"
        << "target_pose=" << Result.Route.TargetPoseName << "\n"
        << "source_bones="
        << Result.Route.SourceRig.Bones.size() << "\n"
        << "target_bones="
        << Result.Route.TargetRig.Bones.size() << "\n"
        << "mapped_chains="
        << Result.Route.ChainPairs.size() << "\n"
        << "fk_chains=" << FkCount << "\n"
        << "ik_chains=" << IkCount << "\n"
        << "translation_scale="
        << Result.Route.GlobalTranslationScale << "\n";
    return 0;
}
