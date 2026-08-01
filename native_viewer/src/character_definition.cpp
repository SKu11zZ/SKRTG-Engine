#include "skrtg/viewer/profile/character_definition.h"

#include "skrtg/core/math/transform.h"
#include "skrtg/viewer/skrv/sha256.h"

#include <fbxsdk.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace skrtg::viewer::profile
{
namespace
{
using Json = nlohmann::json;
using core::math::Compose;
using core::math::Normalize;
using core::math::Quat;
using core::math::TransformRT;
using core::math::Vec3;

constexpr std::uintmax_t MaximumDefinitionBytes =
    16U * 1024U * 1024U;

std::string PathUtf8(const std::filesystem::path& Path)
{
    const std::u8string Value = Path.generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(Value.data()), Value.size());
}

std::filesystem::path PathFromUtf8(const std::string& Value)
{
    return std::filesystem::path(
        std::u8string(Value.begin(), Value.end()));
}

std::string LowerAscii(std::string Value)
{
    for (char& Character : Value)
    {
        if (Character >= 'A' && Character <= 'Z')
            Character = static_cast<char>(Character - 'A' + 'a');
    }
    return Value;
}

std::string UpperAscii(std::string Value)
{
    for (char& Character : Value)
    {
        if (Character >= 'a' && Character <= 'z')
            Character = static_cast<char>(Character - 'a' + 'A');
    }
    return Value;
}

bool IsSha256(const std::string& Value)
{
    return Value.size() == 64 &&
        std::all_of(
            Value.begin(), Value.end(),
            [](const char Character)
            {
                return
                    (Character >= '0' && Character <= '9') ||
                    (Character >= 'a' && Character <= 'f') ||
                    (Character >= 'A' && Character <= 'F');
            });
}

bool IsRestPoseKind(const std::string& Value)
{
    return Value == "t_pose" || Value == "a_pose" ||
        Value == "custom" || Value == "unknown";
}

bool HasOnlyKeys(
    const Json& Value,
    const std::initializer_list<const char*> Allowed,
    const std::string& Label,
    std::string& OutError)
{
    if (!Value.is_object())
    {
        OutError = Label + " must be an object";
        return false;
    }
    std::set<std::string> Keys;
    for (const char* Key : Allowed) Keys.emplace(Key);
    for (const auto& Item : Value.items())
    {
        if (Keys.find(Item.key()) == Keys.end())
        {
            OutError = Label + " contains unsupported field: " + Item.key();
            return false;
        }
    }
    return true;
}

bool ReadText(
    const std::filesystem::path& Path,
    std::string& Out,
    std::string& OutError)
{
    std::error_code Error;
    if (!std::filesystem::is_regular_file(Path, Error) || Error)
    {
        OutError = "definition is not a readable regular file: " +
            PathUtf8(Path);
        return false;
    }
    const std::uintmax_t Size =
        std::filesystem::file_size(Path, Error);
    if (Error || Size > MaximumDefinitionBytes)
    {
        OutError = "definition exceeds the 16 MiB parser limit";
        return false;
    }
    std::ifstream Input(Path, std::ios::binary);
    if (!Input)
    {
        OutError = "failed to open definition: " + PathUtf8(Path);
        return false;
    }
    Out.assign(
        std::istreambuf_iterator<char>(Input),
        std::istreambuf_iterator<char>());
    return true;
}

bool ParseJson(
    const std::string& Text,
    Json& Out,
    std::string& OutError)
{
    try
    {
        std::size_t Events = 0;
        Out = Json::parse(
            Text,
            [&](const int Depth, const Json::parse_event_t, Json&)
            {
                if (++Events > 1'000'000 || Depth > 128)
                    throw std::length_error("JSON complexity limit exceeded");
                return true;
            },
            true,
            false);
        if (!Out.is_object())
        {
            OutError = "definition JSON root must be an object";
            return false;
        }
        return true;
    }
    catch (const std::exception& Error)
    {
        OutError = std::string("failed to parse definition JSON: ") +
            Error.what();
        return false;
    }
}

std::string HashText(const std::string& Text)
{
    return UpperAscii(skrv::Sha256(
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(Text.data()),
            Text.size())));
}

bool FileHash(
    const std::filesystem::path& Path,
    std::string& Out,
    std::string& OutError)
{
    if (!skrv::Sha256File(Path, Out, OutError)) return false;
    Out = UpperAscii(Out);
    return true;
}

Json UECoordinateContract()
{
    return {
        {"handedness", "left"},
        {"forwardAxis", "+X"},
        {"rightAxis", "+Y"},
        {"upAxis", "+Z"},
        {"distanceUnit", "centimeter"},
        {"quaternionComponentOrder", "x,y,z,w"}};
}

bool IsUECoordinateContract(const Json& Value)
{
    return Value.is_object() &&
        Value.value("handedness", "") == "left" &&
        Value.value("forwardAxis", "") == "+X" &&
        Value.value("rightAxis", "") == "+Y" &&
        Value.value("upAxis", "") == "+Z" &&
        Value.value("distanceUnit", "") == "centimeter" &&
        Value.value("quaternionComponentOrder", "") == "x,y,z,w";
}

bool ParseVector(
    const Json& Value,
    const std::size_t Count,
    std::vector<double>& Out)
{
    if (!Value.is_array() || Value.size() != Count) return false;
    Out.clear();
    for (const Json& Item : Value)
    {
        if (!Item.is_number()) return false;
        const double Number = Item.get<double>();
        if (!std::isfinite(Number)) return false;
        Out.push_back(Number);
    }
    return true;
}

bool ParseTransform(const Json& Value, TransformRT& Out)
{
    std::vector<double> Translation;
    std::vector<double> Rotation;
    std::vector<double> Scale;
    if (!Value.is_object() || Value.size() != 3 ||
        !Value.contains("translationCm") ||
        !Value.contains("rotation") || !Value.contains("scale") ||
        !ParseVector(Value.value("translationCm", Json{}), 3, Translation) ||
        !ParseVector(Value.value("rotation", Json{}), 4, Rotation) ||
        !ParseVector(Value.value("scale", Json{}), 3, Scale))
    {
        return false;
    }
    const double RotationLengthSquared =
        Rotation[0] * Rotation[0] + Rotation[1] * Rotation[1] +
        Rotation[2] * Rotation[2] + Rotation[3] * Rotation[3];
    if (RotationLengthSquared < 1.0e-18 ||
        std::abs(Scale[0]) < 1.0e-12 ||
        std::abs(Scale[1]) < 1.0e-12 ||
        std::abs(Scale[2]) < 1.0e-12)
    {
        return false;
    }
    Out.TranslationCm = {Translation[0], Translation[1], Translation[2]};
    Out.Rotation = Normalize(
        {Rotation[0], Rotation[1], Rotation[2], Rotation[3]});
    Out.Scale = {Scale[0], Scale[1], Scale[2]};
    return true;
}

Json TransformJson(const TransformRT& Value)
{
    return {
        {"translationCm",
         {Value.TranslationCm.X,
          Value.TranslationCm.Y,
          Value.TranslationCm.Z}},
        {"rotation",
         {Value.Rotation.X,
          Value.Rotation.Y,
          Value.Rotation.Z,
          Value.Rotation.W}},
        {"scale", {Value.Scale.X, Value.Scale.Y, Value.Scale.Z}}};
}

