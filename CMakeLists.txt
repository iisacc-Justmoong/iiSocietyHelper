cmake_minimum_required(VERSION 3.24)
project(iiSocietyHelper VERSION 0.12.0 LANGUAGES CXX)
include(./cmake/ModuleImports.cmake)
if(EMSCRIPTEN)
    set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS TRUE)
endif()
if(UNIX AND PROJECT_IS_TOP_LEVEL)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ResourceLinks.cmake")
    iiSocietyHelper_compat_links("${CMAKE_CURRENT_SOURCE_DIR}/resources" "${CMAKE_CURRENT_BINARY_DIR}")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# File creation is owned by iiFileProvider.





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
if(BUILD_TESTING)
    set(IISOCIETYHELPER_SYNC_TEST_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources" CACHE STRING
        "Test data directory as seen by the target runtime")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_helper_test_runner "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/resource_runner.py" auto
        "${CMAKE_CURRENT_SOURCE_DIR}/resources")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestWorkspace.cmake")
    add_test(NAME iiSocietyHelper.ResourceWorkspace COMMAND ${_helper_test_runner}
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_resource_runner.py")
    add_test(NAME iiSocietyHelper.ProviderHeaderAlignment COMMAND ${_helper_test_runner}
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_provider_header_alignment.py"
        "${CMAKE_CURRENT_SOURCE_DIR}" @WORK@ "${CMAKE_COMMAND}")
    add_test(NAME iiSocietyHelper.ProviderRuntimeAlignment COMMAND ${_helper_test_runner}
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_provider_runtime_alignment.py"
        "${CMAKE_CURRENT_SOURCE_DIR}" @WORK@ "${CMAKE_COMMAND}"
        "${CMAKE_CURRENT_BINARY_DIR}/tests/provider-runtime-alignment")
    set_tests_properties(iiSocietyHelper.ProviderRuntimeAlignment PROPERTIES RUN_SERIAL TRUE)
    if(UNIX)
        add_test(NAME iiSocietyHelper.ResourceLinks COMMAND ${_helper_test_runner}
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_resource_links.py"
            "${CMAKE_COMMAND}" @WORK@)
    endif()
    if(CMAKE_CROSSCOMPILING)
        set(_helper_test_output_root "${IISOCIETYHELPER_SYNC_TEST_DIRECTORY}")
    else()
        set(_helper_test_output_root "@WORK@")
    endif()
endif()
find_package(Threads REQUIRED)
add_library(iiSocietyHelperResidentNative SHARED src/Client/ResidentNative.cpp src/Client/ResidentNative.h)
add_library(iiSocietyHelperNative::ResidentClient ALIAS iiSocietyHelperResidentNative)
set_target_properties(iiSocietyHelperResidentNative PROPERTIES EXPORT_NAME ResidentClient
    VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR} AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_compile_features(iiSocietyHelperResidentNative PUBLIC cxx_std_23)
target_compile_definitions(iiSocietyHelperResidentNative PRIVATE IISOCIETYHELPER_RESIDENT_BUILD)
target_link_libraries(iiSocietyHelperResidentNative PRIVATE Threads::Threads)
if(EMSCRIPTEN)
    target_compile_options(iiSocietyHelperResidentNative PRIVATE -fPIC)
    target_link_options(iiSocietyHelperResidentNative PRIVATE -sSIDE_MODULE=1)
endif()
target_include_directories(iiSocietyHelperResidentNative PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/include/iiSocietyHelperNative/ResidentNative.h"
    CONTENT "#pragma once\n#include \"${CMAKE_CURRENT_SOURCE_DIR}/src/Client/ResidentNative.h\"\n")
if(WIN32)
    target_link_libraries(iiSocietyHelperResidentNative PRIVATE bcrypt advapi32)
endif()
# The standalone resident client is available in both build profiles.
install(TARGETS iiSocietyHelperResidentNative EXPORT iiSocietyHelperNativeTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES src/Client/ResidentNative.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSocietyHelperNative)
install(EXPORT iiSocietyHelperNativeTargets NAMESPACE iiSocietyHelperNative:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSocietyHelperNative)
configure_package_config_file(cmake/iiSocietyHelperNativeConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/iiSocietyHelperNativeConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSocietyHelperNative)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/iiSocietyHelperNativeConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/iiSocietyHelperNativeConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/iiSocietyHelperNativeConfigVersion.cmake" DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSocietyHelperNative)
set(_helper_root_headers
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/Export.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Account/AccountSession.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Client/Client.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Client/LocalClient.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Client/ResidentClient.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Client/LocalService.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Discovery/AutomaticPairing.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Discovery/DeviceInfo.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Discovery/DiscoveryService.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Discovery/NearbyDevices.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Storage/GroupSessionStore.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Storage/PairingCredentials.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Storage/StateCrypto.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Media/FileDownload.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Media/FileHost.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Media/MediaExport.h"
)
foreach(_header IN LISTS _helper_root_headers)
    get_filename_component(_name "${_header}" NAME)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/include/iiSocietyHelper/${_name}"
        CONTENT "#pragma once\n#include \"${_header}\"\n")
