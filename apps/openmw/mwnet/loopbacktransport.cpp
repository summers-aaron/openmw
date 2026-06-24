#include "loopbacktransport.hpp"

#include <utility>

namespace MWNet
{
    void LoopbackTransport::send(Message message)
    {
        mPending.push_back(std::move(message));
    }

    void LoopbackTransport::update()
    {
        // In-process hand-off: everything sent since the last pump becomes
        // available to receive(). Anything not yet drained is preserved ahead of
        // the newly delivered messages so ordering is stable.
        for (Message& message : mPending)
            mInbound.push_back(std::move(message));
        mPending.clear();
    }

    std::vector<Message> LoopbackTransport::receive()
    {
        return std::exchange(mInbound, {});
    }
}
