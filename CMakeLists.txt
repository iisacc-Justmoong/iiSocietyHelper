cmake_minimum_required(VERSION 3.24)
project(iiSocietyHelper VERSION 0.7.0 LANGUAGES CXX)

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
find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core Sql)
find_package(iiSocietyContainer 0.9.0 CONFIG REQUIRED HINTS "$ENV{HOME}/.local/SDK/iiSocietyContainer")
find_package(iiAcountManager 0.2.0 CONFIG REQUIRED HINTS "$ENV{HOME}/.local/SDK/iiAcountManager")
set(CMAKE_AUTOMOC ON)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(helper_library_type STATIC)
else()
    set(helper_library_type SHARED)
endif()
add_library(iiSocietyHelper ${helper_library_type} iiSocietyHelper.cpp iiSocietyHelper.h Helper.cpp FileSystem.cpp DeliveryStore.cpp DeliveryStore.h ObjectCodec.cpp)
add_library(iiSocietyHelper::iiSocietyHelper ALIAS iiSocietyHelper)
target_compile_features(iiSocietyHelper PUBLIC cxx_std_20)
target_compile_definitions(iiSocietyHelper PRIVATE IISOCIETYHELPER_BUILDING_LIBRARY)
target_link_libraries(iiSocietyHelper PUBLIC Qt6::Core Qt6::Sql
    iiSocietyContainer::iiSocietyContainer iiAcountManager::iiAcountManager)
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

