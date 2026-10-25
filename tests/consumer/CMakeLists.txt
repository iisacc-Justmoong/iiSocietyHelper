cmake_minimum_required(VERSION 3.24)
project(iiSocietyHelperConsumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_testing()
add_executable(iiSocietyHelper_managed_consumer ../managed_storage.cpp)
target_link_libraries(iiSocietyHelper_managed_consumer PRIVATE iiSocietyHelper::iiSocietyHelper iiFileProvider::iiFileProvider)
target_compile_definitions(iiSocietyHelper_managed_consumer PRIVATE HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME iiSocietyHelper.installed_managed_storage COMMAND iiSocietyHelper_managed_consumer)
find_package(iiSocietyHelper 0.11.0 CONFIG REQUIRED)
if(NOT iiSocietyHelper_HAS_MANAGED_FILES)
    message(FATAL_ERROR "The installed Helper must provide managed file storage")
endif()
if(NOT TARGET iiSharedCanvas::Core OR NOT iiSharedCanvas_VERSION VERSION_EQUAL iiSocietyHelper_CANVAS_VERSION)
    message(FATAL_ERROR "Helper must resolve its exact compiled Canvas contract transitively")
endif()
get_target_property(helper_native_binary iiFileProvider::Native IMPORTED_LOCATION_RELEASE)
file(REAL_PATH "${iiFileProviderNative_DIR}/../../.." helper_native_prefix)
file(REAL_PATH "${helper_native_binary}" helper_native_binary)
cmake_path(IS_PREFIX helper_native_prefix "${helper_native_binary}" NORMALIZE helper_native_matches)
if(NOT helper_native_matches)
    message(FATAL_ERROR "The imported Native binary must match the selected Native package; an earlier dependency must not retain an older target")
endif()
find_package(iiSharedCanvas ${iiSocietyHelper_CANVAS_VERSION} EXACT CONFIG REQUIRED COMPONENTS Core)
get_target_property(helper_dependencies iiSocietyHelper::iiSocietyHelper INTERFACE_LINK_LIBRARIES)
if(helper_dependencies MATCHES "iiSociety(Client|Sync|Monitor|Generation)::")
    message(FATAL_ERROR "The consolidated Helper must not import a retired SDK target")
endif()

find_package(iiSocietyHelper 0.11.0 CONFIG REQUIRED COMPONENTS Monitoring OPTIONAL_COMPONENTS GenerationHost)
find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Test Qml)
set(CMAKE_AUTOMOC ON)
add_executable(iiSocietyHelper_installed_snapshot ../sync/snapshot.cpp)
target_link_libraries(iiSocietyHelper_installed_snapshot PRIVATE iiSocietyHelper::iiSocietyHelper Qt6::Test)
target_compile_definitions(iiSocietyHelper_installed_snapshot PRIVATE SYNC_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME iiSocietyHelper.installed_snapshot COMMAND iiSocietyHelper_installed_snapshot)
set_tests_properties(iiSocietyHelper.installed_snapshot PROPERTIES TIMEOUT 120)
add_executable(iiSocietyHelper_installed_status_example ../../example/sync_status.cpp)
target_link_libraries(iiSocietyHelper_installed_status_example PRIVATE iiSocietyHelper::iiSocietyHelper)
add_test(NAME iiSocietyHelper.installed_status_example COMMAND iiSocietyHelper_installed_status_example)
add_executable(iiSocietyHelper_consumer ../hello_world.cpp)
target_link_libraries(iiSocietyHelper_consumer PRIVATE iiSocietyHelper::iiSocietyHelper)
set_target_properties(iiSocietyHelper_consumer PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME iiSocietyHelper.installed_consumer COMMAND iiSocietyHelper_consumer)
add_executable(iiSocietyHelper_installed_observation ../observation.cpp)
target_link_libraries(iiSocietyHelper_installed_observation PRIVATE iiSocietyHelper::iiSocietyHelper Qt6::Test)
target_compile_definitions(iiSocietyHelper_installed_observation PRIVATE HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME iiSocietyHelper.installed_observation COMMAND iiSocietyHelper_installed_observation)
add_executable(iiSocietyHelper_installed_delivery ../delivery.cpp)
target_link_libraries(iiSocietyHelper_installed_delivery PRIVATE iiSocietyHelper::iiSocietyHelper Qt6::Test)
target_compile_definitions(iiSocietyHelper_installed_delivery PRIVATE HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME iiSocietyHelper.installed_delivery COMMAND iiSocietyHelper_installed_delivery)

# The application links only Helper; its exported package resolves both SDKs.
add_executable(iiSocietyHelper_installed_object_peer ../object_peer.cpp ../ObjectTestTypes.h)
target_link_libraries(iiSocietyHelper_installed_object_peer PRIVATE iiSocietyHelper::iiSocietyHelper)
add_executable(iiSocietyHelper_installed_objects ../objects.cpp ../ObjectTestTypes.h)
target_link_libraries(iiSocietyHelper_installed_objects PRIVATE iiSocietyHelper::iiSocietyHelper Qt6::Test Qt6::Qml)
target_compile_definitions(iiSocietyHelper_installed_objects PRIVATE
    HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}"
    HELPER_OBJECT_PEER="$<TARGET_FILE:iiSocietyHelper_installed_object_peer>")
