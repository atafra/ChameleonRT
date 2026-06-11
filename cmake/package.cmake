# Based on OSPRay's dependency redistribution code 
# https://github.com/ospray/ospray/blob/master/cmake/ospray_redistribute_deps.cmake 

macro(crt_install_namelink NAME)
    get_filename_component(TARGET_NAME ${NAME} NAME)
    set(LIB_SUFFIX ${CMAKE_SHARED_LIBRARY_SUFFIX})

    # Create patch version suffixed namelink
    if (APPLE)
        set(LIBREGEX "(.+)[.]([0-9]+)([.][0-9]+[.][0-9]+)${LIB_SUFFIX}")
    else()
        set(LIBREGEX "(.+)${LIB_SUFFIX}[.]([0-9]+)([.][0-9]+[.][0-9]+)")
    endif()
    string(REGEX REPLACE ${LIBREGEX} "\\1" BASE_LIB_NAME ${TARGET_NAME})
    if (CMAKE_MATCH_COUNT GREATER 2)
        if (APPLE)
            set(SYMLINK ${BASE_LIB_NAME}.${CMAKE_MATCH_2}${LIB_SUFFIX})
        else()
            set(SYMLINK ${BASE_LIB_NAME}${LIB_SUFFIX}.${CMAKE_MATCH_2})
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E
            create_symlink ${TARGET_NAME} ${PROJECT_BINARY_DIR}/${SYMLINK})
        install(PROGRAMS ${PROJECT_BINARY_DIR}/${SYMLINK}
            DESTINATION bin)
        set(TARGET_NAME ${SYMLINK})
    endif()

    # Create minor version suffixed namelink
    if (APPLE)
        set(LIBREGEX "(.+)[.]([0-9]+)([.][0-9]+)${LIB_SUFFIX}")
    else()
        set(LIBREGEX "(.+)${LIB_SUFFIX}[.]([0-9]+)([.][0-9]+)")
    endif()
    string(REGEX REPLACE ${LIBREGEX} "\\1" BASE_LIB_NAME ${TARGET_NAME})
    if (CMAKE_MATCH_COUNT GREATER 2)
        if (APPLE)
            set(SYMLINK ${BASE_LIB_NAME}.${CMAKE_MATCH_2}${LIB_SUFFIX})
        else()
            set(SYMLINK ${BASE_LIB_NAME}${LIB_SUFFIX}.${CMAKE_MATCH_2})
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E
            create_symlink ${TARGET_NAME} ${PROJECT_BINARY_DIR}/${SYMLINK})
        install(PROGRAMS ${PROJECT_BINARY_DIR}/${SYMLINK}
            DESTINATION bin)
        set(TARGET_NAME ${SYMLINK})
    endif()

    # Create major version suffixed namelink
    if (APPLE)
        set(LIBREGEX "(.+)[.]([0-9]+)${LIB_SUFFIX}")
    else()
        set(LIBREGEX "(.+)${LIB_SUFFIX}[.]([0-9]+)")
    endif()
    string(REGEX REPLACE ${LIBREGEX} "\\1" BASE_LIB_NAME ${TARGET_NAME})
    if (CMAKE_MATCH_COUNT)
        set(SYMLINK ${PROJECT_BINARY_DIR}/${BASE_LIB_NAME}${LIB_SUFFIX})
        execute_process(COMMAND "${CMAKE_COMMAND}" -E
            create_symlink ${TARGET_NAME} ${SYMLINK})
        install(PROGRAMS ${SYMLINK}
            DESTINATION bin)
    endif()
endmacro()

macro(crt_add_packaged_dependency TARGET_NAME)
    get_target_property(CONFIGURATIONS ${TARGET_NAME} IMPORTED_CONFIGURATIONS)
    list(GET CONFIGURATIONS 0 CONFIGURATION)
    if ("${CONFIGURATION}" STREQUAL "CONFIGURATIONS-NOTFOUND")
        get_target_property(LIBRARY ${TARGET_NAME} IMPORTED_LOCATION)
    else()
        get_target_property(LIBRARY ${TARGET_NAME} IMPORTED_LOCATION_${CONFIGURATION})
    endif()
    # Resolve symlinks in the library name we're given
    file(REAL_PATH ${LIBRARY} LIBRARY)

    # Stage the resolved runtime library next to the executable (and install it)
    # using the same mechanism as the other packaged files so it is available
    # when running directly from the build tree.
    crt_add_packaged_files("${LIBRARY}")
