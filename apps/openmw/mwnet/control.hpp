#ifndef OPENMW_MWNET_CONTROL_H
#define OPENMW_MWNET_CONTROL_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <components/esm3/refnum.hpp>

namespace MWNet
{
    /// The login / character-selection handshake. These ride the Reliable channel under the
    /// sKindControl tag and are exchanged once, when a client connects, before it enters the shared
    /// world. Single-player / loopback never emits them (they are gated on a networked session).

    /// client -> host: "this is who I am". mContentFiles is the client's load order, so the host can
    /// reject a mismatch before it tries to deserialize a character built against different data.
    struct LoginRequest
    {
        std::string mUsername;
        std::vector<std::string> mContentFiles;

        friend bool operator==(const LoginRequest&, const LoginRequest&) = default;
    };

    /// host -> client: the characters registered to this login, for the client to choose from.
    struct CharacterInfo
    {
        std::uint32_t mId; // stable per-character id (the persisted player slot index)
        std::string mName;
        std::uint32_t mLevel;
        std::string mCell;

        friend bool operator==(const CharacterInfo&, const CharacterInfo&) = default;
    };
    struct CharacterList
    {
        std::vector<CharacterInfo> mCharacters;

        friend bool operator==(const CharacterList&, const CharacterList&) = default;
    };

    /// client -> host: resume this existing character.
    struct SelectCharacter
    {
        std::uint32_t mId;

        friend bool operator==(const SelectCharacter&, const SelectCharacter&) = default;
    };

    /// client -> host: start a brand-new character (client runs chargen, then uploads).
    struct CreateNew
    {
        friend bool operator==(const CreateNew&, const CreateNew&) = default;
    };

    /// A full character as an opaque blob: an ESM::Player record serialized with a minimal save
    /// header (see StateManager). Sent host -> client to serve a stored character, and client -> host
    /// to upload the live character so the server's save stays complete.
    struct CharacterData
    {
        std::uint32_t mId;
        std::string mBlob;

        friend bool operator==(const CharacterData&, const CharacterData&) = default;
    };

    /// host -> client: login/selection succeeded; here is your stable network id.
    struct LoginAccept
    {
        ESM::RefNum mNetId;

        friend bool operator==(const LoginAccept&, const LoginAccept&) = default;
    };

    /// host -> client: login refused (e.g. content-file mismatch, unknown character).
    struct LoginReject
    {
        std::string mReason;

        friend bool operator==(const LoginReject&, const LoginReject&) = default;
    };

    using ControlMessage = std::variant<LoginRequest, CharacterList, SelectCharacter, CreateNew,
        CharacterData, LoginAccept, LoginReject>;

    std::vector<std::byte> serializeControl(const ControlMessage& message);

    /// Parse a control message from arbitrary bytes. Returns std::nullopt on any malformed input
    /// (unknown type, truncated field, over-long string) — validated the same way as the other
    /// codecs, so it never crashes or over-allocates on hostile data.
    std::optional<ControlMessage> deserializeControl(std::span<const std::byte> data);
}

#endif
