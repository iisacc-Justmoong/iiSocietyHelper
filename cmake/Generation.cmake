target_sources(iiSocietyHelper PRIVATE src/Generation/Remote.cpp src/Generation/Remote.h)
if(NOT IOS AND NOT ANDROID AND NOT EMSCRIPTEN)
    set(_helper_host_default ON)
else()
    set(_helper_host_default OFF)
endif()
option(IISOCIETYHELPER_BUILD_GENERATION_HOST "Build the desktop native image generation host" ${_helper_host_default})
if(IISOCIETYHELPER_BUILD_GENERATION_HOST)
    if(IOS OR ANDROID OR EMSCRIPTEN)
        message(FATAL_ERROR "GenerationHost requires a supported desktop inference backend")
    endif()
    find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Gui)
    find_package(iiLocalDiffusion CONFIG REQUIRED)
    iisociety_check_provider_runtime_collision(iiLocalDiffusion::iiLocalDiffusion)
    add_library(iiSocietyHelperGenerationHost SHARED src/Generation/Host.cpp src/Generation/Host.h)
    add_library(iiSocietyHelper::GenerationHost ALIAS iiSocietyHelperGenerationHost)
    set_target_properties(iiSocietyHelperGenerationHost PROPERTIES EXPORT_NAME GenerationHost
        POSITION_INDEPENDENT_CODE ON CXX_EXTENSIONS OFF WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_compile_features(iiSocietyHelperGenerationHost PUBLIC cxx_std_23)
    target_compile_definitions(iiSocietyHelperGenerationHost PRIVATE IISOCIETYHELPER_GENERATION_BUILD)
    target_link_libraries(iiSocietyHelperGenerationHost PUBLIC iiSocietyHelper::iiSocietyHelper
        iiLocalDiffusion::iiLocalDiffusion PRIVATE Qt6::Gui)
    install(TARGETS iiSocietyHelperGenerationHost EXPORT iiSocietyHelperGenerationHostTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(EXPORT iiSocietyHelperGenerationHostTargets NAMESPACE iiSocietyHelper:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSocietyHelper)
    if(BUILD_TESTING)
        add_test(NAME iiSocietyHelper.Generation.SharedType COMMAND "${CMAKE_COMMAND}"
            "-DTARGET_TYPE=$<TARGET_PROPERTY:iiSocietyHelperGenerationHost,TYPE>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_target_type.cmake")
        add_executable(iiSocietyHelperGenerationTests tests/generation/generation.cpp)
        target_link_libraries(iiSocietyHelperGenerationTests PRIVATE iiSocietyHelper::GenerationHost Qt6::Gui)
        if(APPLE)
            target_link_options(iiSocietyHelperGenerationTests BEFORE PRIVATE
                "LINKER:-rpath,$<TARGET_FILE_DIR:iiSharedCanvas::Core>")
        endif()
        if(NOT CMAKE_CROSSCOMPILING)
            add_test(NAME iiSocietyHelper.Generation.SharedLink COMMAND "${CMAKE_COMMAND}"
                "-DIMAGE=$<TARGET_FILE:iiSocietyHelperGenerationTests>" "-DEXPECTED=$<TARGET_FILE:iiSocietyHelperGenerationHost>"
                "-DNATIVE_PROVIDER=$<TARGET_FILE:iiFileProvider::Native>"
                "-DQT_PROVIDER=$<TARGET_FILE:iiFileProvider::iiFileProvider>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_link.cmake")
        endif()
        target_compile_definitions(iiSocietyHelperGenerationTests PRIVATE GENERATION_TEST_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/resources")
        iisociety_fixture_test(iiSocietyHelper.Generation.Protocol iiSocietyHelperGenerationTests)
        set_tests_properties(iiSocietyHelper.Generation.Protocol PROPERTIES TIMEOUT 45)
        add_test(NAME iiSocietyHelper.Generation.ctest_contract COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctest_generation.py")
    endif()
endif()
