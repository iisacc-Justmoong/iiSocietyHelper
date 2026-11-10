include("${PROJECT_SOURCE}/cmake/ProviderHeaderAlignment.cmake")
file(READ "${DEPENDENCIES}" dependency_text)
iisociety_check_provider_qt_dependency_text("${EXPECTED_NAME}" "${dependency_text}")
