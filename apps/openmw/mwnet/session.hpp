#ifndef OPENMW_MWNET_SESSION_H
#define OPENMW_MWNET_SESSION_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "message.hpp"

namespace MWNet
{
    class ISessionTransport;
    class NetworkServer;
    class NetworkTransport;

    /// Identity of a peer within a session. 0 is reserved for the local authority
    /// (the host, or "self" over loopback); connected clients are numbered from 1.
    using PeerId = std::uint32_t;

    inline constexpr PeerId sLocalPeer = 0;

    /// A message received from a specific peer this tick.
    struct ReceivedMessage
    {
        PeerId mFrom;
        Message mMessage;
    };

    /// The multi-peer layer above ISessionTransport. The engine talks to one of
    /// these regardless of role: single-player loops back to itself, a host fans a
    /// message out to every connected client, a client sends to its host. This is
    /// what lets the same pumpTransport code drive 0, 1, or N peers.
    class Session
    {
    public:
        virtual ~Session() = default;

        /// Send a message to every connected peer.
        virtual void broadcast(const Message& message) = 0;

        /// Send a message to a single peer (used for the per-client login handshake:
        /// character lists and accept replies are addressed, not fanned out). For a
        /// client or loopback — which have only one peer (the host, or self) — this is
        /// equivalent to broadcast and the peer id is ignored.
        virtual void sendTo(PeerId peer, const Message& message) = 0;

        /// Accept any newly-joined peers, pump I/O, and return everything received
        /// this tick, each tagged with the peer it came from.
        virtual std::vector<ReceivedMessage> poll() = 0;

        /// Peers whose connection went away since the last call (detected during poll). The host
        /// uses this to clean up a departed client's player; loopback and clients report none.
        virtual std::vector<PeerId> takeDisconnected() { return {}; }

        /// Number of connected remote peers (0 for loopback, or a host nobody has
        /// joined yet).
        virtual std::size_t peerCount() const = 0;

        /// True if received state comes from an authority this peer should obey (a
        /// client obeys its host). False for loopback (own echo — never apply) and for
        /// a host (a client is not authoritative over the world; server-authoritative
        /// validation of client input is a later step).
        virtual bool receivesAuthoritativeState() const { return false; }

        /// True if this peer is the authority over the shared world (the host) and so
        /// resolves clients' reported actions. False for loopback and clients.
        virtual bool isAuthority() const { return false; }
    };

    /// Single-player / integrated: one in-process loopback peer (self).
    class LoopbackSession final : public Session
    {
    public:
        LoopbackSession();
        ~LoopbackSession() override;

        void broadcast(const Message& message) override;
        void sendTo(PeerId peer, const Message& message) override;
        std::vector<ReceivedMessage> poll() override;
        std::size_t peerCount() const override;

    private:
        std::unique_ptr<ISessionTransport> mTransport;
    };

    /// Host / dedicated: listen for clients and fan out to all of them.
    class HostSession final : public Session
    {
    public:
        explicit HostSession(std::uint16_t port);
        ~HostSession() override;

        std::uint16_t port() const;

        void broadcast(const Message& message) override;
        void sendTo(PeerId peer, const Message& message) override;
        std::vector<ReceivedMessage> poll() override;
        std::vector<PeerId> takeDisconnected() override;
        std::size_t peerCount() const override;
        bool isAuthority() const override { return true; }

    private:
        struct Client
        {
            PeerId mId;
            std::unique_ptr<NetworkTransport> mTransport;
        };

        std::unique_ptr<NetworkServer> mServer;
        std::vector<Client> mClients;
        std::vector<PeerId> mDisconnected;
        PeerId mNextPeerId = 1;
    };

    /// Client: a single connection to the host. Returns nullptr-wrapped on failure
    /// (isConnected() == false). The host is always peer 0.
    class ClientSession final : public Session
    {
    public:
        static std::unique_ptr<ClientSession> connect(const std::string& host, std::uint16_t port);

        explicit ClientSession(std::unique_ptr<NetworkTransport> transport);
        ~ClientSession() override;

        bool isConnected() const;

        void broadcast(const Message& message) override;
        void sendTo(PeerId peer, const Message& message) override;
        std::vector<ReceivedMessage> poll() override;
        std::size_t peerCount() const override;
        bool receivesAuthoritativeState() const override { return true; }

    private:
        std::unique_ptr<NetworkTransport> mTransport;
    };
}

#endif
