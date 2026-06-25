#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "apps/openmw/mwnet/networktransport.hpp"

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

        // Accept exactly one client on the server, polling briefly for it to arrive.
        std::unique_ptr<NetworkTransport> acceptOne(NetworkServer& server)
        {
            for (int i = 0; i < 200; ++i)
            {
                std::vector<std::unique_ptr<NetworkTransport>> accepted = server.poll();
                if (!accepted.empty())
                    return std::move(accepted.front());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return nullptr;
        }

        // Pump both ends and wait until the receiver has delivered at least one message.
        std::vector<Message> pumpUntilReceived(NetworkTransport& sender, NetworkTransport& receiver)
        {
            for (int i = 0; i < 200; ++i)
            {
                sender.update();
                receiver.update();
                std::vector<Message> got = receiver.receive();
                if (!got.empty())
                    return got;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return {};
        }

        TEST(MWNetNetworkTransportTest, clientConnectsAndIsConnected)
        {
            NetworkServer server(0);
            const std::unique_ptr<NetworkTransport> client = NetworkTransport::connect("127.0.0.1", server.port());
            ASSERT_NE(client, nullptr);
            EXPECT_TRUE(client->isConnected());
            const std::unique_ptr<NetworkTransport> serverSide = acceptOne(server);
            ASSERT_NE(serverSide, nullptr);
            EXPECT_TRUE(serverSide->isConnected());
        }

        TEST(MWNetNetworkTransportTest, roundTripsAPayloadClientToServer)
        {
            NetworkServer server(0);
            const std::unique_ptr<NetworkTransport> client = NetworkTransport::connect("127.0.0.1", server.port());
            ASSERT_NE(client, nullptr);
            const std::unique_ptr<NetworkTransport> serverSide = acceptOne(server);
            ASSERT_NE(serverSide, nullptr);

            client->send(Message{ Channel::Reliable, bytes({ 0xde, 0xad, 0xbe, 0xef }) });
            const std::vector<Message> got = pumpUntilReceived(*client, *serverSide);
            ASSERT_EQ(got.size(), 1u);
            EXPECT_EQ(got[0].mChannel, Channel::Reliable);
            EXPECT_EQ(got[0].mPayload, bytes({ 0xde, 0xad, 0xbe, 0xef }));
        }

        TEST(MWNetNetworkTransportTest, preservesChannelAndMessageBoundaries)
        {
            NetworkServer server(0);
            const std::unique_ptr<NetworkTransport> client = NetworkTransport::connect("127.0.0.1", server.port());
            ASSERT_NE(client, nullptr);
            const std::unique_ptr<NetworkTransport> serverSide = acceptOne(server);
            ASSERT_NE(serverSide, nullptr);

            // Three back-to-back messages on different channels must arrive as three
            // distinct messages with channels intact (framing, not a merged byte stream).
            client->send(Message{ Channel::Unreliable, bytes({ 1 }) });
            client->send(Message{ Channel::Reliable, bytes({ 2, 2 }) });
            client->send(Message{ Channel::Unreliable, {} }); // empty payload
            client->update();

            std::vector<Message> got;
            for (int i = 0; i < 200 && got.size() < 3; ++i)
            {
                serverSide->update();
                std::vector<Message> batch = serverSide->receive();
                got.insert(got.end(), batch.begin(), batch.end());
                if (got.size() < 3)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            ASSERT_EQ(got.size(), 3u);
            EXPECT_EQ(got[0].mChannel, Channel::Unreliable);
            EXPECT_EQ(got[0].mPayload, bytes({ 1 }));
            EXPECT_EQ(got[1].mChannel, Channel::Reliable);
            EXPECT_EQ(got[1].mPayload, bytes({ 2, 2 }));
            EXPECT_EQ(got[2].mChannel, Channel::Unreliable);
            EXPECT_TRUE(got[2].mPayload.empty());
        }

        TEST(MWNetNetworkTransportTest, bidirectionalExchange)
        {
            NetworkServer server(0);
            const std::unique_ptr<NetworkTransport> client = NetworkTransport::connect("127.0.0.1", server.port());
            ASSERT_NE(client, nullptr);
            const std::unique_ptr<NetworkTransport> serverSide = acceptOne(server);
            ASSERT_NE(serverSide, nullptr);

            serverSide->send(Message{ Channel::Reliable, bytes({ 0x10, 0x20 }) });
            const std::vector<Message> atClient = pumpUntilReceived(*serverSide, *client);
            ASSERT_EQ(atClient.size(), 1u);
            EXPECT_EQ(atClient[0].mPayload, bytes({ 0x10, 0x20 }));
        }

        TEST(MWNetNetworkTransportTest, connectFailureReturnsNull)
        {
            // Port 1 on localhost should refuse; connect() must fail gracefully, not throw.
            const std::unique_ptr<NetworkTransport> client = NetworkTransport::connect("127.0.0.1", 1);
            EXPECT_EQ(client, nullptr);
        }
    }
}
