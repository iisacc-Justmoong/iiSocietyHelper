cmake_minimum_required(VERSION 3.24)
project(iiSocietyHelper VERSION 0.9.4 LANGUAGES CXX)
# File creation is owned by iiFileProvider.
find_package(iiFileProvider 0.7.1 CONFIG REQUIRED HINTS "$ENV{IIFILEPROVIDER_PREFIX}" "$ENV{HOME}/.local/SDK/iiFileProvider")





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
find_package(Threads REQUIRED)
find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core Sql Network)
find_package(iiFileProvider 0.7 CONFIG REQUIRED HINTS "$ENV{HOME}/.local/SDK/iiFileProvider")
find_package(iiSocietyContainer 0.16.1 CONFIG REQUIRED HINTS "$ENV{HOME}/.local/SDK/iiSocietyContainer")
find_package(iiSharedCanvas 0.31.1 EXACT CONFIG REQUIRED COMPONENTS Core HINTS "$ENV{HOME}/.local/SDK/iiSharedCanvas")
find_package(iiAcountManager 0.2.8 CONFIG REQUIRED HINTS "$ENV{HOME}/.local/SDK/iiAcountManager")
find_package(iiSocietyMonitor 0.1 CONFIG REQUIRED HINTS "$ENV{HOME}/.local/SDK/iiSocietyMonitor")
find_package(iiSocietySync 0.8.3 CONFIG REQUIRED HINTS "$ENV{HOME}/.local/SDK/iiSocietySync")
set(CMAKE_AUTOMOC ON)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(helper_library_type STATIC)
else()
    set(helper_library_type SHARED)
endif()
add_library(iiSocietyHelper ${helper_library_type} src/iiSocietyHelper.cpp src/iiSocietyHelper.h src/Helper.cpp src/FileSystem.cpp src/CanvasStorage.cpp src/GenerationStorage.cpp src/MediaExport.cpp src/DeliveryStore.cpp src/DeliveryStore.h src/ObjectCodec.cpp src/LocalDirectory.cpp src/LocalDirectory.h)
target_sources(iiSocietyHelper PRIVATE
    src/iiSocietyHelper/FileDownload.h src/iiSocietyHelper/FileDownload.cpp
    src/iiSocietyHelper/Client.h src/iiSocietyHelper/Client.cpp src/iiSocietyHelper/AccountSession.h src/iiSocietyHelper/AccountSession.cpp src/iiSocietyHelper/DeviceInfo.h src/iiSocietyHelper/DeviceInfo.cpp
    src/iiSocietyHelper/DiscoveryService.h src/iiSocietyHelper/NearbyDevices.h src/iiSocietyHelper/NearbyDevices.cpp src/iiSocietyHelper/AutomaticPairing.h src/iiSocietyHelper/AutomaticPairing.cpp
    src/iiSocietyHelper/PairingCredentials.h src/iiSocietyHelper/PairingCredentials.cpp src/iiSocietyHelper/HybridDiscoveryService.cpp
    src/iiSocietyHelper/GroupSessionStore.h src/iiSocietyHelper/GroupSessionStore.cpp src/iiSocietyHelper/StateCrypto.h src/iiSocietyHelper/StateCrypto.cpp src/iiSocietyHelper/Export.h)
add_library(iiSocietyHelper::iiSocietyHelper ALIAS iiSocietyHelper)
target_compile_features(iiSocietyHelper PUBLIC cxx_std_23)
target_compile_definitions(iiSocietyHelper PRIVATE IISOCIETYHELPER_BUILDING_LIBRARY SOCIETY_APP_VERSION="${PROJECT_VERSION}")
if(helper_library_type STREQUAL "STATIC")
    target_compile_definitions(iiSocietyHelper PUBLIC IISOCIETYHELPER_STATIC)
endif()
target_link_libraries(iiSocietyHelper PUBLIC Qt6::Core Qt6::Sql
    iiSocietyContainer::iiSocietyContainer iiSharedCanvas::Core iiAcountManager::iiAcountManager iiSocietySync::iiSocietySync Qt6::Network)
