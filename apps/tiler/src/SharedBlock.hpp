#pragma once

#include "rmw/tiling/Shared.hpp"

namespace rmw::tilerapp
{

// What the tiler shares with the bars (rmw/tiling/Shared.hpp): the block of memory and the event
// that wakes them. The elevated copy (advanced mode) makes both the user's, at medium integrity, so
// the bars, which are not elevated, still open them.
class SharedBlock
{
public:
    explicit SharedBlock(bool elevated);
    ~SharedBlock(); // tells the bars there are no workspaces any more
    SharedBlock(const SharedBlock&) = delete;
    SharedBlock& operator=(const SharedBlock&) = delete;
    SharedBlock(SharedBlock&&) = delete;
    SharedBlock& operator=(SharedBlock&&) = delete;

    // Writes it the way readers expect (an odd sequence meanwhile), then wakes the bars.
    template <class Fill>
    void write(Fill fill)
    {
        if (m_view != nullptr)
        {
            begin();
            fill(*m_view);
            end();
        }
    }

private:
    void begin() noexcept;
    void end() noexcept;

    void* m_mapping = nullptr; // HANDLE
    tiling::SharedState* m_view = nullptr;
    void* m_event = nullptr; // HANDLE
};

} // namespace rmw::tilerapp
