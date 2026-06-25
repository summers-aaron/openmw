#ifndef OPENMW_MWNET_BYTESTREAM_H
#define OPENMW_MWNET_BYTESTREAM_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace MWNet
{
    /// Append-only writer for the wire codecs. Values are written in host byte
    /// order — correct for the loopback (same process) and the in-process
    /// server/client split; the real network transport (M11) normalizes byte
    /// order across heterogeneous hosts.
    class ByteWriter
    {
        std::vector<std::byte>& mOut;

    public:
        explicit ByteWriter(std::vector<std::byte>& out)
            : mOut(out)
        {
        }

        template <class T>
        void write(const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            const auto* bytes = reinterpret_cast<const std::byte*>(&value);
            mOut.insert(mOut.end(), bytes, bytes + sizeof(T));
        }

        void writeString(std::string_view value)
        {
            write(static_cast<std::uint32_t>(value.size()));
            const auto* bytes = reinterpret_cast<const std::byte*>(value.data());
            mOut.insert(mOut.end(), bytes, bytes + value.size());
        }
    };

    /// Bounds-checked reader. Every read is validated against the remaining
    /// buffer, so parsing hostile/corrupt input can fail cleanly but never
    /// crashes, over-reads, or over-allocates.
    class ByteReader
    {
        std::span<const std::byte> mData;
        std::size_t mPos = 0;

    public:
        explicit ByteReader(std::span<const std::byte> data)
            : mData(data)
        {
        }

        // mPos never exceeds size(), so size() - mPos cannot underflow.
        std::size_t remaining() const { return mData.size() - mPos; }

        template <class T>
        [[nodiscard]] bool read(T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            if (sizeof(T) > remaining())
                return false;
            std::memcpy(&value, mData.data() + mPos, sizeof(T));
            mPos += sizeof(T);
            return true;
        }

        [[nodiscard]] bool readString(std::string& out)
        {
            std::uint32_t length = 0;
            if (!read(length))
                return false;
            if (length > remaining())
                return false;
            out.assign(reinterpret_cast<const char*>(mData.data() + mPos), length);
            mPos += length;
            return true;
        }
    };
}

#endif
