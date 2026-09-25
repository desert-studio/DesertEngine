#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

namespace Desert::Assets
{
    // One parameter of a material, keyed by the owning shader's schema name (DSL Properties /
    // #pragma param). Plain aggregate — serialized by reflect-cpp as part of the .demat.
    struct MaterialShaderParam
    {
        std::string Name;
        glm::vec4   Value = glm::vec4( 0.0f );
    };

    // ONE REFERENCE FROM A MATERIAL TO ANOTHER ASSET (MATL 3), named by that asset's header GUID. The slot
    // `Name` is the shader schema's; `Guid` (32 hex digits, AssetGuidToText) IS the reference - its runtime
    // handle is Common::Content::HandleForGuid of it, the one fold every asset kind with a header shares;
    // `Path` only says where the file was when the reference was written (a locator for tools and people,
    // never an identity: a rename keeps the GUID and makes the path stale, not the reference).
    //
    // An EMPTY `Guid` is an authored empty slot ("bind the schema's default"), not an absent one: an
    // instance that clears a slot its parent fills says so with this entry. A path with no GUID is refused
    // by ParseMaterialJson, because a path is not an identity.
    //
    // MATL 2 stored a bare u64 here, a path-derived number the asset itself no longer registers under since
    // its header GUID became its handle (T6a/T6b); Tools/SceneMigrator raises MATL 2 -> 3.
    struct MaterialAssetRef
    {
        std::string Name;
        std::string Guid;
        std::string Path;
    };

    // A reference to a SHADER asset (the cloud material's authored `Medium`). Shaders carry no header GUID -
    // their handle is AssetHandle::FromCookedPath of their file (AssetBase) - so for them the path IS the
    // identity, and the reference is stated as exactly that rather than as a number derived from it.
    struct MaterialShaderRef
    {
        std::string Name;
        std::string Path;
    };

    // THE material asset payload (.demat) — the single protocol for every material.
    //
    // A material is a shader + parameter values, nothing else (Unity model). The shader's schema
    // (declared in the .shader file) defines which params exist, their types, ranges and UI; this
    // struct only stores the values by name. The optimized PBR backend consumes a typed VIEW of
    // these values (PBRSurfaceParams) — an implementation detail, not part of the protocol.
    struct MaterialData
    {
        // First member: the text header (kind Material, GUID, MATL schema version). Stamped and checked
        // only through MaterialFormat.hpp.
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;

        // Shader driving this material. Absent/empty -> "StaticMeshPBR" (the standard surface
        // shader with the batched backend).
        std::optional<std::string> ShaderName;

        std::vector<MaterialShaderParam> Params;
        // Texture2D / TextureCube samplers: TextureAsset (.detex) and SkyboxAsset references, by GUID.
        std::vector<MaterialAssetRef> Textures;
        // The cloud material's non-texture asset slots (CloudType1..4 -> .decloudtype, LayoutPattern and
        // LayoutMask -> .dclayout), by GUID. A list of their own since MATL 3: they are not samplers, and
        // sharing Textures made every texture reader skip them by asking the shader schema what a name meant.
        std::vector<MaterialAssetRef> CloudAssets;
        // Shader-asset slots (the authored cloud Medium), by path: see MaterialShaderRef.
        std::vector<MaterialShaderRef> ShaderRefs;

        // MATERIAL INSTANCE (UE model): when set, this asset is a CHILD of the material whose header GUID this
        // names (32 hex digits, AssetGuidToText), and Params/Textures hold ONLY the overridden values - the
        // shader and every non-overridden parameter come from the parent chain. The same GUID is the header's
        // Dependency (StampMaterialHeader), so a registry sees the edge without reading the body.
        //
        // There is no second identity in the payload: the material IS its header GUID, and its runtime
        // handle is that GUID through the one fold (Handle()). MATL v1 carried a u64 MaterialId beside the
        // GUID - two statements of one identity - and Tools/SceneMigrator removes it (MATL 1 -> 2).
        std::optional<std::string> Parent;

        bool IsInstance() const
        {
            return ParentText() != nullptr;
        }

        /// This material's GUID, from its header; null when there is no header or its GUID is malformed.
        /// ParseMaterialJson refuses both for a file, so a null here is a material never written or read.
        [[nodiscard]] Common::Content::AssetGuid Guid() const
        {
            return Header ? GuidFromText( Header->Guid ) : Common::Content::AssetGuid{};
        }

