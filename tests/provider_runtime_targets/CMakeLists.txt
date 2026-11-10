cmake_minimum_required(VERSION 3.24)
project(ProviderRuntimeAlignment LANGUAGES NONE)
include("${HELPER_SOURCE}/cmake/ProviderHeaderAlignment.cmake")
add_library(iiFileProvider::Native SHARED IMPORTED)
set_target_properties(iiFileProvider::Native PROPERTIES
    IMPORTED_CONFIGURATIONS NOCONFIG
    IMPORTED_LOCATION_NOCONFIG "${SELECTED}"
    IMPORTED_SONAME_NOCONFIG "@rpath/${RUNTIME_NAME}")
if(QT_SELECTED AND QT_RUNTIME_NAME)
    add_library(iiFileProvider::iiFileProvider SHARED IMPORTED)
    set_target_properties(iiFileProvider::iiFileProvider PROPERTIES
        IMPORTED_CONFIGURATIONS NOCONFIG
        IMPORTED_LOCATION_NOCONFIG "${QT_SELECTED}"
        IMPORTED_SONAME_NOCONFIG "@rpath/${QT_RUNTIME_NAME}")
endif()
add_library(example::backend SHARED IMPORTED)
set_target_properties(example::backend PROPERTIES
    IMPORTED_CONFIGURATIONS RELEASE
    IMPORTED_LOCATION_RELEASE "${BACKEND}")
iisociety_check_provider_runtime_collision(example::backend)
