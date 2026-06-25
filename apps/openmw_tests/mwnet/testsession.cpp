#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "apps/openmw/mwnet/networktransport.hpp"
#include "apps/openmw/mwnet/session.hpp"

namespace MWNet
{
    namespace
    {
        std::vector<std::byte> bytes(std::initializer_list<unsigned char> values)
        {
            std::vector<std::byte> result;
            for (unsigned char v : values)
                result.push_back(std::byte{ v });
            return result;
        }

        // Pump host + clients until the host reports the expected number of peers.
        void waitForPeers(HostSession& host, const std::vector<ClientSession*>& clients, std::size_t expected)
        {
            for (int i = 0; i < 300 && host.peerCount() < expected; ++i)
            {
                host.poll();
                for (ClientSession* client : clients)
                    client->poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        TEST(MWNetSessionTest, loopbackEchoesToLocalPeer)
        {
            LoopbackSession session;
            session.broadcast(Message{ Channel::Reliable, bytes({ 7 }) });
            const std::vector<ReceivedMessage> got = session.poll();
            ASSERT_EQ(got.size(), 1u);
            EXPECT_EQ(got[0].mFrom, sLocalPeer);
            EXPECT_EQ(got[0].mMessage.mPayload, bytes({ 7 }));
        }

        TEST(MWNetSessionTest, hostAcceptsClientsAndCountsPeers)
        {
            HostSession host(0);
            const std::unique_ptr<ClientSession> a = ClientSession::connect("127.0.0.1", host.port());
            const std::unique_ptr<ClientSession> b = ClientSession::connect("127.0.0.1", host.port());
            ASSERT_NE(a, nullptr);
            ASSERT_NE(b, nullptr);

            waitForPeers(host, { a.get(), b.get() }, 2);
            EXPECT_EQ(host.peerCount(), 2u);
        }

        TEST(MWNetSessionTest, hostBroadcastReachesAllClients)
        {
            HostSession host(0);
            const std::unique_ptr<ClientSession> a = ClientSession::connect("127.0.0.1", host.port());
            const std::unique_ptr<ClientSession> b = ClientSession::connect("127.0.0.1", host.port());
            ASSERT_NE(a, nullptr);
            ASSERT_NE(b, nullptr);
            waitForPeers(host, { a.get(), b.get() }, 2);

            host.broadcast(Message{ Channel::Unreliable, bytes({ 0x55 }) });

            // broadcast() only queues; the host flushes on its next poll(). Pump host and
            // both clients together until each client has received the message.
            std::vector<ReceivedMessage> atA;
            std::vector<ReceivedMessage> atB;
            for (int i = 0; i < 300 && (atA.empty() || atB.empty()); ++i)
            {
                host.poll();
                std::vector<ReceivedMessage> batchA = a->poll();
                std::vector<ReceivedMessage> batchB = b->poll();
                atA.insert(atA.end(), batchA.begin(), batchA.end());
                atB.insert(atB.end(), batchB.begin(), batchB.end());
                if (atA.empty() || atB.empty())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            ASSERT_EQ(atA.size(), 1u);
            ASSERT_EQ(atB.size(), 1u);
            EXPECT_EQ(atA[0].mFrom, sLocalPeer); // from the host's perspective the client sees peer 0 (host)
            EXPECT_EQ(atA[0].mMessage.mPayload, bytes({ 0x55 }));
            EXPECT_EQ(atB[0].mMessage.mPayload, bytes({ 0x55 }));
        }

        TEST(MWNetSessionTest, hostTagsMessagesByDistinctPeer)
        {
            HostSession host(0);
            const std::unique_ptr<ClientSession> a = ClientSession::connect("127.0.0.1", host.port());
            const std::unique_ptr<ClientSession> b = ClientSession::connect("127.0.0.1", host.port());
            ASSERT_NE(a, nullptr);
            ASSERT_NE(b, nullptr);
            waitForPeers(host, { a.get(), b.get() }, 2);

            a->broadcast(Message{ Channel::Reliable, bytes({ 0xa1 }) });
            b->broadcast(Message{ Channel::Reliable, bytes({ 0xb2 }) });

            std::vector<ReceivedMessage> got;
            for (int i = 0; i < 300 && got.size() < 2; ++i)
            {
                a->poll();
                b->poll();
                std::vector<ReceivedMessage> batch = host.poll();
                got.insert(got.end(), batch.begin(), batch.end());
                if (got.size() < 2)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            ASSERT_EQ(got.size(), 2u);
            // The two messages came from two different, non-local peers.
            EXPECT_NE(got[0].mFrom, got[1].mFrom);
            EXPECT_NE(got[0].mFrom, sLocalPeer);
            EXPECT_NE(got[1].mFrom, sLocalPeer);
        }

        TEST(MWNetSessionTest, hostDropsDisconnectedClient)
        {
            HostSession host(0);
            std::unique_ptr<ClientSession> a = ClientSession::connect("127.0.0.1", host.port());
            ASSERT_NE(a, nullptr);
            waitForPeers(host, { a.get() }, 1);
            ASSERT_EQ(host.peerCount(), 1u);

            a.reset(); // client disconnects

            for (int i = 0; i < 300 && host.peerCount() > 0; ++i)
            {
                host.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            EXPECT_EQ(host.peerCount(), 0u);
        }
    }
}
