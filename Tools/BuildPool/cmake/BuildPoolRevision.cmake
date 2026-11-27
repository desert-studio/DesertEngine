# buildpool_revision(<project dir> <out var>) -- the revision of the BuildPool sources: the first 16 hex digits of
# SHA-256 over (path relative to the project, SHA-256 of the content) of every file the binary is built from
# (CMakeLists.txt, cmake/BuildPoolBootstrap.cmake, Include/, Source/). One revision = one binary in <home>/bin/<rev>;
# trees of the same sources share it, an edited source gets a new one. The one place this hash is defined:
# BuildPool.cmake (configure) and BuildPoolBootstrap.cmake (cmake -P) both include this file.
function(buildpool_revision project_dir out_var)
    file(GLOB_RECURSE _files LIST_DIRECTORIES false RELATIVE "${project_dir}"
        "${project_dir}/Include/*" "${project_dir}/Source/*")
    list(APPEND _files CMakeLists.txt cmake/BuildPoolBootstrap.cmake)
    list(SORT _files)
    set(_manifest "")
    foreach(_file IN LISTS _files)
        file(SHA256 "${project_dir}/${_file}" _digest)
        string(APPEND _manifest "${_file} ${_digest}\n")
    endforeach()
    string(SHA256 _rev "${_manifest}")
    string(SUBSTRING "${_rev}" 0 16 _rev)
    set(${out_var} "${_rev}" PARENT_SCOPE)
    set(${out_var}_FILES "${_files}" PARENT_SCOPE)
endfunction()
