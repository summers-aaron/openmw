#include "session.hpp"

#include <algorithm>
#include <utility>

#include "loopbacktransport.hpp"
#include "networktransport.hpp"

namespace MWNet
{
    // --- LoopbackSession -----------------------------------------------------

    LoopbackSession::LoopbackSession()
        : mTransport(std::make_unique<LoopbackTransport>())
    {
    }

    LoopbackSession::~LoopbackSession() = default;

    void LoopbackSession::broadcast(const Message& message)
    {
        mTransport->send(message);
    }

    void LoopbackSession::sendTo(PeerId, const Message& message)
    {
        mTransport->send(message); // one peer (self) — addressing is moot
    }

    std::vector<ReceivedMessage> LoopbackSession::poll()
    {
        mTransport->update();
        std::vector<ReceivedMessage> received;
        for (Message& message : mTransport->receive())
            received.push_back({ sLocalPeer, std::move(message) });
        return received;
    }

    std::size_t LoopbackSession::peerCount() const
    {
        return 0;
    }

    // --- HostSession ---------------------------------------------------------

    HostSession::HostSession(std::uint16_t port)
        : mServer(std::make_unique<NetworkServer>(port))
    {
    }

    HostSession::~HostSession() = default;

    std::uint16_t HostSession::port() const
    {
        return mServer->port();
    }

    void HostSession::broadcast(const Message& message)
    {
        for (Client& client : mClients)
            client.mTransport->send(message);
    }

    void HostSession::sendTo(PeerId peer, const Message& message)
    {
        for (Client& client : mClients)
            if (client.mId == peer)
            {
                client.mTransport->send(message);
                return;
            }
    }

    std::vector<ReceivedMessage> HostSession::poll()
    {
        // Pick up newly-joined clients.
        for (std::unique_ptr<NetworkTransport>& transport : mServer->poll())
            mClients.push_back({ mNextPeerId++, std::move(transport) });

        std::vector<ReceivedMessage> received;
        for (Client& client : mClients)
        {
            client.mTransport->update();
            for (Message& message : client.mTransport->receive())
                received.push_back({ client.mId, std::move(message) });
        }

        // Drop clients whose connection has gone away, remembering who left so the engine can
        // clean up their player (takeDisconnected).
        for (const Client& client : mClients)
            if (!client.mTransport->isConnected())
                mDisconnected.push_back(client.mId);
        std::erase_if(mClients, [](const Client& client) { return !client.mTransport->isConnected(); });

        return received;
    }

    std::vector<PeerId> HostSession::takeDisconnected()
    {
        return std::exchange(mDisconnected, {});
    }

    std::size_t HostSession::peerCount() const
    {
        return mClients.size();
    }

    // --- ClientSession -------------------------------------------------------

    std::unique_ptr<ClientSession> ClientSession::connect(const std::string& host, std::uint16_t port)
    {
        std::unique_ptr<NetworkTransport> transport = NetworkTransport::connect(host, port);
        if (!transport)
            return nullptr;
        return std::make_unique<ClientSession>(std::move(transport));
    }

    ClientSession::ClientSession(std::unique_ptr<NetworkTransport> transport)
        : mTransport(std::move(transport))
    {
    }

    ClientSession::~ClientSession() = default;

    bool ClientSession::isConnected() const
    {
        return mTransport && mTransport->isConnected();
    }

    void ClientSession::broadcast(const Message& message)
    {
        mTransport->send(message);
    }

    void ClientSession::sendTo(PeerId, const Message& message)
    {
        mTransport->send(message); // one peer (the host) — addressing is moot
    }

    std::vector<ReceivedMessage> ClientSession::poll()
    {
        mTransport->update();
        std::vector<ReceivedMessage> received;
        for (Message& message : mTransport->receive())
            received.push_back({ sLocalPeer, std::move(message) });
        return received;
    }

    std::size_t ClientSession::peerCount() const
    {
        return isConnected() ? 1 : 0;
    }
}
