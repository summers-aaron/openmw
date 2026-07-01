#include <gtest/gtest.h>

#include "apps/openmw/mwnet/loopbacktransport.hpp"

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

        TEST(MWNetLoopbackTransportTest, isAlwaysConnected)
        {
            LoopbackTransport transport;
            EXPECT_TRUE(transport.isConnected());
        }

        TEST(MWNetLoopbackTransportTest, deliversNothingBeforeUpdate)
        {
            LoopbackTransport transport;
            transport.send(Message{ Channel::Reliable, bytes({ 1, 2, 3 }) });
            EXPECT_TRUE(transport.receive().empty());
        }

        TEST(MWNetLoopbackTransportTest, deliversPayloadByteForByteAfterUpdate)
        {
            LoopbackTransport transport;
            transport.send(Message{ Channel::Unreliable, bytes({ 7, 8, 9 }) });
            transport.update();

            const std::vector<Message> received = transport.receive();
            ASSERT_EQ(received.size(), 1u);
            EXPECT_EQ(received[0].mChannel, Channel::Unreliable);
            EXPECT_EQ(received[0].mPayload, bytes({ 7, 8, 9 }));
        }

        TEST(MWNetLoopbackTransportTest, preservesSendOrderAcrossPumps)
        {
            LoopbackTransport transport;
            transport.send(Message{ Channel::Reliable, bytes({ 1 }) });
            transport.send(Message{ Channel::Reliable, bytes({ 2 }) });
            transport.update();
            // A second batch sent before draining the first must queue behind it.
            transport.send(Message{ Channel::Reliable, bytes({ 3 }) });
            transport.update();

            const std::vector<Message> received = transport.receive();
            ASSERT_EQ(received.size(), 3u);
            EXPECT_EQ(received[0].mPayload, bytes({ 1 }));
            EXPECT_EQ(received[1].mPayload, bytes({ 2 }));
            EXPECT_EQ(received[2].mPayload, bytes({ 3 }));
        }

        TEST(MWNetLoopbackTransportTest, receiveDrainsInbox)
        {
            LoopbackTransport transport;
            transport.send(Message{ Channel::Reliable, {} });
            transport.update();
            EXPECT_EQ(transport.receive().size(), 1u);
            // Nothing new sent: the next receive is empty.
            EXPECT_TRUE(transport.receive().empty());
        }
    }
}
