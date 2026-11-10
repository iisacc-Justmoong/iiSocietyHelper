#include "TestWorkspace.h"
#include <QtCore/QString>
#include <filesystem>
#include <iostream>
#include <string_view>

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const bool overridden = std::string_view(argv[1]) == "override";
    const QString fallback = QStringLiteral("configured-cross-target");
    const auto selected = qEnvironmentVariable("IISOCIETYHELPER_TEST_DIRECTORY");
    if (overridden == selected.isEmpty()) return 1;
    const auto text = society_test::root(fallback);
    const auto actual = society_test::rootPath(std::filesystem::path("configured-cross-target"));
#ifdef _WIN32
    const auto expected = std::filesystem::path((overridden ? selected : fallback).toStdWString());
#else
    const auto expected = std::filesystem::path((overridden ? selected : fallback).toUtf8().constData());
#endif
    if (actual != expected || text != (overridden ? selected : fallback)) {
        std::cerr << "Fixture directory selection did not match the expected path\n";
        return 1;
    }
    return 0;
}
