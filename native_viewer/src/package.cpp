#include "skrtg/viewer/skrv/package.h"

#include "manifest_validation.h"
#include "package_inventory.h"
#include "skrtg/viewer/skrv/sha256.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <exception>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

namespace skrtg::viewer::skrv
{
namespace
{
constexpr const char* IntegrityFileName = "integrity.tsv";
constexpr const char* IntegrityMagic = "SKRV_INTEGRITY_V1";
constexpr const char* IntegrityHeader = "role\tpath\tbyte_count\tsha256";
constexpr const char* ManifestFileName = "manifest.json";

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

std::string PortableKey(const std::filesystem::path& Path)
{
    return LowerAscii(Utf8Generic(Path.lexically_normal()));
}

bool IsSafeRelativePath(
    const std::filesystem::path& Path,
    std::string& OutError)
{
    if (Path.empty() || Path.is_absolute() || Path.has_root_path())
    {
        OutError = "package path is empty or absolute";
        return false;
    }
    for (const std::filesystem::path& Component : Path)
    {
        if (Component.empty() || Component == "." || Component == "..")
        {
            OutError = "package path contains an unsafe component";
            return false;
        }
    }
    const std::string Generic = Utf8Generic(Path);
    if (Generic.size() > MaximumPortablePathBytes)
    {
        OutError = "package path exceeds the portable length limit";
        return false;
    }
    for (const unsigned char Character : Generic)
    {
        if (Character < 0x20 || Character > 0x7e ||
            Character == '\\' || Character == ':' || Character == '<' ||
            Character == '>' || Character == '"' || Character == '|' ||
            Character == '?' || Character == '*')
        {
            OutError = "package path is outside the portable ASCII subset";
            return false;
        }
    }
    for (const std::filesystem::path& Component : Path)
    {
        const std::string Name = Utf8Generic(Component);
        if (Name.size() > MaximumPortablePathComponentBytes)
        {
            OutError = "package path component exceeds the portable length limit";
            return false;
        }
        if (Name.back() == '.' || Name.back() == ' ')
        {
            OutError = "package path component has a trailing dot or space";
            return false;
        }
        const std::size_t Dot = Name.find('.');
        const std::string Stem = LowerAscii(Name.substr(0, Dot));
        const bool Device = Stem == "con" || Stem == "prn" ||
            Stem == "aux" || Stem == "nul" ||
            (Stem.size() == 4 &&
             ((Stem.rfind("com", 0) == 0 || Stem.rfind("lpt", 0) == 0) &&
              Stem[3] >= '1' && Stem[3] <= '9'));
        if (Device)
        {
            OutError = "package path uses a reserved Windows device name";
            return false;
        }
    }
    if (Path.lexically_normal() != Path)
    {
        OutError = "package path is not lexically canonical";
        return false;
    }
    return true;
}

bool RoleMatchesPath(
    const EntryRole Role,
    const std::filesystem::path& Path,
    std::string& OutError)
{
    const std::string Generic = Utf8Generic(Path);
    const bool IsManifest = Generic == ManifestFileName;
    const bool IsBlob = Generic.rfind("blobs/", 0) == 0;
    const bool IsExport = Generic.rfind("exports/", 0) == 0;
    if ((Role == EntryRole::Manifest) != IsManifest)
    {
        OutError = "manifest role/path classification is inconsistent";
        return false;
    }
    if ((Role == EntryRole::Blob) != IsBlob)
    {
        OutError = "blob role/path classification is inconsistent";
        return false;
    }
    if ((Role == EntryRole::Export) != IsExport)
    {
        OutError = "export role/path classification is inconsistent";
        return false;
    }
    return true;
}

std::vector<std::string> SplitTabs(const std::string& Line)
{
    std::vector<std::string> Fields;
    std::size_t Start = 0;
    while (true)
    {
        const std::size_t Position = Line.find('\t', Start);
        if (Position == std::string::npos)
        {
            Fields.push_back(Line.substr(Start));
            return Fields;
        }
        Fields.push_back(Line.substr(Start, Position - Start));
        Start = Position + 1;
    }
}

bool IsSha256Hex(const std::string& Value)
{
    return Value.size() == 64 &&
        std::all_of(
            Value.begin(), Value.end(),
            [](const unsigned char Character)
            {
                return std::isxdigit(Character) != 0;
            });
}

bool ParseByteCount(
    const std::string& Value,
    std::uintmax_t& OutValue)
{
    if (Value.empty()) return false;
    const char* Begin = Value.data();
    const char* End = Begin + Value.size();
    const auto Parsed = std::from_chars(Begin, End, OutValue);
    return Parsed.ec == std::errc{} && Parsed.ptr == End;
}

bool ReadWholeFile(
    const std::filesystem::path& Path,
    std::string& Out,
    std::string& OutError)
{
    std::ifstream Input(Path, std::ios::binary);
    if (!Input)
    {
        OutError = "failed to open text file: " + Path.string();
        return false;
    }
    std::ostringstream Buffer;
    Buffer << Input.rdbuf();
    if (!Input.good() && !Input.eof())
    {
        OutError = "failed while reading text file: " + Path.string();
        return false;
    }
    Out = Buffer.str();
    return true;
}

bool ValidateIntegrityResourceShape(
    const std::filesystem::path& Path,
    std::string& OutError)
{
    std::ifstream Input(Path, std::ios::binary);
    if (!Input)
    {
        OutError = "integrity.tsv is missing";
        return false;
    }
    std::array<char, 64U * 1024U> Buffer{};
    std::size_t LineBytes = 0;
    std::size_t LineCount = 0;
    while (Input)
    {
        Input.read(Buffer.data(), static_cast<std::streamsize>(Buffer.size()));
        const std::streamsize Read = Input.gcount();
        for (std::streamsize Index = 0; Index < Read; ++Index)
        {
            if (Buffer[static_cast<std::size_t>(Index)] == '\n')
            {
                ++LineCount;
                LineBytes = 0;
                if (LineCount > MaximumIndexedEntries + 2)
                {
                    OutError = "integrity.tsv exceeds the record-count limit";
                    return false;
                }
            }
            else
            {
                ++LineBytes;
                if (LineBytes > MaximumIntegrityLineBytes)
                {
                    OutError = "integrity.tsv exceeds the line-length limit";
                    return false;
                }
            }
        }
    }
    if (!Input.eof())
    {
        OutError = "failed while preflighting integrity.tsv";
        return false;
    }
    if (LineBytes != 0)
    {
        ++LineCount;
        if (LineCount > MaximumIndexedEntries + 2)
        {
            OutError = "integrity.tsv exceeds the record-count limit";
            return false;
        }
    }
    return true;
}

void RemovePartial(const std::filesystem::path& Path)
{
    std::error_code Ignored;
    std::filesystem::remove_all(Path, Ignored);
}

PackageWriteResult WriteFailure(
    const std::filesystem::path& Output,
    const std::string& Error)
{
    PackageWriteResult Result;
    Result.OutputDirectory = Output;
    Result.Errors.push_back(Error);
    return Result;
}
} // namespace

const char* EntryRoleName(const EntryRole Role)
{
    switch (Role)
    {
    case EntryRole::Manifest: return "manifest";
    case EntryRole::Blob: return "blob";
    case EntryRole::Export: return "export";
    case EntryRole::Auxiliary: return "auxiliary";
    }
    return "auxiliary";
}

bool ParseEntryRole(const std::string& Value, EntryRole& OutRole)
{
    if (Value == "manifest") OutRole = EntryRole::Manifest;
    else if (Value == "blob") OutRole = EntryRole::Blob;
    else if (Value == "export") OutRole = EntryRole::Export;
    else if (Value == "auxiliary") OutRole = EntryRole::Auxiliary;
    else return false;
    return true;
}

PackageWriteResult WriteDirectoryPackage(
    const PackageWriteRequest& Request)
{
    std::error_code ErrorCode;
    const std::filesystem::path Output =
        std::filesystem::absolute(Request.OutputDirectory, ErrorCode)
            .lexically_normal();
    if (Request.OutputDirectory.empty() || Request.Items.empty() || ErrorCode)
        return WriteFailure(Output, "output path and package items are required");
    if (Request.Items.size() > MaximumIndexedEntries)
        return WriteFailure(Output, "package exceeds the indexed-entry limit");

    if (std::filesystem::exists(Output, ErrorCode) || ErrorCode)
        return WriteFailure(Output, "output package already exists or cannot be queried");

    std::set<std::string> Keys;
    int ManifestCount = 0;
    for (const PackageSourceItem& Item : Request.Items)
    {
        std::string Error;
        if (!IsSafeRelativePath(Item.RelativePath, Error) ||
            !RoleMatchesPath(Item.Role, Item.RelativePath, Error))
        {
            return WriteFailure(
                Output, Error + ": " + Utf8Generic(Item.RelativePath));
        }
        if (PortableKey(Item.RelativePath) == LowerAscii(IntegrityFileName))
            return WriteFailure(Output, "integrity.tsv is reserved");
        if (!Keys.insert(PortableKey(Item.RelativePath)).second)
            return WriteFailure(Output, "package contains a portable-path collision");
        if (Item.Role == EntryRole::Manifest) ++ManifestCount;
        if (!std::filesystem::is_regular_file(Item.SourcePath, ErrorCode) ||
            ErrorCode)
        {
            return WriteFailure(
                Output, "package source is not a regular file: " +
                    Item.SourcePath.string());
        }
    }
    if (ManifestCount != 1)
        return WriteFailure(Output, "package requires exactly one manifest.json");

    const auto Nonce = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    const unsigned long long ShortNonce =
        static_cast<unsigned long long>(Nonce) & 0xFFFFFFFFULL;
    const std::filesystem::path Stage = Output.parent_path() /
        (".p-" + std::to_string(ShortNonce));
    if (!std::filesystem::create_directories(Stage, ErrorCode) || ErrorCode)
        return WriteFailure(Output, "failed to create package staging directory");

    PackageWriteResult Result;
    Result.OutputDirectory = Output;
    for (const PackageSourceItem& Item : Request.Items)
    {
        const std::filesystem::path Destination = Stage / Item.RelativePath;
        std::filesystem::create_directories(
            Destination.parent_path(), ErrorCode);
        if (ErrorCode ||
            !std::filesystem::copy_file(
                Item.SourcePath, Destination,
                std::filesystem::copy_options::none, ErrorCode) ||
            ErrorCode)
        {
            RemovePartial(Stage);
            return WriteFailure(Output, "failed to copy package payload");
        }

        IntegrityEntry Entry;
        Entry.Role = Item.Role;
        Entry.RelativePath = Item.RelativePath;
        Entry.ByteCount = std::filesystem::file_size(Destination, ErrorCode);
        std::string HashError;
        if (ErrorCode ||
            !Sha256File(Destination, Entry.Sha256, HashError))
        {
            RemovePartial(Stage);
            return WriteFailure(
                Output, HashError.empty()
                    ? "failed to size package payload" : HashError);
        }
        Result.Entries.push_back(std::move(Entry));
    }

    std::sort(
        Result.Entries.begin(), Result.Entries.end(),
        [](const IntegrityEntry& Left, const IntegrityEntry& Right)
        {
            return PortableKey(Left.RelativePath) <
                PortableKey(Right.RelativePath);
        });

    const std::filesystem::path IntegrityPath = Stage / IntegrityFileName;
    std::ofstream Integrity(IntegrityPath, std::ios::binary);
    Integrity << IntegrityMagic << '\n' << IntegrityHeader << '\n';
    for (const IntegrityEntry& Entry : Result.Entries)
    {
        Integrity << EntryRoleName(Entry.Role) << '\t'
                  << Utf8Generic(Entry.RelativePath) << '\t'
                  << Entry.ByteCount << '\t'
                  << Entry.Sha256 << '\n';
    }
    Integrity.flush();
    if (!Integrity)
    {
        Integrity.close();
        RemovePartial(Stage);
        return WriteFailure(Output, "failed to write integrity.tsv");
    }
    Integrity.close();

    const PackageInspectResult Inspection = InspectDirectoryPackage(Stage);
    if (!Inspection.Success)
    {
        const std::string Error = Inspection.Errors.empty()
            ? "staged package verification failed"
            : Inspection.Errors.front();
        RemovePartial(Stage);
        return WriteFailure(Output, Error);
    }

    std::filesystem::rename(Stage, Output, ErrorCode);
    if (ErrorCode)
    {
        RemovePartial(Stage);
        return WriteFailure(Output, "failed to atomically commit package directory");
    }
    Result.Success = true;
    return Result;
}

PackageInspectResult InspectDirectoryPackage(
    const std::filesystem::path& PackageDirectory,
    const PackageInspectOptions& Options)
{
    PackageInspectResult Result;
    std::error_code ErrorCode;
    const std::filesystem::path AbsolutePackageDirectory =
        std::filesystem::absolute(PackageDirectory, ErrorCode);
    if (PackageDirectory.empty() || ErrorCode)
    {
        Result.Errors.push_back("SKRV package path cannot be resolved");
        return Result;
    }
    Result.PackageDirectory = AbsolutePackageDirectory.lexically_normal();
    if (!std::filesystem::is_directory(Result.PackageDirectory, ErrorCode) ||
        ErrorCode)
    {
        Result.Errors.push_back("SKRV package directory does not exist");
        return Result;
    }
    detail::PackageInventory ActualInventory;
    std::size_t InventoryEntryCount = 0;
    const std::size_t InventoryEntryLimit = std::min(
        Options.InventoryEntryLimit,
        MaximumPackageInventoryEntries);
    if (InventoryEntryLimit == 0)
    {
        Result.Errors.push_back("package inventory limit must be positive");
        return Result;
    }
    std::filesystem::recursive_directory_iterator Iterator(
        Result.PackageDirectory,
        std::filesystem::directory_options::none,
        ErrorCode);
    const std::filesystem::recursive_directory_iterator End;
    if (ErrorCode)
    {
        Result.Errors.push_back("failed to begin package inventory");
        return Result;
    }
    while (Iterator != End)
    {
        ++InventoryEntryCount;
        if (InventoryEntryCount > InventoryEntryLimit)
        {
            Result.Errors.push_back(
                "package exceeds the filesystem inventory limit");
            return Result;
        }
        const std::filesystem::directory_entry DirectoryEntry = *Iterator;
        const std::filesystem::file_status Status =
            DirectoryEntry.symlink_status(ErrorCode);
        if (ErrorCode || std::filesystem::is_symlink(Status))
        {
            Result.Errors.push_back(
                "package contains a symlink or unreadable entry");
            return Result;
        }
        if (!std::filesystem::is_regular_file(Status) &&
            !std::filesystem::is_directory(Status))
        {
            Result.Errors.push_back("package contains a special filesystem entry");
            return Result;
        }
        const std::filesystem::path Relative =
            std::filesystem::relative(
                DirectoryEntry.path(), Result.PackageDirectory, ErrorCode);
        std::string RelativeError;
        if (ErrorCode || !IsSafeRelativePath(Relative, RelativeError))
        {
            Result.Errors.push_back(
                RelativeError.empty()
                    ? "failed to inventory package path" : RelativeError);
            return Result;
        }
        const std::string RelativeKey = PortableKey(Relative);
        if (!ActualInventory.Register(
                RelativeKey, std::filesystem::is_regular_file(Status)))
        {
            Result.Errors.push_back(
                "package contains a case-insensitive path collision: " +
                Utf8Generic(Relative));
            return Result;
        }
        Iterator.increment(ErrorCode);
        if (ErrorCode)
        {
            Result.Errors.push_back("failed while inventorying package files");
            return Result;
        }
    }
    const std::filesystem::path IntegrityPath =
        Result.PackageDirectory / IntegrityFileName;
    const std::uintmax_t IntegrityBytes =
        std::filesystem::file_size(IntegrityPath, ErrorCode);
    if (ErrorCode || IntegrityBytes > MaximumIntegrityIndexBytes)
    {
        Result.Errors.push_back(
            "integrity.tsv is missing or exceeds the size limit");
        return Result;
    }
    std::string IntegrityShapeError;
    if (!ValidateIntegrityResourceShape(
            IntegrityPath, IntegrityShapeError))
    {
        Result.Errors.push_back(IntegrityShapeError);
        return Result;
    }
    std::string IntegrityHashError;
    if (!Sha256File(
            IntegrityPath, Result.IntegrityIndexSha256,
            IntegrityHashError))
    {
        Result.Errors.push_back(IntegrityHashError);
        return Result;
    }
    std::ifstream Input(
        IntegrityPath,
        std::ios::binary);
    if (!Input)
    {
        Result.Errors.push_back("integrity.tsv is missing");
        return Result;
    }
    std::string Line;
    if (!std::getline(Input, Line) || Line != IntegrityMagic ||
        !std::getline(Input, Line) || Line != IntegrityHeader)
    {
        Result.Errors.push_back("integrity.tsv header is invalid");
        return Result;
    }

    std::set<std::string> Keys;
    std::string PreviousKey;
    int ManifestCount = 0;
    std::size_t LineNumber = 2;
    while (std::getline(Input, Line))
    {
        ++LineNumber;
        if (Result.Entries.size() >= MaximumIndexedEntries)
        {
            Result.Errors.push_back(
                "integrity.tsv exceeds the record-count limit");
            return Result;
        }
        if (!Line.empty() && Line.back() == '\r') Line.pop_back();
        if (Line.size() > MaximumIntegrityLineBytes)
        {
            Result.Errors.push_back(
                "integrity.tsv exceeds the line-length limit");
            return Result;
        }
        if (Line.empty())
        {
            Result.Errors.push_back(
                "integrity.tsv contains an empty record at line " +
                std::to_string(LineNumber));
            return Result;
        }
        const std::vector<std::string> Fields = SplitTabs(Line);
        IntegrityEntry Entry;
        if (Fields.size() != 4 ||
            !ParseEntryRole(Fields[0], Entry.Role) ||
            !ParseByteCount(Fields[2], Entry.ByteCount) ||
            !IsSha256Hex(Fields[3]))
        {
            Result.Errors.push_back(
                "integrity.tsv record is invalid at line " +
                std::to_string(LineNumber));
            return Result;
        }
        try
        {
            Entry.RelativePath = PathFromUtf8(Fields[1]);
        }
        catch (const std::exception&)
        {
            Result.Errors.push_back(
                "integrity.tsv path encoding is invalid at line " +
                std::to_string(LineNumber));
            return Result;
        }
        Entry.Sha256 = Fields[3];
        std::transform(
            Entry.Sha256.begin(), Entry.Sha256.end(),
            Entry.Sha256.begin(),
            [](const unsigned char Character)
            {
                return static_cast<char>(std::toupper(Character));
            });
        std::string PathError;
        if (!IsSafeRelativePath(Entry.RelativePath, PathError) ||
            !RoleMatchesPath(Entry.Role, Entry.RelativePath, PathError) ||
            PortableKey(Entry.RelativePath) == LowerAscii(IntegrityFileName))
        {
            Result.Errors.push_back(
                PathError + " at integrity line " +
                std::to_string(LineNumber));
            return Result;
        }
        const std::string Key = PortableKey(Entry.RelativePath);
        if (!Keys.insert(Key).second ||
            (!PreviousKey.empty() && Key <= PreviousKey))
        {
            Result.Errors.push_back(
                "integrity.tsv paths are duplicated or not canonically sorted");
            return Result;
        }
        PreviousKey = Key;
        if (Entry.Role == EntryRole::Manifest) ++ManifestCount;

        const std::filesystem::path FullPath =
            Result.PackageDirectory / Entry.RelativePath;
        const std::filesystem::file_status Status =
            std::filesystem::symlink_status(FullPath, ErrorCode);
        if (ErrorCode || std::filesystem::is_symlink(Status) ||
            !std::filesystem::is_regular_file(Status))
        {
            Result.Errors.push_back(
                "indexed package file is missing, non-regular, or a symlink: " +
                Utf8Generic(Entry.RelativePath));
            return Result;
        }
        const std::uintmax_t ActualSize =
            std::filesystem::file_size(FullPath, ErrorCode);
        if (ErrorCode || ActualSize != Entry.ByteCount)
        {
            Result.Errors.push_back(
                "package byte count mismatch: " +
                Utf8Generic(Entry.RelativePath));
            return Result;
        }
        if (Options.VerifyHashes)
        {
            std::string ActualHash;
            std::string HashError;
            if (!Sha256File(FullPath, ActualHash, HashError) ||
                ActualHash != Entry.Sha256)
            {
                Result.Errors.push_back(
                    HashError.empty()
                        ? "package SHA-256 mismatch: " +
                            Utf8Generic(Entry.RelativePath)
                        : HashError);
                return Result;
            }
        }
        if (Entry.Role == EntryRole::Manifest)
            Result.ManifestSha256 = Entry.Sha256;
        Result.Entries.push_back(std::move(Entry));
    }
    if (!Input.eof())
    {
        Result.Errors.push_back("failed while reading integrity.tsv");
        return Result;
    }
    if (Result.Entries.empty() || ManifestCount != 1)
    {
        Result.Errors.push_back("package requires exactly one indexed manifest");
        return Result;
    }

    std::string Manifest;
    std::string ReadError;
    const auto ManifestEntry = std::find_if(
        Result.Entries.begin(), Result.Entries.end(),
        [](const IntegrityEntry& Entry)
        {
            return Entry.Role == EntryRole::Manifest;
        });
    if (ManifestEntry == Result.Entries.end() ||
        ManifestEntry->ByteCount > MaximumManifestBytes)
    {
        Result.Errors.push_back("manifest is missing or exceeds the size limit");
        return Result;
    }
    if (!ReadWholeFile(
            Result.PackageDirectory / ManifestFileName,
            Manifest, ReadError))
    {
        Result.Errors.push_back(ReadError);
        return Result;
    }
    if (!ValidateManifestJson(
            Manifest, Result.Entries,
            Result.Manifest, Result.Errors))
    {
        return Result;
    }
    if (Options.RejectUnindexedFiles)
    {
        for (const std::string& Key : ActualInventory.GetRegularFiles())
        {
            if (Key != LowerAscii(IntegrityFileName) &&
                Keys.find(Key) == Keys.end())
            {
                Result.Errors.push_back(
                    "package contains an unindexed file: " + Key);
                return Result;
            }
        }
    }

    Result.Success = true;
    return Result;
}
} // namespace skrtg::viewer::skrv
