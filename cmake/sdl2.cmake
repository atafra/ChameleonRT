if (TARGET SDL2::SDL2)
    return()
endif()

option(CHAMELEONRT_AUTO_FETCH_SDL2 "Auto-download SDL2 binaries when not found (Windows only)" ON)
set(SDL2_PREBUILT_DIR "" CACHE PATH "Path to existing SDL2 package root (contains cmake/sdl2-config.cmake)")
set(CHAMELEONRT_SDL2_ARCHIVE_SHA256 "" CACHE STRING "Optional SHA256 for the pinned SDL2 archive")

set(CHAMELEONRT_SDL2_VERSION "2.32.10")
set(CHAMELEONRT_SDL2_SOURCE "unresolved" CACHE INTERNAL "How SDL2 was resolved")

# Escape hatch: use a caller-provided SDL2 package root.
if (SDL2_PREBUILT_DIR)
    set(_sdl2_prebuilt_cmake "${SDL2_PREBUILT_DIR}/cmake")
    if (EXISTS "${_sdl2_prebuilt_cmake}/sdl2-config.cmake" OR EXISTS "${_sdl2_prebuilt_cmake}/SDL2Config.cmake")
        set(SDL2_DIR "${_sdl2_prebuilt_cmake}" CACHE PATH "SDL2 package configuration directory" FORCE)
        find_package(SDL2 CONFIG REQUIRED NO_DEFAULT_PATH PATHS "${SDL2_DIR}")
        set(CHAMELEONRT_SDL2_SOURCE "prebuilt" CACHE INTERNAL "How SDL2 was resolved" FORCE)
        message(STATUS "SDL2: using SDL2_PREBUILT_DIR=${SDL2_PREBUILT_DIR}")
        return()
    endif()

    message(FATAL_ERROR "SDL2_PREBUILT_DIR='${SDL2_PREBUILT_DIR}' does not contain cmake/sdl2-config.cmake")
endif()

# First, prefer any already discoverable SDL2 package (system, vcpkg, conan, etc.).
find_package(SDL2 CONFIG QUIET)
if (SDL2_FOUND)
    set(CHAMELEONRT_SDL2_SOURCE "system" CACHE INTERNAL "How SDL2 was resolved" FORCE)
    message(STATUS "SDL2: found existing package via CMake search")
    return()
endif()

# Windows-only fallback: download pinned SDL2 VC binary package.
if (WIN32 AND CHAMELEONRT_AUTO_FETCH_SDL2)
    set(_sdl2_archive_name "SDL2-devel-${CHAMELEONRT_SDL2_VERSION}-VC.zip")
    set(_sdl2_url "https://github.com/libsdl-org/SDL/releases/download/release-${CHAMELEONRT_SDL2_VERSION}/${_sdl2_archive_name}")
    set(_sdl2_cache_root "${CMAKE_CURRENT_BINARY_DIR}/sdl2")
    set(_sdl2_archive_path "${_sdl2_cache_root}/${_sdl2_archive_name}")
    set(_sdl2_extract_root "")
    set(_sdl2_cmake_dir "")

    file(MAKE_DIRECTORY "${_sdl2_cache_root}")

    set(_sdl2_candidate_roots
        "${_sdl2_cache_root}/SDL2-${CHAMELEONRT_SDL2_VERSION}"
        "${_sdl2_cache_root}/SDL2-devel-${CHAMELEONRT_SDL2_VERSION}-VC")
    foreach(_sdl2_candidate ${_sdl2_candidate_roots})
        if (EXISTS "${_sdl2_candidate}/cmake/sdl2-config.cmake" OR EXISTS "${_sdl2_candidate}/cmake/SDL2Config.cmake")
            set(_sdl2_extract_root "${_sdl2_candidate}")
            set(_sdl2_cmake_dir "${_sdl2_extract_root}/cmake")
            break()
        endif()
    endforeach()

    if (NOT _sdl2_cmake_dir)
        if (NOT EXISTS "${_sdl2_archive_path}")
            message(STATUS "SDL2: downloading pinned Windows package ${_sdl2_archive_name}")
            if (CHAMELEONRT_SDL2_ARCHIVE_SHA256)
                file(DOWNLOAD "${_sdl2_url}" "${_sdl2_archive_path}"
                    EXPECTED_HASH "SHA256=${CHAMELEONRT_SDL2_ARCHIVE_SHA256}"
                    SHOW_PROGRESS
                    STATUS _sdl2_download_status
                    LOG _sdl2_download_log)
            else()
                message(WARNING "CHAMELEONRT_SDL2_ARCHIVE_SHA256 is empty; SDL2 archive integrity is not hash-verified yet")
                file(DOWNLOAD "${_sdl2_url}" "${_sdl2_archive_path}"
                    SHOW_PROGRESS
                    STATUS _sdl2_download_status
                    LOG _sdl2_download_log)
            endif()

            list(GET _sdl2_download_status 0 _sdl2_download_code)
            if (NOT _sdl2_download_code EQUAL 0)
                message(FATAL_ERROR "SDL2 download failed from '${_sdl2_url}' with status ${_sdl2_download_status}. Log: ${_sdl2_download_log}")
            endif()
        endif()

        file(ARCHIVE_EXTRACT INPUT "${_sdl2_archive_path}" DESTINATION "${_sdl2_cache_root}")

        foreach(_sdl2_candidate ${_sdl2_candidate_roots})
            if (EXISTS "${_sdl2_candidate}/cmake/sdl2-config.cmake" OR EXISTS "${_sdl2_candidate}/cmake/SDL2Config.cmake")
                set(_sdl2_extract_root "${_sdl2_candidate}")
                set(_sdl2_cmake_dir "${_sdl2_extract_root}/cmake")
                break()
            endif()
        endforeach()
    endif()

    if (NOT _sdl2_cmake_dir)
        message(FATAL_ERROR "Auto-fetched SDL2 package did not contain expected CMake config under '${_sdl2_cache_root}'")
    endif()

    set(SDL2_DIR "${_sdl2_cmake_dir}" CACHE PATH "SDL2 package configuration directory" FORCE)
    find_package(SDL2 CONFIG REQUIRED NO_DEFAULT_PATH PATHS "${SDL2_DIR}")
    set(CHAMELEONRT_SDL2_SOURCE "auto-fetch" CACHE INTERNAL "How SDL2 was resolved" FORCE)
    message(STATUS "SDL2: using auto-fetched Windows package ${CHAMELEONRT_SDL2_VERSION}")
    return()
endif()

if (WIN32)
    message(FATAL_ERROR "SDL2 was not found. Set SDL2_DIR/SDL2_PREBUILT_DIR, or enable CHAMELEONRT_AUTO_FETCH_SDL2 for Windows auto-download.")
endif()

message(FATAL_ERROR "SDL2 was not found on this platform. Install SDL2 or set SDL2_DIR/SDL2_PREBUILT_DIR.")