bool ValidateAndCanonicalize(
    Json& Definition,
    CharacterDefinitionSummary& Out,
    std::string& OutError)
{
    if (!HasOnlyKeys(
            Definition,
            {"schema", "schemaVersion", "character",
             "coordinateContract", "skeleton", "retarget",
             "provenance", "readiness"},
            "Character Definition", OutError))
        return false;
    if (Definition.value("schema", "") != CharacterDefinitionSchema ||
        Definition.value("schemaVersion", 0) != 1)
    {
        OutError = "unsupported normalized Character Definition schema";
        return false;
    }
    Json Coordinate = Definition.value("coordinateContract", Json::object());
    if (!HasOnlyKeys(
            Coordinate,
            {"handedness", "forwardAxis", "rightAxis", "upAxis",
             "distanceUnit", "quaternionComponentOrder"},
            "Character Definition coordinateContract", OutError))
        return false;
    if (!IsUECoordinateContract(Coordinate))
    {
        OutError =
            "Character Definition must be normalized to left-handed +X forward, +Y right, +Z up, centimeters, quaternion x,y,z,w";
        return false;
    }
    // Importers may accept source-specific descriptive extensions, but the
    // neutral model always emits one exact coordinate contract.
    Definition["coordinateContract"] = UECoordinateContract();

    Json Character = Definition.value("character", Json::object());
    if (!HasOnlyKeys(
            Character,
            {"id", "displayName", "rigAssetName", "restPoseKind"},
            "Character Definition character", OutError))
        return false;
    Out.CharacterId = Character.value("id", "");
    Out.DisplayName = Character.value("displayName", "");
    Out.RigAssetName = Character.value("rigAssetName", "");
    Out.RestPoseKind = Character.value("restPoseKind", Out.RestPoseKind);
    if (!IsRestPoseKind(Out.RestPoseKind))
    {
        OutError = "restPoseKind must be t_pose, a_pose, custom, or unknown";
        return false;
    }
    Character["restPoseKind"] = Out.RestPoseKind;
    Definition["character"] = Character;

    Json Skeleton = Definition.value("skeleton", Json::object());
    Json Bones = Skeleton.value("bones", Json{});
    if (!HasOnlyKeys(
            Skeleton,
            {"fingerprintSha256", "fingerprintKind", "bones"},
            "Character Definition skeleton", OutError))
        return false;
    if (!Bones.is_array() || Bones.empty())
    {
        OutError = "Character Definition skeleton.bones must be non-empty";
        return false;
    }
    if (Bones.size() > 8192)
    {
        OutError = "Character Definition exceeds the 8192 bone limit";
        return false;
    }
    std::set<std::string> Names;
    std::unordered_map<std::string, int> IndexByName;
    std::vector<int> Parents;
    std::vector<TransformRT> Models;
    Parents.reserve(Bones.size());
    Models.reserve(Bones.size());
    for (std::size_t Index = 0; Index < Bones.size(); ++Index)
    {
        Json& Bone = Bones[Index];
        const int BoneIndex = Bone.value("index", -1);
        const int Parent = Bone.value("parentIndex", -2);
        const std::string Name = Bone.value("name", "");
        TransformRT Local;
        if (!HasOnlyKeys(
                Bone, {"index", "parentIndex", "name", "local", "model"},
                "Character Definition bone", OutError) ||
            BoneIndex != static_cast<int>(Index) ||
            Parent < -1 || Parent >= static_cast<int>(Index) ||
            Name.empty() || Name.size() > 256 ||
            !Names.insert(Name).second ||
            !ParseTransform(Bone.value("local", Json{}), Local))
        {
            OutError = "malformed bone at index " + std::to_string(Index);
            return false;
        }
        const TransformRT RebuiltModel = Parent < 0
            ? Local
            : Compose(Models[static_cast<std::size_t>(Parent)], Local);
        TransformRT Model = RebuiltModel;
        if (Bone.contains("model"))
        {
            TransformRT ProvidedModel;
            if (!ParseTransform(Bone.at("model"), ProvidedModel))
            {
                OutError = "malformed model transform at bone: " + Name;
                return false;
            }
            if (!core::math::NearlyEqual(
                    RebuiltModel, ProvidedModel,
                    1.0e-4, 1.0e-6, 1.0e-6))
            {
                OutError =
                    "model transform does not match rebuilt local hierarchy at bone: " +
                    Name;
                return false;
            }
        }
        Bone["local"] = TransformJson(Local);
        Bone["model"] = TransformJson(Model);
        Parents.push_back(Parent);
        Models.push_back(Model);
        IndexByName.emplace(Name, BoneIndex);
    }
    Skeleton["bones"] = Bones;
    std::string Fingerprint = UpperAscii(
        Skeleton.value("fingerprintSha256", ""));
    if (Fingerprint.empty())
    {
        Fingerprint = HashText(Bones.dump());
        Skeleton["fingerprintKind"] =
            "skrtg_canonical_skeleton_v1";
    }
    if (!IsSha256(Fingerprint))
    {
        OutError = "skeleton fingerprintSha256 is invalid";
        return false;
    }
    Skeleton["fingerprintSha256"] = Fingerprint;
    Out.SkeletonSignatureSha256 = Fingerprint;
    Out.SkeletonSignatureKind = Skeleton.value(
        "fingerprintKind", "external_sha256");
    Definition["skeleton"] = Skeleton;

    Json Retarget = Definition.value("retarget", Json::object());
    if (!HasOnlyKeys(
            Retarget, {"rootBone", "pelvisBone", "chains"},
            "Character Definition retarget", OutError))
        return false;
    Out.RetargetRootBone = Retarget.value("rootBone", "");
    Out.RetargetPelvisBone = Retarget.value("pelvisBone", "");
    Json Chains = Retarget.value("chains", Json::array());
    if (!Chains.is_array() || Chains.size() > 4096)
    {
        OutError = "Character Definition retarget.chains is invalid";
        return false;
    }
    std::set<std::string> ChainNames;
    for (std::size_t ChainIndex = 0;
         ChainIndex < Chains.size(); ++ChainIndex)
    {
        Json& Chain = Chains[ChainIndex];
        const std::string Name = Chain.value("name", "");
        const std::string Start = Chain.value("startBone", "");
        const std::string End = Chain.value("endBone", "");
        if (!HasOnlyKeys(
                Chain, {"name", "startBone", "endBone", "ikGoal"},
                "Character Definition chain", OutError) ||
            Name.empty() ||
            !ChainNames.insert(Name).second ||
            IndexByName.find(Start) == IndexByName.end() ||
            IndexByName.find(End) == IndexByName.end())
        {
            OutError = "malformed retarget chain at index " +
                std::to_string(ChainIndex);
            return false;
        }
        int Current = IndexByName.at(End);
        const int StartIndex = IndexByName.at(Start);
        while (Current >= 0 && Current != StartIndex)
            Current = Parents[static_cast<std::size_t>(Current)];
        if (Current != StartIndex)
        {
            OutError = "retarget chain is reversed or disconnected: " + Name;
            return false;
        }
        if (!Chain.contains("ikGoal")) Chain["ikGoal"] = "";
    }
    Retarget["chains"] = Chains;
    Definition["retarget"] = Retarget;

    Out.BoneCount = Bones.size();
    Out.ChainCount = Chains.size();
    Out.MissingRequirements.clear();
    if (Out.RetargetRootBone.empty() ||
        IndexByName.find(Out.RetargetRootBone) == IndexByName.end())
        Out.MissingRequirements.push_back("retarget.rootBone");
    if (Out.RetargetPelvisBone.empty() ||
        IndexByName.find(Out.RetargetPelvisBone) == IndexByName.end())
        Out.MissingRequirements.push_back("retarget.pelvisBone");
    if (Chains.empty())
        Out.MissingRequirements.push_back("retarget.chains");
    if (Out.RigAssetName.empty())
        Out.MissingRequirements.push_back("character.rigAssetName");
    Out.RuntimeDefinitionComplete = Out.MissingRequirements.empty();
    if (Definition.contains("readiness") &&
        !HasOnlyKeys(
            Definition.at("readiness"),
            {"runtimeDefinitionComplete", "missingRequirements"},
            "Character Definition readiness", OutError))
        return false;
    Definition["readiness"] = {
        {"runtimeDefinitionComplete", Out.RuntimeDefinitionComplete},
        {"missingRequirements", Out.MissingRequirements}};
    if (Definition.contains("provenance") &&
        !HasOnlyKeys(
            Definition.at("provenance"),
            {"sourceFormat", "sourceSchema", "sourceFileName",
             "sourceSha256", "adapter", "adapterVersion"},
            "Character Definition provenance", OutError))
        return false;
    return true;
}

