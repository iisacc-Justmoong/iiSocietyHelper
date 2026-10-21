set(iiSocietyHelper_PACKAGE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/iiSocietyHelper")
if(NOT iiFileProviderNative_VERSION)
    message(FATAL_ERROR "The compiled Native provider version is required for package export")
endif()
set(IISOCIETYHELPER_NATIVE_VERSION "${iiFileProviderNative_VERSION}")
configure_package_config_file(cmake/iiSocietyHelperConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/iiSocietyHelperConfig.cmake" INSTALL_DESTINATION "${iiSocietyHelper_PACKAGE_DIR}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/iiSocietyHelperConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/iiSocietyHelperConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/iiSocietyHelperConfigVersion.cmake" DESTINATION "${iiSocietyHelper_PACKAGE_DIR}")
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocietyHelper/docs FILES_MATCHING PATTERN "*.md")
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocietyHelper)
install(DIRECTORY licenses/ DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocietyHelper/licenses)