target_link_libraries(iiSocietyHelper PRIVATE iiFileProvider::iiFileProvider iiSocietyMonitor::iiSocietyMonitor)
target_link_libraries(iiSocietyHelper PRIVATE Threads::Threads)
target_sources(iiSocietyHelper PRIVATE src/FilePresentation.cpp src/FilePresentation_p.h)
target_link_libraries(iiSocietyHelper PRIVATE iiFileProvider::Native)
target_include_directories(iiSocietyHelper PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
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
    target_sources(iiSocietyHelper PRIVATE src/platform/apple/ObservationDirectory.mm)
    set_source_files_properties(src/platform/apple/ObservationDirectory.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(iiSocietyHelper PRIVATE "-framework Foundation")
    set_target_properties(iiSocietyHelper PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
endif()

if(WIN32)
    target_link_libraries(iiSocietyHelper PRIVATE advapi32)
endif()
if(APPLE)
    find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Concurrent)
    target_link_libraries(iiSocietyHelper PRIVATE Qt6::Concurrent)
    enable_language(OBJCXX)
    target_sources(iiSocietyHelper PRIVATE src/iiSocietyHelper/AppleStateLocation.mm)
    set_source_files_properties(src/iiSocietyHelper/AppleStateLocation.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
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
    target_sources(iiSocietyHelper PRIVATE src/iiSocietyHelper/IosDeviceInfo.mm)
    add_library(iiSocietyHelperCrypto STATIC src/iiSocietyHelper/IosStateCrypto.swift)
    set_target_properties(iiSocietyHelperCrypto PROPERTIES AUTOMOC OFF Swift_LANGUAGE_VERSION 5)
    target_compile_options(iiSocietyHelperCrypto PRIVATE "$<$<COMPILE_LANGUAGE:Swift>:-parse-as-library>")
    target_link_libraries(iiSocietyHelperCrypto PRIVATE "-framework CryptoKit" "-framework Foundation")
    target_link_libraries(iiSocietyHelper PRIVATE iiSocietyHelperCrypto "-framework UIKit")
    install(TARGETS iiSocietyHelperCrypto EXPORT iiSocietyHelperTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
elseif(ANDROID)
    set(SOCIETY_ANDROID_OPENSSL_DIR "" CACHE PATH "OpenSSL runtime directory")
    target_include_directories(iiSocietyHelper PRIVATE "${SOCIETY_ANDROID_OPENSSL_DIR}/../include")
    target_link_libraries(iiSocietyHelper PRIVATE "${SOCIETY_ANDROID_OPENSSL_DIR}/libcrypto_3.so")
else()
    find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
    target_link_libraries(iiSocietyHelper PRIVATE OpenSSL::Crypto)
endif()
if(ANDROID)
    target_sources(iiSocietyHelper PRIVATE src/iiSocietyHelper/AndroidDiscoveryService.cpp)
else()
    find_path(IISOCIETYHELPER_DNSSD_INCLUDE dns_sd.h PATH_SUFFIXES avahi-compat-libdns_sd REQUIRED)
    target_sources(iiSocietyHelper PRIVATE src/iiSocietyHelper/DnsSdDiscoveryService.cpp)
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
    find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Test)
    add_executable(iiSocietyHelper_presence_worker_tests tests/presence_worker.cpp)
    target_compile_features(iiSocietyHelper_presence_worker_tests PRIVATE cxx_std_23)
    target_link_libraries(iiSocietyHelper_presence_worker_tests PRIVATE Qt6::Test Threads::Threads)
    add_test(NAME iiSocietyHelper.presence_worker COMMAND iiSocietyHelper_presence_worker_tests)
    add_executable(iiSocietyHelper_merged_api_tests tests/merged_api.cpp)
    target_link_libraries(iiSocietyHelper_merged_api_tests PRIVATE iiSocietyHelper::iiSocietyHelper)
    target_compile_definitions(iiSocietyHelper_merged_api_tests PRIVATE HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME iiSocietyHelper.merged_api COMMAND iiSocietyHelper_merged_api_tests)
    if(NOT CMAKE_CROSSCOMPILING)
        add_executable(iiSocietyHelper_session_tests tests/client/client.cpp)
        target_link_libraries(iiSocietyHelper_session_tests PRIVATE iiSocietyHelper::iiSocietyHelper Qt6::Test)
        target_compile_definitions(iiSocietyHelper_session_tests PRIVATE
            SOCIETY_ACCOUNT_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/client/account.json"
            CLIENT_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
        add_test(NAME iiSocietyHelper.session COMMAND iiSocietyHelper_session_tests)
        set_tests_properties(iiSocietyHelper.session PROPERTIES TIMEOUT 300 ENVIRONMENT "SOCIETY_DISABLE_SESSION_RESTORE=1")
        if(APPLE AND NOT IOS)
            # Exercise the private native Keychain adapter without exporting it.
            add_executable(iiSocietyHelper_apple_state_tests tests/client/apple_state.mm src/iiSocietyHelper/AppleStateLocation.mm)
            target_link_libraries(iiSocietyHelper_apple_state_tests PRIVATE iiSocietyHelper::iiSocietyHelper Qt6::Test Qt6::Concurrent "-framework Security")
            set_target_properties(iiSocietyHelper_apple_state_tests PROPERTIES OBJCXX_STANDARD 23)
            add_test(NAME iiSocietyHelper.apple_state COMMAND iiSocietyHelper_apple_state_tests)
        endif()
    endif()
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
        target_link_libraries(iiSocietyHelper_filesystem_tests PRIVATE iiSocietyHelper::iiSocietyHelper iiSocietyMonitor::iiSocietyMonitor iiFileProvider::iiFileProvider iiFileProvider::Native Qt6::Test Qt6::Qml)
        target_compile_definitions(iiSocietyHelper_filesystem_tests PRIVATE
            HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}"
            HELPER_FILESYSTEM_PEER="$<TARGET_FILE:iiSocietyHelper_filesystem_peer>"
            HELPER_DIAGNOSTIC_EXECUTABLE="$<TARGET_FILE:ii-society-helper>")
        add_dependencies(iiSocietyHelper_filesystem_tests iiSocietyHelper_filesystem_peer ii-society-helper)
        add_test(NAME iiSocietyHelper.filesystem COMMAND iiSocietyHelper_filesystem_tests)
        set_tests_properties(iiSocietyHelper.filesystem PROPERTIES TIMEOUT 120)
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
install(FILES src/iiSocietyHelper.h src/DeliveryStore.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY src/iiSocietyHelper/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSocietyHelper FILES_MATCHING PATTERN "*.h")
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocietyHelper/docs FILES_MATCHING PATTERN "*.md")
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