Json NormalizeUEIKRig(
    const Json& Source,
    const std::string& RestPoseKind)
{
    const Json Asset = Source.value("asset", Json::object());
    const auto Vector = [](const Json& Value, const bool Quaternion)
    {
        if (Value.is_array()) return Value;
        if (!Value.is_object()) return Json{};
        Json Result = {
            Value.value("x", 0.0),
            Value.value("y", 0.0),
            Value.value("z", 0.0)};
        if (Quaternion) Result.push_back(Value.value("w", 1.0));
        return Result;
    };
    const auto Transform = [&](const Json& Value)
    {
        if (!Value.is_object()) return Json{};
        return Json{
            {"translationCm",
             Vector(
                 Value.contains("translationCm")
                     ? Value.at("translationCm")
                     : Value.value("translation", Json{}),
                 false)},
            {"rotation", Vector(Value.value("rotation", Json{}), true)},
            {"scale", Vector(Value.value("scale", Json{}), false)}};
    };
    Json Bones = Json::array();
    const Json SourceBones = Source.value(
        "referenceSkeleton", Json::object()).value("bones", Json::array());
    if (SourceBones.is_array())
    {
        for (const Json& Bone : SourceBones)
        {
            Bones.push_back({
                {"index", Bone.value("index", -1)},
                {"parentIndex", Bone.value("parentIndex", -2)},
                {"name", Bone.value("name", "")},
                {"local", Transform(Bone.value("local", Json{}))},
                {"model", Transform(Bone.value("model", Json{}))}});
        }
    }
    Json Chains = Json::array();
    const Json SourceChains = Source.value("retargetChains", Json::array());
    if (SourceChains.is_array())
    {
        for (const Json& Chain : SourceChains)
        {
            std::string Goal = Chain.value("ikGoal", "");
            if (Goal == "None") Goal.clear();
            Chains.push_back({
                {"name", Chain.value("name", "")},
                {"startBone", Chain.value("startBone", "")},
                {"endBone", Chain.value("endBone", "")},
                {"ikGoal", Goal}});
        }
    }
    const Json Reference =
        Source.value("referenceSkeleton", Json::object());
    Json Definition = {
        {"schema", CharacterDefinitionSchema},
        {"schemaVersion", 1},
        {"character",
         {{"id", ""},
          {"displayName", Asset.value("assetName", "")},
          {"rigAssetName", Asset.value("assetName", "")},
          {"restPoseKind", RestPoseKind}}},
        {"coordinateContract", UECoordinateContract()},
        {"skeleton",
         {{"fingerprintSha256",
           Reference.value("fingerprintSha256", "")},
          {"fingerprintKind",
           "ue_export_reference_skeleton_sha256"},
          {"bones", Bones}}},
        {"retarget",
         {{"rootBone", Source.value("retargetRootBone", "")},
          {"pelvisBone", Source.value("retargetPelvisBone", "")},
          {"chains", Chains}}}};
    return Definition;
}

struct XmlNode
{
    std::string Name;
    std::map<std::string, std::string> Attributes;
    std::vector<XmlNode> Children;
};

class StrictXmlParser
{
public:
    explicit StrictXmlParser(const std::string& Text) : Text_(Text) {}

    bool Parse(XmlNode& Out, std::string& OutError)
    {
        SkipWhitespace();
        if (StartsWith("<?xml"))
        {
            const std::size_t End = Text_.find("?>", Position_ + 5);
            if (End == std::string::npos)
                return Fail("unterminated XML declaration", OutError);
            Position_ = End + 2;
        }
        SkipWhitespace();
        if (!ParseElement(Out, 0, OutError)) return false;
        SkipWhitespace();
        if (Position_ != Text_.size())
            return Fail("content follows the XML root element", OutError);
        return true;
    }

private:
    bool Fail(const std::string& Message, std::string& OutError)
    {
        OutError = "invalid Character Definition XML at byte " +
            std::to_string(Position_) + ": " + Message;
        return false;
    }

    bool StartsWith(const std::string_view Value) const
    {
        return Text_.compare(Position_, Value.size(), Value) == 0;
    }

    void SkipWhitespace()
    {
        while (Position_ < Text_.size() &&
               (Text_[Position_] == ' ' || Text_[Position_] == '\t' ||
                Text_[Position_] == '\r' || Text_[Position_] == '\n'))
        {
            ++Position_;
        }
    }

    bool ParseName(std::string& Out)
    {
        const std::size_t Begin = Position_;
        while (Position_ < Text_.size())
        {
            const char Character = Text_[Position_];
            const bool Allowed =
                (Character >= 'a' && Character <= 'z') ||
                (Character >= 'A' && Character <= 'Z') ||
                (Character >= '0' && Character <= '9') ||
                Character == '_' || Character == '-' ||
                Character == ':' || Character == '.';
            if (!Allowed) break;
            ++Position_;
        }
        if (Position_ == Begin) return false;
        Out = Text_.substr(Begin, Position_ - Begin);
        return true;
    }

    bool DecodeAttribute(
        const std::string& Encoded,
        std::string& Out,
        std::string& OutError)
    {
        Out.clear();
        for (std::size_t Index = 0; Index < Encoded.size(); ++Index)
        {
            if (Encoded[Index] != '&')
            {
                Out.push_back(Encoded[Index]);
                continue;
            }
            const std::size_t End = Encoded.find(';', Index + 1);
            if (End == std::string::npos)
                return Fail("unterminated XML entity", OutError);
            const std::string Entity =
                Encoded.substr(Index, End - Index + 1);
            if (Entity == "&amp;") Out.push_back('&');
            else if (Entity == "&quot;") Out.push_back('"');
            else if (Entity == "&apos;") Out.push_back('\'');
            else if (Entity == "&lt;") Out.push_back('<');
            else if (Entity == "&gt;") Out.push_back('>');
            else return Fail("unsupported XML entity", OutError);
            Index = End;
        }
        return true;
    }

