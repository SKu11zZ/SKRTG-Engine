#include "skrtg/viewer/profile/character_profile.h"

#include "skrtg/viewer/skrv/sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace skrtg::viewer::profile
{
namespace
{
using Json = nlohmann::json;

constexpr std::array<char, 16> PackageMagic = {
    'S', 'K', 'R', 'T', 'G', 'P', 'R', 'O',
    'F', 'I', 'L', 'E', 'V', '1', '\r', '\n'};
constexpr const char* PackageSchema =
    "skrtg.character_profile_package.v1";
constexpr const char* ProfileSchema =
    "skrtg.character_profile.v1";
constexpr const char* InstallSchema =
    "skrtg.character_profile_install.v1";
constexpr const char* IntegrityMagic =
    "SKRTGPROFILE_INTEGRITY_V1";
constexpr const char* IntegrityHeader =
    "role\tpath\tbyte_count\tsha256";
constexpr const char* PackageFileName = "package.skrtgprofile";
constexpr const char* InstallFileName = "install.json";
constexpr const char* ContentDirectoryName = "content";
constexpr const char* ManifestPath = "manifest.json";
constexpr const char* IntegrityPath = "integrity.tsv";
constexpr const char* ProfilePath = "profile.json";
constexpr const char* RigPath = "rig/ik_rig.json";
constexpr const char* AlignmentPath =
    "alignment/canonical_to_character.ikretargeter.json";
constexpr const char* RestPath = "rest/character_rest.fbx";

struct PendingEntry
{
    std::filesystem::path RelativePath;
    std::filesystem::path SourcePath;
    std::string InlineBytes;
    std::uint64_t ByteCount = 0;
    std::string Sha256;
};

struct ParsedPackage
{
    std::map<std::string, ProfilePackageEntry> Entries;
    std::map<std::string, std::string> Text;
};

std::string Utf8Generic(const std::filesystem::path& Path)
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

std::optional<std::filesystem::path> EnvironmentPath(
    const char* Name)
{
#if defined(_WIN32)
    char* Value = nullptr;
    std::size_t Size = 0;
    const errno_t Result = _dupenv_s(&Value, &Size, Name);
    if (Result != 0 || Value == nullptr || *Value == '\0')
    {
        std::free(Value);
        return std::nullopt;
    }
    const std::filesystem::path Path = PathFromUtf8(Value);
    std::free(Value);
    return Path;
#else
    const char* Value = std::getenv(Name);
    if (Value == nullptr || *Value == '\0')
        return std::nullopt;
    return PathFromUtf8(Value);
#endif
}

std::string UpperAscii(std::string Value)
{
    std::transform(
        Value.begin(), Value.end(), Value.begin(),
        [](const unsigned char Character)
        {
            return static_cast<char>(std::toupper(Character));
        });
    return Value;
}

std::string LowerAscii(std::string Value)
{
    std::transform(
        Value.begin(), Value.end(), Value.begin(),
        [](const unsigned char Character)
        {
            return static_cast<char>(std::tolower(Character));
        });
    return Value;
}

bool IsSha256(const std::string& Value)
{
    return Value.size() == 64 &&
        std::all_of(
            Value.begin(), Value.end(),
            [](const unsigned char Character)
            {
                return std::isxdigit(Character) != 0;
            });
}

bool HasExtension(
    const std::filesystem::path& Path,
    const std::string& Extension)
{
    return LowerAscii(Path.extension().string()) == Extension;
}

bool IsRegularFile(const std::filesystem::path& Path)
{
    std::error_code Error;
    return !Path.empty() &&
        std::filesystem::is_regular_file(Path, Error) && !Error;
}

bool IsReparsePoint(
    const std::filesystem::path& Path,
    std::error_code& OutError)
{
#if defined(_WIN32)
    const DWORD Attributes =
        GetFileAttributesW(Path.c_str());
    if (Attributes == INVALID_FILE_ATTRIBUTES)
    {
        OutError = std::error_code(
            static_cast<int>(GetLastError()),
            std::system_category());
        return false;
    }
    OutError.clear();
    return (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    const std::filesystem::file_status Status =
        std::filesystem::symlink_status(Path, OutError);
    return !OutError && std::filesystem::is_symlink(Status);
#endif
}

bool IsPlainDirectory(
    const std::filesystem::path& Path,
    std::string& OutError)
{
    std::error_code StatusError;
    const std::filesystem::file_status Status =
        std::filesystem::symlink_status(Path, StatusError);
    if (StatusError ||
        !std::filesystem::is_directory(Status) ||
        std::filesystem::is_symlink(Status))
    {
        OutError =
            "managed profile path is not a plain directory: " +
            Utf8Generic(Path);
        return false;
    }
    std::error_code ReparseError;
    if (IsReparsePoint(Path, ReparseError) || ReparseError)
    {
        OutError =
            "managed profile path is a reparse point or unavailable: " +
            Utf8Generic(Path);
        return false;
    }
    return true;
}

bool IsPlainRegularFile(
    const std::filesystem::path& Path,
    std::string& OutError)
{
    std::error_code StatusError;
    const std::filesystem::file_status Status =
        std::filesystem::symlink_status(Path, StatusError);
    if (StatusError ||
        !std::filesystem::is_regular_file(Status) ||
        std::filesystem::is_symlink(Status))
    {
        OutError =
            "managed profile path is not a plain file: " +
            Utf8Generic(Path);
        return false;
    }
    std::error_code ReparseError;
    if (IsReparsePoint(Path, ReparseError) || ReparseError)
    {
        OutError =
            "managed profile file is a reparse point or unavailable: " +
            Utf8Generic(Path);
        return false;
    }
    return true;
}

bool IsSafePortablePath(
    const std::filesystem::path& Path,
    std::string& OutError)
{
    if (Path.empty() || Path.is_absolute() || Path.has_root_path())
    {
        OutError = "profile entry path is empty or absolute";
        return false;
    }
    const std::filesystem::path Normal = Path.lexically_normal();
    if (Normal != Path)
    {
        OutError = "profile entry path is not lexically canonical";
        return false;
    }
    const std::string Generic = Utf8Generic(Path);
    if (Generic.empty() || Generic.size() > 1024)
    {
        OutError = "profile entry path exceeds the portable limit";
        return false;
    }
    for (const std::filesystem::path& Component : Path)
    {
        if (Component.empty() || Component == "." || Component == "..")
        {
            OutError = "profile entry path contains an unsafe component";
            return false;
        }
        const std::string Name = Utf8Generic(Component);
        if (Name.empty() || Name.size() > 255 ||
            Name.back() == '.' || Name.back() == ' ')
        {
            OutError =
                "profile entry path has an invalid portable component";
            return false;
        }
        const std::size_t Dot = Name.find('.');
        const std::string Stem = LowerAscii(Name.substr(0, Dot));
        const bool Device = Stem == "con" || Stem == "prn" ||
            Stem == "aux" || Stem == "nul" ||
            (Stem.size() == 4 &&
             ((Stem.rfind("com", 0) == 0 ||
               Stem.rfind("lpt", 0) == 0) &&
              Stem[3] >= '1' && Stem[3] <= '9'));
        if (Device)
        {
            OutError =
                "profile entry path uses a reserved Windows name";
            return false;
        }
    }
    for (const unsigned char Character : Generic)
    {
        if (Character < 0x20 || Character > 0x7e ||
            Character == '\\' || Character == ':' || Character == '<' ||
            Character == '>' || Character == '"' || Character == '|' ||
            Character == '?' || Character == '*')
        {
            OutError =
                "profile entry path is outside portable ASCII";
            return false;
        }
    }
    return true;
}

std::string HashText(const std::string& Text)
{
    return skrv::Sha256(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(Text.data()), Text.size()));
}

bool ReadTextFile(
    const std::filesystem::path& Path,
    const std::uint64_t Limit,
    std::string& Out,
    std::string& OutError)
{
    std::error_code SizeError;
    const std::uintmax_t Size = std::filesystem::file_size(
        Path, SizeError);
    if (SizeError || Size > Limit)
    {
        OutError = "text input is unavailable or exceeds its limit: " +
            Utf8Generic(Path);
        return false;
    }
    std::ifstream Input(Path, std::ios::binary);
    if (!Input)
    {
        OutError = "failed to open text input: " + Utf8Generic(Path);
        return false;
    }
    Out.resize(static_cast<std::size_t>(Size));
    if (!Out.empty())
    {
        Input.read(
            Out.data(), static_cast<std::streamsize>(Out.size()));
        if (Input.gcount() !=
            static_cast<std::streamsize>(Out.size()))
        {
            OutError =
                "failed to read complete text input: " +
                Utf8Generic(Path);
            return false;
        }
    }
    return true;
}

bool ParseJson(
    const std::string& Text,
    const char* Label,
    Json& Out,
    std::string& OutError)
{
    try
    {
        std::size_t EventCount = 0;
        const auto LimitComplexity =
            [&](const int Depth,
                const Json::parse_event_t,
                Json&) -> bool
        {
            ++EventCount;
            if (Depth > 128 || EventCount > 1'000'000)
            {
                throw std::length_error(
                    "JSON nesting or node limit exceeded");
            }
            return true;
        };
        Out = Json::parse(
            Text, LimitComplexity, true, false);
        if (!Out.is_object())
        {
            OutError = std::string(Label) + " must be a JSON object";
            return false;
        }
        return true;
    }
    catch (const std::exception& Error)
    {
        OutError = std::string("failed to parse ") + Label + ": " +
            Error.what();
        return false;
    }
}

bool HasExportIdentity(
    const Json& Value,
    const char* Kind,
    std::string& OutError)
{
    const std::string Schema = Value.value("schema", "");
    const int Version = Value.value("schemaVersion", 0);
    const bool Supported =
        (Schema == "skrtg.ue_ik_asset_export.v1" && Version == 1) ||
        (Schema == "skrtg.ue_ik_asset_export.v2" && Version == 2);
    if (!Supported || Value.value("kind", "") != Kind ||
        !Value.value("valid", true))
    {
        OutError =
            std::string("unsupported or invalid UE export: ") + Kind;
        return false;
    }
    const Json Coordinate =
        Value.value("coordinateContract", Json::object());
    if (!Coordinate.is_object() ||
        Coordinate.value("handedness", "") != "left" ||
        Coordinate.value("forwardAxis", "") != "+X" ||
        Coordinate.value("rightAxis", "") != "+Y" ||
        Coordinate.value("upAxis", "") != "+Z" ||
        Coordinate.value("distanceUnit", "") != "centimeter" ||
        Coordinate.value("quaternionComponentOrder", "") !=
            "x,y,z,w")
    {
        OutError =
            std::string("unsupported coordinate contract in ") + Kind;
        return false;
    }
    return true;
}

bool ValidateUEProfileDocuments(
    const Json& Rig,
    const Json& Alignment,
    std::string& OutFingerprint,
    std::string& OutError)
{
    if (!HasExportIdentity(Rig, "ikRigDefinition", OutError) ||
        !HasExportIdentity(Alignment, "ikRetargeter", OutError))
    {
        return false;
    }
    if (Rig.at("coordinateContract") !=
        Alignment.at("coordinateContract"))
    {
        OutError =
            "IK Rig and alignment Retargeter coordinate contracts differ";
        return false;
    }
    const Json Reference =
        Rig.value("referenceSkeleton", Json::object());
    OutFingerprint = UpperAscii(
        Reference.value("fingerprintSha256", ""));
    if (!IsSha256(OutFingerprint) ||
        !Reference.value("bones", Json::array()).is_array() ||
        Reference.value("bones", Json::array()).empty())
    {
        OutError =
            "IK Rig reference skeleton fingerprint or bones are invalid";
        return false;
    }
    const std::string RigAsset =
        Rig.value("asset", Json::object()).value("assetName", "");
    const std::string AlignmentTargetRig =
        Alignment.value("target", Json::object())
            .value("ikRig", Json::object())
            .value("assetName", "");
    if (RigAsset.empty() || AlignmentTargetRig.empty() ||
        RigAsset != AlignmentTargetRig)
    {
        OutError =
            "alignment Retargeter target IK Rig does not match the profile IK Rig";
        return false;
    }
    if (!Alignment.contains("source") ||
        !Alignment.at("source").is_object() ||
        !Alignment.contains("target") ||
        !Alignment.at("target").is_object())
    {
        OutError =
            "alignment Retargeter source/target records are missing";
        return false;
    }
    return true;
}

template <typename Integer>
void WriteLittleEndian(std::ostream& Output, const Integer Value)
{
    static_assert(std::is_unsigned_v<Integer>);
    std::array<char, sizeof(Integer)> Bytes{};
    for (std::size_t Index = 0; Index < Bytes.size(); ++Index)
    {
        Bytes[Index] = static_cast<char>(
            (Value >> (Index * 8U)) & 0xffU);
    }
    Output.write(
        Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
}

template <typename Integer>
bool ReadLittleEndian(std::istream& Input, Integer& Out)
{
    static_assert(std::is_unsigned_v<Integer>);
    std::array<unsigned char, sizeof(Integer)> Bytes{};
    Input.read(
        reinterpret_cast<char*>(Bytes.data()),
        static_cast<std::streamsize>(Bytes.size()));
    if (Input.gcount() !=
        static_cast<std::streamsize>(Bytes.size()))
    {
        return false;
    }
    Out = 0;
    for (std::size_t Index = 0; Index < Bytes.size(); ++Index)
    {
        Out |= static_cast<Integer>(Bytes[Index]) << (Index * 8U);
    }
    return true;
}

bool CopyFileToStream(
    const std::filesystem::path& Path,
    std::ostream& Output,
    std::string& OutError)
{
    std::ifstream Input(Path, std::ios::binary);
    if (!Input)
    {
        OutError = "failed to open package input: " + Utf8Generic(Path);
        return false;
    }
    std::array<char, 64 * 1024> Buffer{};
    while (Input)
    {
        Input.read(
            Buffer.data(), static_cast<std::streamsize>(Buffer.size()));
        const std::streamsize Count = Input.gcount();
        if (Count > 0)
        {
            Output.write(Buffer.data(), Count);
            if (!Output)
            {
                OutError = "failed while writing profile package";
                return false;
            }
        }
    }
    if (!Input.eof())
    {
        OutError = "failed while reading package input: " +
            Utf8Generic(Path);
        return false;
    }
    return true;
}

bool WriteContainer(
    const std::filesystem::path& Path,
    const std::vector<PendingEntry>& Entries,
    std::string& OutError)
{
    std::ofstream Output(
        Path, std::ios::binary | std::ios::trunc);
    if (!Output)
    {
        OutError = "failed to create profile package: " +
            Utf8Generic(Path);
        return false;
    }
    Output.write(PackageMagic.data(), PackageMagic.size());
    WriteLittleEndian<std::uint32_t>(
        Output, static_cast<std::uint32_t>(Entries.size()));
    for (const PendingEntry& Entry : Entries)
    {
        const std::string Relative = Utf8Generic(Entry.RelativePath);
        WriteLittleEndian<std::uint16_t>(
            Output, static_cast<std::uint16_t>(Relative.size()));
        WriteLittleEndian<std::uint16_t>(Output, 0);
        WriteLittleEndian<std::uint64_t>(Output, Entry.ByteCount);
        Output.write(Entry.Sha256.data(), Entry.Sha256.size());
        Output.write(
            Relative.data(),
            static_cast<std::streamsize>(Relative.size()));
        if (!Entry.InlineBytes.empty() || Entry.SourcePath.empty())
        {
            Output.write(
                Entry.InlineBytes.data(),
                static_cast<std::streamsize>(
                    Entry.InlineBytes.size()));
        }
        else if (!CopyFileToStream(
                     Entry.SourcePath, Output, OutError))
        {
            return false;
        }
        if (!Output)
        {
            OutError = "failed while writing profile package entry: " +
                Relative;
            return false;
        }
    }
    Output.flush();
    if (!Output)
    {
        OutError = "failed to flush profile package";
        return false;
    }
    return true;
}

bool ReadEntryText(
    std::istream& Input,
    const std::uint64_t ByteCount,
    const std::uint64_t Limit,
    std::string& Out,
    std::string& OutHash,
    std::string& OutError)
{
    if (ByteCount > Limit ||
        ByteCount > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max()))
    {
        OutError = "profile text entry exceeds its size limit";
        return false;
    }
    Out.resize(static_cast<std::size_t>(ByteCount));
    if (!Out.empty())
    {
        Input.read(
            Out.data(), static_cast<std::streamsize>(Out.size()));
        if (Input.gcount() !=
            static_cast<std::streamsize>(Out.size()))
        {
            OutError =
                "profile package ended inside a text entry";
            return false;
        }
    }
    OutHash = HashText(Out);
    return true;
}

bool ParseResource(
    const Json& Resources,
    const char* Field,
    const char* RequiredPath,
    const std::map<std::string, ProfilePackageEntry>& Entries,
    ProfileResource& Out,
    std::string& OutError)
{
    if (!Resources.contains(Field) ||
        !Resources.at(Field).is_object())
    {
        OutError = std::string("profile resource is missing: ") + Field;
        return false;
    }
    const Json& Binding = Resources.at(Field);
    const std::string Path = Binding.value("path", "");
    const std::string Hash =
        UpperAscii(Binding.value("sha256", ""));
    if (Path != RequiredPath || !IsSha256(Hash) ||
        !Binding.contains("byteCount") ||
        !Binding.at("byteCount").is_number_unsigned())
    {
        OutError = std::string("invalid profile resource binding: ") +
            Field;
        return false;
    }
    const auto Found = Entries.find(Path);
    if (Found == Entries.end() ||
        Found->second.Sha256 != Hash ||
        Found->second.ByteCount !=
            Binding.at("byteCount").get<std::uint64_t>())
    {
        OutError = std::string(
            "profile resource does not match package inventory: ") +
            Field;
        return false;
    }
    Out.RelativePath = PathFromUtf8(Path);
    Out.ByteCount = Found->second.ByteCount;
    Out.Sha256 = Found->second.Sha256;
    return true;
}

bool ParseProfileDescriptor(
    const Json& Value,
    const std::map<std::string, ProfilePackageEntry>& Entries,
    CharacterProfileDescriptor& Out,
    std::string& OutError)
{
    if (Value.value("schema", "") != ProfileSchema ||
        Value.value("schemaVersion", 0) != 1)
    {
        OutError = "unsupported character profile schema";
        return false;
    }
    Out.ProfileId = Value.value("profileId", "");
    Out.ProfileVersion = Value.value("profileVersion", "");
    Out.DisplayName = Value.value("displayName", "");
    Out.CanonicalProfileId =
        Value.value("canonicalProfileId", "");
    Out.DefinitionKind = Value.value("definitionKind", "");
    Out.SkeletonSignatureSha256 = UpperAscii(
        Value.value("skeletonSignatureSha256", ""));
    Out.UnrealEngineVersion =
        Value.value("unrealEngineVersion", "");
    Out.RetargetRootBone =
        Value.value("retargetRootBone", "");
    Out.RetargetPelvisBone =
        Value.value("retargetPelvisBone", "");
    Out.SourceEnabled = Value.value("sourceEnabled", true);
    Out.TargetEnabled = Value.value("targetEnabled", true);
    if (!IsCharacterProfileId(Out.ProfileId) ||
        !IsCharacterProfileVersion(Out.ProfileVersion) ||
        Out.DisplayName.empty() || Out.DisplayName.size() > 256 ||
        !IsCharacterProfileId(Out.CanonicalProfileId) ||
        (Out.DefinitionKind != "ue_ik_json_v1" &&
         Out.DefinitionKind != "ue_ik_json_v2") ||
        !IsSha256(Out.SkeletonSignatureSha256) ||
        (!Out.SourceEnabled && !Out.TargetEnabled))
    {
        OutError = "character profile identity or capabilities are invalid";
        return false;
    }
    const auto ProfileEntry = Entries.find(ProfilePath);
    if (ProfileEntry == Entries.end())
    {
        OutError = "profile.json is absent from package inventory";
        return false;
    }
    Out.ProfileJson.RelativePath = ProfilePath;
    Out.ProfileJson.ByteCount = ProfileEntry->second.ByteCount;
    Out.ProfileJson.Sha256 = ProfileEntry->second.Sha256;
    const Json Resources =
        Value.value("resources", Json::object());
    return ParseResource(
               Resources, "restFbx", RestPath, Entries,
               Out.RestFbx, OutError) &&
        ParseResource(
               Resources, "ikRigJson", RigPath, Entries,
               Out.IkRigJson, OutError) &&
        ParseResource(
               Resources, "alignmentRetargeterJson",
               AlignmentPath, Entries,
               Out.AlignmentRetargeterJson, OutError);
}

bool ValidateManifest(
    const Json& Manifest,
    const CharacterProfileDescriptor& Profile,
    const std::map<std::string, ProfilePackageEntry>& Entries,
    std::string& OutError)
{
    if (Manifest.value("schema", "") != PackageSchema ||
        Manifest.value("schemaVersion", 0) != 1 ||
        Manifest.value("profileId", "") != Profile.ProfileId ||
        Manifest.value("profileVersion", "") !=
            Profile.ProfileVersion ||
        !Manifest.contains("payloads") ||
        !Manifest.at("payloads").is_array() ||
        Manifest.at("payloads").size() != 4)
    {
        OutError = "profile package manifest identity is invalid";
        return false;
    }
    std::set<std::string> Paths;
    for (const Json& Item : Manifest.at("payloads"))
    {
        if (!Item.is_object())
        {
            OutError = "profile manifest payload is not an object";
            return false;
        }
        const std::string Path = Item.value("path", "");
        const std::string Hash =
            UpperAscii(Item.value("sha256", ""));
        const auto Found = Entries.find(Path);
        if (Found == Entries.end() ||
            !Paths.insert(Path).second ||
            !Item.contains("byteCount") ||
            !Item.at("byteCount").is_number_unsigned() ||
            Found->second.ByteCount !=
                Item.at("byteCount").get<std::uint64_t>() ||
            Found->second.Sha256 != Hash)
        {
            OutError =
                "profile manifest payload does not match inventory";
            return false;
        }
    }
    const std::set<std::string> Required = {
        ProfilePath, RigPath, AlignmentPath, RestPath};
    if (Paths != Required)
    {
        OutError = "profile manifest payload set is incomplete";
        return false;
    }
    return true;
}

std::vector<std::string> SplitTabs(const std::string& Line)
{
    std::vector<std::string> Result;
    std::size_t Begin = 0;
    while (true)
    {
        const std::size_t Position = Line.find('\t', Begin);
        if (Position == std::string::npos)
        {
            Result.push_back(Line.substr(Begin));
            return Result;
        }
        Result.push_back(Line.substr(Begin, Position - Begin));
        Begin = Position + 1;
    }
}

bool ValidateIntegrity(
    const std::string& Text,
    const std::map<std::string, ProfilePackageEntry>& Entries,
    std::string& OutError)
{
    std::istringstream Input(Text);
    std::string Line;
    if (!std::getline(Input, Line))
    {
        OutError = "profile integrity index is empty";
        return false;
    }
    if (!Line.empty() && Line.back() == '\r') Line.pop_back();
    if (Line != IntegrityMagic ||
        !std::getline(Input, Line))
    {
        OutError = "profile integrity index header is invalid";
        return false;
    }
    if (!Line.empty() && Line.back() == '\r') Line.pop_back();
    if (Line != IntegrityHeader)
    {
        OutError = "profile integrity column header is invalid";
        return false;
    }
    std::set<std::string> Paths;
    while (std::getline(Input, Line))
    {
        if (!Line.empty() && Line.back() == '\r') Line.pop_back();
        if (Line.empty()) continue;
        const std::vector<std::string> Fields = SplitTabs(Line);
        if (Fields.size() != 4)
        {
            OutError = "profile integrity record has wrong field count";
            return false;
        }
        const auto Found = Entries.find(Fields[1]);
        std::uint64_t ByteCount = 0;
        try
        {
            std::size_t Parsed = 0;
            ByteCount = std::stoull(Fields[2], &Parsed);
            if (Parsed != Fields[2].size()) throw std::invalid_argument("");
        }
        catch (const std::exception&)
        {
            OutError = "profile integrity byte count is invalid";
            return false;
        }
        if (Found == Entries.end() ||
            !Paths.insert(Fields[1]).second ||
            Found->second.ByteCount != ByteCount ||
            Found->second.Sha256 != UpperAscii(Fields[3]))
        {
            OutError = "profile integrity record does not match inventory";
            return false;
        }
    }
    const std::set<std::string> Required = {
        ProfilePath, RigPath, AlignmentPath, RestPath};
    if (Paths != Required)
    {
        OutError = "profile integrity payload set is incomplete";
        return false;
    }
    return true;
}

bool ParseContainer(
    const std::filesystem::path& PackagePath,
    ParsedPackage& Out,
    std::string& OutError)
{
    std::error_code SizeError;
    const std::uintmax_t FileSize =
        std::filesystem::file_size(PackagePath, SizeError);
    if (SizeError || FileSize < PackageMagic.size() + 4 ||
        FileSize > MaximumProfilePackageBytes)
    {
        OutError =
            "profile package size is unavailable or outside v1 limits";
        return false;
    }
    std::ifstream Input(PackagePath, std::ios::binary);
    if (!Input)
    {
        OutError = "profile package is not readable: " +
            Utf8Generic(PackagePath);
        return false;
    }
    std::array<char, PackageMagic.size()> Magic{};
    Input.read(Magic.data(), Magic.size());
    if (Input.gcount() !=
            static_cast<std::streamsize>(Magic.size()) ||
        Magic != PackageMagic)
    {
        OutError = "profile package magic is invalid";
        return false;
    }
    std::uint32_t EntryCount = 0;
    if (!ReadLittleEndian(Input, EntryCount) ||
        EntryCount == 0 || EntryCount > MaximumProfileEntries)
    {
        OutError = "profile package entry count is invalid";
        return false;
    }
    std::uint64_t Consumed = PackageMagic.size() + sizeof(EntryCount);
    std::set<std::string> PortableKeys;
    for (std::uint32_t Index = 0; Index < EntryCount; ++Index)
    {
        std::uint16_t PathBytes = 0;
        std::uint16_t Flags = 0;
        std::uint64_t PayloadBytes = 0;
        if (!ReadLittleEndian(Input, PathBytes) ||
            !ReadLittleEndian(Input, Flags) ||
            !ReadLittleEndian(Input, PayloadBytes))
        {
            OutError = "profile package record header is truncated";
            return false;
        }
        Consumed += 12;
        if (PathBytes == 0 || PathBytes > 1024 || Flags != 0)
        {
            OutError = "profile package record flags or path length is invalid";
            return false;
        }
        std::array<char, 64> DeclaredHash{};
        Input.read(DeclaredHash.data(), DeclaredHash.size());
        if (Input.gcount() !=
            static_cast<std::streamsize>(DeclaredHash.size()))
        {
            OutError = "profile package record hash is truncated";
            return false;
        }
        Consumed += DeclaredHash.size();
        const std::string Declared(
            DeclaredHash.data(), DeclaredHash.size());
        if (!IsSha256(Declared) || Declared != UpperAscii(Declared))
        {
            OutError = "profile package record hash is invalid";
            return false;
        }
        std::string PathText(PathBytes, '\0');
        Input.read(
            PathText.data(), static_cast<std::streamsize>(PathText.size()));
        if (Input.gcount() !=
            static_cast<std::streamsize>(PathText.size()))
        {
            OutError = "profile package record path is truncated";
            return false;
        }
        Consumed += PathBytes;
        const std::filesystem::path Relative = PathFromUtf8(PathText);
        if (!IsSafePortablePath(Relative, OutError) ||
            !PortableKeys.insert(LowerAscii(PathText)).second)
        {
            if (OutError.empty())
                OutError = "profile package has a case-colliding entry";
            return false;
        }
        if (PayloadBytes > FileSize - Consumed)
        {
            OutError = "profile package payload exceeds file bounds";
            return false;
        }
        std::string Computed;
        const bool TextEntry =
            PathText == ManifestPath ||
            PathText == IntegrityPath ||
            PathText == ProfilePath ||
            PathText == RigPath ||
            PathText == AlignmentPath;
        if (TextEntry)
        {
            const std::uint64_t Limit =
                (PathText == RigPath || PathText == AlignmentPath)
                ? MaximumDefinitionJsonBytes
                : MaximumProfileJsonBytes;
            std::string Text;
            if (!ReadEntryText(
                    Input, PayloadBytes, Limit, Text,
                    Computed, OutError))
            {
                return false;
            }
            Out.Text.emplace(PathText, std::move(Text));
        }
        else if (!skrv::Sha256StreamRange(
                     Input, PayloadBytes, Computed, OutError))
        {
            return false;
        }
        Consumed += PayloadBytes;
        Computed = UpperAscii(Computed);
        if (Computed != Declared)
        {
            OutError =
                "profile package entry SHA-256 mismatch: " + PathText;
            return false;
        }
        Out.Entries.emplace(
            PathText,
            ProfilePackageEntry{
                Relative, PayloadBytes, Computed});
    }
    if (Consumed != FileSize)
    {
        OutError = "profile package has trailing or unparsed bytes";
        return false;
    }
    const std::set<std::string> Required = {
        ManifestPath, IntegrityPath, ProfilePath,
        RigPath, AlignmentPath, RestPath};
    std::set<std::string> Actual;
    for (const auto& [Path, Entry] : Out.Entries)
    {
        (void)Entry;
        Actual.insert(Path);
    }
    if (Actual != Required)
    {
        OutError =
            "profile package v1 requires exactly six declared entries";
        return false;
    }
    return true;
}

PendingEntry InlineEntry(
    const char* Path,
    std::string Bytes)
{
    PendingEntry Result;
    Result.RelativePath = PathFromUtf8(Path);
    Result.ByteCount = Bytes.size();
    Result.Sha256 = UpperAscii(HashText(Bytes));
    Result.InlineBytes = std::move(Bytes);
    return Result;
}

PendingEntry FileEntry(
    const char* Path,
    const std::filesystem::path& Source,
    const std::uint64_t ByteCount,
    std::string Sha256)
{
    PendingEntry Result;
    Result.RelativePath = PathFromUtf8(Path);
    Result.SourcePath = Source;
    Result.ByteCount = ByteCount;
    Result.Sha256 = UpperAscii(std::move(Sha256));
    return Result;
}

Json ResourceJson(const PendingEntry& Entry)
{
    return {
        {"path", Utf8Generic(Entry.RelativePath)},
        {"byteCount", Entry.ByteCount},
        {"sha256", Entry.Sha256}};
}

Json PayloadJson(
    const char* Role,
    const PendingEntry& Entry)
{
    return {
        {"role", Role},
        {"path", Utf8Generic(Entry.RelativePath)},
        {"byteCount", Entry.ByteCount},
        {"sha256", Entry.Sha256}};
}

std::string IntegrityText(
    const PendingEntry& Profile,
    const PendingEntry& Rig,
    const PendingEntry& Alignment,
    const PendingEntry& Rest)
{
    std::ostringstream Output;
    Output << IntegrityMagic << '\n'
           << IntegrityHeader << '\n';
    const auto Write = [&](const char* Role, const PendingEntry& Entry)
    {
        Output << Role << '\t'
               << Utf8Generic(Entry.RelativePath) << '\t'
               << Entry.ByteCount << '\t'
               << Entry.Sha256 << '\n';
    };
    Write("profile", Profile);
    Write("rig", Rig);
    Write("alignment", Alignment);
    Write("rest", Rest);
    return Output.str();
}

bool ReadInstalledReceipt(
    const std::filesystem::path& Directory,
    std::string& OutPackageSha,
    std::string& OutError) try
{
    std::string Text;
    if (!IsPlainRegularFile(
            Directory / InstallFileName, OutError) ||
        !ReadTextFile(
            Directory / InstallFileName,
            MaximumProfileJsonBytes, Text, OutError))
    {
        return false;
    }
    Json Receipt;
    if (!ParseJson(Text, "profile install receipt", Receipt, OutError) ||
        Receipt.value("schema", "") != InstallSchema ||
        Receipt.value("schemaVersion", 0) != 1)
    {
        if (OutError.empty())
            OutError = "unsupported profile install receipt";
        return false;
    }
    OutPackageSha = UpperAscii(
        Receipt.value("packageSha256", ""));
    if (!IsSha256(OutPackageSha))
    {
        OutError = "profile install receipt package hash is invalid";
        return false;
    }
    return true;
}
catch (const std::exception& Error)
{
    OutError =
        std::string("invalid profile install receipt: ") +
        Error.what();
    return false;
}
catch (...)
{
    OutError = "invalid profile install receipt";
    return false;
}

bool ExtractContainer(
    const std::filesystem::path& PackagePath,
    const std::filesystem::path& ContentRoot,
    const ProfileInspectResult& Inspection,
    std::string& OutError)
{
    std::ifstream Input(PackagePath, std::ios::binary);
    if (!Input)
    {
        OutError = "installed profile package cannot be reopened";
        return false;
    }
    Input.seekg(
        static_cast<std::streamoff>(PackageMagic.size() + 4),
        std::ios::beg);
    for (std::size_t Index = 0;
         Index < Inspection.Entries.size(); ++Index)
    {
        std::uint16_t PathBytes = 0;
        std::uint16_t Flags = 0;
        std::uint64_t PayloadBytes = 0;
        if (!ReadLittleEndian(Input, PathBytes) ||
            !ReadLittleEndian(Input, Flags) ||
            !ReadLittleEndian(Input, PayloadBytes))
        {
            OutError =
                "profile package changed before extraction";
            return false;
        }
        std::array<char, 64> Hash{};
        Input.read(Hash.data(), Hash.size());
        std::string PathText(PathBytes, '\0');
        Input.read(
            PathText.data(), static_cast<std::streamsize>(PathText.size()));
        const auto Found = std::find_if(
            Inspection.Entries.begin(),
            Inspection.Entries.end(),
            [&](const ProfilePackageEntry& Entry)
            {
                return Utf8Generic(Entry.RelativePath) == PathText;
            });
        if (Flags != 0 || Found == Inspection.Entries.end() ||
            Found->ByteCount != PayloadBytes ||
            Found->Sha256 !=
                std::string(Hash.data(), Hash.size()))
        {
            OutError =
                "profile package inventory changed before extraction";
            return false;
        }
        const std::filesystem::path OutputPath =
            ContentRoot / Found->RelativePath;
        std::error_code DirectoryError;
        std::filesystem::create_directories(
            OutputPath.parent_path(), DirectoryError);
        if (DirectoryError)
        {
            OutError = "failed to create profile content directory";
            return false;
        }
        std::ofstream Output(
            OutputPath, std::ios::binary | std::ios::trunc);
        if (!Output)
        {
            OutError = "failed to create installed profile entry";
            return false;
        }
        std::array<char, 64 * 1024> Buffer{};
        std::uint64_t Remaining = PayloadBytes;
        while (Remaining > 0)
        {
            const std::size_t Requested = static_cast<std::size_t>(
                std::min<std::uint64_t>(Remaining, Buffer.size()));
            Input.read(
                Buffer.data(),
                static_cast<std::streamsize>(Requested));
            if (Input.gcount() !=
                static_cast<std::streamsize>(Requested))
            {
                OutError =
                    "profile package ended during extraction";
                return false;
            }
            Output.write(
                Buffer.data(),
                static_cast<std::streamsize>(Requested));
            if (!Output)
            {
                OutError =
                    "failed while writing installed profile entry";
                return false;
            }
            Remaining -= Requested;
        }
        Output.close();
        std::string ExtractedHash;
        if (!skrv::Sha256File(
                OutputPath, ExtractedHash, OutError) ||
            UpperAscii(ExtractedHash) != Found->Sha256)
        {
            if (OutError.empty())
                OutError =
                    "installed profile entry failed SHA-256 verification";
            return false;
        }
    }
    return true;
}

bool VerifyInstalledResources(
    const InstalledCharacterProfile& Profile,
    const std::vector<ProfilePackageEntry>& ExpectedEntries,
    std::string& OutError)
{
    const std::filesystem::path ContentRoot =
        Profile.InstallDirectory / ContentDirectoryName;
    if (!IsPlainDirectory(ContentRoot, OutError))
        return false;

    std::set<std::string> ExpectedPaths;
    std::set<std::string> ExpectedDirectories;
    for (const ProfilePackageEntry& Entry : ExpectedEntries)
    {
        const std::string Relative =
            Utf8Generic(Entry.RelativePath);
        if (!ExpectedPaths.insert(Relative).second)
        {
            OutError =
                "installed profile inventory contains duplicate paths";
            return false;
        }
        std::filesystem::path Parent =
            Entry.RelativePath.parent_path();
        while (!Parent.empty())
        {
            ExpectedDirectories.insert(Utf8Generic(Parent));
            Parent = Parent.parent_path();
        }
        const std::filesystem::path Path =
            (ContentRoot / Entry.RelativePath)
                .lexically_normal();
        if (!IsPlainRegularFile(Path, OutError))
            return false;
        std::error_code SizeError;
        const std::uintmax_t Size =
            std::filesystem::file_size(Path, SizeError);
        if (SizeError || Size != Entry.ByteCount)
        {
            OutError =
                "installed profile resource byte count mismatch: " +
                Relative;
            return false;
        }
        std::string Hash;
        if (!skrv::Sha256File(Path, Hash, OutError) ||
            UpperAscii(Hash) != Entry.Sha256)
        {
            if (OutError.empty())
                OutError =
                    "installed profile resource SHA-256 mismatch: " +
                    Relative;
            return false;
        }
    }

    std::set<std::string> ActualPaths;
    std::set<std::string> ActualDirectories;
    std::error_code IteratorError;
    std::filesystem::recursive_directory_iterator Iterator(
        ContentRoot,
        std::filesystem::directory_options::skip_permission_denied,
        IteratorError);
    const std::filesystem::recursive_directory_iterator End;
    std::size_t Scanned = 0;
    while (!IteratorError && Iterator != End &&
           Scanned <= MaximumProfileEntries * 2)
    {
        const std::filesystem::directory_entry Entry = *Iterator;
        Iterator.increment(IteratorError);
        ++Scanned;
        const std::filesystem::path RelativePath =
            Entry.path().lexically_relative(ContentRoot);
        std::string PathError;
        if (!IsSafePortablePath(RelativePath, PathError))
        {
            OutError =
                "installed profile contains an unsafe path";
            return false;
        }
        const std::string Relative = Utf8Generic(RelativePath);
        std::error_code StatusError;
        const std::filesystem::file_status Status =
            Entry.symlink_status(StatusError);
        if (StatusError || std::filesystem::is_symlink(Status))
        {
            OutError =
                "installed profile contains an unavailable or linked "
                "entry: " + Relative;
            return false;
        }
        std::error_code ReparseError;
        if (IsReparsePoint(Entry.path(), ReparseError) ||
            ReparseError)
        {
            OutError =
                "installed profile contains a reparse point: " +
                Relative;
            return false;
        }
        if (std::filesystem::is_directory(Status))
            ActualDirectories.insert(Relative);
        else if (std::filesystem::is_regular_file(Status))
            ActualPaths.insert(Relative);
        else
        {
            OutError =
                "installed profile contains an unsupported entry: " +
                Relative;
            return false;
        }
    }
    if (IteratorError ||
        Scanned > MaximumProfileEntries * 2)
    {
        OutError =
            "installed profile content cannot be enumerated safely";
        return false;
    }
    if (ActualPaths != ExpectedPaths ||
        ActualDirectories != ExpectedDirectories)
    {
        OutError =
            "installed profile content does not exactly match the "
            "package inventory";
        return false;
    }
    return true;
}

void RemoveTree(const std::filesystem::path& Path)
{
    std::error_code Ignored;
    std::filesystem::remove_all(Path, Ignored);
}

bool RemoveTreeChecked(
    const std::filesystem::path& Path,
    std::string& OutError)
{
    std::error_code ExistsError;
    if (!std::filesystem::exists(Path, ExistsError))
    {
        if (!ExistsError) return true;
        OutError =
            "failed to inspect managed staging path: " +
            ExistsError.message();
        return false;
    }
    std::error_code ReparseError;
    if (IsReparsePoint(Path, ReparseError) || ReparseError)
    {
        OutError =
            "refused to remove a reparse-point staging path";
        return false;
    }
    std::error_code RemoveError;
    std::filesystem::remove_all(Path, RemoveError);
    if (RemoveError)
    {
        OutError =
            "failed to clear managed staging path: " +
            RemoveError.message();
        return false;
    }
    ExistsError.clear();
    if (std::filesystem::exists(Path, ExistsError) || ExistsError)
    {
        OutError =
            "managed staging path remained after cleanup";
        return false;
    }
    return true;
}
} // namespace

bool IsCharacterProfileId(const std::string& Value)
{
    if (Value.empty() || Value.size() > MaximumProfileIdBytes ||
        Value.front() == '.' || Value.front() == '-' ||
        Value.front() == '_' || Value.back() == '.' ||
        Value.back() == '-' || Value.back() == '_')
    {
        return false;
    }
    if (!std::all_of(
        Value.begin(), Value.end(),
        [](const unsigned char Character)
        {
            return (Character >= 'a' && Character <= 'z') ||
                (Character >= '0' && Character <= '9') ||
                Character == '.' || Character == '-' ||
                Character == '_';
        }))
    {
        return false;
    }
    const std::size_t Dot = Value.find('.');
    const std::string Stem = Value.substr(0, Dot);
    return Stem != "con" && Stem != "prn" &&
        Stem != "aux" && Stem != "nul" &&
        !(Stem.size() == 4 &&
          ((Stem.rfind("com", 0) == 0 ||
            Stem.rfind("lpt", 0) == 0) &&
           Stem[3] >= '1' && Stem[3] <= '9'));
}

bool IsCharacterProfileVersion(const std::string& Value)
{
    if (Value.empty() || Value.size() > MaximumProfileVersionBytes)
        return false;
    const std::size_t Dash = Value.find('-');
    const std::string Core = Value.substr(0, Dash);
    std::size_t Begin = 0;
    int Components = 0;
    while (Begin <= Core.size())
    {
        const std::size_t End = Core.find('.', Begin);
        const std::string Part = Core.substr(
            Begin, End == std::string::npos
                ? std::string::npos
                : End - Begin);
        if (Part.empty() ||
            (Part.size() > 1 && Part.front() == '0') ||
            !std::all_of(
                Part.begin(), Part.end(),
                [](const unsigned char Character)
                {
                    return Character >= '0' && Character <= '9';
                }))
        {
            return false;
        }
        ++Components;
        if (End == std::string::npos) break;
        Begin = End + 1;
    }
    if (Components != 3) return false;
    if (Dash == std::string::npos) return true;

    const std::string Suffix = Value.substr(Dash + 1);
    if (Suffix.empty()) return false;
    Begin = 0;
    while (Begin <= Suffix.size())
    {
        const std::size_t End = Suffix.find('.', Begin);
        const std::string Identifier = Suffix.substr(
            Begin, End == std::string::npos
                ? std::string::npos
                : End - Begin);
        if (Identifier.empty() ||
            !std::all_of(
                Identifier.begin(), Identifier.end(),
                [](const unsigned char Character)
                {
                    return (Character >= 'a' &&
                            Character <= 'z') ||
                        (Character >= '0' &&
                         Character <= '9') ||
                        Character == '-';
                }))
        {
            return false;
        }
        const bool Numeric = std::all_of(
            Identifier.begin(), Identifier.end(),
            [](const unsigned char Character)
            {
                return Character >= '0' && Character <= '9';
            });
        if (Numeric && Identifier.size() > 1 &&
            Identifier.front() == '0')
        {
            return false;
        }
        if (End == std::string::npos) break;
        Begin = End + 1;
    }
    return true;
}

int CompareCharacterProfileVersions(
    const std::string& Left,
    const std::string& Right)
{
    if (Left == Right) return 0;
    if (!IsCharacterProfileVersion(Left) ||
        !IsCharacterProfileVersion(Right))
    {
        return Left < Right ? -1 : 1;
    }
    const auto Split =
        [](const std::string& Value, const char Separator)
        {
            std::vector<std::string> Result;
            std::size_t Begin = 0;
            while (Begin <= Value.size())
            {
                const std::size_t End =
                    Value.find(Separator, Begin);
                Result.push_back(Value.substr(
                    Begin, End == std::string::npos
                        ? std::string::npos
                        : End - Begin));
                if (End == std::string::npos) break;
                Begin = End + 1;
            }
            return Result;
        };
    const auto CompareNumericText =
        [](const std::string& A, const std::string& B)
        {
            if (A.size() != B.size())
                return A.size() < B.size() ? -1 : 1;
            if (A == B) return 0;
            return A < B ? -1 : 1;
        };
    const std::size_t LeftDash = Left.find('-');
    const std::size_t RightDash = Right.find('-');
    const std::vector<std::string> LeftCore =
        Split(Left.substr(0, LeftDash), '.');
    const std::vector<std::string> RightCore =
        Split(Right.substr(0, RightDash), '.');
    for (std::size_t Index = 0; Index < 3; ++Index)
    {
        const int Compared = CompareNumericText(
            LeftCore[Index], RightCore[Index]);
        if (Compared != 0) return Compared;
    }
    const bool LeftPrerelease = LeftDash != std::string::npos;
    const bool RightPrerelease = RightDash != std::string::npos;
    if (LeftPrerelease != RightPrerelease)
        return LeftPrerelease ? -1 : 1;
    if (!LeftPrerelease) return 0;
    const std::vector<std::string> LeftIdentifiers =
        Split(Left.substr(LeftDash + 1), '.');
    const std::vector<std::string> RightIdentifiers =
        Split(Right.substr(RightDash + 1), '.');
    const std::size_t Count = std::min(
        LeftIdentifiers.size(), RightIdentifiers.size());
    for (std::size_t Index = 0; Index < Count; ++Index)
    {
        const std::string& A = LeftIdentifiers[Index];
        const std::string& B = RightIdentifiers[Index];
        if (A == B) continue;
        const bool ANumeric = std::all_of(
            A.begin(), A.end(),
            [](const unsigned char Character)
            {
                return Character >= '0' && Character <= '9';
            });
        const bool BNumeric = std::all_of(
            B.begin(), B.end(),
            [](const unsigned char Character)
            {
                return Character >= '0' && Character <= '9';
            });
        if (ANumeric != BNumeric) return ANumeric ? -1 : 1;
        return ANumeric ? CompareNumericText(A, B)
                        : (A < B ? -1 : 1);
    }
    if (LeftIdentifiers.size() == RightIdentifiers.size())
        return 0;
    return LeftIdentifiers.size() <
            RightIdentifiers.size()
        ? -1
        : 1;
}

std::filesystem::path DefaultCharacterProfileStore()
{
    if (const auto Override =
            EnvironmentPath("SKRTG_PROFILE_STORE"))
        return *Override;
    if (const auto Local = EnvironmentPath("LOCALAPPDATA"))
        return *Local / "SKRTG" / "Profiles";
    std::error_code Error;
    const std::filesystem::path Temporary =
        std::filesystem::temp_directory_path(Error);
    if (!Error && !Temporary.empty())
        return Temporary / "SKRTG" / "Profiles";
    return std::filesystem::current_path() / ".skrtg_profiles";
}

ProfilePackResult WriteCharacterProfilePackage(
    const ProfilePackRequest& Request) try
{
    ProfilePackResult Result;
    Result.PackagePath =
        std::filesystem::absolute(Request.OutputPackage)
            .lexically_normal();
    auto Fail = [&](std::string Error)
    {
        Result.Errors.push_back(std::move(Error));
        return Result;
    };
    if (!HasExtension(Result.PackagePath, ".skrtgprofile"))
        return Fail("output package must use .skrtgprofile");
    if (!IsCharacterProfileId(Request.ProfileId))
        return Fail("profile id is invalid");
    if (!IsCharacterProfileVersion(Request.ProfileVersion))
        return Fail("profile version must use semantic version form");
    if (Request.DisplayName.empty() ||
        Request.DisplayName.size() > 256)
    {
        return Fail("profile display name is empty or too long");
    }
    if (!IsCharacterProfileId(Request.CanonicalProfileId))
        return Fail("canonical profile id is invalid");
    if (!Request.SourceEnabled && !Request.TargetEnabled)
        return Fail("profile must be enabled as source or target");
    if (!IsRegularFile(Request.RestFbx) ||
        !HasExtension(Request.RestFbx, ".fbx"))
    {
        return Fail("rest input must be a readable FBX file");
    }
    if (!IsRegularFile(Request.IkRigJson) ||
        !HasExtension(Request.IkRigJson, ".json") ||
        !IsRegularFile(Request.AlignmentRetargeterJson) ||
        !HasExtension(Request.AlignmentRetargeterJson, ".json"))
    {
        return Fail(
            "IK Rig and alignment inputs must be readable JSON files");
    }
    std::error_code ExistsError;
    if (std::filesystem::exists(Result.PackagePath, ExistsError) ||
        ExistsError)
    {
        return Fail("output package already exists or cannot be checked");
    }

    std::string RigText;
    std::string AlignmentText;
    std::string Error;
    if (!ReadTextFile(
            Request.IkRigJson, MaximumDefinitionJsonBytes,
            RigText, Error) ||
        !ReadTextFile(
            Request.AlignmentRetargeterJson,
            MaximumDefinitionJsonBytes, AlignmentText, Error))
    {
        return Fail(Error);
    }
    Json Rig;
    Json Alignment;
    if (!ParseJson(RigText, "IK Rig JSON", Rig, Error) ||
        !ParseJson(
            AlignmentText, "alignment Retargeter JSON",
            Alignment, Error))
    {
        return Fail(Error);
    }
    std::string Fingerprint;
    if (!ValidateUEProfileDocuments(
            Rig, Alignment, Fingerprint, Error))
    {
        return Fail(Error);
    }

    std::error_code SizeError;
    const std::uint64_t RestBytes =
        std::filesystem::file_size(Request.RestFbx, SizeError);
    if (SizeError || RestBytes > MaximumProfilePackageBytes)
        return Fail("rest FBX size is unavailable or too large");
    const std::uint64_t RigBytes = RigText.size();
    const std::uint64_t AlignmentBytes = AlignmentText.size();
    std::string RestHash;
    std::string RigHash;
    std::string AlignmentHash;
    if (!skrv::Sha256File(Request.RestFbx, RestHash, Error) ||
        !skrv::Sha256File(Request.IkRigJson, RigHash, Error) ||
        !skrv::Sha256File(
            Request.AlignmentRetargeterJson,
            AlignmentHash, Error))
    {
        return Fail(Error);
    }
    PendingEntry Rest = FileEntry(
        RestPath, Request.RestFbx, RestBytes, RestHash);
    PendingEntry RigEntry = FileEntry(
        RigPath, Request.IkRigJson, RigBytes, RigHash);
    PendingEntry AlignmentEntry = FileEntry(
        AlignmentPath, Request.AlignmentRetargeterJson,
        AlignmentBytes, AlignmentHash);

    const int ExportVersion = Rig.value("schemaVersion", 0);
    Json ProfileJson = {
        {"schema", ProfileSchema},
        {"schemaVersion", 1},
        {"profileId", Request.ProfileId},
        {"profileVersion", Request.ProfileVersion},
        {"displayName", Request.DisplayName},
        {"canonicalProfileId", Request.CanonicalProfileId},
        {"definitionKind",
         ExportVersion == 2
             ? "ue_ik_json_v2"
             : "ue_ik_json_v1"},
        {"skeletonSignatureSha256", Fingerprint},
        {"unrealEngineVersion",
         Rig.value("unrealEngineVersion", "")},
        {"retargetRootBone",
         Rig.value("retargetRootBone", "")},
        {"retargetPelvisBone",
         Rig.value("retargetPelvisBone", "")},
        {"sourceEnabled", Request.SourceEnabled},
        {"targetEnabled", Request.TargetEnabled},
        {"coordinateContract", Rig.at("coordinateContract")},
        {"resources",
         {
             {"restFbx", ResourceJson(Rest)},
             {"ikRigJson", ResourceJson(RigEntry)},
             {"alignmentRetargeterJson",
              ResourceJson(AlignmentEntry)},
         }}};
    PendingEntry Profile = InlineEntry(
        ProfilePath, ProfileJson.dump(2) + "\n");

    Json Manifest = {
        {"schema", PackageSchema},
        {"schemaVersion", 1},
        {"profileId", Request.ProfileId},
        {"profileVersion", Request.ProfileVersion},
        {"payloads",
         Json::array(
             {PayloadJson("profile", Profile),
              PayloadJson("rig", RigEntry),
              PayloadJson("alignment", AlignmentEntry),
              PayloadJson("rest", Rest)})}};
    PendingEntry ManifestEntry = InlineEntry(
        ManifestPath, Manifest.dump(2) + "\n");
    PendingEntry IntegrityEntry = InlineEntry(
        IntegrityPath,
        IntegrityText(Profile, RigEntry, AlignmentEntry, Rest));
    std::vector<PendingEntry> Entries = {
        std::move(ManifestEntry),
        std::move(IntegrityEntry),
        std::move(Profile),
        std::move(RigEntry),
        std::move(AlignmentEntry),
        std::move(Rest)};

    std::uint64_t Total = PackageMagic.size() + 4;
    for (const PendingEntry& Entry : Entries)
    {
        std::string PathError;
        const std::string Path = Utf8Generic(Entry.RelativePath);
        if (!IsSafePortablePath(Entry.RelativePath, PathError) ||
            Path.size() >
                std::numeric_limits<std::uint16_t>::max())
        {
            return Fail(PathError);
        }
        const std::uint64_t HeaderBytes =
            2 + 2 + 8 + 64 + Path.size();
        if (Entry.ByteCount >
                MaximumProfilePackageBytes - Total ||
            HeaderBytes >
                MaximumProfilePackageBytes - Total -
                    Entry.ByteCount)
        {
            return Fail("profile package exceeds the v1 size limit");
        }
        Total += HeaderBytes + Entry.ByteCount;
    }
    std::error_code DirectoryError;
    std::filesystem::create_directories(
        Result.PackagePath.parent_path(), DirectoryError);
    if (DirectoryError)
        return Fail("failed to create profile package directory");
    std::filesystem::path Partial = Result.PackagePath;
    Partial += ".partial";
    std::error_code PartialError;
    std::filesystem::remove(Partial, PartialError);
    if (PartialError)
        return Fail("failed to clear stale profile package partial");
    if (!WriteContainer(Partial, Entries, Error))
    {
        std::filesystem::remove(Partial, PartialError);
        return Fail(Error);
    }
    const ProfileInspectResult Inspection =
        InspectCharacterProfilePackage(Partial);
    if (!Inspection.Success)
    {
        std::filesystem::remove(Partial, PartialError);
        Result.Errors = Inspection.Errors;
        return Result;
    }
    std::filesystem::rename(
        Partial, Result.PackagePath, PartialError);
    if (PartialError)
    {
        std::filesystem::remove(Partial, DirectoryError);
        return Fail("failed to commit profile package: " +
            PartialError.message());
    }
    const ProfileInspectResult Committed =
        InspectCharacterProfilePackage(Result.PackagePath);
    if (!Committed.Success)
    {
        Result.Errors = Committed.Errors;
        return Result;
    }
    Result.Success = true;
    Result.PackageSha256 = Committed.PackageSha256;
    Result.Profile = Committed.Profile;
    Result.Entries = Committed.Entries;
    return Result;
}
catch (const std::exception& Error)
{
    ProfilePackResult Result;
    Result.PackagePath =
        std::filesystem::absolute(Request.OutputPackage)
            .lexically_normal();
    std::error_code Ignored;
    std::filesystem::path Partial = Result.PackagePath;
    Partial += ".partial";
    std::filesystem::remove(Partial, Ignored);
    Result.Errors.push_back(
        std::string("invalid character profile input: ") +
        Error.what());
    return Result;
}
catch (...)
{
    ProfilePackResult Result;
    Result.PackagePath =
        std::filesystem::absolute(Request.OutputPackage)
            .lexically_normal();
    Result.Errors.push_back("invalid character profile input");
    return Result;
}

ProfileInspectResult InspectCharacterProfilePackage(
    const std::filesystem::path& PackagePath) try
{
    ProfileInspectResult Result;
    Result.PackagePath =
        std::filesystem::absolute(PackagePath).lexically_normal();
    ParsedPackage Parsed;
    std::string Error;
    if (!IsPlainRegularFile(Result.PackagePath, Error) ||
        !ParseContainer(Result.PackagePath, Parsed, Error))
    {
        Result.Errors.push_back(
            Error.empty() ? "profile package is not a readable file" :
                            Error);
        return Result;
    }
    Json Manifest;
    Json Profile;
    Json Rig;
    Json Alignment;
    if (!ParseJson(
            Parsed.Text.at(ManifestPath),
            "profile package manifest", Manifest, Error) ||
        !ParseJson(
            Parsed.Text.at(ProfilePath),
            "character profile", Profile, Error) ||
        !ParseJson(
            Parsed.Text.at(RigPath), "profile IK Rig", Rig, Error) ||
        !ParseJson(
            Parsed.Text.at(AlignmentPath),
            "profile alignment Retargeter", Alignment, Error))
    {
        Result.Errors.push_back(Error);
        return Result;
    }
    if (!ParseProfileDescriptor(
            Profile, Parsed.Entries, Result.Profile, Error) ||
        !ValidateManifest(
            Manifest, Result.Profile, Parsed.Entries, Error) ||
        !ValidateIntegrity(
            Parsed.Text.at(IntegrityPath),
            Parsed.Entries, Error))
    {
        Result.Errors.push_back(Error);
        return Result;
    }
    std::string Fingerprint;
    if (!ValidateUEProfileDocuments(
            Rig, Alignment, Fingerprint, Error) ||
        Fingerprint != Result.Profile.SkeletonSignatureSha256)
    {
        Result.Errors.push_back(
            Error.empty()
                ? "profile skeleton signature does not match IK Rig"
                : Error);
        return Result;
    }
    if (!skrv::Sha256File(
            Result.PackagePath, Result.PackageSha256, Error))
    {
        Result.Errors.push_back(Error);
        return Result;
    }
    Result.PackageSha256 = UpperAscii(Result.PackageSha256);
    for (const auto& [Path, Entry] : Parsed.Entries)
    {
        (void)Path;
        Result.Entries.push_back(Entry);
    }
    std::sort(
        Result.Entries.begin(), Result.Entries.end(),
        [](const auto& Left, const auto& Right)
        {
            return Utf8Generic(Left.RelativePath) <
                Utf8Generic(Right.RelativePath);
        });
    Result.Success = true;
    return Result;
}
catch (const std::exception& Error)
{
    ProfileInspectResult Result;
    Result.PackagePath =
        std::filesystem::absolute(PackagePath)
            .lexically_normal();
    Result.Errors.push_back(
        std::string("invalid character profile package metadata: ") +
        Error.what());
    return Result;
}
catch (...)
{
    ProfileInspectResult Result;
    Result.PackagePath =
        std::filesystem::absolute(PackagePath)
            .lexically_normal();
    Result.Errors.push_back(
        "invalid character profile package metadata");
    return Result;
}

ProfileInstallResult InstallCharacterProfilePackage(
    const std::filesystem::path& PackagePath,
    const std::filesystem::path& StoreRoot) try
{
    ProfileInstallResult Result;
    const ProfileInspectResult Source =
        InspectCharacterProfilePackage(PackagePath);
    if (!Source.Success)
    {
        Result.Errors = Source.Errors;
        return Result;
    }
    const std::filesystem::path AbsoluteRoot =
        std::filesystem::absolute(StoreRoot).lexically_normal();
    const std::filesystem::path ProfileRoot =
        AbsoluteRoot / Source.Profile.ProfileId;
    const std::filesystem::path Final =
        ProfileRoot / Source.Profile.ProfileVersion;
    std::error_code DirectoryError;
    std::string ManagedPathError;
    const bool StoreExists =
        std::filesystem::exists(
            AbsoluteRoot, DirectoryError);
    if (DirectoryError)
    {
        Result.Errors.push_back(
            "failed to inspect profile store directory");
        return Result;
    }
    if (StoreExists &&
        !IsPlainDirectory(AbsoluteRoot, ManagedPathError))
    {
        Result.Errors.push_back(ManagedPathError);
        return Result;
    }
    if (!StoreExists)
    {
        std::filesystem::create_directories(
            AbsoluteRoot, DirectoryError);
        if (DirectoryError ||
            !IsPlainDirectory(
                AbsoluteRoot, ManagedPathError))
        {
            Result.Errors.push_back(
                ManagedPathError.empty()
                    ? "failed to create a plain profile store directory"
                    : ManagedPathError);
            return Result;
        }
    }
    DirectoryError.clear();
    const bool ProfileRootExists =
        std::filesystem::exists(
            ProfileRoot, DirectoryError);
    if (DirectoryError)
    {
        Result.Errors.push_back(
            "failed to inspect profile identity directory");
        return Result;
    }
    if (ProfileRootExists &&
        !IsPlainDirectory(ProfileRoot, ManagedPathError))
    {
        Result.Errors.push_back(ManagedPathError);
        return Result;
    }
    if (!ProfileRootExists)
    {
        const bool CreatedProfileRoot =
            std::filesystem::create_directory(
                ProfileRoot, DirectoryError);
        if (DirectoryError || !CreatedProfileRoot ||
            !IsPlainDirectory(
                ProfileRoot, ManagedPathError))
        {
            Result.Errors.push_back(
                ManagedPathError.empty()
                    ? "failed to create a plain profile identity "
                      "directory"
                    : ManagedPathError);
            return Result;
        }
    }
    bool RepairDamagedInstall = false;
    std::error_code ExistsError;
    if (std::filesystem::exists(Final, ExistsError) && !ExistsError)
    {
        std::string ExistingHash;
        std::string Error;
        if (IsPlainDirectory(Final, Error) &&
            ReadInstalledReceipt(Final, ExistingHash, Error) &&
            ExistingHash == Source.PackageSha256)
        {
            const ProfileInspectResult Existing =
                InspectCharacterProfilePackage(
                    Final / PackageFileName);
            if (Existing.Success &&
                Existing.PackageSha256 == ExistingHash &&
                Existing.PackageSha256 == Source.PackageSha256 &&
                Existing.Profile.ProfileId ==
                    Source.Profile.ProfileId &&
                Existing.Profile.ProfileVersion ==
                    Source.Profile.ProfileVersion)
            {
                InstalledCharacterProfile Installed = {
                    Existing.Profile,
                    Final,
                    Final / PackageFileName,
                    Existing.PackageSha256};
                if (VerifyInstalledResources(
                        Installed, Existing.Entries, Error))
                {
                    Result.Success = true;
                    Result.AlreadyInstalled = true;
                    Result.Installed = std::move(Installed);
                    return Result;
                }
                RepairDamagedInstall = true;
            }
        }
        if (!RepairDamagedInstall)
        {
            Result.Errors.push_back(
                "a different or invalid profile is already installed at " +
                Utf8Generic(Final));
            return Result;
        }
    }
    if (ExistsError)
    {
        Result.Errors.push_back(
            "failed to inspect profile install destination");
        return Result;
    }
    const std::filesystem::path Partial =
        ProfileRoot /
        (Source.Profile.ProfileVersion + ".partial." +
         Source.PackageSha256.substr(0, 12));
    std::string StagingError;
    if (!RemoveTreeChecked(Partial, StagingError))
    {
        Result.Errors.push_back(StagingError);
        return Result;
    }
    DirectoryError.clear();
    const bool CreatedPartial =
        std::filesystem::create_directory(
            Partial, DirectoryError);
    if (DirectoryError || !CreatedPartial ||
        !IsPlainDirectory(Partial, StagingError))
    {
        Result.Errors.push_back(
            StagingError.empty()
                ? "failed to create a new empty profile install partial"
                : StagingError);
        return Result;
    }
    std::error_code EmptyError;
    if (std::filesystem::directory_iterator(
            Partial, EmptyError) !=
            std::filesystem::directory_iterator() ||
        EmptyError)
    {
        RemoveTree(Partial);
        Result.Errors.push_back(
            "profile install partial was not empty after creation");
        return Result;
    }
    const std::filesystem::path Copied =
        Partial / PackageFileName;
    std::filesystem::copy_file(
        Source.PackagePath, Copied,
        std::filesystem::copy_options::none, DirectoryError);
    if (DirectoryError)
    {
        RemoveTree(Partial);
        Result.Errors.push_back(
            "failed to copy profile package into store");
        return Result;
    }
    const ProfileInspectResult CopiedInspection =
        InspectCharacterProfilePackage(Copied);
    if (!CopiedInspection.Success ||
        CopiedInspection.PackageSha256 != Source.PackageSha256)
    {
        RemoveTree(Partial);
        Result.Errors = CopiedInspection.Errors;
        if (Result.Errors.empty())
            Result.Errors.push_back(
                "profile package changed while installing");
        return Result;
    }
    std::string Error;
    if (!ExtractContainer(
            Copied, Partial / ContentDirectoryName,
            CopiedInspection, Error))
    {
        RemoveTree(Partial);
        Result.Errors.push_back(Error);
        return Result;
    }
    const Json Receipt = {
        {"schema", InstallSchema},
        {"schemaVersion", 1},
        {"profileId", Source.Profile.ProfileId},
        {"profileVersion", Source.Profile.ProfileVersion},
        {"packageSha256", Source.PackageSha256},
        {"contentRoot", ContentDirectoryName}};
    {
        std::ofstream Output(
            Partial / InstallFileName,
            std::ios::binary | std::ios::trunc);
        if (!Output)
        {
            RemoveTree(Partial);
            Result.Errors.push_back(
                "failed to create profile install receipt");
            return Result;
        }
        Output << Receipt.dump(2) << '\n';
        if (!Output)
        {
            RemoveTree(Partial);
            Result.Errors.push_back(
                "failed to write profile install receipt");
            return Result;
        }
    }
    std::filesystem::path RepairBackup;
    if (RepairDamagedInstall)
    {
        const auto Stamp =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
        RepairBackup = ProfileRoot /
            (Source.Profile.ProfileVersion +
             ".repair-backup." + std::to_string(Stamp));
        DirectoryError.clear();
        std::filesystem::rename(
            Final, RepairBackup, DirectoryError);
        if (DirectoryError)
        {
            RemoveTree(Partial);
            Result.Errors.push_back(
                "failed to preserve damaged profile before repair: " +
                DirectoryError.message());
            return Result;
        }
    }
    DirectoryError.clear();
    std::filesystem::rename(Partial, Final, DirectoryError);
    if (DirectoryError)
    {
        RemoveTree(Partial);
        if (RepairDamagedInstall)
        {
            std::error_code RestoreError;
            std::filesystem::rename(
                RepairBackup, Final, RestoreError);
            if (RestoreError)
            {
                Result.Errors.push_back(
                    "failed to restore the previous damaged profile: " +
                    RestoreError.message());
            }
        }
        Result.Errors.push_back(
            "failed to commit installed profile: " +
            DirectoryError.message());
        return Result;
    }
    Result.Installed = {
        Source.Profile,
        Final,
        Final / PackageFileName,
        Source.PackageSha256};
    if (!VerifyInstalledResources(
            Result.Installed, CopiedInspection.Entries, Error))
    {
        RemoveTree(Final);
        if (RepairDamagedInstall)
        {
            std::error_code RestoreError;
            std::filesystem::rename(
                RepairBackup, Final, RestoreError);
            if (RestoreError)
            {
                Result.Errors.push_back(
                    "failed to restore the previous damaged profile: " +
                    RestoreError.message());
            }
        }
        Result.Errors.push_back(Error);
        return Result;
    }
    if (RepairDamagedInstall)
    {
        std::string CleanupError;
        if (!RemoveTreeChecked(RepairBackup, CleanupError))
        {
            Result.Errors.push_back(CleanupError);
            return Result;
        }
    }
    Result.Success = true;
    return Result;
}
catch (const std::exception& Error)
{
    ProfileInstallResult Result;
    Result.Errors.push_back(
        std::string("failed to install character profile: ") +
        Error.what());
    return Result;
}
catch (...)
{
    ProfileInstallResult Result;
    Result.Errors.push_back(
        "failed to install character profile");
    return Result;
}

ProfileDiscoveryResult DiscoverInstalledCharacterProfiles(
    const std::filesystem::path& StoreRoot) try
{
    ProfileDiscoveryResult Result;
    Result.StoreRoot =
        std::filesystem::absolute(StoreRoot).lexically_normal();
    std::error_code Error;
    if (!std::filesystem::exists(Result.StoreRoot, Error) && !Error)
    {
        Result.Success = true;
        return Result;
    }
    std::string StoreError;
    if (Error ||
        !IsPlainDirectory(Result.StoreRoot, StoreError))
    {
        Result.Errors.push_back(
            StoreError.empty()
                ? "character profile store is not a readable directory"
                : StoreError);
        return Result;
    }
    std::size_t Scanned = 0;
    const auto InspectInstallDirectory =
        [&](const std::filesystem::path& Directory)
    {
        std::string ReceiptHash;
        std::string ReadError;
        if (!ReadInstalledReceipt(
                Directory, ReceiptHash, ReadError))
        {
            Result.Warnings.push_back(
                "ignored invalid profile install at " +
                Utf8Generic(Directory) + ": " + ReadError);
            return;
        }
        const ProfileInspectResult Inspection =
            InspectCharacterProfilePackage(
                Directory / PackageFileName);
        if (!Inspection.Success ||
            Inspection.PackageSha256 != ReceiptHash ||
            Directory.filename() !=
                Inspection.Profile.ProfileVersion ||
            Directory.parent_path().filename() !=
                Inspection.Profile.ProfileId)
        {
            Result.Warnings.push_back(
                "ignored inconsistent profile install at " +
                Utf8Generic(Directory));
            return;
        }
        InstalledCharacterProfile Installed = {
            Inspection.Profile,
            Directory,
            Directory / PackageFileName,
            Inspection.PackageSha256};
        if (!VerifyInstalledResources(
                Installed, Inspection.Entries, ReadError))
        {
            Result.Warnings.push_back(
                "ignored damaged profile install at " +
                Utf8Generic(Directory) + ": " + ReadError);
            return;
        }
        Result.Profiles.push_back(std::move(Installed));
    };

    std::filesystem::directory_iterator ProfileIterator(
        Result.StoreRoot,
        std::filesystem::directory_options::skip_permission_denied,
        Error);
    const std::filesystem::directory_iterator End;
    while (!Error && ProfileIterator != End && Scanned < 8192)
    {
        const std::filesystem::directory_entry ProfileEntry =
            *ProfileIterator;
        ProfileIterator.increment(Error);
        ++Scanned;
        std::string PlainPathError;
        if (!IsPlainDirectory(
                ProfileEntry.path(), PlainPathError))
        {
            continue;
        }
        const std::string ProfileId =
            Utf8Generic(ProfileEntry.path().filename());
        if (!IsCharacterProfileId(ProfileId)) continue;

        std::error_code VersionError;
        std::filesystem::directory_iterator VersionIterator(
            ProfileEntry.path(),
            std::filesystem::directory_options::skip_permission_denied,
            VersionError);
        while (!VersionError && VersionIterator != End &&
               Scanned < 8192)
        {
            const std::filesystem::directory_entry VersionEntry =
                *VersionIterator;
            VersionIterator.increment(VersionError);
            ++Scanned;
            if (!IsPlainDirectory(
                    VersionEntry.path(), PlainPathError))
            {
                continue;
            }
            const std::string ProfileVersion =
                Utf8Generic(VersionEntry.path().filename());
            if (!IsCharacterProfileVersion(ProfileVersion))
                continue;
            const std::filesystem::path Directory =
                Result.StoreRoot / ProfileId / ProfileVersion;
            if (!IsPlainRegularFile(
                    Directory / InstallFileName,
                    PlainPathError))
            {
                continue;
            }
            InspectInstallDirectory(Directory);
        }
        if (VersionError)
        {
            Result.Errors.push_back(
                "failed while scanning character profile versions: " +
                VersionError.message());
        }
    }
    if (Error)
    {
        Result.Errors.push_back(
            "failed while scanning character profile store: " +
            Error.message());
    }
    if (Scanned >= 8192)
    {
        Result.Errors.push_back(
            "character profile store exceeds the scan limit");
    }
    std::sort(
        Result.Profiles.begin(), Result.Profiles.end(),
        [](const auto& Left, const auto& Right)
        {
            if (Left.Profile.ProfileId != Right.Profile.ProfileId)
            {
                return Left.Profile.ProfileId <
                    Right.Profile.ProfileId;
            }
            return CompareCharacterProfileVersions(
                       Left.Profile.ProfileVersion,
                       Right.Profile.ProfileVersion) < 0;
        });
    Result.Success = Result.Errors.empty();
    return Result;
}
catch (const std::exception& Error)
{
    ProfileDiscoveryResult Result;
    Result.StoreRoot = StoreRoot.lexically_normal();
    Result.Errors.push_back(
        std::string("failed to discover character profiles: ") +
        Error.what());
    return Result;
}
catch (...)
{
    ProfileDiscoveryResult Result;
    Result.StoreRoot = StoreRoot.lexically_normal();
    Result.Errors.push_back(
        "failed to discover character profiles");
    return Result;
}

ProfileDeleteResult DeleteInstalledCharacterProfile(
    const InstalledCharacterProfile& Profile,
    const std::filesystem::path& StoreRoot) try
{
    ProfileDeleteResult Result;
    if (!IsCharacterProfileId(Profile.Profile.ProfileId) ||
        !IsCharacterProfileVersion(Profile.Profile.ProfileVersion))
    {
        Result.Errors.push_back(
            "refused to delete a profile with invalid identity");
        return Result;
    }
    const std::filesystem::path Root =
        std::filesystem::absolute(StoreRoot).lexically_normal();
    const std::filesystem::path Expected =
        Root / Profile.Profile.ProfileId /
        Profile.Profile.ProfileVersion;
    const std::filesystem::path Actual =
        std::filesystem::absolute(Profile.InstallDirectory)
            .lexically_normal();
    if (Actual != Expected)
    {
        Result.Errors.push_back(
            "refused to delete a profile outside the managed store");
        return Result;
    }
    std::string ReceiptHash;
    std::string Error;
    if (!IsPlainDirectory(Root, Error) ||
        !IsPlainDirectory(Expected.parent_path(), Error) ||
        !IsPlainDirectory(Actual, Error) ||
        !ReadInstalledReceipt(Actual, ReceiptHash, Error) ||
        ReceiptHash != Profile.PackageSha256)
    {
        Result.Errors.push_back(
            "refused to delete a profile with an invalid receipt");
        return Result;
    }
    if (!IsPlainDirectory(Actual, Error))
    {
        Result.Errors.push_back(
            "refused to delete a profile whose managed path changed");
        return Result;
    }
    std::error_code RemoveError;
    const std::uintmax_t Removed =
        std::filesystem::remove_all(Actual, RemoveError);
    if (RemoveError || Removed == 0)
    {
        Result.Errors.push_back(
            "failed to remove installed profile");
        return Result;
    }
    std::error_code ParentError;
    std::filesystem::remove(
        Expected.parent_path(), ParentError);
    Result.Success = true;
    return Result;
}
catch (const std::exception& Error)
{
    ProfileDeleteResult Result;
    Result.Errors.push_back(
        std::string("failed to delete character profile: ") +
        Error.what());
    return Result;
}
catch (...)
{
    ProfileDeleteResult Result;
    Result.Errors.push_back(
        "failed to delete character profile");
    return Result;
}

std::filesystem::path InstalledProfileResourcePath(
    const InstalledCharacterProfile& Profile,
    const ProfileResource& Resource)
{
    return (Profile.InstallDirectory / ContentDirectoryName /
            Resource.RelativePath)
        .lexically_normal();
}
} // namespace skrtg::viewer::profile
