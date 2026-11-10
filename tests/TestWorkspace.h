#pragma once

#include <cstdlib>
#include <filesystem>

#ifdef QT_CORE_LIB
#include <QtCore/QString>
#endif

namespace society_test {

// A resource runner provides a private root. A cross-target test can still
// use its explicitly configured device path when no host override is present.
inline std::filesystem::path rootPath(const std::filesystem::path& fallback)
{
#ifdef _WIN32
    if (const auto* value = _wgetenv(L"IISOCIETYHELPER_TEST_DIRECTORY"); value && *value)
        return std::filesystem::path(value);
#else
    if (const auto* value = std::getenv("IISOCIETYHELPER_TEST_DIRECTORY"); value && *value)
        return std::filesystem::path(value);
#endif
    return fallback;
}

#ifdef QT_CORE_LIB
inline QString root(const QString& fallback)
{
    const auto value = qEnvironmentVariable("IISOCIETYHELPER_TEST_DIRECTORY");
    return value.isEmpty() ? fallback : value;
}
#endif
}
