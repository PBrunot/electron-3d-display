/**
 * @file remote_command.h
 * @brief Single-slot, lock-free mailbox between net/web_remote.cpp's HTTP task (producer)
 *        and the render task's chooser/viewer loops (consumer), plus the reverse channel the
 *        viewers publish "what's on screen now" through for the web page to highlight.
 *
 * Single slot, latest-wins -- not a queue: a phone user tapping Fe then Cu in quick
 * succession wants Cu, not a Fe transition followed by a Cu one. Everything is packed into
 * one std::atomic<uint32_t> per direction, so neither side ever blocks the other (the render
 * loop just does one atomic exchange per frame).
 *
 * Arguments are validated by the producer (web_remote.cpp) before post(), so the consumers
 * act on them without re-checking ranges.
 */
#pragma once

#include <cstdint>

namespace remote
{
    enum class Command : uint8_t
    {
        kNone,
        kShowElement, ///< arg = Z (1..kMaxDisplayZ)
        kShowOrbital, ///< arg = kOrbitalLibrary index
        kNext,        ///< Same as a Down tilt-hold in the current viewer.
        kPrev,        ///< Same as an Up tilt-hold in the current viewer.
        kDissect,     ///< Same as a Right tilt-hold in atom_view (start, or cancel if running).
        kMenu,        ///< Same as a Left tilt-hold: back to chooser.h's menu.
    };

    struct Request
    {
        Command cmd = Command::kNone;
        int arg = 0;
    };

    /// Any task. Overwrites whatever request is still pending.
    void post(Request request);

    /// Any task. Posts only if the mailbox is empty -- used by a viewer handing a request it
    /// can't serve back to the chooser, without clobbering a newer one that arrived meanwhile.
    void postIfEmpty(Request request);

    /// Render task. Returns and clears the pending request (cmd == kNone if there was none).
    [[nodiscard]] Request take();

    /// Render task. Clears and returns true only if the pending request is `cmd`.
    [[nodiscard]] bool takeIf(Command cmd);

    [[nodiscard]] bool pending();

    enum class ViewMode : uint8_t
    {
        kMenu,
        kOrbital,
        kElement,
    };

    struct ViewState
    {
        ViewMode mode = ViewMode::kMenu;
        int index = 0; ///< Z for kElement, kOrbitalLibrary index for kOrbital, unused for kMenu.
    };

    /// Render task: publish what is being shown (or is being switched to) right now.
    void publishState(ViewState state);

    /// Any task.
    [[nodiscard]] ViewState currentState();
} // namespace remote
