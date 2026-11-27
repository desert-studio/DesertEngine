#pragma once
// TWINCHECK: a hand-written GLSL struct or block that repeats a generated one is refused at build time, so a
// layout has one declaration. A twin is (a) a struct/block of a generated struct's or block's GLSL name, or (b)
// one of >= 2 members whose ordered member names match a generated shape's for at least 80 % (LCS / max length;
// names compared lower-case with a leading m_ stripped). Tools/ShaderProtocolGen/TwinAllowList.txt names the known
// false matches, `path:Name - reason`, one per line.
#include "GlslEmit.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Desert::ShaderProtocolGen
{
    struct TwinAllow
    {
        std::string Path; // shader-root relative, '/' separators
        std::string Name;
        std::string Reason;
        uint32_t    Line = 0;     // in the allow-list file
        bool        Used = false; // set by FindTwins
    };

    struct TwinFinding
    {
        std::string Path; // shader-root relative
        uint32_t    Line = 0;
        std::string Name;
        std::string GeneratedPath;
        std::string SourceHeader;
        bool        SameName = false; // rule (a); false = rule (b), the member sequence
    };

    // Parses the allow-list; a malformed line is an error string `file:line: reason`.
    [[nodiscard]] std::vector<std::string> ReadAllowList( const std::filesystem::path& file,
                                                          std::vector<TwinAllow>&      out );

    // Scans every .glslh/.shader/.glsl under @p shaderRoot except Generated/ and returns the twins not allowed;
    // marks the allow entries it meets as Used.
    [[nodiscard]] std::vector<TwinFinding> FindTwins( const std::filesystem::path&    shaderRoot,
                                                      std::span<const GeneratedShape> shapes,
                                                      std::vector<TwinAllow>&         allowList );
} // namespace Desert::ShaderProtocolGen