endmacro()

# crt_add_packaged_files(<files>... [DEPENDS <targets>...])
#
# Stages the given runtime libraries next to the executable. When the files are
# produced by other targets (e.g. ExternalProject builds such as oidn_ext or
# dpcpp_ext), pass those targets via DEPENDS so the copy steps build *after* the
# libraries exist. Without this the staging projects have no build-order
# dependency on the producers and a clean build races them, trying to copy files
# that do not exist yet (the copy then only succeeds on a second build).
macro(crt_add_packaged_files)
    cmake_parse_arguments(CRT_PKG "" "" "DEPENDS" ${ARGN})

    if (NOT TARGET crt_stage_packaged_files)
        add_custom_target(crt_stage_packaged_files ALL)
        if (CRT_DEPENDENCY_FOLDER)
            set_target_properties(crt_stage_packaged_files PROPERTIES
                FOLDER "${CRT_DEPENDENCY_FOLDER}")
        endif()
    endif()

    foreach(LIBRARY ${CRT_PKG_UNPARSED_ARGUMENTS})
        if (NOT LIBRARY)
            continue()
        endif()

        # Build a human readable target name from the library's file name so the
        # generated IDE projects are self-describing (e.g. "stage_OpenImageDenoise_dll")
        # instead of an opaque "crt_stage_packaged_file_<md5>".
        get_filename_component(COPY_TARGET_FILE "${LIBRARY}" NAME)
        string(REGEX REPLACE "[^A-Za-z0-9]" "_" COPY_TARGET_SUFFIX "${COPY_TARGET_FILE}")
        set(COPY_TARGET_NAME "stage_${COPY_TARGET_SUFFIX}")

        # Guard against two different paths sharing the same file name: keep the
        # descriptive name unique by appending a short content hash so neither
        # file is silently dropped.
        if (TARGET ${COPY_TARGET_NAME} AND NOT "${LIBRARY}" STREQUAL "${CRT_STAGED_PATH_${COPY_TARGET_NAME}}")
            string(MD5 COPY_TARGET_HASH "${LIBRARY}")
            string(SUBSTRING "${COPY_TARGET_HASH}" 0 8 COPY_TARGET_HASH)
            set(COPY_TARGET_NAME "stage_${COPY_TARGET_SUFFIX}_${COPY_TARGET_HASH}")
        endif()

        if (NOT TARGET ${COPY_TARGET_NAME})
            # Stage next to the executable. For multi-config generators (e.g.
            # Visual Studio) the executable lives in a per-config subfolder
            # (Debug/Release/...), so copy to the executable's actual output
            # directory rather than the raw PROJECT_BINARY_DIR. make_directory
            # guards against the destination not existing yet on a clean build.
            add_custom_target(${COPY_TARGET_NAME}
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:chameleonrt>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIBRARY}" "$<TARGET_FILE_DIR:chameleonrt>"
                VERBATIM)
            set(CRT_STAGED_PATH_${COPY_TARGET_NAME} "${LIBRARY}")
            if (CRT_DEPENDENCY_FOLDER)
                set_target_properties(${COPY_TARGET_NAME} PROPERTIES
                    FOLDER "${CRT_DEPENDENCY_FOLDER}")
            endif()
            if (CRT_PKG_DEPENDS)
                add_dependencies(${COPY_TARGET_NAME} ${CRT_PKG_DEPENDS})
            endif()
        endif()
        add_dependencies(crt_stage_packaged_files ${COPY_TARGET_NAME})

        crt_install_namelink("${LIBRARY}")
        install(PROGRAMS "${LIBRARY}"
            DESTINATION bin)
    endforeach()
endmacro()