    bool ParseElement(
        XmlNode& Out,
        const int Depth,
        std::string& OutError)
    {
        if (Depth > 64 || ++NodeCount_ > 100000)
            return Fail("XML complexity limit exceeded", OutError);
        if (Position_ >= Text_.size() || Text_[Position_] != '<')
            return Fail("expected element", OutError);
        if (StartsWith("<!") || StartsWith("<?") || StartsWith("</"))
            return Fail("DTD, comments, processing instructions, and stray closing tags are not allowed", OutError);
        ++Position_;
        if (!ParseName(Out.Name))
            return Fail("element name is missing", OutError);
        while (true)
        {
            SkipWhitespace();
            if (StartsWith("/>"))
            {
                Position_ += 2;
                return true;
            }
            if (StartsWith(">"))
            {
                ++Position_;
                break;
            }
            std::string Name;
            if (!ParseName(Name))
                return Fail("attribute name is missing", OutError);
            SkipWhitespace();
            if (Position_ >= Text_.size() || Text_[Position_] != '=')
                return Fail("attribute equals sign is missing", OutError);
            ++Position_;
            SkipWhitespace();
            if (Position_ >= Text_.size() ||
                (Text_[Position_] != '"' && Text_[Position_] != '\''))
                return Fail("attribute value must be quoted", OutError);
            const char Quote = Text_[Position_++];
            const std::size_t Begin = Position_;
            while (Position_ < Text_.size() && Text_[Position_] != Quote)
            {
                if (Text_[Position_] == '<')
                    return Fail("less-than is not allowed in an attribute", OutError);
                ++Position_;
            }
            if (Position_ >= Text_.size())
                return Fail("unterminated attribute value", OutError);
            std::string Value;
            if (!DecodeAttribute(
                    Text_.substr(Begin, Position_ - Begin),
                    Value, OutError))
                return false;
            ++Position_;
            if (!Out.Attributes.emplace(Name, std::move(Value)).second)
                return Fail("duplicate XML attribute", OutError);
        }
        while (true)
        {
            SkipWhitespace();
            if (StartsWith("</"))
            {
                Position_ += 2;
                std::string Closing;
                if (!ParseName(Closing) || Closing != Out.Name)
                    return Fail("mismatched closing element", OutError);
                SkipWhitespace();
                if (!StartsWith(">"))
                    return Fail("closing element is malformed", OutError);
                ++Position_;
                return true;
            }
            if (Position_ >= Text_.size())
                return Fail("unterminated element", OutError);
            if (Text_[Position_] != '<')
                return Fail("element text is not allowed", OutError);
            XmlNode Child;
            if (!ParseElement(Child, Depth + 1, OutError)) return false;
            Out.Children.push_back(std::move(Child));
        }
    }

    const std::string& Text_;
    std::size_t Position_ = 0;
    std::size_t NodeCount_ = 0;
};

const XmlNode* SingleChild(
    const XmlNode& Parent,
    const std::string& Name,
    std::string& OutError)
{
    const XmlNode* Found = nullptr;
    for (const XmlNode& Child : Parent.Children)
    {
        if (Child.Name != Name) continue;
        if (Found != nullptr)
        {
            OutError = "duplicate XML element: " + Name;
            return nullptr;
        }
        Found = &Child;
    }
    if (Found == nullptr) OutError = "missing XML element: " + Name;
    return Found;
}

std::string Attribute(
    const XmlNode& Node,
    const char* Name,
    const std::string& Default = {})
{
    const auto It = Node.Attributes.find(Name);
    return It == Node.Attributes.end() ? Default : It->second;
}

bool HasOnlyXmlAttributes(
    const XmlNode& Node,
    const std::initializer_list<const char*> Allowed,
    std::string& OutError)
{
    std::set<std::string> Names;
    for (const char* Name : Allowed) Names.emplace(Name);
    for (const auto& [Name, Value] : Node.Attributes)
    {
        (void)Value;
        if (Names.find(Name) == Names.end())
        {
            OutError = "unsupported XML attribute on " + Node.Name +
                ": " + Name;
            return false;
        }
    }
    return true;
}

bool HasOnlyXmlChildren(
    const XmlNode& Node,
    const std::initializer_list<const char*> Allowed,
    std::string& OutError)
{
    std::set<std::string> Names;
    for (const char* Name : Allowed) Names.emplace(Name);
    for (const XmlNode& Child : Node.Children)
    {
        if (Names.find(Child.Name) == Names.end())
        {
            OutError = "unsupported XML element under " + Node.Name +
                ": " + Child.Name;
            return false;
        }
    }
    return true;
}

bool ParseInteger(const std::string& Text, int& Out)
{
    const char* Begin = Text.data();
    const char* End = Begin + Text.size();
    const auto Result = std::from_chars(Begin, End, Out);
    return Result.ec == std::errc{} && Result.ptr == End;
}

template <std::size_t Count>
bool ParseCsv(const std::string& Text, Json& Out)
{
    Out = Json::array();
    std::size_t Begin = 0;
    for (std::size_t Index = 0; Index < Count; ++Index)
    {
        const std::size_t End =
            Index + 1 == Count ? Text.size() : Text.find(',', Begin);
        if (End == std::string::npos || End == Begin) return false;
        try
        {
            std::size_t Parsed = 0;
            const double Value = std::stod(Text.substr(Begin, End - Begin), &Parsed);
            if (Parsed != End - Begin || !std::isfinite(Value)) return false;
            Out.push_back(Value);
        }
        catch (...)
        {
            return false;
        }
        Begin = End + 1;
    }
    return Begin == Text.size() + 1;
}

bool TransformFromXml(
    const XmlNode& Bone,
    const std::string& Prefix,
    Json& Out,
    const bool Required,
    std::string& OutError)
{
    const std::string Translation =
        Attribute(Bone, (Prefix + "TranslationCm").c_str());
    const std::string Rotation =
        Attribute(Bone, (Prefix + "Rotation").c_str());
    const std::string Scale =
        Attribute(Bone, (Prefix + "Scale").c_str());
    if (!Required && Translation.empty() && Rotation.empty() && Scale.empty())
        return true;
    Json TranslationJson;
    Json RotationJson;
    Json ScaleJson;
    if (!ParseCsv<3>(Translation, TranslationJson) ||
        !ParseCsv<4>(Rotation, RotationJson) ||
        !ParseCsv<3>(Scale, ScaleJson))
    {
        OutError = "invalid " + Prefix + " transform on XML Bone";
        return false;
    }
    Out = {
        {"translationCm", TranslationJson},
        {"rotation", RotationJson},
        {"scale", ScaleJson}};
    return true;
}