endforeach()
# Keep the legacy flat umbrella, delivery and private source includes available.
foreach(_header IN ITEMS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/iiSocietyHelper.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Delivery/DeliveryStore.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Delivery/LocalDirectory.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Discovery/PresenceWorker_p.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Files/CallbackGate_p.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Files/FilePresentation_p.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Storage/Managed/ManagedFileOperation_p.h")
    get_filename_component(_name "${_header}" NAME)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/include/${_name}"
        CONTENT "#pragma once\n#include \"${_header}\"\n")
endforeach()
foreach(_feature Sync Monitoring Generation)
    file(GLOB_RECURSE _feature_headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/${_feature}/*.h")
    foreach(_header IN LISTS _feature_headers)
        file(RELATIVE_PATH _relative "${CMAKE_CURRENT_SOURCE_DIR}/src" "${_header}")
        file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/include/iiSocietyHelper/${_relative}"
            CONTENT "#pragma once\n#include \"${_header}\"\n")
    endforeach()
endforeach()
if(BUILD_TESTING)
    add_test(NAME iiSocietyHelper.SourceLayout COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_layout.cmake")
endif()
option(IISOCIETYHELPER_BUILD_CORE "Build the account, IPC, storage, sync and remote-generation facade" ON)
if(NOT IISOCIETYHELPER_BUILD_CORE)
    set(IISOCIETYHELPER_BUILD_GENERATION_HOST OFF)
    set(IISOCIETYHELPER_WITH_BLE OFF)
    include(cmake/Monitoring.cmake)
    install(DIRECTORY src/Monitoring DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSocietyHelper FILES_MATCHING PATTERN "*.h")
    include(cmake/Package.cmake)
    return()
endif()
find_package(iiWebServer 0.6.0 CONFIG REQUIRED)
find_package(iiFileProviderTemporary CONFIG REQUIRED)
find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core Gui Sql Network)
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(iiSocietyHelperWorkspacePathTests tests/workspace_path.cpp)
    target_link_libraries(iiSocietyHelperWorkspacePathTests PRIVATE Qt6::Core)
    iisociety_fixture_test(iiSocietyHelper.ResourcePathOverride iiSocietyHelperWorkspacePathTests
        override)
    add_test(NAME iiSocietyHelper.ResourcePathFallback COMMAND "${CMAKE_COMMAND}" -E env
        --unset=IISOCIETYHELPER_TEST_DIRECTORY $<TARGET_FILE:iiSocietyHelperWorkspacePathTests>
        fallback)
endif()
if(APPLE AND NOT IOS AND TARGET WrapOpenGL::WrapOpenGL)
    get_target_property(_helper_gl WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
    if(_helper_gl)
        list(FILTER _helper_gl EXCLUDE REGEX "AGL")
        set_property(TARGET WrapOpenGL::WrapOpenGL PROPERTY INTERFACE_LINK_LIBRARIES "${_helper_gl}")
    endif()
endif()
find_package(iiFileProvider 0.7 CONFIG REQUIRED HINTS "$ENV{HOME}/.local/SDK/iiFileProvider")
include(cmake/ProviderHeaderAlignment.cmake)
iisociety_check_provider_targets()
find_package(iiSocietyContainer 0.16.1 CONFIG REQUIRED HINTS "$ENV{HOME}/.local/SDK/iiSocietyContainer")
if(NOT iiSocietyContainer_HAS_MANAGED_FILES)
    message(FATAL_ERROR "Install iiSocietyContainer with managed iiFileProvider storage before building iiSocietyHelper")
endif()
if(NOT iiSocietyContainer_HAS_APPLICATION_STORAGE)
    message(FATAL_ERROR "Install iiSocietyContainer with application-owned shared logical storage before building Helper")
endif()
set(IISOCIETYHELPER_CANVAS_VERSION "0.35.39" CACHE STRING "Exact Canvas ABI shared by this Helper profile and its consumers")
find_package(iiSharedCanvas ${IISOCIETYHELPER_CANVAS_VERSION} EXACT CONFIG REQUIRED COMPONENTS Core HINTS "$ENV{HOME}/.local/SDK/iiSharedCanvas")
find_package(iiAccountManager 0.2.8 CONFIG REQUIRED HINTS "$ENV{HOME}/.local/SDK/iiAccountManager")
iisociety_check_provider_runtime_collision(iiWebServer::iiWebServer
    iiSocietyContainer::iiSocietyContainer iiSharedCanvas::Core)
set(CMAKE_AUTOMOC ON)

set(helper_library_type SHARED)
add_library(iiSocietyHelper ${helper_library_type} src/Core/iiSocietyHelper.cpp src/Core/iiSocietyHelper.h src/Core/Helper.cpp src/Files/FileSystem.cpp src/Storage/Managed/ManagedStorage.cpp src/Storage/Managed/ManagedFileOperation_p.h src/Storage/Canvas/CanvasStorage.cpp src/Storage/Generation/GenerationStorage.cpp src/Media/MediaExport.cpp src/Delivery/DeliveryStore.cpp src/Delivery/DeliveryStore.h src/Objects/ObjectCodec.cpp src/Delivery/LocalDirectory.cpp src/Delivery/LocalDirectory.h)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set_target_properties(iiSocietyHelper PROPERTIES FRAMEWORK TRUE)
elseif(EMSCRIPTEN)
    target_compile_options(iiSocietyHelper PRIVATE -fPIC)
    target_link_options(iiSocietyHelper PRIVATE -sSIDE_MODULE=1)
endif()
target_sources(iiSocietyHelper PRIVATE
    src/Media/FileDownload.h src/Media/FileDownload.cpp src/Media/FileHost.h src/Media/FileHost.cpp
    src/Client/Client.h src/Client/Client.cpp src/Client/LocalFrame.h src/Client/LocalFrame.cpp
    src/Client/LocalClient.h src/Client/LocalClient.cpp src/Client/LocalService.h src/Client/LocalService.cpp
    src/Client/ResidentClient.h src/Client/ResidentClient.cpp
    src/Account/AccountSession.h src/Account/AccountSession.cpp src/Discovery/DeviceInfo.h src/Discovery/DeviceInfo.cpp
    src/Discovery/DiscoveryService.h src/Discovery/NearbyDevices.h src/Discovery/NearbyDevices.cpp src/Discovery/AutomaticPairing.h src/Discovery/AutomaticPairing.cpp
    src/Storage/PairingCredentials.h src/Storage/PairingCredentials.cpp src/Discovery/HybridDiscoveryService.cpp
    src/Storage/GroupSessionStore.h src/Storage/GroupSessionStore.cpp src/Storage/StateCrypto.h src/Storage/StateCrypto.cpp src/Core/Export.h)
add_library(iiSocietyHelper::iiSocietyHelper ALIAS iiSocietyHelper)
target_compile_features(iiSocietyHelper PUBLIC cxx_std_23)
target_compile_definitions(iiSocietyHelper PRIVATE IISOCIETYHELPER_BUILDING_LIBRARY SOCIETY_APP_VERSION="${PROJECT_VERSION}")
if(helper_library_type STREQUAL "STATIC")
    target_compile_definitions(iiSocietyHelper PUBLIC IISOCIETYHELPER_STATIC)
endif()
target_link_libraries(iiSocietyHelper PUBLIC Qt6::Core Qt6::Sql
    iiSocietyContainer::iiSocietyContainer iiSharedCanvas::Core iiAccountManager::iiAccountManager Qt6::Network Qt6::Gui iiWebServer::iiWebServer)
target_link_libraries(iiSocietyHelper PRIVATE iiFileProvider::iiFileProvider iiSocietyHelper::Monitoring iiFileProvider::Temporary)
target_link_libraries(iiSocietyHelper PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(iiSocietyHelper PRIVATE advapi32)
endif()
target_sources(iiSocietyHelper PRIVATE src/Files/FilePresentation.cpp src/Files/FilePresentation_p.h)
target_link_libraries(iiSocietyHelper PRIVATE iiFileProvider::Native)
target_include_directories(iiSocietyHelper PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
set_target_properties(iiSocietyHelper PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    INSTALL_RPATH_USE_LINK_PATH TRUE)

include(cmake/Monitoring.cmake)
include(cmake/Sync.cmake)
include(cmake/Generation.cmake)

if(APPLE)
    enable_language(OBJCXX)
    target_sources(iiSocietyHelper PRIVATE src/platform/apple/ObservationDirectory.mm)
    set_source_files_properties(src/platform/apple/ObservationDirectory.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(iiSocietyHelper PRIVATE "-framework Foundation")
    set_target_properties(iiSocietyHelper PROPERTIES OBJCXX_STANDARD 23 OBJCXX_STANDARD_REQUIRED YES)
endif()

if(WIN32)
    target_link_libraries(iiSocietyHelper PRIVATE advapi32)
endif()
if(APPLE)
    find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Concurrent)
    target_link_libraries(iiSocietyHelper PRIVATE Qt6::Concurrent)
    enable_language(OBJCXX)
    target_sources(iiSocietyHelper PRIVATE src/Storage/AppleStateLocation.mm)
    set_source_files_properties(src/Storage/AppleStateLocation.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(iiSocietyHelper PRIVATE "-framework Foundation" "-framework Security")
    set_target_properties(iiSocietyHelper PROPERTIES OBJCXX_STANDARD 23 OBJCXX_STANDARD_REQUIRED ON)
endif()
if(IOS)
    if(NOT CMAKE_GENERATOR STREQUAL "Xcode" AND NOT CMAKE_Swift_COMPILER_TARGET)
        set(CMAKE_Swift_COMPILER_TARGET "arm64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}")
        if(CMAKE_OSX_SYSROOT MATCHES "iphonesimulator")
            string(APPEND CMAKE_Swift_COMPILER_TARGET "-simulator")
        endif()
    endif()
    enable_language(Swift)
    target_sources(iiSocietyHelper PRIVATE src/Discovery/IosDeviceInfo.mm)
    add_library(iiSocietyHelperCrypto STATIC src/Storage/IosStateCrypto.swift)
    set_target_properties(iiSocietyHelperCrypto PROPERTIES AUTOMOC OFF Swift_LANGUAGE_VERSION 5)
    target_compile_options(iiSocietyHelperCrypto PRIVATE "$<$<COMPILE_LANGUAGE:Swift>:-parse-as-library>")
    target_link_libraries(iiSocietyHelperCrypto PRIVATE "-framework CryptoKit" "-framework Foundation")
    target_link_libraries(iiSocietyHelper PRIVATE iiSocietyHelperCrypto "-framework UIKit")
elseif(ANDROID)
    set(SOCIETY_ANDROID_OPENSSL_DIR "" CACHE PATH "OpenSSL runtime directory")
    target_include_directories(iiSocietyHelper PRIVATE "${SOCIETY_ANDROID_OPENSSL_DIR}/../include")
    target_link_libraries(iiSocietyHelper PRIVATE "${SOCIETY_ANDROID_OPENSSL_DIR}/libcrypto_3.so")
else()
    find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
    target_link_libraries(iiSocietyHelper PRIVATE OpenSSL::Crypto)
endif()
if(ANDROID)
    target_sources(iiSocietyHelper PRIVATE src/Discovery/AndroidDiscoveryService.cpp)
else()
    find_path(IISOCIETYHELPER_DNSSD_INCLUDE dns_sd.h PATH_SUFFIXES avahi-compat-libdns_sd REQUIRED)
    target_sources(iiSocietyHelper PRIVATE src/Discovery/DnsSdDiscoveryService.cpp)
    target_include_directories(iiSocietyHelper PRIVATE "${IISOCIETYHELPER_DNSSD_INCLUDE}")
    if(NOT APPLE)
        find_library(IISOCIETYHELPER_DNSSD_LIBRARY NAMES dns_sd dnssd REQUIRED)
        target_link_libraries(iiSocietyHelper PRIVATE "${IISOCIETYHELPER_DNSSD_LIBRARY}")
    endif()
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT ANDROID AND NOT EMSCRIPTEN)
    add_executable(ii-society-helper tools/observe.cpp)
    target_link_libraries(ii-society-helper PRIVATE iiSocietyHelper::iiSocietyHelper)
    set_target_properties(ii-society-helper PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE
        INSTALL_RPATH "$<IF:$<BOOL:${APPLE}>,@loader_path/../${CMAKE_INSTALL_LIBDIR},$ORIGIN/../${CMAKE_INSTALL_LIBDIR}>")
    install(TARGETS ii-society-helper RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(BUILD_TESTING)
    add_executable(iiSocietyHelper_application_storage_tests tests/application_storage.cpp)
    target_link_libraries(iiSocietyHelper_application_storage_tests PRIVATE iiSocietyHelper::iiSocietyHelper)
    target_compile_definitions(iiSocietyHelper_application_storage_tests PRIVATE HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/resources")
    iisociety_fixture_test(iiSocietyHelper.application_storage iiSocietyHelper_application_storage_tests)
    iisociety_fixture_test(iiSocietyHelper.application_storage_offline iiSocietyHelper_application_storage_tests --offline)
    add_test(NAME iiSocietyHelper.application_storage_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_application_storage.py")
    set_tests_properties(iiSocietyHelper.application_storage iiSocietyHelper.application_storage_offline PROPERTIES TIMEOUT 240)
    add_executable(iiSocietyHelper_managed_storage_tests tests/managed_storage.cpp)
    target_link_libraries(iiSocietyHelper_managed_storage_tests PRIVATE iiSocietyHelper::iiSocietyHelper iiFileProvider::iiFileProvider)
    target_compile_definitions(iiSocietyHelper_managed_storage_tests PRIVATE HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/resources")
    iisociety_fixture_test(iiSocietyHelper.managed_storage iiSocietyHelper_managed_storage_tests)
    set_tests_properties(iiSocietyHelper.managed_storage PROPERTIES TIMEOUT 120)
    add_test(NAME iiSocietyHelper.ctest_boundary COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_boundary.py")
    add_executable(iiSocietyHelper_presence_worker_tests tests/presence_worker.cpp)
    target_compile_features(iiSocietyHelper_presence_worker_tests PRIVATE cxx_std_23)
    target_link_libraries(iiSocietyHelper_presence_worker_tests PRIVATE Qt6::Core Threads::Threads)
    iisociety_fixture_test(iiSocietyHelper.presence_worker iiSocietyHelper_presence_worker_tests)
    add_test(NAME iiSocietyHelper.presence_worker_ctest_contract
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_presence_worker.py")
    add_executable(iiSocietyHelper_merged_api_tests tests/merged_api.cpp)
    target_link_libraries(iiSocietyHelper_merged_api_tests PRIVATE iiSocietyHelper::iiSocietyHelper)
    target_compile_definitions(iiSocietyHelper_merged_api_tests PRIVATE HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/resources")
    iisociety_fixture_test(iiSocietyHelper.merged_api iiSocietyHelper_merged_api_tests)
    if(NOT CMAKE_CROSSCOMPILING)
        if(UNIX)
            add_executable(iiSocietyHelper_resident_native_tests tests/client/resident_native.cpp)
            target_link_libraries(iiSocietyHelper_resident_native_tests PRIVATE iiSocietyHelperNative::ResidentClient Threads::Threads)
            target_compile_definitions(iiSocietyHelper_resident_native_tests PRIVATE RESIDENT_TEST_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/resources")
            iisociety_fixture_test(iiSocietyHelper.resident_native iiSocietyHelper_resident_native_tests)
        endif()
        add_executable(iiSocietyHelper_resident_client_tests tests/client/resident_client.cpp src/Client/LocalFrame.cpp)
        target_link_libraries(iiSocietyHelper_resident_client_tests PRIVATE iiSocietyHelper::iiSocietyHelper)
        iisociety_fixture_test(iiSocietyHelper.resident_client iiSocietyHelper_resident_client_tests)
        add_executable(iiSocietyHelperSignalRecorderTests tests/signal_recorder_tests.cpp)
        target_link_libraries(iiSocietyHelperSignalRecorderTests PRIVATE Qt6::Core)
        target_compile_features(iiSocietyHelperSignalRecorderTests PRIVATE cxx_std_23)
        set_target_properties(iiSocietyHelperSignalRecorderTests PROPERTIES AUTOMOC ON)
        add_test(NAME iiSocietyHelper.signal_recorder COMMAND iiSocietyHelperSignalRecorderTests)
        add_test(NAME iiSocietyHelper.local_transport_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_local_transport.py")
        add_test(NAME iiSocietyHelper.resident_python COMMAND ${_helper_test_runner}
            "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/client/test_resident_client.py")
        add_executable(iiSocietyHelper_local_frame_tests tests/client/local_frame.cpp src/Client/LocalFrame.cpp src/Client/LocalFrame.h)
        target_link_libraries(iiSocietyHelper_local_frame_tests PRIVATE Qt6::Core)
        iisociety_fixture_test(iiSocietyHelper.local_frame iiSocietyHelper_local_frame_tests)
        add_test(NAME iiSocietyHelper.local_frame_ctest_contract
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_local_frame.py")
        add_executable(iiSocietyHelper_session_tests tests/client/client.cpp src/Client/LocalFrame.cpp)
        target_link_libraries(iiSocietyHelper_session_tests PRIVATE iiSocietyHelper::iiSocietyHelper)
        target_compile_definitions(iiSocietyHelper_session_tests PRIVATE
            SOCIETY_ACCOUNT_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/client/account.json"
            CLIENT_TEST_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/resources")
        iisociety_fixture_test(iiSocietyHelper.session iiSocietyHelper_session_tests)
        iisociety_fixture_test(iiSocietyHelper.session.offline iiSocietyHelper_session_tests stateRemovalReportsLegacyFailure)
        set_tests_properties(iiSocietyHelper.session.offline PROPERTIES TIMEOUT 30 ENVIRONMENT "SOCIETY_DISABLE_SESSION_RESTORE=1")
        add_test(NAME iiSocietyHelper.session_ctest_contract
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_session.py")
        add_test(NAME iiSocietyHelper.automatic_pairing_idle COMMAND iiSocietyHelper_session_tests automaticPairingIdleDoesNotPoll)
        set_tests_properties(iiSocietyHelper.automatic_pairing_idle PROPERTIES TIMEOUT 30)
        add_test(NAME iiSocietyHelper.automatic_pairing_retry COMMAND iiSocietyHelper_session_tests automaticPairingRetriesAtDeadline)
        set_tests_properties(iiSocietyHelper.automatic_pairing_retry PROPERTIES TIMEOUT 30)
        set_tests_properties(iiSocietyHelper.session PROPERTIES TIMEOUT 300 ENVIRONMENT "SOCIETY_DISABLE_SESSION_RESTORE=1")
        if(APPLE AND NOT IOS)
            # Exercise the private native Keychain adapter without exporting it.
            add_executable(iiSocietyHelper_apple_state_tests tests/client/apple_state.mm src/Storage/AppleStateLocation.mm)
            target_link_libraries(iiSocietyHelper_apple_state_tests PRIVATE iiSocietyHelper::iiSocietyHelper Qt6::Concurrent "-framework Security")
            target_compile_definitions(iiSocietyHelper_apple_state_tests PRIVATE HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/resources")
            set_target_properties(iiSocietyHelper_apple_state_tests PROPERTIES OBJCXX_STANDARD 23)
            iisociety_fixture_test(iiSocietyHelper.apple_state iiSocietyHelper_apple_state_tests)
            add_test(NAME iiSocietyHelper.apple_state_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_apple_state.py")
        endif()
    endif()
    add_executable(iiSocietyHelper_delivery_tests tests/delivery.cpp)
    target_link_libraries(iiSocietyHelper_delivery_tests PRIVATE iiSocietyHelper::iiSocietyHelper)
    target_compile_definitions(iiSocietyHelper_delivery_tests PRIVATE HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/resources")
    iisociety_fixture_test(iiSocietyHelper.delivery iiSocietyHelper_delivery_tests)
    iisociety_fixture_test(iiSocietyHelper.delivery_legacy_interop iiSocietyHelper_delivery_tests legacySqliteAndNativeDeliveryRemainInteroperable)
    iisociety_fixture_test(iiSocietyHelper.delivery_ready_lock iiSocietyHelper_delivery_tests initializedStoreDoesNotWaitForAnotherInitializationLease)
    iisociety_fixture_test(iiSocietyHelper.delivery_new_lock iiSocietyHelper_delivery_tests firstSchemaCreationStillRequiresItsInitializationLease)
    set_tests_properties(iiSocietyHelper.delivery_legacy_interop iiSocietyHelper.delivery_ready_lock iiSocietyHelper.delivery_new_lock PROPERTIES TIMEOUT 120)
    add_test(NAME iiSocietyHelper.delivery_ctest_contract
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_delivery.py")
    set_tests_properties(iiSocietyHelper.delivery PROPERTIES TIMEOUT 30)
    add_executable(iiSocietyHelper_delivery_native_database_tests tests/delivery_native_database.cpp)
    target_compile_features(iiSocietyHelper_delivery_native_database_tests PRIVATE cxx_std_23)
    target_link_libraries(iiSocietyHelper_delivery_native_database_tests PRIVATE iiSocietyHelper::iiSocietyHelper iiFileProvider::Native)
    target_compile_definitions(iiSocietyHelper_delivery_native_database_tests PRIVATE HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/resources")
    iisociety_fixture_test(iiSocietyHelper.delivery_native_database iiSocietyHelper_delivery_native_database_tests)
    set_tests_properties(iiSocietyHelper.delivery_native_database PROPERTIES TIMEOUT 120)
    add_test(NAME iiSocietyHelper.native_delivery_boundary COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_delivery_boundary.py")
    add_executable(iiSocietyHelper_observation_tests tests/observation.cpp)
    target_link_libraries(iiSocietyHelper_observation_tests PRIVATE iiSocietyHelper::iiSocietyHelper iiFileProvider::Native)
    target_compile_definitions(iiSocietyHelper_observation_tests PRIVATE
        HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/resources")
    iisociety_fixture_test(iiSocietyHelper.observation iiSocietyHelper_observation_tests)
    iisociety_fixture_test(iiSocietyHelper.observation.offline iiSocietyHelper_observation_tests asynchronousLifecycleKeepsOwnerResponsive)
    iisociety_fixture_test(iiSocietyHelper.observation.delivery_thread iiSocietyHelper_observation_tests asynchronousStopPreservesSynchronousDeliveryAndAllowsRestart)
    set_tests_properties(iiSocietyHelper.observation.delivery_thread PROPERTIES TIMEOUT 120)
    set_tests_properties(iiSocietyHelper.observation iiSocietyHelper.observation.offline PROPERTIES TIMEOUT 90)
    add_test(NAME iiSocietyHelper.observation_ctest_contract
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_observation.py")
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
        target_link_libraries(iiSocietyHelper_object_tests PRIVATE iiSocietyHelper::iiSocietyHelper Qt6::Qml)
        target_compile_definitions(iiSocietyHelper_object_tests PRIVATE
            HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/resources"
            HELPER_OBJECT_PEER="$<TARGET_FILE:iiSocietyHelper_object_peer>")
        add_dependencies(iiSocietyHelper_object_tests iiSocietyHelper_object_peer)
        iisociety_fixture_test(iiSocietyHelper.objects iiSocietyHelper_object_tests)
        add_test(NAME iiSocietyHelper.objects_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_objects.py")
        set_tests_properties(iiSocietyHelper.objects PROPERTIES TIMEOUT 30)
        add_executable(iiSocietyHelper_account_tests tests/account.cpp)
        target_link_libraries(iiSocietyHelper_account_tests PRIVATE iiSocietyHelper::iiSocietyHelper Qt6::Qml)
        add_executable(iiSocietyHelperQtSignalCounterTests tests/qt_signal_counter_tests.cpp)
        target_compile_features(iiSocietyHelperQtSignalCounterTests PRIVATE cxx_std_23)
        target_link_libraries(iiSocietyHelperQtSignalCounterTests PRIVATE Qt6::Core)
        add_test(NAME iiSocietyHelper.qt_signal_counter COMMAND iiSocietyHelperQtSignalCounterTests)
        add_test(NAME iiSocietyHelper.account_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_account.py")
        target_compile_definitions(iiSocietyHelper_account_tests PRIVATE HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/resources")
        iisociety_fixture_test(iiSocietyHelper.account iiSocietyHelper_account_tests)
        set_tests_properties(iiSocietyHelper.account PROPERTIES TIMEOUT 30)
        add_executable(iiSocietyHelper_filesystem_peer tests/filesystem_peer.cpp)
        target_link_libraries(iiSocietyHelper_filesystem_peer PRIVATE iiSocietyHelper::iiSocietyHelper)
        add_executable(iiSocietyHelper_filesystem_tests tests/filesystem.cpp)
        target_link_libraries(iiSocietyHelper_filesystem_tests PRIVATE iiSocietyHelper::iiSocietyHelper iiSocietyHelper::Monitoring iiFileProvider::iiFileProvider iiFileProvider::Native Qt6::Qml)
        target_compile_definitions(iiSocietyHelper_filesystem_tests PRIVATE
            HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/resources"
            HELPER_FILESYSTEM_PEER="$<TARGET_FILE:iiSocietyHelper_filesystem_peer>"
            HELPER_DIAGNOSTIC_EXECUTABLE="$<TARGET_FILE:ii-society-helper>")
        add_dependencies(iiSocietyHelper_filesystem_tests iiSocietyHelper_filesystem_peer ii-society-helper)
        iisociety_fixture_test(iiSocietyHelper.filesystem iiSocietyHelper_filesystem_tests)
        iisociety_fixture_test(iiSocietyHelper.filesystem.offline iiSocietyHelper_filesystem_tests deferredStorageDoesNotDiscoverTheDefaultDrive)
        add_test(NAME iiSocietyHelper.filesystem_ctest_contract
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_filesystem.py")
        # Includes durable original capture and cancellation/drain on teardown.
        # Keep the individual UI dispatch and selection deadlines unchanged.
        set_tests_properties(iiSocietyHelper.filesystem iiSocietyHelper.filesystem.offline PROPERTIES TIMEOUT 300)
        add_executable(iiSocietyHelper_process_tests tests/process_observation.cpp)
        target_link_libraries(iiSocietyHelper_process_tests PRIVATE iiSocietyHelper::iiSocietyHelper)
        target_compile_definitions(iiSocietyHelper_process_tests PRIVATE
            HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/resources"
            HELPER_OBSERVER_EXECUTABLE="$<TARGET_FILE:ii-society-helper>")
        add_dependencies(iiSocietyHelper_process_tests ii-society-helper)
        iisociety_fixture_test(iiSocietyHelper.process_observation iiSocietyHelper_process_tests)
        set_tests_properties(iiSocietyHelper.process_observation PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
        add_test(NAME iiSocietyHelper.process_observation_ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_process_observation.py")
    endif()
    add_executable(iiSocietyHelper_hello_world tests/hello_world.cpp)
    target_link_libraries(iiSocietyHelper_hello_world PRIVATE iiSocietyHelper::iiSocietyHelper)
    set_target_properties(iiSocietyHelper_hello_world PROPERTIES CXX_EXTENSIONS OFF)
    iisociety_fixture_test(iiSocietyHelper.hello_world iiSocietyHelper_hello_world)
endif()

install(TARGETS iiSocietyHelper EXPORT iiSocietyHelperTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} FRAMEWORK DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES src/Core/iiSocietyHelper.h src/Delivery/DeliveryStore.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES src/Client/resident_client.py DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocietyHelper/python)
install(FILES ${_helper_root_headers} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSocietyHelper)
install(DIRECTORY src/Sync src/Monitoring src/Generation DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSocietyHelper FILES_MATCHING PATTERN "*.h"
    PATTERN "ConfinedFiles.h" EXCLUDE PATTERN "NamespaceJournal.h" EXCLUDE
    PATTERN "WatchPaths.h" EXCLUDE PATTERN "FilePresentation_p.h" EXCLUDE PATTERN "NativeControlPaths_p.h" EXCLUDE)
install(EXPORT iiSocietyHelperTargets NAMESPACE iiSocietyHelper:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSocietyHelper)
include(cmake/Package.cmake)

include(cmake/SyncTests.cmake)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME iiSocietyHelper.module_boundary COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/module_boundary.py")
    # The consolidated suites share one physical fixture volume. Preserve their
    # strict transfer deadlines when the caller requests parallel CTest jobs.
    get_property(_helper_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${_helper_tests} PROPERTIES RESOURCE_LOCK iiSocietyHelperStorage)
endif()
