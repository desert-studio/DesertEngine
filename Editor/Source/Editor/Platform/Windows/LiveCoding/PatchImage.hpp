#pragma once

#include "CoffObject.hpp"
#include "LinkerMap.hpp"

#include <Editor/LiveCoding/LiveCodingTypes.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace Desert::Editor::LiveCoding
{
    /// A loaded image as the patch links against it: its base, its map, its import table, and the other loaded
    /// images of the index for an export the image itself does not import. MapObjects and Originals run parallel
    /// to Link's objects: a patch may hold sources of several unity batches of one image.
    struct ImageSymbols
    {
        std::uintptr_t                      Base = 0;    // GetModuleHandleW(image)
        const LinkerMap*                    Map  = nullptr;
        std::span<const std::string>        MapObjects;  // the map's object name for statics ("unity_0_cxx.cxx.obj")
        std::span<const Coff::Object* const> Originals;  // the object the image was linked from: data size checks
        std::span<const std::uintptr_t>     OtherImages; // module bases, export lookup by decorated name
    };

    struct Redirect
    {
        std::uintptr_t Original;    // the entry in the image
        std::uintptr_t Replacement; // the entry in the patch memory
        std::string    Name;
    };

    /// The objects of one image's patch, linked into memory within +-2 GB of the image (VirtualAlloc with address
    /// hints, 64 KB steps away from the image), .text RX, .rdata R, .data/.bss RW, unwind registered
    /// (RtlAddFunctionTable). Never freed: a redirected entry may be on some thread's stack until exit.
    class PatchImage
    {
    public:
        /// @p carrying: CodeView::Carriers::Functions of the recompiled sources; only those are redirected.
        [[nodiscard]] static Common::ResultStr<PatchImage> Link( std::span<const Coff::Object> objects,
                                                                const ImageSymbols& image,
                                                                const std::unordered_set<std::string>& carrying,
                                                                PatchReport& report );

        [[nodiscard]] std::span<const Redirect> Redirects() const { return m_Redirects; }
        /// The `??__E` of data the image lacked, in object order; run after the redirects.
        [[nodiscard]] std::span<void ( *const )()> NewDataInitialisers() const { return m_Initialisers; }

    private:
        std::uintptr_t           m_Begin = 0;
        std::uintptr_t           m_End   = 0;
        std::vector<Redirect>    m_Redirects;
        std::vector<void ( * )()> m_Initialisers;
    };
} // namespace Desert::Editor::LiveCoding