bool NormalizeXml(
    const std::string& Text,
    const std::string& RestPoseKind,
    Json& Out,
    std::string& OutError)
{
    XmlNode Root;
    StrictXmlParser Parser(Text);
    if (!Parser.Parse(Root, OutError)) return false;
    if (Root.Name != "CharacterDefinition" ||
        Attribute(Root, "schema") != CharacterDefinitionSchema ||
        Attribute(Root, "schemaVersion") != "1")
    {
        OutError = "unsupported Character Definition XML root/schema";
        return false;
    }
    if (!HasOnlyXmlAttributes(
            Root, {"schema", "schemaVersion"}, OutError) ||
        !HasOnlyXmlChildren(
            Root, {"Character", "Coordinate", "Skeleton", "Retarget"},
            OutError))
        return false;
    const XmlNode* Character = SingleChild(Root, "Character", OutError);
    if (Character == nullptr) return false;
    const XmlNode* Coordinate = SingleChild(Root, "Coordinate", OutError);
    if (Coordinate == nullptr) return false;
    const XmlNode* Skeleton = SingleChild(Root, "Skeleton", OutError);
    if (Skeleton == nullptr) return false;
    const XmlNode* Retarget = SingleChild(Root, "Retarget", OutError);
    if (Retarget == nullptr) return false;
    if (!Character->Children.empty() || !Coordinate->Children.empty() ||
        !HasOnlyXmlAttributes(
            *Character,
            {"id", "displayName", "rigAssetName", "restPoseKind"},
            OutError) ||
        !HasOnlyXmlAttributes(
            *Coordinate,
            {"handedness", "forwardAxis", "rightAxis", "upAxis",
             "distanceUnit", "quaternionComponentOrder"},
            OutError) ||
        !HasOnlyXmlAttributes(
            *Skeleton, {"fingerprintSha256", "fingerprintKind"},
            OutError) ||
        !HasOnlyXmlChildren(*Skeleton, {"Bone"}, OutError) ||
        !HasOnlyXmlAttributes(
            *Retarget, {"rootBone", "pelvisBone"}, OutError) ||
        !HasOnlyXmlChildren(*Retarget, {"Chain"}, OutError))
    {
        if (OutError.empty())
            OutError = "Character and Coordinate XML elements must be empty";
        return false;
    }

    Json Bones = Json::array();
    for (const XmlNode& Bone : Skeleton->Children)
    {
        if (Bone.Name != "Bone")
        {
            OutError = "only Bone elements are allowed under Skeleton";
            return false;
        }
        if (!Bone.Children.empty() ||
            !HasOnlyXmlAttributes(
                Bone,
                {"index", "parentIndex", "name",
                 "localTranslationCm", "localRotation", "localScale",
                 "modelTranslationCm", "modelRotation", "modelScale"},
                OutError))
        {
            if (OutError.empty())
                OutError = "XML Bone elements must be empty";
            return false;
        }
        int Index = -1;
        int ParentIndex = -2;
        if (!ParseInteger(Attribute(Bone, "index"), Index) ||
            !ParseInteger(Attribute(Bone, "parentIndex"), ParentIndex))
        {
            OutError = "XML Bone index or parentIndex is invalid";
            return false;
        }
        Json Local;
        Json Model;
        if (!TransformFromXml(Bone, "local", Local, true, OutError) ||
            !TransformFromXml(Bone, "model", Model, false, OutError))
            return false;
        Json Item = {
            {"index", Index},
            {"parentIndex", ParentIndex},
            {"name", Attribute(Bone, "name")},
            {"local", Local}};
        if (!Model.is_null()) Item["model"] = Model;
        Bones.push_back(std::move(Item));
    }

    Json Chains = Json::array();
    for (const XmlNode& Chain : Retarget->Children)
    {
        if (Chain.Name != "Chain")
        {
            OutError = "only Chain elements are allowed under Retarget";
            return false;
        }
        if (!Chain.Children.empty() ||
            !HasOnlyXmlAttributes(
                Chain, {"name", "startBone", "endBone", "ikGoal"},
                OutError))
        {
            if (OutError.empty())
                OutError = "XML Chain elements must be empty";
            return false;
        }
        Chains.push_back({
            {"name", Attribute(Chain, "name")},
            {"startBone", Attribute(Chain, "startBone")},
            {"endBone", Attribute(Chain, "endBone")},
            {"ikGoal", Attribute(Chain, "ikGoal")}});
    }
    const std::string DeclaredPose = Attribute(
        *Character, "restPoseKind", RestPoseKind);
    Out = {
        {"schema", CharacterDefinitionSchema},
        {"schemaVersion", 1},
        {"character",
         {{"id", Attribute(*Character, "id")},
          {"displayName", Attribute(*Character, "displayName")},
          {"rigAssetName", Attribute(*Character, "rigAssetName")},
          {"restPoseKind", DeclaredPose}}},
        {"coordinateContract",
         {{"handedness", Attribute(*Coordinate, "handedness")},
          {"forwardAxis", Attribute(*Coordinate, "forwardAxis")},
          {"rightAxis", Attribute(*Coordinate, "rightAxis")},
          {"upAxis", Attribute(*Coordinate, "upAxis")},
          {"distanceUnit", Attribute(*Coordinate, "distanceUnit")},
          {"quaternionComponentOrder",
           Attribute(*Coordinate, "quaternionComponentOrder")}}},
        {"skeleton",
         {{"fingerprintSha256",
           Attribute(*Skeleton, "fingerprintSha256")},
          {"fingerprintKind",
           Attribute(*Skeleton, "fingerprintKind",
                     "external_sha256")},
          {"bones", Bones}}},
        {"retarget",
         {{"rootBone", Attribute(*Retarget, "rootBone")},
          {"pelvisBone", Attribute(*Retarget, "pelvisBone")},
          {"chains", Chains}}}};
    if (Out["skeleton"]["fingerprintSha256"].get<std::string>().empty())
        Out["skeleton"].erase("fingerprintSha256");
    return true;
}

std::string CanonicalBoneName(const std::string& Name)
{
    const std::size_t Colon = Name.find_last_of(':');
    return Colon == std::string::npos ? Name : Name.substr(Colon + 1);
}

void CollectNodes(FbxNode* Node, std::vector<FbxNode*>& Out)
{
    if (Node == nullptr) return;
    Out.push_back(Node);
    for (int Index = 0; Index < Node->GetChildCount(); ++Index)
        CollectNodes(Node->GetChild(Index), Out);
}

void CollectSkinLinks(FbxNode* Node, std::set<FbxNode*>& Out)
{
    if (Node == nullptr) return;
    if (FbxMesh* Mesh = Node->GetMesh())
    {
        const int SkinCount = Mesh->GetDeformerCount(FbxDeformer::eSkin);
        for (int SkinIndex = 0; SkinIndex < SkinCount; ++SkinIndex)
        {
            FbxSkin* Skin = FbxCast<FbxSkin>(
                Mesh->GetDeformer(SkinIndex, FbxDeformer::eSkin));
            if (Skin == nullptr) continue;
            for (int ClusterIndex = 0;
                 ClusterIndex < Skin->GetClusterCount(); ++ClusterIndex)
            {
                FbxCluster* Cluster = Skin->GetCluster(ClusterIndex);
                if (Cluster != nullptr && Cluster->GetLink() != nullptr)
                    Out.insert(Cluster->GetLink());
            }
        }
    }
    for (int Index = 0; Index < Node->GetChildCount(); ++Index)
        CollectSkinLinks(Node->GetChild(Index), Out);
}

bool IsSkeletonNode(FbxNode* Node)
{
    const FbxNodeAttribute* Attribute =
        Node != nullptr ? Node->GetNodeAttribute() : nullptr;
    return Attribute != nullptr &&
        Attribute->GetAttributeType() == FbxNodeAttribute::eSkeleton;
}

TransformRT ConvertFbxTransformToUE(const FbxAMatrix& Matrix)
{
    const FbxVector4 Translation = Matrix.GetT();
    const FbxQuaternion Rotation = Matrix.GetQ();
    const FbxVector4 Scale = Matrix.GetS();
    TransformRT Result;
    Result.TranslationCm = {Translation[0], -Translation[1], Translation[2]};
    Result.Rotation = Normalize(
        {Rotation[0], -Rotation[1], Rotation[2], -Rotation[3]});
    Result.Scale = {Scale[0], Scale[1], Scale[2]};
    return Result;
}

struct FbxSceneOwner
{
    FbxManager* Manager = nullptr;
    FbxScene* Scene = nullptr;
    ~FbxSceneOwner()
    {
        if (Manager != nullptr) Manager->Destroy();
    }
};

