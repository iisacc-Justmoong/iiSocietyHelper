#include "CTestSupport.h"

#include <sstream>
#include <stdexcept>
#include <string>

int main()
{
    std::ostringstream errors;
    std::ostringstream output;
    auto* previous = std::cerr.rdbuf(errors.rdbuf());
    auto* previousOutput = std::cout.rdbuf(output.rdbuf());
    helper_ctest::CTestRunner empty;
    const int emptyResult = empty.result();
    helper_ctest::CTestRunner failure;
    bool continued = false;
    failure.run("bad-row", [] { CTEST_CHECK(false); });
    failure.run("next-row", [&] { continued = true; });
    const int failureResult = failure.result();
    helper_ctest::CTestRunner skipped;
    skipped.run("unsupported", [] { helper_ctest::skip("fixture"); });
    const int skippedResult = skipped.result();
    helper_ctest::CTestRunner mixed;
    mixed.run("unsupported", [] { helper_ctest::skip("fixture"); });
    mixed.run("supported", [] {});
    const int mixedResult = mixed.result();
    std::cerr.rdbuf(previous);
    std::cout.rdbuf(previousOutput);
    if (emptyResult == 0 || failureResult == 0 || skippedResult != 77 || mixedResult != 0 || !continued ||
        errors.str().find("No test cases") == std::string::npos ||
        errors.str().find("bad-row") == std::string::npos ||
        errors.str().find("ctest_support.cpp:") == std::string::npos)
        return 1;

    bool selected = false, unexpected = false;
    helper_ctest::CTestRunner filtered("selected");
    filtered.run("other", [&] { unexpected = true; });
    filtered.run("selected", [&] { selected = true; });
    if (!selected || unexpected || filtered.result() != 0) return 1;

    int groupRows = 0;
    helper_ctest::CTestRunner group("batch");
    group.run("batch:first", [&] { ++groupRows; });
    group.run("batch:second", [&] { ++groupRows; });
    group.run("batch-other:first", [&] { unexpected = true; });
    if (group.result() != 0 || groupRows != 2 || unexpected) return 1;

    int exactRows = 0;
    helper_ctest::CTestRunner row("batch:second");
    row.run("batch:first", [&] { ++exactRows; });
    row.run("batch:second", [&] { ++exactRows; });
    if (row.result() != 0 || exactRows != 1) return 1;

    helper_ctest::CTestRunner success;
    success.run("checks", [] {
        int evaluations = 0;
        CTEST_CHECK(true);
        CTEST_COMPARE(++evaluations, 1);
        CTEST_COMPARE(evaluations, 1);
        CTEST_THROWS(std::runtime_error, throw std::runtime_error("expected"));
    });
    return success.result();
}
