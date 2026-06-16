#ifndef OPENMW_COMPONENTS_NETWORK_NETWORKMANAGER_H
#define OPENMW_COMPONENTS_NETWORK_NETWORKMANAGER_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Net
{
    using PeerId = std::uint32_t;

    struct NetworkManagerImpl; // defined in the .cpp (pimpl)

    // A received (or to-be-dispatched) message. `data` is an opaque payload — the
    // caller decides its meaning (the Lua binding stores LuaUtil::serialize output).
    struct Message
    {
        PeerId mPeer;
        std::string mEvent;
        std::string mData;
    };

    // Minimal framed-TCP transport for the multiplayer layer. Engine-agnostic: it moves
    // opaque (event, data) messages between a server and clients and knows nothing about
    // Lua or the game. Runs a Boost.Asio io_context on its own thread; the game thread only
    // touches the lock-guarded inbound queue (poll) and posts outbound work (send).
    //
    // Wire frame: [uint32 bodyLen][uint16 eventLen][event bytes][data bytes], bodyLen and
    // eventLen big-endian. One NetworkManager is either a server (listen) or a client
    // (connect), never both.
    class NetworkManager
    {
    public:
        NetworkManager();
        ~NetworkManager();

        NetworkManager(const NetworkManager&) = delete;
        NetworkManager& operator=(const NetworkManager&) = delete;

        // Start accepting clients on the port (server role). Throws on bind failure.
        void listen(std::uint16_t port);

        // Connect to a server (client role). Non-blocking; connection completes on the
        // network thread. Throws on immediate resolve failure.
        void connect(const std::string& host, std::uint16_t port);

        // Queue a message to one peer (server: a client; client: the server, peer ignored).
        void send(PeerId peer, std::string_view event, std::string_view data);
        // Queue a message to every connected peer.
        void broadcast(std::string_view event, std::string_view data);

        // Drain everything received since the last call (call from the game/Lua thread).
        std::vector<Message> poll();

        bool isServer() const { return mIsServer; }
        std::vector<PeerId> peers() const;

    private:
        std::unique_ptr<NetworkManagerImpl> mImpl;
        std::atomic<bool> mIsServer{ false };
    };
}

#endif