if(APPLE)
    enable_language(OBJCXX)
    target_sources(iiSocietyHelper PRIVATE platform/apple/ObservationDirectory.mm)
    set_source_files_properties(platform/apple/ObservationDirectory.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(iiSocietyHelper PRIVATE "-framework Foundation")
    set_target_properties(iiSocietyHelper PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT ANDROID AND NOT EMSCRIPTEN)
    add_executable(ii-society-helper tools/observe.cpp)
    target_link_libraries(ii-society-helper PRIVATE iiSocietyHelper::iiSocietyHelper)
    set_target_properties(ii-society-helper PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE
        INSTALL_RPATH "$<IF:$<BOOL:${APPLE}>,@loader_path/../${CMAKE_INSTALL_LIBDIR},$ORIGIN/../${CMAKE_INSTALL_LIBDIR}>")
    install(TARGETS ii-society-helper RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(BUILD_TESTING)
    find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Test)
    add_executable(iiSocietyHelper_delivery_tests tests/delivery.cpp)
    target_link_libraries(iiSocietyHelper_delivery_tests PRIVATE iiSocietyHelper::iiSocietyHelper Qt6::Test)
    target_compile_definitions(iiSocietyHelper_delivery_tests PRIVATE HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME iiSocietyHelper.delivery COMMAND iiSocietyHelper_delivery_tests)
    set_tests_properties(iiSocietyHelper.delivery PROPERTIES TIMEOUT 30)
    add_executable(iiSocietyHelper_observation_tests tests/observation.cpp)
    target_link_libraries(iiSocietyHelper_observation_tests PRIVATE iiSocietyHelper::iiSocietyHelper Qt6::Test)
    target_compile_definitions(iiSocietyHelper_observation_tests PRIVATE
        HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME iiSocietyHelper.observation COMMAND iiSocietyHelper_observation_tests)
    set_tests_properties(iiSocietyHelper.observation PROPERTIES TIMEOUT 90)
    if(APPLE AND NOT CMAKE_CROSSCOMPILING)
        execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
            OUTPUT_VARIABLE helper_macos_sdk OUTPUT_STRIP_TRAILING_WHITESPACE)
        get_target_property(helper_qt_includes Qt6::Core INTERFACE_INCLUDE_DIRECTORIES)
        set(helper_include_args)
        foreach(helper_include IN LISTS helper_qt_includes)
            list(APPEND helper_include_args "-I${helper_include}")
        endforeach()
        get_filename_component(helper_qt_frameworks "${Qt6_DIR}/../.." ABSOLUTE)
        add_test(NAME iiSocietyHelper.ios_directory_syntax COMMAND "${CMAKE_OBJCXX_COMPILER}"
            -fsyntax-only -std=c++20 -fobjc-arc
            -target "${CMAKE_SYSTEM_PROCESSOR}-apple-ios16.0-macabi"
            -isysroot "${helper_macos_sdk}" -iframework "${helper_qt_frameworks}"
            ${helper_include_args} "${CMAKE_CURRENT_SOURCE_DIR}/tests/apple_directory_syntax.mm")
        set_tests_properties(iiSocietyHelper.ios_directory_syntax PROPERTIES TIMEOUT 30)
    endif()
    if(TARGET ii-society-helper)
        find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Qml)
        add_executable(iiSocietyHelper_object_peer tests/object_peer.cpp tests/ObjectTestTypes.h)
        target_link_libraries(iiSocietyHelper_object_peer PRIVATE iiSocietyHelper::iiSocietyHelper)
        add_executable(iiSocietyHelper_object_tests tests/objects.cpp tests/ObjectTestTypes.h)
        target_link_libraries(iiSocietyHelper_object_tests PRIVATE iiSocietyHelper::iiSocietyHelper Qt6::Test Qt6::Qml)
        target_compile_definitions(iiSocietyHelper_object_tests PRIVATE
            HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}"
            HELPER_OBJECT_PEER="$<TARGET_FILE:iiSocietyHelper_object_peer>")
        add_dependencies(iiSocietyHelper_object_tests iiSocietyHelper_object_peer)
        add_test(NAME iiSocietyHelper.objects COMMAND iiSocietyHelper_object_tests)
        set_tests_properties(iiSocietyHelper.objects PROPERTIES TIMEOUT 30)
        add_executable(iiSocietyHelper_account_tests tests/account.cpp)
        target_link_libraries(iiSocietyHelper_account_tests PRIVATE iiSocietyHelper::iiSocietyHelper Qt6::Test Qt6::Qml)
        target_compile_definitions(iiSocietyHelper_account_tests PRIVATE HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
        add_test(NAME iiSocietyHelper.account COMMAND iiSocietyHelper_account_tests)
        set_tests_properties(iiSocietyHelper.account PROPERTIES TIMEOUT 30)
        add_executable(iiSocietyHelper_filesystem_peer tests/filesystem_peer.cpp)
        target_link_libraries(iiSocietyHelper_filesystem_peer PRIVATE iiSocietyHelper::iiSocietyHelper)
        add_executable(iiSocietyHelper_filesystem_tests tests/filesystem.cpp)
        target_link_libraries(iiSocietyHelper_filesystem_tests PRIVATE iiSocietyHelper::iiSocietyHelper Qt6::Test Qt6::Qml)
        target_compile_definitions(iiSocietyHelper_filesystem_tests PRIVATE
            HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}"
            HELPER_FILESYSTEM_PEER="$<TARGET_FILE:iiSocietyHelper_filesystem_peer>"
            HELPER_DIAGNOSTIC_EXECUTABLE="$<TARGET_FILE:ii-society-helper>")
        add_dependencies(iiSocietyHelper_filesystem_tests iiSocietyHelper_filesystem_peer ii-society-helper)
        add_test(NAME iiSocietyHelper.filesystem COMMAND iiSocietyHelper_filesystem_tests)
        set_tests_properties(iiSocietyHelper.filesystem PROPERTIES TIMEOUT 30)
        add_executable(iiSocietyHelper_process_tests tests/process_observation.cpp)
        target_link_libraries(iiSocietyHelper_process_tests PRIVATE iiSocietyHelper::iiSocietyHelper Qt6::Test)
        target_compile_definitions(iiSocietyHelper_process_tests PRIVATE
            HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}"
            HELPER_OBSERVER_EXECUTABLE="$<TARGET_FILE:ii-society-helper>")
        add_dependencies(iiSocietyHelper_process_tests ii-society-helper)
        add_test(NAME iiSocietyHelper.process_observation COMMAND iiSocietyHelper_process_tests)
        set_tests_properties(iiSocietyHelper.process_observation PROPERTIES TIMEOUT 90)
    endif()
    add_executable(iiSocietyHelper_hello_world tests/hello_world.cpp)
    target_link_libraries(iiSocietyHelper_hello_world PRIVATE iiSocietyHelper::iiSocietyHelper)
    set_target_properties(iiSocietyHelper_hello_world PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME iiSocietyHelper.hello_world COMMAND iiSocietyHelper_hello_world)
endif()

install(TARGETS iiSocietyHelper EXPORT iiSocietyHelperTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES iiSocietyHelper.h DeliveryStore.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
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
