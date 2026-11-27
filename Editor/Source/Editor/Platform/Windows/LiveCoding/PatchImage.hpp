#pragma once

#include "CoffObject.hpp"
#include "LinkerMap.hpp"

#include <Editor/LiveCoding/LiveCodingTypes.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Desert::Editor::LiveCoding
{
    /// An object an image was linked from, read once per session, with its defined symbols by normalised name
    /// (data size checks, unchanged-initialiser and unchanged-constant checks look names up here).
    class OriginalObject
    {
    public:
        explicit OriginalObject( Coff::Object object );

        [[nodiscard]] const Coff::Object& Object() const
        {
            return m_Object;
        }
        [[nodiscard]] std::optional<std::uint32_t> Find( const std::string& normalised ) const;

    private:
        Coff::Object                                   m_Object;
        std::unordered_map<std::string, std::uint32_t> m_ByName; // normalised -> the defining, non-local symbol
    };

    /// What the applied patches of one image defined, by normalised name. A later patch links against it as it
    /// links against the image (a global or a function-local static a patch added stays one object), and
    /// redirects every earlier entry of a function it lays out again (a callback stored while patch N-1 ran keeps
    /// reaching the newest code).
    struct PatchHistory
    {
        struct Definition
        {
            std::uintptr_t Address  = 0;
            std::uint32_t  Size     = 0;
            bool           Function = false;
        };
        std::unordered_map<std::string, Definition>                  Latest;
        std::unordered_map<std::string, std::vector<std::uintptr_t>> Entries; // every patch's entry of a function
    };

    /// A loaded image as the patch links against it: its base, its map, its import table, and the other loaded
    /// images of the index for an export the image itself does not import. MapObjects and Originals run parallel
    /// to Link's objects: a patch may hold sources of several unity batches of one image.
    struct ImageSymbols
    {
        std::string                  Name;     // the image's file name, for refusals
        std::uintptr_t               Base = 0; // GetModuleHandleW(image)
        const LinkerMap*             Map  = nullptr;
        std::span<const std::string> MapObjects; // the map's object name for statics ("unity_0_cxx.cxx.obj")
        std::span<const OriginalObject* const> Originals;         // the object the image was linked from
        std::span<const std::uintptr_t>        OtherImages;       // module bases, export lookup by decorated name
        const PatchHistory*                    History = nullptr; // the patches of this image applied before
    };

    struct Redirect
    {
        std::uintptr_t Original;    // the entry in the image or in an earlier patch
        std::uintptr_t Replacement; // the entry in the patch memory
        std::string    Name;
    };

    /// The objects of one image's patch, linked into memory within +-2 GB of the image (VirtualAlloc with address
    /// hints, 64 KB steps away from the image), .text RX, .rdata R, .data/.bss RW, unwind registered
    /// (RtlAddFunctionTable). Every function is laid out after 5 bytes of CC, so a later patch redirects it the
    /// way it redirects the image's. Never freed: a redirected entry may be on some thread's stack until exit.
    class PatchImage
    {
    public:
        /// @p carrying: CodeView::Carriers::Functions of the recompiled sources; only those are redirected.
        [[nodiscard]] static Common::ResultStr<PatchImage> Link( std::span<const Coff::Object> objects,
                                                                const ImageSymbols& image,
                                                                const std::unordered_set<std::string>& carrying,
                                                                PatchReport& report );

        [[nodiscard]] std::span<const Redirect> Redirects() const { return m_Redirects; }
        /// The `??__E` of data the image and the earlier patches lack, in object order; run before the redirects
        /// (no thread reaches the new data until an entry leads into the patch).
        [[nodiscard]] std::span<void ( *const )()> NewDataInitialisers() const { return m_Initialisers; }
        /// What this patch defines, for PatchHistory once it is applied.
        [[nodiscard]] const PatchHistory& Defined() const
        {
            return m_Defined;
        }

    private:
        std::uintptr_t            m_Begin = 0;
        std::uintptr_t            m_End   = 0;
        std::vector<Redirect>     m_Redirects;
        std::vector<void ( * )()> m_Initialisers;
        PatchHistory              m_Defined;
    };
} // namespace Desert::Editor::LiveCoding