        /// THE MATERIAL'S HANDLE: its GUID through Common::Content::HandleForGuid, the one fold every
        /// consumer shares (asset handle, MaterialService's external id, a mesh slot's reference).
        [[nodiscard]] Common::UUID Handle() const
        {
            return HandleOf( Guid() );
        }

        /// `Parent`'s text when this is an instance, null otherwise. ONE call answers both halves, so a caller
        /// never dereferences `Parent` on the word of a separate IsInstance().
        [[nodiscard]] const std::string* ParentText() const
        {
            if ( !Parent.has_value() || Parent->empty() )
                return nullptr;
            return &*Parent;
        }

        /// The GUID `Parent` names; null when this is not an instance or `Parent` is malformed.
        [[nodiscard]] Common::Content::AssetGuid ParentGuid() const
        {
            const std::string* parent = ParentText();
            return parent != nullptr ? GuidFromText( *parent ) : Common::Content::AssetGuid{};
        }

        /// Makes this an instance of `parent`, or a base material when `parent` is null.
        void SetParent( const Common::Content::AssetGuid& parent )
        {
            if ( parent.IsNull() )
                Parent.reset();
            else
                Parent = Common::Content::AssetGuidToText( parent );
        }

        /// THE PARENT THIS INSTANCE OVERRIDES, as the handle it is registered under, or nothing when this
        /// asset is not an instance. ONE call answers both halves, so the guard and the value cannot be
        /// asked of two different copies of this struct.
        [[nodiscard]] std::optional<Common::UUID> InstanceParentId() const
        {
            if ( !IsInstance() )
                return std::nullopt;
            return HandleOf( ParentGuid() );
        }

        static Common::UUID HandleOf( const Common::Content::AssetGuid& guid )
        {
            return Common::UUID( static_cast<uint64_t>( Common::Content::HandleForGuid( guid ) ) );
        }

        static Common::Content::AssetGuid GuidFromText( std::string_view text )
        {
            const auto parsed = Common::Content::AssetGuidFromText( text );
            return parsed ? parsed.GetValue() : Common::Content::AssetGuid{};
        }

        // ── Queries ────────────────────────────────────────────────────────────────
        const glm::vec4* FindParam( std::string_view name ) const
        {
            for ( const auto& p : Params )
                if ( p.Name == name )
                    return &p.Value;
            return nullptr;
        }

        glm::vec4 GetParam( std::string_view name, const glm::vec4& fallback = glm::vec4( 0.0f ) ) const
        {
            const auto* v = FindParam( name );
            return v ? *v : fallback;
        }

        float GetFloat( std::string_view name, float fallback = 0.0f ) const
        {
            const auto* v = FindParam( name );
            return v ? v->x : fallback;
        }

        void SetParam( std::string_view name, const glm::vec4& value )
        {
            for ( auto& p : Params )
                if ( p.Name == name )
                {
                    p.Value = value;
                    return;
                }
            Params.push_back( { std::string( name ), value } );
        }

        // STOP SAYING ANYTHING ABOUT @p name. Returns whether an entry was actually removed.
        //
        // THE PAIR OF SetParam, and erasing is not the same as writing the default in. What this material
        // is silent about is answered by whoever reads it — the shader's `Properties … = 0.45` for a base
        // material, the parent chain for an instance (MaterialService::ResolveOverrides) — and that answer
        // is resolved at READ time. Writing the default in would freeze today's answer into the file, so a
        // later edit to the parent, or to the shader, would stop reaching this material. That is a pin, and
        // it is the opposite of a reset.
        //
        // Order of the surviving entries is preserved: MaterialData::Params is compared by NAME
        // (MaterialEdit::AuthoredValuesEqual), so order carries no meaning — but a reorder here would show
        // up as a spurious diff in every `.demat` the editor rewrites.
        bool RemoveParam( std::string_view name )
        {
            for ( auto it = Params.begin(); it != Params.end(); ++it )
                if ( it->Name == name )
                {
                    Params.erase( it );
                    return true;
                }
            return false;
        }

