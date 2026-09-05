cmake_minimum_required(VERSION 3.24)
project(iiSocietyHelper VERSION 0.1.0 LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    if(DEFINED ENV{HOME})
        set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local/SDK/iiSocietyHelper" CACHE PATH "Installation prefix" FORCE)
    elseif(DEFINED ENV{USERPROFILE})
        set(CMAKE_INSTALL_PREFIX "$ENV{USERPROFILE}/.local/SDK/iiSocietyHelper" CACHE PATH "Installation prefix" FORCE)
    endif()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)
find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core)

add_library(iiSocietyHelper SHARED iiSocietyHelper.cpp iiSocietyHelper.h)
add_library(iiSocietyHelper::iiSocietyHelper ALIAS iiSocietyHelper)
target_compile_features(iiSocietyHelper PUBLIC cxx_std_20)
target_compile_definitions(iiSocietyHelper PRIVATE IISOCIETYHELPER_BUILDING_LIBRARY)
target_link_libraries(iiSocietyHelper PUBLIC Qt6::Core)
target_include_directories(iiSocietyHelper PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
set_target_properties(iiSocietyHelper PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    INSTALL_RPATH_USE_LINK_PATH TRUE)

if(BUILD_TESTING)
    add_executable(iiSocietyHelper_hello_world tests/hello_world.cpp)
    target_link_libraries(iiSocietyHelper_hello_world PRIVATE iiSocietyHelper::iiSocietyHelper)
    set_target_properties(iiSocietyHelper_hello_world PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME iiSocietyHelper.hello_world COMMAND iiSocietyHelper_hello_world)
endif()

install(TARGETS iiSocietyHelper EXPORT iiSocietyHelperTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES iiSocietyHelper.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocietyHelper)
set(iiSocietyHelper_PACKAGE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/iiSocietyHelper")
configure_package_config_file(cmake/iiSocietyHelperConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/iiSocietyHelperConfig.cmake"
    INSTALL_DESTINATION "${iiSocietyHelper_PACKAGE_DIR}")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/iiSocietyHelperConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(EXPORT iiSocietyHelperTargets
    NAMESPACE iiSocietyHelper::
    DESTINATION "${iiSocietyHelper_PACKAGE_DIR}")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/iiSocietyHelperConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/iiSocietyHelperConfigVersion.cmake"
    DESTINATION "${iiSocietyHelper_PACKAGE_DIR}")
