#ifndef OPENMW_MWNET_NETWORKTRANSPORT_H
#define OPENMW_MWNET_NETWORKTRANSPORT_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sessiontransport.hpp"

namespace MWNet
{
    /// A real TCP implementation of the session transport, behind the same
    /// ISessionTransport seam the loopback uses. Messages are length-prefixed and
    /// tagged with their channel; I/O is non-blocking (update() never blocks the
    /// game loop). For this first cut both channels ride one ordered TCP stream —
    /// the Unreliable channel therefore gets reliable, head-of-line-blocked
    /// delivery; a UDP path for it is a later optimization. Asio lives entirely in
    /// the .cpp (PIMPL) so its heavy headers don't leak into the rest of the build.
    class NetworkTransport final : public ISessionTransport
    {
    public:
        struct Impl;

        explicit NetworkTransport(std::unique_ptr<Impl> impl);
        ~NetworkTransport() override;

        /// Connect to a listening host (blocking). Returns nullptr on failure.
        static std::unique_ptr<NetworkTransport> connect(const std::string& host, std::uint16_t port);

        void send(Message message) override;
        void update() override;
        std::vector<Message> receive() override;
        bool isConnected() const override;

    private:
        std::unique_ptr<Impl> mImpl;
    };

    /// Listens for incoming TCP connections and hands out a NetworkTransport per
    /// accepted client. poll() is non-blocking; the host calls it each tick to
    /// pick up newly-joined clients.
    class NetworkServer
    {
    public:
        struct Impl;

        explicit NetworkServer(std::uint16_t port);
        ~NetworkServer();

        /// The bound port (useful when constructed with port 0 = auto-assign).
        std::uint16_t port() const;

        /// Accept all currently-pending connections, returning a transport for each.
        std::vector<std::unique_ptr<NetworkTransport>> poll();

    private:
        std::unique_ptr<Impl> mImpl;
    };
}

#endif