bool NormalizeRestFbx(
    const std::filesystem::path& Path,
    const std::string& RestPoseKind,
    Json& Out,
    std::string& OutError)
{
    FbxSceneOwner Owner;
    Owner.Manager = FbxManager::Create();
    if (Owner.Manager == nullptr)
    {
        OutError = "FbxManager::Create failed";
        return false;
    }
    FbxIOSettings* Settings = FbxIOSettings::Create(Owner.Manager, IOSROOT);
    Owner.Manager->SetIOSettings(Settings);
    FbxImporter* Importer = FbxImporter::Create(Owner.Manager, "");
    const std::string NativePath = Path.string();
    if (Importer == nullptr ||
        !Importer->Initialize(
            NativePath.c_str(), -1, Owner.Manager->GetIOSettings()))
    {
        OutError = Importer != nullptr
            ? std::string("FBX importer initialize failed: ") +
                Importer->GetStatus().GetErrorString()
            : "FbxImporter::Create failed";
        if (Importer != nullptr) Importer->Destroy();
        return false;
    }
    Owner.Scene = FbxScene::Create(Owner.Manager, "SKRTG_ProfileProbe");
    if (Owner.Scene == nullptr || !Importer->Import(Owner.Scene))
    {
        OutError = Owner.Scene != nullptr
            ? std::string("FBX import failed: ") +
                Importer->GetStatus().GetErrorString()
            : "FbxScene::Create failed";
        Importer->Destroy();
        return false;
    }
    Importer->Destroy();

    const FbxAxisSystem OriginalAxis =
        Owner.Scene->GetGlobalSettings().GetAxisSystem();
    const auto Front = static_cast<FbxAxisSystem::EFrontVector>(
        -FbxAxisSystem::eParityOdd);
    const FbxAxisSystem UEImportAxis(
        FbxAxisSystem::eZAxis, Front, FbxAxisSystem::eRightHanded);
    if (OriginalAxis != UEImportAxis)
    {
        FbxRootNodeUtility::RemoveAllFbxRoots(Owner.Scene);
        UEImportAxis.ConvertScene(Owner.Scene);
    }
    if (Owner.Scene->GetGlobalSettings().GetSystemUnit() != FbxSystemUnit::cm)
        FbxSystemUnit::cm.ConvertScene(Owner.Scene);
    Owner.Scene->GetAnimationEvaluator()->Reset();

    std::vector<FbxNode*> AllNodes;
    CollectNodes(Owner.Scene->GetRootNode(), AllNodes);
    std::set<FbxNode*> SkinLinks;
    CollectSkinLinks(Owner.Scene->GetRootNode(), SkinLinks);
    std::vector<FbxNode*> Bones;
    for (FbxNode* Node : AllNodes)
    {
        if (IsSkeletonNode(Node) || SkinLinks.find(Node) != SkinLinks.end())
            Bones.push_back(Node);
    }
    if (Bones.empty())
    {
        OutError = "FBX contains no skeleton or skin-linked bone nodes";
        return false;
    }
    if (Bones.size() > 8192)
    {
        OutError = "FBX skeleton exceeds the 8192 bone limit";
        return false;
    }
    std::unordered_map<FbxNode*, int> BoneIndices;
    std::set<std::string> Names;
    for (std::size_t Index = 0; Index < Bones.size(); ++Index)
    {
        const std::string Name = CanonicalBoneName(
            Bones[Index]->GetName() != nullptr ? Bones[Index]->GetName() : "");
        if (Name.empty() || !Names.insert(Name).second)
        {
            OutError = "FBX contains an empty or namespace-ambiguous bone name: " + Name;
            return false;
        }
        BoneIndices.emplace(Bones[Index], static_cast<int>(Index));
    }
    FbxTime Time;
    Time.SetSecondDouble(0.0);
    Json BoneJson = Json::array();
    for (std::size_t Index = 0; Index < Bones.size(); ++Index)
    {
        FbxNode* Node = Bones[Index];
        FbxNode* Parent = Node->GetParent();
        int ParentIndex = -1;
        while (Parent != nullptr)
        {
            const auto Found = BoneIndices.find(Parent);
            if (Found != BoneIndices.end())
            {
                ParentIndex = Found->second;
                break;
            }
            Parent = Parent->GetParent();
        }
        const FbxAMatrix ModelMatrix = Node->EvaluateGlobalTransform(Time);
        const FbxAMatrix LocalMatrix = ParentIndex < 0
            ? ModelMatrix
            : Bones[static_cast<std::size_t>(ParentIndex)]
                  ->EvaluateGlobalTransform(Time)
                  .Inverse() * ModelMatrix;
        BoneJson.push_back({
            {"index", Index},
            {"parentIndex", ParentIndex},
            {"name", CanonicalBoneName(Node->GetName())},
            {"local", TransformJson(ConvertFbxTransformToUE(LocalMatrix))},
            {"model", TransformJson(ConvertFbxTransformToUE(ModelMatrix))}});
    }
    const std::string Stem = PathUtf8(Path.stem());
    Out = {
        {"schema", CharacterDefinitionSchema},
        {"schemaVersion", 1},
        {"character",
         {{"id", ""},
          {"displayName", Stem},
          {"rigAssetName", ""},
          {"restPoseKind", RestPoseKind}}},
        {"coordinateContract", UECoordinateContract()},
        {"skeleton", {{"bones", BoneJson}}},
        {"retarget",
         {{"rootBone", ""}, {"pelvisBone", ""},
          {"chains", Json::array()}}}};
    return true;
}

bool WriteTextAtomic(
    const std::filesystem::path& Path,
    const std::string& Text,
    std::string& OutError)
{
    std::error_code Error;
    if (std::filesystem::exists(Path, Error) || Error)
    {
        OutError = "output already exists or cannot be checked: " +
            PathUtf8(Path);
        return false;
    }
    std::filesystem::create_directories(Path.parent_path(), Error);
    if (Error)
    {
        OutError = "failed to create output directory: " + Error.message();
        return false;
    }
    std::filesystem::path Partial = Path;
    Partial += ".partial";
    std::filesystem::remove(Partial, Error);
    Error.clear();
    {
        std::ofstream Output(Partial, std::ios::binary | std::ios::trunc);
        if (!Output)
        {
            OutError = "failed to open partial output";
            return false;
        }
        Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
        if (!Output)
        {
            OutError = "failed to write complete output";
            Output.close();
            std::filesystem::remove(Partial, Error);
            return false;
        }
    }
    std::filesystem::rename(Partial, Path, Error);
    if (Error)
    {
        std::filesystem::remove(Partial, Error);
        OutError = "failed to commit output";
        return false;
    }
    return true;
}

std::filesystem::path ResolveRequestPath(
    const std::filesystem::path& Base,
    const std::string& Text)
{
    const std::filesystem::path Value = PathFromUtf8(Text);
    return std::filesystem::absolute(
        Value.is_absolute() ? Value : Base / Value).lexically_normal();
}

Json BuildUEIKRig(
    const Json& Definition,
    const CharacterProfileCreateRequest& Request)
{
    const Json Character = Definition.at("character");
    const Json Skeleton = Definition.at("skeleton");
    const Json Retarget = Definition.at("retarget");
    const std::string AssetName = Character.value(
        "rigAssetName", "IK_" + Request.ProfileId);
    return {
        {"schema", "skrtg.ue_ik_asset_export.v2"},
        {"schemaVersion", 2},
        {"kind", "ikRigDefinition"},
        {"valid", true},
        {"unrealEngineVersion", "external_definition"},
        {"coordinateContract", Definition.at("coordinateContract")},
        {"asset",
         {{"assetName", AssetName},
          {"objectPath", "skrtg://profiles/" + Request.ProfileId +
              "/" + AssetName}}},
        {"retargetRootBone", Retarget.at("rootBone")},
        {"retargetPelvisBone", Retarget.at("pelvisBone")},
        {"referenceSkeleton", Skeleton},
        {"retargetChains", Retarget.at("chains")}};
}

struct TemporaryDirectory
{
    std::filesystem::path Path;
    TemporaryDirectory()
    {
        const auto Stamp = std::chrono::high_resolution_clock::now()
            .time_since_epoch().count();
        Path = std::filesystem::temp_directory_path() /
            ("skrtg_profile_create_" + std::to_string(Stamp));
        std::filesystem::create_directories(Path);
    }
    ~TemporaryDirectory()
    {
        std::error_code Error;
        std::filesystem::remove_all(Path, Error);
    }
};
} // namespace

