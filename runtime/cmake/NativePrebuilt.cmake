# Consume a precompiled aurora + third-party package instead of compiling
# aurora-main and its extern/ tree.
#
# Produced by launcher/Prepare-NativePrebuilt.ps1 with the same toolchain and the
# same canonical flag set the consuming build uses, which is what makes a single
# package valid for every user. The translated shards and the products still
# compile locally and still link targets named aurora::gx / aurora::pad /
# aurora::si / aurora::vi / aurora::mtx. The package may also carry the native
# runtime archive; cmake/RuntimeLibrary.cmake decides whether it can be used.
#
# The aurora and dependency source trees are still present in the workspace - the
# runtime includes aurora headers directly - so the package records include
# directories as tokens (@AURORA@, @RUNTIME@, @DEPS@, @PKG@) that resolve here
# rather than baking a maintainer machine's paths into a redistributable.

get_filename_component(MKW_NP_PACKAGE_DIR "${MKW_NATIVE_PREBUILT_DIR}" ABSOLUTE)
if(NOT EXISTS "${MKW_NP_PACKAGE_DIR}/native_prebuilt.cmake")
    message(FATAL_ERROR
        "MKW_NATIVE_PREBUILT_DIR does not contain native_prebuilt.cmake: ${MKW_NP_PACKAGE_DIR}")
endif()

# The package ships as <workspace>/Dependencies/native_prebuilt, next to the
# pinned sources it was built against.
get_filename_component(MKW_NP_DEPS_DIR "${MKW_NP_PACKAGE_DIR}" DIRECTORY)
get_filename_component(MKW_NP_AURORA_DIR "${MKW_AURORA_DIR}" ABSOLUTE)
get_filename_component(MKW_NP_RUNTIME_DIR "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)

include("${MKW_NP_PACKAGE_DIR}/native_prebuilt.cmake")

function(mkw_np_resolve out_var)
    set(_resolved "")
    foreach(_item IN LISTS ARGN)
        string(REPLACE "@PKG@" "${MKW_NP_PACKAGE_DIR}" _item "${_item}")
        string(REPLACE "@AURORA@" "${MKW_NP_AURORA_DIR}" _item "${_item}")
        string(REPLACE "@RUNTIME@" "${MKW_NP_RUNTIME_DIR}" _item "${_item}")
        string(REPLACE "@DEPS@" "${MKW_NP_DEPS_DIR}" _item "${_item}")
        list(APPEND _resolved "${_item}")
    endforeach()
    set(${out_var} "${_resolved}" PARENT_SCOPE)
endfunction()

mkw_np_resolve(_mkw_np_includes ${MKW_NP_INCLUDE_DIRECTORIES})
mkw_np_resolve(_mkw_np_links ${MKW_NP_LINK_LIBRARIES})
mkw_np_resolve(_mkw_np_dawn_config ${MKW_NP_DAWN_CONFIG_DIR})
set(MKW_NP_RUNTIME_ARCHIVE_PATH "")
if(MKW_NP_RUNTIME_ARCHIVE)
    mkw_np_resolve(MKW_NP_RUNTIME_ARCHIVE_PATH "${MKW_NP_RUNTIME_ARCHIVE}")
endif()
foreach(_dir IN LISTS _mkw_np_includes)
    if(NOT IS_DIRECTORY "${_dir}")
        message(FATAL_ERROR
            "The precompiled package expects an include directory that this workspace "
            "does not have: ${_dir}")
    endif()
endforeach()

# Dawn's own packaged config (DawnTargets.cmake) links dawn::webgpu_dawn against
# Threads::Threads directly. A from-source aurora build resolves that as a side effect of
# add_subdirectory(aurora-main) pulling in Dawn's own CMakeLists.txt; this mode never runs that
# subdirectory at all, so nothing else would ever define it (verified directly: configuring without
# this fails with "The link interface of target dawn::webgpu_dawn contains: Threads::Threads but
# the target was not found"). find_package(Threads) is one of CMake's most basic finder modules and
# is a no-op on Windows (its threading support is already part of the CRT), so this is safe on
# every platform this package format targets, not just the one that first hit the failure.
find_package(Threads REQUIRED)

# Dawn is a prebuilt package on both sides; resolve the same install tree the
# package was built against so its imported target (and therefore
# webgpu_dawn.dll / dxcompiler.dll / dxil.dll) is available exactly as a
# from-source aurora build would provide it.
if(NOT EXISTS "${_mkw_np_dawn_config}/DawnConfig.cmake")
    message(FATAL_ERROR "Prebuilt Dawn package is missing DawnConfig.cmake: ${_mkw_np_dawn_config}")
endif()
set(_mkw_np_saved_find_global ${CMAKE_FIND_PACKAGE_TARGETS_GLOBAL})
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)
find_package(Dawn REQUIRED CONFIG PATHS "${_mkw_np_dawn_config}" NO_DEFAULT_PATH)
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ${_mkw_np_saved_find_global})
if(NOT TARGET dawn::webgpu_dawn)
    message(FATAL_ERROR "Prebuilt Dawn package did not provide dawn::webgpu_dawn")
endif()

# One interface target carries the whole compile/link surface. The public aurora
# names then reduce to aliases of it, which keeps the link line free of the
# fivefold duplication that giving each of them the full list would produce.
add_library(mkw_native_prebuilt INTERFACE IMPORTED GLOBAL)
set_target_properties(mkw_native_prebuilt PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_mkw_np_includes}"
    INTERFACE_COMPILE_DEFINITIONS "${MKW_NP_COMPILE_DEFINITIONS}"
    INTERFACE_COMPILE_OPTIONS "${MKW_NP_COMPILE_OPTIONS}"
    INTERFACE_LINK_LIBRARIES "${_mkw_np_links}")

foreach(_target IN LISTS MKW_NP_AURORA_TARGETS)
    add_library(${_target} INTERFACE IMPORTED GLOBAL)
    set_target_properties(${_target} PROPERTIES INTERFACE_LINK_LIBRARIES mkw_native_prebuilt)
endforeach()

list(LENGTH _mkw_np_links _mkw_np_link_count)
message(STATUS
    "Aurora and third-party libraries come from the precompiled package "
    "(${_mkw_np_link_count} link inputs); they are not compiled here")
