#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Desert::Editor::LiveCoding::Coff
{
    enum class RelocationKind : std::uint16_t // IMAGE_REL_AMD64_*
    {
        Addr64   = 0x1,
        Addr32Nb = 0x3,
        Rel32    = 0x4, // Rel32_1..Rel32_5 = 0x5..0x9: the displacement ends 1..5 bytes before the next instruction
        Rel32_1  = 0x5,
        Rel32_2  = 0x6,
        Rel32_3  = 0x7,
        Rel32_4  = 0x8,
        Rel32_5  = 0x9,
        Section  = 0xA,
        SecRel   = 0xB,
    };

    struct Relocation
    {
        std::uint32_t  Offset; // in the section
        std::uint32_t  Symbol; // raw symbol table index (aux records counted)
        RelocationKind Kind;
    };

    enum class ComdatSelect : std::uint8_t // IMAGE_COMDAT_SELECT_*; None = not a COMDAT
    {
        None = 0, NoDuplicates = 1, Any = 2, SameSize = 3, ExactMatch = 4, Associative = 5, Largest = 6,
    };

    struct Section
    {
        std::string                Name;            // ".text$mn", ".rdata", ".debug$S", ".pdata", ".xdata", ".tls$"
        std::uint32_t              Characteristics; // IMAGE_SCN_*
        std::span<const std::byte> Bytes;           // empty for uninitialised data
        std::uint32_t              Size;            // SizeOfRawData (uninitialised: the size to reserve)
        std::vector<Relocation>    Relocations;
        ComdatSelect               Comdat           = ComdatSelect::None;
        std::uint32_t              AssociatedWith   = 0; // 1-based section, for Associative
    };

    struct Symbol
    {
        std::string   Name;           // decorated; empty for an aux record slot
        std::int32_t  Section = 0;    // 1-based; 0 undefined, -1 absolute, -2 debug
        std::uint32_t Value   = 0;    // offset in the section
        std::uint8_t  StorageClass = 0; // IMAGE_SYM_CLASS_EXTERNAL / STATIC / LABEL
        bool          Function     = false; // complex type DTYPE_FUNCTION
        std::uint32_t WeakDefault  = ~0u;   // a weak external: the symbol it resolves to when nothing defines it
    };

    /// One object file, regular or /bigobj, held in memory. Indices are the file's own (aux records keep a slot).
    /// Move-only: every Section::Bytes views m_Bytes, which a move keeps and a copy would leave behind.
    class Object
    {
    public:
        Object()                             = default;
        Object( Object&& )                   = default;
        Object& operator=( Object&& )        = default;
        Object( const Object& )              = delete;
        Object& operator=( const Object& )   = delete;

        [[nodiscard]] static Common::ResultStr<Object> Load( const std::filesystem::path& path );

        [[nodiscard]] std::span<const Section> Sections() const { return m_Sections; } // [0] = section 1
        [[nodiscard]] std::span<const Symbol>  Symbols() const { return m_Symbols; }
        /// Bytes from the symbol to the next symbol of its section, or to the section end; computed once at Load.
        [[nodiscard]] std::uint32_t Extent( std::uint32_t symbol ) const;
        [[nodiscard]] const std::filesystem::path& Path() const { return m_Path; }

    private:
        void ComputeExtents();

        std::filesystem::path  m_Path;
        std::vector<std::byte> m_Bytes;
        std::vector<Section>   m_Sections;
        std::vector<Symbol>    m_Symbols;
        std::vector<std::uint32_t> m_Extents; // per symbol, 0 for an undefined one or an aux slot
    };
} // namespace Desert::Editor::LiveCoding::Coff