const char* CharacterDefinitionFormatName(
    const CharacterDefinitionFormat Format)
{
    switch (Format)
    {
    case CharacterDefinitionFormat::Auto: return "auto";
    case CharacterDefinitionFormat::UEIKRigJson: return "ue_ik_rig_json";
    case CharacterDefinitionFormat::SKRTGCharacterJson:
        return "skrtg_character_json";
    case CharacterDefinitionFormat::SKRTGCharacterXml:
        return "skrtg_character_xml";
    case CharacterDefinitionFormat::RestFbx: return "rest_fbx";
    }
    return "unknown";
}

bool ParseCharacterDefinitionFormat(
    const std::string& Text,
    CharacterDefinitionFormat& OutFormat)
{
    const std::string Value = LowerAscii(Text);
    if (Value == "auto") OutFormat = CharacterDefinitionFormat::Auto;
    else if (Value == "ue_ik_rig_json" || Value == "ue-json")
        OutFormat = CharacterDefinitionFormat::UEIKRigJson;
    else if (Value == "skrtg_character_json" || Value == "json")
        OutFormat = CharacterDefinitionFormat::SKRTGCharacterJson;
    else if (Value == "skrtg_character_xml" || Value == "xml")
        OutFormat = CharacterDefinitionFormat::SKRTGCharacterXml;
    else if (Value == "rest_fbx" || Value == "fbx")
        OutFormat = CharacterDefinitionFormat::RestFbx;
    else return false;
    return true;
}

std::vector<std::string> SupportedCharacterDefinitionAdapters()
{
    return {
        "ue_ik_rig_json.v1 (UE exporter JSON v1/v2)",
        "skrtg_character_json.v1",
        "skrtg_character_xml.v1",
        "rest_fbx_probe.v1 (normalization only; mapping required)"};
}

CharacterDefinitionInspectResult InspectCharacterDefinition(
    const std::filesystem::path& SourcePath,
    const CharacterDefinitionInspectOptions& Options) try
{
    CharacterDefinitionInspectResult Result;
    Result.SourcePath = std::filesystem::absolute(SourcePath).lexically_normal();
    if (!IsRestPoseKind(Options.RestPoseKind))
    {
        Result.Errors.push_back(
            "restPoseKind must be t_pose, a_pose, custom, or unknown");
        return Result;
    }
    std::string HashError;
    if (!FileHash(Result.SourcePath, Result.Definition.InputSha256, HashError))
    {
        Result.Errors.push_back(HashError);
        return Result;
    }

    CharacterDefinitionFormat Format = Options.Format;
    const std::string Extension = LowerAscii(
        PathUtf8(Result.SourcePath.extension()));
    if (Format == CharacterDefinitionFormat::Auto)
    {
        if (Extension == ".fbx") Format = CharacterDefinitionFormat::RestFbx;
        else if (Extension == ".xml")
            Format = CharacterDefinitionFormat::SKRTGCharacterXml;
        else if (Extension != ".json")
        {
            Result.Errors.push_back(
                "unsupported Character Definition extension; use --format only for a registered adapter");
            return Result;
        }
    }

    Json Normalized;
    std::string Error;
    if (Format == CharacterDefinitionFormat::RestFbx)
    {
        Result.Definition.SourceFormat = "rest_fbx";
        Result.Definition.SourceSchema = "fbx_scene";
        Result.Definition.AdapterId = "rest_fbx_probe";
        Result.Definition.AdapterVersion = "1";
        Result.Definition.RestPoseKind = Options.RestPoseKind;
        if (!NormalizeRestFbx(
                Result.SourcePath, Options.RestPoseKind,
                Normalized, Error))
        {
            Result.Errors.push_back(Error);
            return Result;
        }
        Result.Warnings.push_back(
            "FBX supplies a rest skeleton, not retarget semantics; root, pelvis, chains, and alignment must be authored explicitly");
    }
    else
    {
        std::string Text;
        if (!ReadText(Result.SourcePath, Text, Error))
        {
            Result.Errors.push_back(Error);
            return Result;
        }
        if (Format == CharacterDefinitionFormat::SKRTGCharacterXml)
        {
            Result.Definition.SourceFormat = "skrtg_character_xml";
            Result.Definition.SourceSchema = CharacterDefinitionSchema;
            Result.Definition.AdapterId = "skrtg_character_xml";
            Result.Definition.AdapterVersion = "1";
            Result.Definition.RestPoseKind = Options.RestPoseKind;
            if (!NormalizeXml(Text, Options.RestPoseKind, Normalized, Error))
            {
                Result.Errors.push_back(Error);
                return Result;
            }
        }
        else
        {
            Json Source;
            if (!ParseJson(Text, Source, Error))
            {
                Result.Errors.push_back(Error);
                return Result;
            }
            const bool IsUE =
                (Source.value("schema", "") ==
                     "skrtg.ue_ik_asset_export.v1" ||
                 Source.value("schema", "") ==
                     "skrtg.ue_ik_asset_export.v2") &&
                Source.value("kind", "") == "ikRigDefinition";
            const bool IsSKRTG =
                Source.value("schema", "") == CharacterDefinitionSchema &&
                Source.value("schemaVersion", 0) == 1;
            if (Options.Format == CharacterDefinitionFormat::Auto)
                Format = IsUE
                    ? CharacterDefinitionFormat::UEIKRigJson
                    : CharacterDefinitionFormat::SKRTGCharacterJson;
            if (Format == CharacterDefinitionFormat::UEIKRigJson && !IsUE)
            {
                Result.Errors.push_back(
                    "JSON does not identify a supported UE IK Rig export");
                return Result;
            }
            if (Format == CharacterDefinitionFormat::SKRTGCharacterJson &&
                !IsSKRTG)
            {
                Result.Errors.push_back(
                    "JSON does not identify skrtg.character_definition.v1");
                return Result;
            }
            if (IsUE)
            {
                Result.Definition.SourceFormat = "ue_ik_rig_json";
                Result.Definition.SourceSchema = Source.value("schema", "");
                Result.Definition.AdapterId = "ue_ik_rig_json";
                Result.Definition.AdapterVersion = "1";
                Result.Definition.RestPoseKind = Options.RestPoseKind;
                Normalized = NormalizeUEIKRig(Source, Options.RestPoseKind);
            }
            else
            {
                Result.Definition.SourceFormat = "skrtg_character_json";
                Result.Definition.SourceSchema = CharacterDefinitionSchema;
                Result.Definition.AdapterId = "skrtg_character_json";
                Result.Definition.AdapterVersion = "1";
                Result.Definition.RestPoseKind = Options.RestPoseKind;
                Normalized = Source;
                if (Options.RestPoseKind != "unknown")
                    Normalized["character"]["restPoseKind"] =
                        Options.RestPoseKind;
            }
        }
    }

    Normalized["provenance"] = {
        {"sourceFormat", Result.Definition.SourceFormat},
        {"sourceSchema", Result.Definition.SourceSchema},
        {"sourceFileName", PathUtf8(Result.SourcePath.filename())},
        {"sourceSha256", Result.Definition.InputSha256},
        {"adapter", Result.Definition.AdapterId},
        {"adapterVersion", Result.Definition.AdapterVersion}};
    if (!ValidateAndCanonicalize(Normalized, Result.Definition, Error))
    {
        Result.Errors.push_back(Error);
        return Result;
    }
    Result.NormalizedJson = Normalized.dump(2) + "\n";
    Result.Success = true;
    return Result;
}
catch (const std::exception& Error)
{
    CharacterDefinitionInspectResult Result;
    Result.SourcePath = std::filesystem::absolute(SourcePath).lexically_normal();
    Result.Errors.push_back(
        std::string("Character Definition inspection failed: ") + Error.what());
    return Result;
}

