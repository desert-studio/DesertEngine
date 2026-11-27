#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Desert::Editor::LiveCoding
{
    /// "?g@?A0x344769b6@VFX@Desert@@3_KA" -> "?g@?A0x@VFX@Desert@@3_KA": the per-TU anonymous namespace hash differs
    /// between the unity object and the source compiled alone; a unity batch cannot hold the same name twice.
    [[nodiscard]] std::string NormaliseAnonymousNamespace( std::string_view decorated );

    /// The linker's /MAP of one image: "Publics by Value" and "Static symbols", by normalised name.
    class LinkerMap
    {
    public:
        [[nodiscard]] static Common::ResultStr<LinkerMap> Load( const std::filesystem::path& map );

        /// The RVA of an external symbol, or of a static one defined by @p object (the "Lib:Object" column's object,
        /// "unity_0_cxx.cxx.obj"). Import thunks are externals whose object is the import library's DLL.
        [[nodiscard]] std::optional<std::uint32_t> Find( std::string_view decorated, std::string_view object ) const;
        [[nodiscard]] std::optional<std::uint32_t> SectionStart( std::string_view section ) const; // ".tls"

    private:
        struct Entry { std::uint32_t Rva; std::string Object; bool Static; };
        std::unordered_multimap<std::string, Entry>    m_ByName;
        std::unordered_map<std::string, std::uint32_t> m_Sections;
    };
} // namespace Desert::Editor::LiveCoding
