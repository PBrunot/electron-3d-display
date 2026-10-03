#include "ux/remote_command.h"

#include <atomic>

namespace remote
{
    namespace
    {
        // Layout for both words: bits 16..23 = enum, bits 0..15 = arg. A zero word is
        // "Command::kNone"/"ViewMode::kMenu, 0", i.e. the natural empty/boot state.
        std::atomic<uint32_t> gRequest{0};
        std::atomic<uint32_t> gState{0};

        constexpr uint32_t pack(uint8_t tag, int arg)
        {
            return (uint32_t(tag) << 16) | (uint32_t(arg) & 0xFFFFu);
        }

        constexpr Request unpackRequest(uint32_t word)
        {
            return {static_cast<Command>(word >> 16), int(word & 0xFFFFu)};
        }
    } // namespace

    void post(Request request)
    {
        gRequest.store(pack(static_cast<uint8_t>(request.cmd), request.arg));
    }

    void postIfEmpty(Request request)
    {
        uint32_t expected = 0;
        gRequest.compare_exchange_strong(expected, pack(static_cast<uint8_t>(request.cmd), request.arg));
    }

    Request take()
    {
        return unpackRequest(gRequest.exchange(0));
    }

    bool takeIf(Command cmd)
    {
        uint32_t word = gRequest.load();
        if (unpackRequest(word).cmd != cmd)
            return false;
        // Fails only if a newer request replaced it in between -- leave that one for take().
        return gRequest.compare_exchange_strong(word, 0);
    }

    bool pending()
    {
        return gRequest.load() != 0;
    }

    void publishState(ViewState state)
    {
        gState.store(pack(static_cast<uint8_t>(state.mode), state.index));
    }

    ViewState currentState()
    {
        uint32_t word = gState.load();
        return {static_cast<ViewMode>(word >> 16), int(word & 0xFFFFu)};
    }
} // namespace remote
