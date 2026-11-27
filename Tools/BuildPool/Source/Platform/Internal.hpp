// BuildPool platform: helpers shared by every OS implementation of BuildPool/Platform.hpp. Included only by the
// files under Source/Platform/.
#pragma once

#include "BuildPool/Result.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace buildpool::detail
{
    // The first 16 hex digits of SHA-256(loweredHome): the part of an endpoint name derived from the pool home.
    // The caller lower-cases the home's canonical path by its OS's rules first.
    std::string EndpointHash( std::string_view loweredHome );

    // Frames are a u32 little-endian length followed by that many bytes (Platform.hpp).
    inline constexpr size_t kFrameHeaderBytes = 4;
    inline constexpr size_t kMaxMessageBytes  = size_t{ 1 } << 20;

    // `message` with its frame header in front; an Error when it is over kMaxMessageBytes.
    Result<std::vector<std::byte>> Frame( std::span<const std::byte> message );

    // Bytes as they arrive from a stream, cut into whole messages.
    class FrameReader
    {
    public:
        void Append( std::span<const std::byte> bytes );
        // The next whole message; empty while it is incomplete; an Error when a header announces more than
        // kMaxMessageBytes (the stream is then unusable).
        Result<std::optional<std::vector<std::byte>>> Next();

    private:
        std::vector<std::byte> m_bytes;
        size_t                 m_offset = 0;
    };
} // namespace buildpool::detail
