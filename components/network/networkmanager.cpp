#include "networkmanager.hpp"

#include <array>
#include <cstring>

#include <boost/asio/connect.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <components/debug/debuglog.hpp>

namespace Net
{
    namespace asio = boost::asio;
    using asio::ip::tcp;

    namespace
    {
        void putU32(std::string& out, std::uint32_t v)
        {
            out.push_back(static_cast<char>((v >> 24) & 0xff));
            out.push_back(static_cast<char>((v >> 16) & 0xff));
            out.push_back(static_cast<char>((v >> 8) & 0xff));
            out.push_back(static_cast<char>(v & 0xff));
        }
        std::uint32_t getU32(const unsigned char* p) { return (std::uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
        std::uint16_t getU16(const unsigned char* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }

        // [u32 bodyLen][u16 eventLen][event][data]
        std::string frame(std::string_view event, std::string_view data)
        {
            std::string f;
            const std::uint32_t bodyLen = static_cast<std::uint32_t>(2 + event.size() + data.size());
            putU32(f, bodyLen);
            f.push_back(static_cast<char>((event.size() >> 8) & 0xff));
            f.push_back(static_cast<char>(event.size() & 0xff));
            f.append(event);
            f.append(data);
            return f;
        }
    }

    // One TCP connection (a client's link to the server, or the server's link to a client).
    class Session : public std::enable_shared_from_this<Session>
    {
    public:
        Session(tcp::socket socket, PeerId id, NetworkManagerImpl& owner)
            : mSocket(std::move(socket))
            , mId(id)
            , mOwner(owner)
        {
        }

        void start() { readHeader(); }
        PeerId id() const { return mId; }

        void write(const std::shared_ptr<std::string>& framed)
        {
            const bool idle = mOutbox.empty();
            mOutbox.push_back(framed);
            if (idle)
                writeNext();
        }

        void close()
        {
            boost::system::error_code ec;
            mSocket.shutdown(tcp::socket::shutdown_both, ec);
            mSocket.close(ec);
        }

    private:
        void readHeader();
        void readBody(std::uint32_t bodyLen);
        void writeNext()
        {
            auto self = shared_from_this();
            asio::async_write(mSocket, asio::buffer(*mOutbox.front()),
                [this, self](const boost::system::error_code& ec, std::size_t) {
                    if (ec)
                        return;
                    mOutbox.pop_front();
                    if (!mOutbox.empty())
                        writeNext();
                });
        }

        tcp::socket mSocket;
        PeerId mId;
        NetworkManagerImpl& mOwner;
        std::array<unsigned char, 4> mHeader;
        std::vector<unsigned char> mBody;
        std::deque<std::shared_ptr<std::string>> mOutbox;
    };

    struct NetworkManagerImpl
    {
        asio::io_context mIo;
        asio::executor_work_guard<asio::io_context::executor_type> mWork{ mIo.get_executor() };
        std::thread mThread;
        std::unique_ptr<tcp::acceptor> mAcceptor;

        std::mutex mMutex; // guards mSessions + mInbox
        std::map<PeerId, std::shared_ptr<Session>> mSessions;
        std::deque<Message> mInbox;
        PeerId mNextId{ 1 };

        ~NetworkManagerImpl()
        {
            mWork.reset();
            mIo.stop();
            if (mThread.joinable())
                mThread.join();
        }

        // Start the IO thread once. connect() may be called repeatedly (retry until the server
        // is up); reassigning a still-joinable std::thread would call std::terminate. The work
        // guard keeps mIo.run() alive, so once started the thread stays running.
        void run()
        {
            if (!mThread.joinable())
                mThread = std::thread([this] { mIo.run(); });
        }

        void pushInbound(Message&& m)
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mInbox.push_back(std::move(m));
        }

        void addSession(const std::shared_ptr<Session>& s)
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mSessions.emplace(s->id(), s);
        }

        void removeSession(PeerId id)
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mSessions.erase(id);
        }

