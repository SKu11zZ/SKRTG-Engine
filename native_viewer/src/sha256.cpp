#include "skrtg/viewer/skrv/sha256.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace skrtg::viewer::skrv
{
namespace
{
constexpr std::array<std::uint32_t, 64> RoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

constexpr std::uint32_t RotateRight(
    const std::uint32_t Value,
    const unsigned Count)
{
    return (Value >> Count) | (Value << (32U - Count));
}

class Sha256State
{
public:
    void Update(const std::span<const std::byte> Bytes)
    {
        for (const std::byte Byte : Bytes)
        {
            Buffer_[BufferSize_++] =
                static_cast<std::uint8_t>(Byte);
            ++TotalBytes_;
            if (BufferSize_ == Buffer_.size())
            {
                Transform(Buffer_.data());
                BufferSize_ = 0;
            }
        }
    }

    std::array<std::uint8_t, 32> Finalize()
    {
        const std::uint64_t TotalBits = TotalBytes_ * 8U;
        Buffer_[BufferSize_++] = 0x80U;

        if (BufferSize_ > 56)
        {
            while (BufferSize_ < Buffer_.size())
                Buffer_[BufferSize_++] = 0;
            Transform(Buffer_.data());
            BufferSize_ = 0;
        }
        while (BufferSize_ < 56)
            Buffer_[BufferSize_++] = 0;
        for (int Shift = 56; Shift >= 0; Shift -= 8)
        {
            Buffer_[BufferSize_++] = static_cast<std::uint8_t>(
                (TotalBits >> Shift) & 0xffU);
        }
        Transform(Buffer_.data());

        std::array<std::uint8_t, 32> Digest{};
        for (std::size_t Index = 0; Index < State_.size(); ++Index)
        {
            Digest[Index * 4] = static_cast<std::uint8_t>(
                State_[Index] >> 24U);
            Digest[Index * 4 + 1] = static_cast<std::uint8_t>(
                State_[Index] >> 16U);
            Digest[Index * 4 + 2] = static_cast<std::uint8_t>(
                State_[Index] >> 8U);
            Digest[Index * 4 + 3] = static_cast<std::uint8_t>(
                State_[Index]);
        }
        return Digest;
    }

private:
    void Transform(const std::uint8_t* Block)
    {
        std::array<std::uint32_t, 64> Words{};
        for (std::size_t Index = 0; Index < 16; ++Index)
        {
            const std::size_t Offset = Index * 4;
            Words[Index] =
                (static_cast<std::uint32_t>(Block[Offset]) << 24U) |
                (static_cast<std::uint32_t>(Block[Offset + 1]) << 16U) |
                (static_cast<std::uint32_t>(Block[Offset + 2]) << 8U) |
                static_cast<std::uint32_t>(Block[Offset + 3]);
        }
        for (std::size_t Index = 16; Index < Words.size(); ++Index)
        {
            const std::uint32_t S0 =
                RotateRight(Words[Index - 15], 7) ^
                RotateRight(Words[Index - 15], 18) ^
                (Words[Index - 15] >> 3U);
            const std::uint32_t S1 =
                RotateRight(Words[Index - 2], 17) ^
                RotateRight(Words[Index - 2], 19) ^
                (Words[Index - 2] >> 10U);
            Words[Index] = Words[Index - 16] + S0 +
                Words[Index - 7] + S1;
        }

        std::uint32_t A = State_[0];
        std::uint32_t B = State_[1];
        std::uint32_t C = State_[2];
        std::uint32_t D = State_[3];
        std::uint32_t E = State_[4];
        std::uint32_t F = State_[5];
        std::uint32_t G = State_[6];
        std::uint32_t H = State_[7];

        for (std::size_t Index = 0; Index < Words.size(); ++Index)
        {
            const std::uint32_t Sum1 =
                RotateRight(E, 6) ^ RotateRight(E, 11) ^
                RotateRight(E, 25);
            const std::uint32_t Choice = (E & F) ^ ((~E) & G);
            const std::uint32_t Temporary1 =
                H + Sum1 + Choice + RoundConstants[Index] + Words[Index];
            const std::uint32_t Sum0 =
                RotateRight(A, 2) ^ RotateRight(A, 13) ^
                RotateRight(A, 22);
            const std::uint32_t Majority =
                (A & B) ^ (A & C) ^ (B & C);
            const std::uint32_t Temporary2 = Sum0 + Majority;
            H = G;
            G = F;
            F = E;
            E = D + Temporary1;
            D = C;
            C = B;
            B = A;
            A = Temporary1 + Temporary2;
        }

        State_[0] += A;
        State_[1] += B;
        State_[2] += C;
        State_[3] += D;
        State_[4] += E;
        State_[5] += F;
        State_[6] += G;
        State_[7] += H;
    }

    std::array<std::uint32_t, 8> State_ = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::uint8_t, 64> Buffer_{};
    std::size_t BufferSize_ = 0;
    std::uint64_t TotalBytes_ = 0;
};

std::string ToHex(const std::array<std::uint8_t, 32>& Digest)
{
    std::ostringstream Output;
    Output << std::uppercase << std::hex << std::setfill('0');
    for (const std::uint8_t Byte : Digest)
        Output << std::setw(2) << static_cast<unsigned>(Byte);
    return Output.str();
}
} // namespace

std::string Sha256(const std::span<const std::byte> Bytes)
{
    Sha256State State;
    State.Update(Bytes);
    return ToHex(State.Finalize());
}

bool Sha256File(
    const std::filesystem::path& Path,
    std::string& OutUpperHexDigest,
    std::string& OutError)
{
    OutUpperHexDigest.clear();
    OutError.clear();
    std::ifstream Input(Path, std::ios::binary);
    if (!Input)
    {
        OutError = "failed to open file for SHA-256: " + Path.string();
        return false;
    }

    Sha256State State;
    std::array<char, 64 * 1024> Buffer{};
    while (Input)
    {
        Input.read(Buffer.data(),
                   static_cast<std::streamsize>(Buffer.size()));
        const std::streamsize Count = Input.gcount();
        if (Count > 0)
        {
            State.Update(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(Buffer.data()),
                static_cast<std::size_t>(Count)));
        }
    }
    if (!Input.eof())
    {
        OutError = "failed while hashing file: " + Path.string();
        return false;
    }
    OutUpperHexDigest = ToHex(State.Finalize());
    return true;
}

bool Sha256StreamRange(
    std::istream& Input,
    const std::uint64_t ByteCount,
    std::string& OutUpperHexDigest,
    std::string& OutError)
{
    OutUpperHexDigest.clear();
    OutError.clear();
    Sha256State State;
    std::array<char, 64 * 1024> Buffer{};
    std::uint64_t Remaining = ByteCount;
    while (Remaining > 0)
    {
        const std::size_t Requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(Remaining, Buffer.size()));
        Input.read(
            Buffer.data(), static_cast<std::streamsize>(Requested));
        const std::streamsize Count = Input.gcount();
        if (Count != static_cast<std::streamsize>(Requested))
        {
            OutError =
                "stream ended before the declared byte range";
            return false;
        }
        State.Update(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(Buffer.data()),
            Requested));
        Remaining -= Requested;
    }
    OutUpperHexDigest = ToHex(State.Finalize());
    return true;
}
} // namespace skrtg::viewer::skrv
