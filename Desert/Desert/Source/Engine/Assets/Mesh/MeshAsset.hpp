#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AssetEvents.hpp>
#include <Engine/Geometry/MeshTypes.hpp>

#include <Common/Content/AssetEnvelope.hpp>

#include <filesystem>
#include <system_error>
#include <vector>

namespace Desert::Assets
{
    class MeshAsset : public AssetBase, public AssetsEventSystem
    {
    public:
        // THE MESH'S IDENTITY IS ITS HEADER GUID, adopted HERE rather than in the load: the asset manager
        // keys its handle lookup at creation, and a scene's mesh reference is resolved and registered with
        // the MeshService before anything parses the file — so a handle adopted at load (as the material
        // does) would arrive after the path-derived one had already been handed out.
        //
        // The header is read through `ReadAssetHeaderIfStated`, the one reader that picks the format by the
        // file's own leading bytes: a mesh is either a cooked MeshBinary v3 (GUID after the 64-byte header)
        // or a DAST envelope (MeshSourceAsset), and the extension says neither. Reading only the MeshBinary
        // prefix gave an envelope the path-derived handle while the content registry, reading the same
        // file through this same function, stated the GUID's — two identities for one mesh.
        // A file that states no GUID (absent — a cook-create names a file about to be written — or a pre-v3
        // cook) keeps the path-derived handle and a null Guid(); a malformed header does too, and the load
        // refuses it by name, so no mesh is ever READY under that handle. RecordOnly: identity is all this
        // needs, and the load judges the subsystem versions.
        MeshAsset( const AssetPriority priority, const Common::Filepath& filepath, const AssetTypeID type )
             : AssetBase( priority, filepath, type )
        {
            std::error_code missing;
            if ( !std::filesystem::is_regular_file( m_Metadata.Filepath, missing ) )
                return;
            const Common::Content::AssetHeaderReadContext recordOnly{ {}, true };
            const auto stated = Common::Content::ReadAssetHeaderIfStated( m_Metadata.Filepath, recordOnly );
            if ( !stated )
                return;
            const auto& header = stated.GetValue();
            if ( !header.has_value() || header->Guid.IsNull() )
                return;
            m_Guid = header->Guid;
            AdoptHandleFromFile( Common::UUID( static_cast<uint64_t>( Common::Content::HandleForGuid( m_Guid ) ) ),
                                 Common::AssetHandle::StableKeyForPath( m_Metadata.Filepath ) );
        }
        virtual ~MeshAsset() = default;

        // The header GUID this mesh was created from; null when the file states none.
        [[nodiscard]] const Common::Content::AssetGuid& Guid() const
        {
            return m_Guid;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::Mesh;
        }

        // A SINGULAR `GetMaterialHandle( submeshIndex )` STOOD BESIDE THIS ONE and Г12 removed it, along
        // with the two helpers that existed only to serve it: a shared bounds check and a
        // `NullMaterialHandle()` constant. All three had zero callers; every caller in the engine and the
        // editor uses the plural below.
        //
        // The check is worth a sentence, because deleting a guard deserves an argument rather than a
        // shrug. It was added after a real defect — both implementations indexed the vector with no bound
        // and one never populated it — and its error path named the mesh, the index and the size. But two
        // facts make it unreachable rather than merely unused: all five callers of the plural accessor
        // iterate it or take its size, none indexes with a bare `[i]`; and the loaders build exactly one
        // handle per submesh from the same parsed data, so the two sizes agree BY CONSTRUCTION after any
        // successful load. A guard against a state the constructor cannot produce, reached through a
        // function nobody calls, is not safety — it is the appearance of it.
        //
        // If a per-index accessor is ever wanted again, it needs that bounds check back: the reason it
        // was written has not stopped being true, only stopped being reachable.
        virtual const std::vector<Common::UUID>& GetMaterialHandles() const = 0;
        virtual bool                             IsSkinned() const          = 0;

        // THE DRAWABLE PARTS OF THIS MESH — on the base, because the one thing every caller of the mesh
        // services needs to know about a mesh asset is how many pieces it has, and until now that question
        // could only be asked of a *concrete* type. Both subclasses already had this exact signature; only
        // the base did not, so `MeshService::Register` could not compare what it BUILT against what the
        // asset HOLDS and shipped a mesh with zero submeshes built from an unparsed shell. See
        // MeshService::BuildAndCache for the relation this makes expressible.
        virtual const std::vector<Submesh>& GetSubmeshes() const = 0;

        // Blendshapes for this mesh (empty when it has none). Overridden by Static/SkinnedMeshAsset; the base
        // default lets any MeshAsset* be queried uniformly (e.g. the Details morph widget).
        virtual const std::vector<MorphTarget>& GetMorphTargets() const
        {
            static const std::vector<MorphTarget> kEmpty;
            return kEmpty;
        }

    private:
        Common::Content::AssetGuid m_Guid;
    };

} // namespace Desert::Assets