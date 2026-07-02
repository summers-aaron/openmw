#include "networktransport.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <components/debug/debuglog.hpp>

namespace MWNet
{
    namespace
    {
        using boost::asio::ip::tcp;

        // Frame header: [channel:1][payloadLength:4]. A peer-supplied length above
        // this cap is treated as a protocol violation and drops the connection,
        // rather than buffering unboundedly.
        constexpr std::size_t sHeaderSize = 5;
        constexpr std::uint32_t sMaxPayload = 16u * 1024u * 1024u;

        std::uint8_t channelToByte(Channel channel)
        {
            return channel == Channel::Reliable ? 0 : 1;
        }

        Channel channelFromByte(std::uint8_t value)
        {
            return value == 0 ? Channel::Reliable : Channel::Unreliable;
        }
    }

    struct NetworkTransport::Impl
    {
        std::shared_ptr<boost::asio::io_context> mIo;
        tcp::socket mSocket;
        std::vector<std::byte> mOutgoing; // framed bytes awaiting write
        std::vector<std::byte> mIncoming; // bytes read but not yet a complete frame
        std::vector<Message> mReceived; // deframed messages awaiting receive()
        bool mConnected = true;

        Impl(std::shared_ptr<boost::asio::io_context> io, tcp::socket socket)
            : mIo(std::move(io))
            , mSocket(std::move(socket))
        {
            boost::system::error_code ec;
            mSocket.non_blocking(true, ec);
        }

        void flushWrites()
        {
            while (!mOutgoing.empty())
            {
                boost::system::error_code ec;
                const std::size_t written = mSocket.write_some(boost::asio::buffer(mOutgoing), ec);
                if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again)
                    break;
                if (ec)
                {
                    mConnected = false;
                    return;
                }
                mOutgoing.erase(mOutgoing.begin(), mOutgoing.begin() + written);
            }
        }

        void fillReads()
        {
            std::array<std::byte, 8192> buffer;
            while (true)
            {
                boost::system::error_code ec;
                const std::size_t read = mSocket.read_some(boost::asio::buffer(buffer), ec);
                if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again)
                    break;
                if (ec)
                {
                    // eof / reset / etc. — the peer is gone.
                    mConnected = false;
                    break;
                }
                if (read == 0)
                    break;
                mIncoming.insert(mIncoming.end(), buffer.begin(), buffer.begin() + read);
            }
        }

        void deframe()
        {
            std::size_t offset = 0;
            while (mIncoming.size() - offset >= sHeaderSize)
            {
                const std::byte* header = mIncoming.data() + offset;
                std::uint32_t length = 0;
                std::memcpy(&length, header + 1, sizeof(length));
                if (length > sMaxPayload)
                {
                    // A garbage length means the reliable byte stream has desynced (the frame this
                    // header claims can't be real). Log the header bytes and channel so a recurrence
                    // is diagnosable rather than a bare "dropped connection".
                    char header16[3 * 16 + 1] = { 0 };
                    for (std::size_t k = offset, w = 0; k < mIncoming.size() && k < offset + 16; ++k, w += 3)
                        std::snprintf(header16 + w, 4, "%02x ", static_cast<unsigned>(static_cast<std::uint8_t>(mIncoming[k])));
                    Log(Debug::Warning) << "NetworkTransport: peer sent oversized frame (" << length
                                        << " bytes) on channel " << static_cast<int>(static_cast<std::uint8_t>(header[0]))
                                        << "; stream desynced (buffered=" << (mIncoming.size() - offset)
                                        << ", bytes: " << header16 << "); dropping connection";
                    mConnected = false;
                    mIncoming.clear();
                    return;
                }
                if (mIncoming.size() - offset < sHeaderSize + length)
                    break; // frame not fully arrived yet

                Message message;
                message.mChannel = channelFromByte(static_cast<std::uint8_t>(header[0]));
                message.mPayload.assign(header + sHeaderSize, header + sHeaderSize + length);
                mReceived.push_back(std::move(message));
                offset += sHeaderSize + length;
            }
            if (offset > 0)
                mIncoming.erase(mIncoming.begin(), mIncoming.begin() + offset);
        }
    };

    NetworkTransport::NetworkTransport(std::unique_ptr<Impl> impl)
        : mImpl(std::move(impl))
    {
    }

    NetworkTransport::~NetworkTransport() = default;

    std::unique_ptr<NetworkTransport> NetworkTransport::connect(const std::string& host, std::uint16_t port)
    {
        try
        {
            auto io = std::make_shared<boost::asio::io_context>();
            tcp::socket socket(*io);
            tcp::resolver resolver(*io);
            boost::asio::connect(socket, resolver.resolve(host, std::to_string(port)));
            return std::make_unique<NetworkTransport>(std::make_unique<Impl>(io, std::move(socket)));
        }
        catch (const std::exception& e)
        {
            Log(Debug::Error) << "NetworkTransport: failed to connect to " << host << ":" << port << ": " << e.what();
            return nullptr;
        }
    }

    void NetworkTransport::send(Message message)
    {
        if (message.mPayload.size() > sMaxPayload)
        {
            Log(Debug::Error) << "NetworkTransport: refusing to send oversized message (" << message.mPayload.size()
                              << " bytes)";
            return;
        }
        const std::uint8_t channel = channelToByte(message.mChannel);
        const std::uint32_t length = static_cast<std::uint32_t>(message.mPayload.size());
        mImpl->mOutgoing.push_back(std::byte{ channel });
        const auto* lengthBytes = reinterpret_cast<const std::byte*>(&length);
        mImpl->mOutgoing.insert(mImpl->mOutgoing.end(), lengthBytes, lengthBytes + sizeof(length));
        mImpl->mOutgoing.insert(mImpl->mOutgoing.end(), message.mPayload.begin(), message.mPayload.end());
    }

    void NetworkTransport::update()
    {
        if (!mImpl->mConnected)
            return;
        mImpl->flushWrites();
        mImpl->fillReads();
        mImpl->deframe();
    }

    std::vector<Message> NetworkTransport::receive()
    {
        return std::exchange(mImpl->mReceived, {});
    }

    bool NetworkTransport::isConnected() const
    {
        return mImpl->mConnected;
    }

    struct NetworkServer::Impl
    {
        std::shared_ptr<boost::asio::io_context> mIo = std::make_shared<boost::asio::io_context>();
        tcp::acceptor mAcceptor;

        explicit Impl(std::uint16_t port)
            : mAcceptor(*mIo, tcp::endpoint(tcp::v4(), port))
        {
            boost::system::error_code ec;
            mAcceptor.non_blocking(true, ec);
        }
    };

    NetworkServer::NetworkServer(std::uint16_t port)
        : mImpl(std::make_unique<Impl>(port))
    {
    }

    NetworkServer::~NetworkServer() = default;

    std::uint16_t NetworkServer::port() const
    {
        return mImpl->mAcceptor.local_endpoint().port();
    }

    std::vector<std::unique_ptr<NetworkTransport>> NetworkServer::poll()
    {
        std::vector<std::unique_ptr<NetworkTransport>> accepted;
        while (true)
        {
            boost::system::error_code ec;
            tcp::socket socket(*mImpl->mIo);
            mImpl->mAcceptor.accept(socket, ec);
            if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again)
                break;
            if (ec)
            {
                Log(Debug::Warning) << "NetworkServer: accept failed: " << ec.message();
                break;
            }
            accepted.push_back(
                std::make_unique<NetworkTransport>(std::make_unique<NetworkTransport::Impl>(mImpl->mIo, std::move(socket))));
        }
        return accepted;
    }
}