        // ── Asset references ───────────────────────────────────────────────────────────
        // Every getter answers the runtime HANDLE (HandleForGuid of the stated GUID; 0 for an absent slot,
        // an empty one or a malformed GUID, which ParseMaterialJson has already refused for a file). Every
        // setter takes the referenced asset's GUID and its current path; a null GUID authors an empty slot.

        uint64_t GetTexture( std::string_view name ) const
        {
            return HandleOfRef( FindRef( Textures, name ) );
        }

        void SetTexture( std::string_view name, const Common::Content::AssetGuid& guid, std::string_view path )
        {
            SetRef( Textures, name, guid, path );
        }

        uint64_t GetCloudAsset( std::string_view name ) const
        {
            return HandleOfRef( FindRef( CloudAssets, name ) );
        }

        void SetCloudAsset( std::string_view name, const Common::Content::AssetGuid& guid, std::string_view path )
        {
            SetRef( CloudAssets, name, guid, path );
        }

        /// The shader a ShaderRefs slot names, as the handle that shader asset is registered under; 0 when
        /// the slot is absent or empty.
        uint64_t GetShaderRef( std::string_view name ) const
        {
            for ( const auto& r : ShaderRefs )
                if ( r.Name == name )
                    return r.Path.empty() ? 0
                                          : static_cast<uint64_t>( Common::AssetHandle::FromCookedPath( r.Path ) );
            return 0;
        }

        /// Names the shader at `path` (the path its asset was loaded from); an empty path authors an empty slot.
        void SetShaderRef( std::string_view name, std::string_view path )
        {
            for ( auto& r : ShaderRefs )
                if ( r.Name == name )
                {
                    r.Path = std::string( path );
                    return;
                }
            ShaderRefs.push_back( { std::string( name ), std::string( path ) } );
        }

        /// Every slot of all three lists as (name, runtime handle), in list order - the in-memory view a
        /// consumer that binds by slot name reads (MaterialService::ResolveOverrides). It never carries a
        /// GUID or a path, and it never carries a number the file stated: every handle is folded here.
        template <typename TSink>
        void ForEachSlotHandle( TSink&& sink ) const
        {
            for ( const auto& r : Textures )
                sink( r.Name, HandleOfRef( &r ) );
            for ( const auto& r : CloudAssets )
                sink( r.Name, HandleOfRef( &r ) );
            for ( const auto& r : ShaderRefs )
                sink( r.Name, GetShaderRef( r.Name ) );
        }

        /// The GUIDs this material references (parent, textures, cloud assets), each once, in that order -
        /// what the header's Dependencies state (StampMaterialHeader). Malformed and empty ones are skipped.
        [[nodiscard]] std::vector<std::string> ReferencedGuidTexts() const
        {
            std::vector<std::string> out;
            const auto               add = [&out]( const std::string& text )
            {
                if ( text.empty() || GuidFromText( text ).IsNull() )
                    return;
                for ( const auto& seen : out )
                    if ( seen == text )
                        return;
                out.push_back( text );
            };
            if ( const std::string* parent = ParentText() )
                add( *parent );
            for ( const auto& r : Textures )
                add( r.Guid );
            for ( const auto& r : CloudAssets )
                add( r.Guid );
            return out;
        }

    private:
        static const MaterialAssetRef* FindRef( const std::vector<MaterialAssetRef>& refs, std::string_view name )
        {
            for ( const auto& r : refs )
                if ( r.Name == name )
                    return &r;
            return nullptr;
        }

        static uint64_t HandleOfRef( const MaterialAssetRef* ref )
        {
            if ( ref == nullptr || ref->Guid.empty() )
                return 0;
            const auto guid = GuidFromText( ref->Guid );
            return guid.IsNull() ? 0 : static_cast<uint64_t>( HandleOf( guid ) );
        }

        static void SetRef( std::vector<MaterialAssetRef>& refs, std::string_view name,
                            const Common::Content::AssetGuid& guid, std::string_view path )
        {
            std::string text  = guid.IsNull() ? std::string() : Common::Content::AssetGuidToText( guid );
            std::string where = guid.IsNull() ? std::string() : std::string( path );
            for ( auto& r : refs )
                if ( r.Name == name )
                {
                    r.Guid = std::move( text );
                    r.Path = std::move( where );
                    return;
                }
            refs.push_back( { std::string( name ), std::move( text ), std::move( where ) } );
        }
    };
} // namespace Desert::Assets
