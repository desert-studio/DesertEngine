#include <Engine/Graphic/Systems/RayTracing/RayTracingGather.hpp>

#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Geometry/StaticMesh.hpp>
#include <Engine/Graphic/InstanceCullDistance.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>

#include <algorithm>
#include <format>
#include <unordered_map>

namespace Desert::Graphic::RayTracing
{
    namespace
    {
        using System::MeshRenderer;

        // One section group of a row: the submeshes one queued record draws and the material they shade with.
        struct SectionSource
        {
            uint64_t                Visible  = 0; // bit i = submesh i drawn by this record
            const MaterialInstance* Instance = nullptr;
            const Material*         Surface  = nullptr;
            bool                    Masked   = false;
            // A generic record's own hit group (null: Surface's); Masked when it carries an alpha test.
            const RayTracingProgramHit* ProgramHit = nullptr;
        };

        // One GPU-scene row of the static / generic queues before its BLAS is resolved.
        struct RowRecord
        {
            Mesh*                      MeshPtr = nullptr;
            glm::mat4                  World{ 1.0f };
            uint32_t                   Row         = 0;
            bool                       CastShadows = false;
            RayTracingPrimitiveKind    Kind        = RayTracingPrimitiveKind::Static;
            std::vector<SectionSource> Sections; // the row's records, in queue order
        };

        // The records of one ISM field: every section group of one entity's mesh over one transform snapshot. The
        // groups share the instances' rows (BuildObjectMotions keys them {Entity, instance}), so the field is one
        // primitive per instance whose BLAS holds every group's sections.
        struct FieldRecord
        {
            const MeshRenderer::InstancedMeshRenderData* First = nullptr; // rows, transforms, cull, shadows
            std::vector<SectionSource>                   Sections;
            bool                                         CastShadows = false;
        };

        void Count( RayTracingGatherResult& out, const RayTracingSkip reason )
        {
            ++out.Skipped[static_cast<std::size_t>( reason )];
        }

        SectionSource SourceOf( const uint64_t visible, const MeshRenderer::SectionShading& shading )
        {
            return { visible, shading.Instance, shading.Surface, shading.Masked };
        }

        // The BLAS masks of @p mesh drawn by @p sections: a submesh no section draws is hidden; one a Masked
        // section draws is non-opaque. Each submesh belongs to one section (Desert::SectionMaterialSlot).
        RayTracingSubmeshMasks MasksOf( const Mesh& mesh, std::span<const SectionSource> sections )
        {
            const std::size_t submeshes = std::min<std::size_t>( mesh.GetSubmeshes().size(), 64 );
            const uint64_t    all       = submeshes >= 64 ? ~0ull : ( ( 1ull << submeshes ) - 1ull );
            uint64_t          visible   = 0;
            uint64_t          masked    = 0;
            for ( const SectionSource& section : sections )
            {
                visible |= section.Visible;
                if ( section.Masked )
                    masked |= section.Visible;
            }
            return { .Hidden = all & ~visible, .NonOpaque = all & visible & masked };
        }

        // Is every submesh of @p mesh hidden by @p masks (nothing left to trace)?
        bool NothingVisible( const Mesh& mesh, const RayTracingSubmeshMasks& masks )
        {
            const std::size_t submeshes = std::min<std::size_t>( mesh.GetSubmeshes().size(), 64 );
            const uint64_t    all       = submeshes >= 64 ? ~0ull : ( ( 1ull << submeshes ) - 1ull );
            return submeshes > 0 && masks.Hidden == all;
        }

        // Appends @p primitive's segment run: one record per segment of its BLAS, by the rule the BLAS was built
        // with (DescribeMeshGeometry: visible, at least one triangle, submesh order), the material the section
        // that draws the submesh shades with. A submesh past the 64th is visible in every record (the mask holds
        // 64) and takes the first section's material.
        void AppendSegmentRecords( const Mesh& mesh, std::span<const SectionSource> sections,
                                   RayTracingPrimitive& primitive, RayTracingGatherResult& out )
        {
            primitive.FirstSegmentRecord     = static_cast<uint32_t>( out.SegmentRecords.size() );
            const std::vector<Submesh>& subs = mesh.GetSubmeshes();
            for ( std::size_t index = 0; index < subs.size(); ++index )
            {
                if ( IsSubmeshHidden( primitive.Submeshes.Hidden, index ) || !HasRayTracingSegment( subs[index] ) )
                    continue;
                const auto owner =
                     std::ranges::find_if( sections, [index]( const SectionSource& section )
                                           { return index >= 64u || ( section.Visible >> index & 1ull ) != 0; } );
                RayTracingSegmentRecord& record = out.SegmentRecords.emplace_back();
                record.SubmeshIndex             = static_cast<uint32_t>( index );
                record.Opaque                   = !IsSubmeshHidden( primitive.Submeshes.NonOpaque, index );
                if ( owner != sections.end() )
                {
                    record.Instance        = owner->Instance;
                    record.SurfaceMaterial = owner->Surface;
                    record.ProgramHit      = owner->ProgramHit;
                }
            }
            primitive.SegmentRecordCount =
                 static_cast<uint32_t>( out.SegmentRecords.size() ) - primitive.FirstSegmentRecord;
        }

