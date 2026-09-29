# Third-party licences

One row per third-party library whose licence carries an obligation for the shipped product (notice, attribution,
licence text). The licence text itself stays with the library, at the path given.

| Library | Version | Licence | Licence / notice files | Used by |
|---|---|---|---|---|
| OpenSubdiv (Pixar) | v3_6_0 (submodule `ThirdParty/OpenSubdiv`) | Modified Apache 2.0 | `ThirdParty/OpenSubdiv/LICENSE.txt`, `ThirdParty/OpenSubdiv/NOTICE.txt` | Modeling Subdivide (`Engine/Geometry/MeshCore/DynamicMesh/Operations/SubdividePoly.cpp`); only the CPU layers sdc/vtr/far are compiled (`BuildScripts/ThirdParty/OpenSubdiv.lua`) |
| vk-bootstrap (Charles Giessen) | v1.3.290, commit 5836b6cb (vendored `ThirdParty/vk-bootstrap`) | MIT | `ThirdParty/vk-bootstrap/LICENSE.txt` | Vulkan instance/device creation and DeviceCaps probing (VKF1) |
| tinyexr (Syoyo Fujita; contains OpenEXR code, ILM) | v1.0.13 (vendored `ThirdParty/tinyexr`, sha256 in `tinyexr.cpp`) | BSD-3-Clause (+ OpenEXR BSD-3) | `ThirdParty/tinyexr/LICENSE.txt` | `.exr` texture sources in the texture cook (`Editor/Source/Editor/Import/TextureImporter.cpp`); deflate via stb, no miniz |
