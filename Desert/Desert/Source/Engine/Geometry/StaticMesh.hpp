#pragma once

#include "Mesh.hpp"

namespace Desert
{
    class StaticMesh : public Mesh
    {
    public:
        StaticMesh( const std::vector<Vertex>& vertices, const std::vector<Index>& indices,
                    const std::vector<Submesh>& submeshes, const std::vector<MeshVertexStreams>& streams );

        [[nodiscard]] MeshType GetType() const override
        {
            return MeshType::Static;
        }

        [[nodiscard]] virtual Common::BoolResultWithCodes<MeshError> Invalidate() override;
    };

} // namespace Desert