bool WriteNormalizedCharacterDefinition(
    const CharacterDefinitionInspectResult& Definition,
    const std::filesystem::path& OutputJson,
    std::string& OutError)
{
    if (!Definition.Success || Definition.NormalizedJson.empty())
    {
        OutError = "only a successfully inspected definition can be written";
        return false;
    }
    if (LowerAscii(PathUtf8(OutputJson.extension())) != ".json")
    {
        OutError = "normalized Character Definition output must use .json";
        return false;
    }
    return WriteTextAtomic(
        std::filesystem::absolute(OutputJson).lexically_normal(),
        Definition.NormalizedJson, OutError);
}

bool ReadCharacterProfileCreateRequest(
    const std::filesystem::path& RequestJson,
    CharacterProfileCreateRequest& OutRequest,
    std::string& OutError)
{
    std::string Text;
    if (!ReadText(RequestJson, Text, OutError)) return false;
    Json Root;
    if (!ParseJson(Text, Root, OutError)) return false;
    if (!HasOnlyKeys(
            Root,
            {"schema", "schemaVersion", "profile", "inputs", "output"},
            "Profile create request", OutError))
        return false;
    if (Root.value("schema", "") != ProfileCreateRequestSchema ||
        Root.value("schemaVersion", 0) != 1)
    {
        OutError = "unsupported profile create request schema";
        return false;
    }
    const Json Profile = Root.value("profile", Json::object());
    const Json Inputs = Root.value("inputs", Json::object());
    const Json Output = Root.value("output", Json::object());
    if (!HasOnlyKeys(
            Profile,
            {"id", "version", "displayName", "canonicalProfileId",
             "restPoseKind", "sourceEnabled", "targetEnabled"},
            "Profile create request profile", OutError) ||
        !HasOnlyKeys(
            Inputs,
            {"restFbx", "definition", "alignmentRetargeterJson", "format"},
            "Profile create request inputs", OutError) ||
        !HasOnlyKeys(
            Output, {"package"},
            "Profile create request output", OutError))
        return false;
    const std::filesystem::path AbsoluteRequest =
        std::filesystem::absolute(RequestJson).lexically_normal();
    const std::filesystem::path Base = AbsoluteRequest.parent_path();
    OutRequest = {};
    OutRequest.RequestFile = AbsoluteRequest;
    OutRequest.ProfileId = Profile.value("id", "");
    OutRequest.ProfileVersion = Profile.value("version", "");
    OutRequest.DisplayName = Profile.value("displayName", "");
    OutRequest.CanonicalProfileId =
        Profile.value("canonicalProfileId", "ue5_manny");
    OutRequest.RestPoseKind = Profile.value("restPoseKind", "unknown");
    OutRequest.SourceEnabled = Profile.value("sourceEnabled", true);
    OutRequest.TargetEnabled = Profile.value("targetEnabled", true);
    const std::string RestFbxText = Inputs.value("restFbx", "");
    const std::string DefinitionText = Inputs.value("definition", "");
    const std::string AlignmentText =
        Inputs.value("alignmentRetargeterJson", "");
    const std::string PackageText = Output.value("package", "");
    if (!IsCharacterProfileId(OutRequest.ProfileId) ||
        !IsCharacterProfileVersion(OutRequest.ProfileVersion) ||
        OutRequest.DisplayName.empty() ||
        OutRequest.DisplayName.size() > 256 ||
        !IsCharacterProfileId(OutRequest.CanonicalProfileId) ||
        !IsRestPoseKind(OutRequest.RestPoseKind) ||
        (!OutRequest.SourceEnabled && !OutRequest.TargetEnabled) ||
        RestFbxText.empty() || DefinitionText.empty() ||
        AlignmentText.empty() || PackageText.empty())
    {
        OutError = "profile create request identity, role, pose, or path is invalid";
        return false;
    }
    OutRequest.RestFbx = ResolveRequestPath(
        Base, RestFbxText);
    OutRequest.DefinitionFile = ResolveRequestPath(
        Base, DefinitionText);
    OutRequest.AlignmentRetargeterJson = ResolveRequestPath(
        Base, AlignmentText);
    OutRequest.OutputPackage = ResolveRequestPath(
        Base, PackageText);
    if (!ParseCharacterDefinitionFormat(
            Inputs.value("format", "auto"), OutRequest.Format))
    {
        OutError = "unknown Character Definition format hint";
        return false;
    }
    return true;
}

CharacterProfileCreateResult CreateCharacterProfile(
    const CharacterProfileCreateRequest& Request)
{
    CharacterProfileCreateResult Result;
    Result.Stage = "definition";
    CharacterDefinitionInspectOptions Options;
    Options.Format = Request.Format;
    Options.RestPoseKind = Request.RestPoseKind;
    const CharacterDefinitionInspectResult Definition =
        InspectCharacterDefinition(Request.DefinitionFile, Options);
    Result.Definition = Definition.Definition;
    Result.Warnings = Definition.Warnings;
    if (!Definition.Success)
    {
        Result.Errors = Definition.Errors;
        return Result;
    }
    if (!Definition.Definition.RuntimeDefinitionComplete)
    {
        Result.Errors.push_back(
            "Character Definition is not runtime-complete; provide every reported missing requirement before packaging");
        for (const std::string& Missing :
             Definition.Definition.MissingRequirements)
            Result.Errors.push_back("missing: " + Missing);
        return Result;
    }

    std::filesystem::path RigPath = Request.DefinitionFile;
    TemporaryDirectory Temporary;
    if (Definition.Definition.SourceFormat != "ue_ik_rig_json")
    {
        Result.Stage = "compile_definition";
        Json Normalized;
        std::string Error;
        if (!ParseJson(Definition.NormalizedJson, Normalized, Error))
        {
            Result.Errors.push_back(Error);
            return Result;
        }
        RigPath = Temporary.Path / "compiled_ik_rig.json";
        std::ofstream Output(RigPath, std::ios::binary | std::ios::trunc);
        const std::string Bytes = BuildUEIKRig(Normalized, Request).dump(2) + "\n";
        Output.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
        if (!Output)
        {
            Result.Errors.push_back("failed to write compiled IK Rig JSON");
            return Result;
        }
    }

    Result.Stage = "package";
    ProfilePackRequest Pack;
    Pack.OutputPackage = Request.OutputPackage;
    Pack.ProfileId = Request.ProfileId;
    Pack.ProfileVersion = Request.ProfileVersion;
    Pack.DisplayName = Request.DisplayName;
    Pack.CanonicalProfileId = Request.CanonicalProfileId;
    Pack.RestFbx = Request.RestFbx;
    Pack.IkRigJson = RigPath;
    Pack.AlignmentRetargeterJson = Request.AlignmentRetargeterJson;
    Pack.SourceEnabled = Request.SourceEnabled;
    Pack.TargetEnabled = Request.TargetEnabled;
    Pack.SourceDefinitionFormat = Definition.Definition.SourceFormat;
    Pack.SourceDefinitionSha256 = Definition.Definition.InputSha256;
    Pack.DefinitionImporter = Definition.Definition.AdapterId;
    Pack.DefinitionImporterVersion = Definition.Definition.AdapterVersion;
    Pack.RestPoseKind = Definition.Definition.RestPoseKind;
    Result.Package = WriteCharacterProfilePackage(Pack);
    if (!Result.Package.Success)
    {
        Result.Errors = Result.Package.Errors;
        return Result;
    }
    Result.Success = true;
    Result.Stage = "complete";
    return Result;
}
} // namespace skrtg::viewer::profile
