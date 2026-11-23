#pragma once

// THE MESH CA (Docs/contracts/NET-ARCH.md §2M.2: "all mesh links run over GNS too, inside the datacentre"): every
// role process presents a GNS certificate signed by the mesh CA, and a mesh peer accepts a link only when GNS
// verified that chain and the certificate names the role it claims (MeshLink.hpp MeshEndpoint). The keys live
// outside the repository: a dev CA is generated locally (DesertServer --mesh-dev-ca <dir>); an official
// deployment issues them from its own CA (NET-MESH-7).
//   <dir>/ca.cert          the self-signed root, base64 - every process trusts it
//   <dir>/ca.key           the root's private key (PEM, 0600) - issues more certificates, never leaves the host
//   <dir>/<identity>.cert  one role's certificate with its private key, base64 (0600)

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Net
{
    // Generates a new root and one certificate per identity ("mesh-sim-1"), valid @p days. False + @p error.
    bool CreateMeshDevCa( const std::filesystem::path& directory, const std::vector<std::string>& identities,
                          int days, std::string& error );
    // Trusts <dir>/ca.cert and makes <dir>/<identity>.cert this process's GNS certificate (identity "str:<id>").
    // Call after the transport exists (GNS initialised). False + @p error.
    bool InstallMeshCertificate( const std::filesystem::path& directory, std::string_view identity,
                                 std::string& error );
} // namespace Desert::Net