add_dependencies(iiSocietyHelper_installed_objects iiSocietyHelper_installed_object_peer)
add_test(NAME iiSocietyHelper.installed_objects COMMAND iiSocietyHelper_installed_objects)
set_tests_properties(iiSocietyHelper.installed_objects PROPERTIES TIMEOUT 30)

add_executable(iiSocietyHelper_installed_account ../account.cpp)
target_link_libraries(iiSocietyHelper_installed_account PRIVATE iiSocietyHelper::iiSocietyHelper Qt6::Test Qt6::Qml)
target_compile_definitions(iiSocietyHelper_installed_account PRIVATE HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME iiSocietyHelper.installed_account COMMAND iiSocietyHelper_installed_account)
set_tests_properties(iiSocietyHelper.installed_account PROPERTIES TIMEOUT 30)

add_executable(iiSocietyHelper_installed_filesystem_peer ../filesystem_peer.cpp)
target_link_libraries(iiSocietyHelper_installed_filesystem_peer PRIVATE iiSocietyHelper::iiSocietyHelper)
add_executable(iiSocietyHelper_installed_filesystem ../filesystem.cpp)
find_program(helper_installed_diagnostic ii-society-helper
    PATHS "${iiSocietyHelper_DIR}/../../../bin" NO_DEFAULT_PATH REQUIRED)
target_link_libraries(iiSocietyHelper_installed_filesystem PRIVATE iiSocietyHelper::iiSocietyHelper iiSocietyHelper::Monitoring iiFileProvider::iiFileProvider Qt6::Test Qt6::Qml)
target_compile_definitions(iiSocietyHelper_installed_filesystem PRIVATE
    HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}"
    HELPER_FILESYSTEM_PEER="$<TARGET_FILE:iiSocietyHelper_installed_filesystem_peer>"
    HELPER_DIAGNOSTIC_EXECUTABLE="${helper_installed_diagnostic}")
add_dependencies(iiSocietyHelper_installed_filesystem iiSocietyHelper_installed_filesystem_peer)
add_test(NAME iiSocietyHelper.installed_filesystem COMMAND iiSocietyHelper_installed_filesystem)
set_tests_properties(iiSocietyHelper.installed_filesystem PROPERTIES TIMEOUT 300)

# Both public surfaces compile and run with the one installed SDK target.
add_executable(iiSocietyHelper_installed_merged_api ../merged_api.cpp)
target_link_libraries(iiSocietyHelper_installed_merged_api PRIVATE iiSocietyHelper::iiSocietyHelper)
target_compile_definitions(iiSocietyHelper_installed_merged_api PRIVATE HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME iiSocietyHelper.installed_merged_api COMMAND iiSocietyHelper_installed_merged_api)

add_executable(iiSocietyHelper_installed_client ../client/client.cpp)
target_link_libraries(iiSocietyHelper_installed_client PRIVATE iiSocietyHelper::iiSocietyHelper Qt6::Test)
target_compile_definitions(iiSocietyHelper_installed_client PRIVATE
    SOCIETY_ACCOUNT_FIXTURE="${CMAKE_CURRENT_LIST_DIR}/../client/account.json"
    CLIENT_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME iiSocietyHelper.installed_host_drive COMMAND iiSocietyHelper_installed_client
    downloadsAndUploadsWithoutASocietyApplicationProcess)
set_tests_properties(iiSocietyHelper.installed_host_drive PROPERTIES TIMEOUT 300
    ENVIRONMENT "SOCIETY_DISABLE_SESSION_RESTORE=1")
add_test(NAME iiSocietyHelper.installed_local_drive COMMAND iiSocietyHelper_installed_client
    applicationsOnTheRegisteredHostUseItsNativeDriveWithoutASelfPeer)
set_tests_properties(iiSocietyHelper.installed_local_drive PROPERTIES TIMEOUT 45
    ENVIRONMENT "SOCIETY_DISABLE_SESSION_RESTORE=1")

find_package(iiSocietyContainer 0.16.1 CONFIG REQUIRED COMPONENTS Maintenance)
add_executable(iiSocietyHelper_installed_sync ../sync/replica.cpp)
target_link_libraries(iiSocietyHelper_installed_sync PRIVATE iiSocietyHelper::iiSocietyHelper
    iiSocietyContainer::Maintenance iiFileProvider::Temporary Qt6::Test)
target_compile_definitions(iiSocietyHelper_installed_sync PRIVATE SYNC_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME iiSocietyHelper.installed_sync COMMAND iiSocietyHelper_installed_sync)
set_tests_properties(iiSocietyHelper.installed_sync PROPERTIES TIMEOUT 90)
if(TARGET iiSocietyHelper::GenerationHost)
    add_executable(iiSocietyHelper_installed_generation ../generation/generation.cpp)
    target_link_libraries(iiSocietyHelper_installed_generation PRIVATE iiSocietyHelper::GenerationHost Qt6::Test Qt6::Gui)
    target_compile_definitions(iiSocietyHelper_installed_generation PRIVATE GENERATION_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME iiSocietyHelper.installed_generation COMMAND iiSocietyHelper_installed_generation)
    set_tests_properties(iiSocietyHelper.installed_generation PROPERTIES TIMEOUT 45)
endif()

# Keep storage-heavy integration cases from competing for fixture-volume I/O.
get_property(_helper_consumer_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${_helper_consumer_tests} PROPERTIES RESOURCE_LOCK iiSocietyHelperStorage)
