#include "control.hpp"

#include "bytestream.hpp"

namespace MWNet
{
    namespace
    {
        // Wire format version. Bumped if any layout below changes incompatibly.
        constexpr std::uint8_t sVersion = 3;

        void writeBody(ByteWriter& writer, const LoginRequest& message)
        {
            writer.writeString(message.mUsername);
            writer.write(static_cast<std::uint32_t>(message.mContentFiles.size()));
            for (const std::string& file : message.mContentFiles)
                writer.writeString(file);
        }

        void writeBody(ByteWriter& writer, const CharacterList& message)
        {
            writer.write(static_cast<std::uint32_t>(message.mCharacters.size()));
            for (const CharacterInfo& character : message.mCharacters)
            {
                writer.write(character.mId);
                writer.writeString(character.mName);
                writer.write(character.mLevel);
                writer.writeString(character.mCell);
            }
        }

        void writeBody(ByteWriter& writer, const SelectCharacter& message) { writer.write(message.mId); }

        void writeBody(ByteWriter&, const CreateNew&) {}

        void writeBody(ByteWriter& writer, const CharacterData& message)
        {
            writer.write(message.mId);
            writer.writeString(message.mBlob);
        }

        void writeBody(ByteWriter& writer, const LoginAccept& message)
        {
            writer.write(message.mNetId.mIndex);
            writer.write(message.mNetId.mContentFile);
        }

        void writeBody(ByteWriter& writer, const LoginReject& message) { writer.writeString(message.mReason); }

        void writeBody(ByteWriter& writer, const WorldJournal& message) { writer.writeString(message.mBlob); }

        void writeBody(ByteWriter& writer, const CellStateRequest& message) { writer.writeString(message.mCellId); }

        void writeBody(ByteWriter& writer, const CellStateData& message)
        {
            writer.writeString(message.mCellId);
            writer.writeString(message.mBlob);
        }

        // Read a length-prefixed list of strings, validating the count against the buffer first (each
        // string is at least its 4-byte length prefix).
        [[nodiscard]] bool readStringList(ByteReader& reader, std::vector<std::string>& out)
        {
            std::uint32_t count = 0;
            if (!reader.read(count) || count > reader.remaining() / sizeof(std::uint32_t))
                return false;
            out.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                std::string value;
                if (!reader.readString(value))
                    return false;
                out.push_back(std::move(value));
            }
            return true;
        }
    }

    std::vector<std::byte> serializeControl(const ControlMessage& message)
    {
        std::vector<std::byte> out;
        ByteWriter writer(out);
        writer.write(sVersion);
        writer.write(static_cast<std::uint8_t>(message.index()));
        std::visit([&](const auto& body) { writeBody(writer, body); }, message);
        return out;
    }

    std::optional<ControlMessage> deserializeControl(std::span<const std::byte> data)
    {
        ByteReader reader(data);

        std::uint8_t version = 0;
        if (!reader.read(version) || version != sVersion)
            return std::nullopt;

        std::uint8_t type = 0;
        if (!reader.read(type))
            return std::nullopt;

        switch (type)
        {
            case 0: // LoginRequest
            {
                LoginRequest message;
                if (!reader.readString(message.mUsername) || !readStringList(reader, message.mContentFiles))
                    return std::nullopt;
                return message;
            }
            case 1: // CharacterList
            {
                CharacterList message;
                std::uint32_t count = 0;
                // Smallest CharacterInfo: id + name-len + level + cell-len = 16 bytes.
                if (!reader.read(count) || count > reader.remaining() / 16)
                    return std::nullopt;
                message.mCharacters.reserve(count);
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    CharacterInfo character;
                    if (!reader.read(character.mId) || !reader.readString(character.mName)
                        || !reader.read(character.mLevel) || !reader.readString(character.mCell))
                        return std::nullopt;
                    message.mCharacters.push_back(std::move(character));
                }
                return message;
            }
            case 2: // SelectCharacter
            {
                SelectCharacter message;
                if (!reader.read(message.mId))
                    return std::nullopt;
                return message;
            }
            case 3: // CreateNew
                return CreateNew{};
            case 4: // CharacterData
            {
                CharacterData message;
                if (!reader.read(message.mId) || !reader.readString(message.mBlob))
                    return std::nullopt;
                return message;
            }
            case 5: // LoginAccept
            {
                LoginAccept message;
                if (!reader.read(message.mNetId.mIndex) || !reader.read(message.mNetId.mContentFile))
                    return std::nullopt;
                return message;
            }
            case 6: // LoginReject
            {
                LoginReject message;
                if (!reader.readString(message.mReason))
                    return std::nullopt;
                return message;
            }
            case 7: // WorldJournal
            {
                WorldJournal message;
                if (!reader.readString(message.mBlob))
                    return std::nullopt;
                return message;
            }
            case 8: // CellStateRequest
            {
                CellStateRequest message;
                if (!reader.readString(message.mCellId))
                    return std::nullopt;
                return message;
            }
            case 9: // CellStateData
            {
                CellStateData message;
                if (!reader.readString(message.mCellId) || !reader.readString(message.mBlob))
                    return std::nullopt;
                return message;
            }
            default:
                return std::nullopt; // unknown type from a newer/hostile peer
        }
    }
}
