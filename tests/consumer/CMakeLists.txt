cmake_minimum_required(VERSION 3.24)
project(iiSocietyHelperConsumer LANGUAGES CXX)
enable_testing()
find_package(iiSocietyHelper 0.7.1 CONFIG REQUIRED)
get_target_property(helper_dependencies iiSocietyHelper::iiSocietyHelper INTERFACE_LINK_LIBRARIES)
if(helper_dependencies MATCHES "iiSocietySync|iiServerHost")
    message(FATAL_ERROR "Local Helper IPC must not depend on device synchronization or transport")
endif()
find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Test Qml)
set(CMAKE_AUTOMOC ON)
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
target_link_libraries(iiSocietyHelper_installed_filesystem PRIVATE iiSocietyHelper::iiSocietyHelper Qt6::Test Qt6::Qml)
target_compile_definitions(iiSocietyHelper_installed_filesystem PRIVATE
    HELPER_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}"
    HELPER_FILESYSTEM_PEER="$<TARGET_FILE:iiSocietyHelper_installed_filesystem_peer>"
    HELPER_DIAGNOSTIC_EXECUTABLE="${helper_installed_diagnostic}")
add_dependencies(iiSocietyHelper_installed_filesystem iiSocietyHelper_installed_filesystem_peer)
add_test(NAME iiSocietyHelper.installed_filesystem COMMAND iiSocietyHelper_installed_filesystem)
set_tests_properties(iiSocietyHelper.installed_filesystem PROPERTIES TIMEOUT 30)