        // Resolves @p mesh's BLAS for @p masks; false: the row is skipped (counted, the error kept).
        bool ResolveGeometry( RayTracingGeometryVariants& variants, Mesh& mesh,
                              const RayTracingSubmeshMasks& masks, RayTracingGatherResult& out,
                              const void*& geometry )
        {
            const Common::ResultStr<const RayTracingGeometryResource*> resolved = variants.Resolve( mesh, masks );
            if ( !resolved )
            {
                out.Errors.push_back( resolved.GetError() );
                Count( out, RayTracingSkip::NoGeometry );
                return false;
            }
            geometry = resolved.GetValue();
            return true;
        }
        // A mesh without an uploader BLAS whose buffers are resident: the procedural cache builds its BLAS. A
        // mesh still streaming in has neither and stays NoGeometry.
        bool IsResident( const Mesh& mesh )
        {
            return mesh.GetVertexBuffer() != nullptr && mesh.GetIndexBuffer() != nullptr;
        }
    } // namespace

    void GatherRayTracingPrimitives( const RayTracingGatherInputs& inputs, RayTracingGeometryVariants& variants,
                                     RayTracingGatherResult& out )
    {
        out.Clear();

        // Static + generic: merge the records of one row (same row AND same mesh; a row is one object's world).
        // Each record is one section group (Rules::SplitMeshSlotDraws): its submeshes and its material.
        std::vector<RowRecord>                    rows;
        std::unordered_map<uint32_t, std::size_t> rowIndex;
        const auto add = [&]( Mesh* mesh, const glm::mat4& world, const uint32_t row, const SectionSource& section,
                              const bool castShadows, const RayTracingPrimitiveKind kind )
        {
            if ( mesh == nullptr )
                return;
            if ( const auto found = rowIndex.find( row );
                 found != rowIndex.end() && rows[found->second].MeshPtr == mesh )
            {
                RowRecord& merged = rows[found->second];
                merged.Sections.push_back( section );
                merged.CastShadows = merged.CastShadows || castShadows;
                return;
            }
            rowIndex[row] = rows.size();
            rows.push_back( { mesh, world, row, castShadows, kind, { section } } );
        };
        for ( const MeshRenderer::StaticMeshRenderData& data : inputs.Static )
            add( data.Mesh, data.Transform, data.MotionRow,
                 SourceOf( ~data.HiddenSubmeshes, MeshRenderer::ShadingOf( data ) ), data.CastShadows,
                 RayTracingPrimitiveKind::Static );
        // A generic record's material is its own program (a per-slot data-driven material, or a shader override
        // with no slot material): opaque, unless the record carries its own hit group with an alpha test (3D
        // text's glyph coverage, RayTracingProgramHit) - then its segments are non-opaque and their hits run that
        // test.
        for ( const MeshRenderer::GenericMeshRenderData& data : inputs.Generic )
            if ( data.VisibleInRayTracing )
            {
                const bool program = !data.RayHit.Template.empty();
                add( data.Mesh, data.Transform, data.MotionRow,
                     { .Visible    = data.VisibleSubmeshMask,
                       .Surface    = data.SlotMaterial,
                       .Masked     = program && data.RayHit.Opacity.has_value(),
                       .ProgramHit = program ? &data.RayHit : nullptr },
                     data.CastShadows, RayTracingPrimitiveKind::Generic );
            }

        for ( const RowRecord& record : rows )
        {
            const RayTracingSubmeshMasks masks = MasksOf( *record.MeshPtr, record.Sections );
            if ( NothingVisible( *record.MeshPtr, masks ) )
            {
                Count( out, RayTracingSkip::NothingVisible );
                continue;
            }
            const void* geometry = nullptr;
            if ( !ResolveGeometry( variants, *record.MeshPtr, masks, out, geometry ) )
                continue;
            RayTracingPrimitive& primitive = out.Primitives.emplace_back();
            primitive.Geometry             = geometry;
            primitive.World                = record.World;
            primitive.GpuSceneRow          = record.Row;
            primitive.SourceMesh           = record.MeshPtr;
            primitive.CastShadows          = record.CastShadows;
            primitive.Submeshes            = masks;
            primitive.Kind                 = record.Kind;
            if ( geometry == nullptr && IsResident( *record.MeshPtr ) )
            {
                primitive.ProceduralMesh = record.MeshPtr;
                primitive.Kind           = RayTracingPrimitiveKind::Procedural;
            }
            AppendSegmentRecords( *record.MeshPtr, record.Sections, primitive, out );
        }

        // ISM / foliage: the section groups of one field merge (FieldRecord); every instance is its own row and
        // TLAS instance, all sharing the field's BLAS and its one segment run.
        std::vector<FieldRecord> fields;
        for ( const MeshRenderer::InstancedMeshRenderData& data : inputs.Instanced )
        {
            if ( data.Mesh == nullptr || !data.Transforms )
                continue;
            const SectionSource section = SourceOf( ~data.HiddenSubmeshes, MeshRenderer::ShadingOf( data ) );
            const auto          same    = std::ranges::find_if( fields,
                                                                [&data]( const FieldRecord& field )
                                                                {
                                                        return field.First->Entity == data.Entity &&
                                                               field.First->Mesh == data.Mesh &&
                                                               field.First->Transforms == data.Transforms;
                                                    } );
            if ( same != fields.end() )
            {
                same->Sections.push_back( section );
                same->CastShadows = same->CastShadows || data.CastShadows;
                continue;
            }
            fields.push_back( { &data, { section }, data.CastShadows } );
        }
        for ( const FieldRecord& field : fields )
        {
            const MeshRenderer::InstancedMeshRenderData& data  = *field.First;
            const RayTracingSubmeshMasks                 masks = MasksOf( *data.Mesh, field.Sections );
            if ( NothingVisible( *data.Mesh, masks ) )
            {
                Count( out, RayTracingSkip::NothingVisible );
                continue;
            }
            const void* geometry = nullptr;
            if ( !ResolveGeometry( variants, *data.Mesh, masks, out, geometry ) )
                continue;
            const auto                    firstRecord  = static_cast<uint32_t>( out.SegmentRecords.size() );
            uint32_t                      recordCount  = 0;
            bool                          recordsAdded = false;
            const InstanceCullDistance    cull       = ScaledByLens( data.CullDistance, inputs.DrawDistanceScale );
            const std::vector<glm::mat4>& transforms = *data.Transforms;
            for ( std::size_t instance = 0; instance < transforms.size(); ++instance )
            {
                if ( !KeepsInstanceAtDistance( cull, static_cast<uint32_t>( instance ),
                                               glm::vec3( transforms[instance][3] ), inputs.ViewOrigin ) )
                    continue;
                RayTracingPrimitive& primitive = out.Primitives.emplace_back();
                primitive.Geometry             = geometry;
                primitive.World                = transforms[instance];
                primitive.GpuSceneRow          = inputs.InstancedMotionRows[data.FirstMotionRow + instance];
                primitive.SourceMesh           = data.Mesh;
                primitive.CastShadows          = field.CastShadows;
                primitive.Submeshes            = masks;
                primitive.Kind                 = RayTracingPrimitiveKind::Instanced;
                if ( geometry == nullptr && IsResident( *data.Mesh ) )
                    primitive.ProceduralMesh = data.Mesh;
                if ( !recordsAdded )
                {
                    AppendSegmentRecords( *data.Mesh, field.Sections, primitive, out );
                    recordCount  = primitive.SegmentRecordCount;
                    recordsAdded = true;
                }
                primitive.FirstSegmentRecord = firstRecord;
                primitive.SegmentRecordCount = recordCount;
            }
        }

        // Skinned: the per-frame dynamic BLAS fills Geometry (SelectDynamicGeometry); until then counted Skinned.
        // One section per material (MeshRenderer::SkinnedSection); the BLAS leaves out what no section draws and
        // builds a masked section's segments non-opaque.
        std::vector<SectionSource> skinnedSections;
        for ( const MeshRenderer::SkinnedMeshRenderData& data : inputs.Skinned )
        {
            RayTracingPrimitive& primitive = out.Primitives.emplace_back();
            primitive.World                = data.Transform;
            primitive.GpuSceneRow          = data.MotionRow;
            primitive.Kind                 = RayTracingPrimitiveKind::Skinned;
            primitive.DeformedMesh         = data.Mesh;
            if ( data.Mesh == nullptr )
                continue;
            skinnedSections.clear();
            for ( const MeshRenderer::SkinnedSection& section : data.Sections )
                skinnedSections.push_back(
                     SourceOf( ~section.HiddenSubmeshes, MeshRenderer::ShadingOf( section ) ) );
            primitive.Submeshes = MasksOf( *data.Mesh, skinnedSections );
            AppendSegmentRecords( *data.Mesh, skinnedSections, primitive, out );
        }
    }
} // namespace Desert::Graphic::RayTracing