        void accept()
        {
            mAcceptor->async_accept([this](const boost::system::error_code& ec, tcp::socket socket) {
                if (!ec)
                {
                    const PeerId id = mNextId++;
                    auto session = std::make_shared<Session>(std::move(socket), id, *this);
                    addSession(session);
                    Log(Debug::Info) << "Net: client connected, peer " << id;
                    session->start();
                }
                if (mAcceptor->is_open())
                    accept();
            });
        }
    };

    void Session::readHeader()
    {
        auto self = shared_from_this();
        asio::async_read(mSocket, asio::buffer(mHeader),
            [this, self](const boost::system::error_code& ec, std::size_t) {
                if (ec)
                {
                    mOwner.removeSession(mId);
                    return;
                }
                readBody(getU32(mHeader.data()));
            });
    }

    void Session::readBody(std::uint32_t bodyLen)
    {
        mBody.resize(bodyLen);
        auto self = shared_from_this();
        asio::async_read(mSocket, asio::buffer(mBody),
            [this, self, bodyLen](const boost::system::error_code& ec, std::size_t) {
                if (ec)
                {
                    mOwner.removeSession(mId);
                    return;
                }
                if (bodyLen >= 2)
                {
                    const std::uint16_t eventLen = getU16(mBody.data());
                    if (2u + eventLen <= bodyLen)
                    {
                        Message m;
                        m.mPeer = mId;
                        m.mEvent.assign(reinterpret_cast<const char*>(mBody.data() + 2), eventLen);
                        m.mData.assign(reinterpret_cast<const char*>(mBody.data() + 2 + eventLen), bodyLen - 2 - eventLen);
                        mOwner.pushInbound(std::move(m));
                    }
                }
                readHeader();
            });
    }

    NetworkManager::NetworkManager()
        : mImpl(std::make_unique<NetworkManagerImpl>())
    {
    }

    NetworkManager::~NetworkManager() = default;

    void NetworkManager::listen(std::uint16_t port)
    {
        mIsServer = true;
        mImpl->mAcceptor = std::make_unique<tcp::acceptor>(mImpl->mIo, tcp::endpoint(tcp::v4(), port));
        mImpl->accept();
        mImpl->run();
        Log(Debug::Info) << "Net: listening on port " << port;
    }

    void NetworkManager::connect(const std::string& host, std::uint16_t port)
    {
        mIsServer = false;
        auto resolver = std::make_shared<tcp::resolver>(mImpl->mIo);
        auto endpoints = resolver->resolve(host, std::to_string(port));
        auto socket = std::make_shared<tcp::socket>(mImpl->mIo);
        NetworkManagerImpl* impl = mImpl.get();
        asio::async_connect(*socket, endpoints,
            [impl, socket, resolver](const boost::system::error_code& ec, const tcp::endpoint&) {
                if (ec)
                {
                    Log(Debug::Error) << "Net: connect failed: " << ec.message();
                    return;
                }
                auto session = std::make_shared<Session>(std::move(*socket), 1, *impl);
                impl->addSession(session);
                Log(Debug::Info) << "Net: connected to server";
                session->start();
            });
        mImpl->run();
    }

    void NetworkManager::send(PeerId peer, std::string_view event, std::string_view data)
    {
        auto framed = std::make_shared<std::string>(frame(event, data));
        NetworkManagerImpl* impl = mImpl.get();
        asio::post(impl->mIo, [impl, peer, framed] {
            std::shared_ptr<Session> s;
            {
                std::lock_guard<std::mutex> lock(impl->mMutex);
                auto it = impl->mSessions.find(peer);
                if (it == impl->mSessions.end())
                {
                    // Client role: a single session to the server, whatever its id.
                    if (!impl->mSessions.empty())
                        s = impl->mSessions.begin()->second;
                }
                else
                    s = it->second;
            }
            if (s)
                s->write(framed);
        });
    }

    void NetworkManager::broadcast(std::string_view event, std::string_view data)
    {
        auto framed = std::make_shared<std::string>(frame(event, data));
        NetworkManagerImpl* impl = mImpl.get();
        asio::post(impl->mIo, [impl, framed] {
            std::vector<std::shared_ptr<Session>> targets;
            {
                std::lock_guard<std::mutex> lock(impl->mMutex);
                for (auto& [id, s] : impl->mSessions)
                    targets.push_back(s);
            }
            for (auto& s : targets)
                s->write(framed);
        });
    }

    std::vector<Message> NetworkManager::poll()
    {
        std::vector<Message> out;
        std::lock_guard<std::mutex> lock(mImpl->mMutex);
        out.assign(std::make_move_iterator(mImpl->mInbox.begin()), std::make_move_iterator(mImpl->mInbox.end()));
        mImpl->mInbox.clear();
        return out;
    }

    std::vector<PeerId> NetworkManager::peers() const
    {
        std::vector<PeerId> out;
        std::lock_guard<std::mutex> lock(mImpl->mMutex);
        for (auto& [id, s] : mImpl->mSessions)
            out.push_back(id);
        return out;
    }
}
