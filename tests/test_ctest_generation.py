from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "destructionDoesNotWaitForAnUnresponsiveInference",
    "serializesMultipleClientsWithoutLosingCompletedJobs",
    "remoteCompletesWithoutAnyLocalModelAndRetriesSubmitOnce",
    "rejectsDifferentOwnerChangedReferenceAndIdReuse",
    "cancellationBeforeSubmitAndDuringInference",
    "rejectsCorruptImageWithoutReportingCompletion",
)


class GenerationCTestContract(unittest.TestCase):
    def test_cases_and_signal_arguments_remain(self):
        source = (ROOT / "tests/generation/generation.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        self.assertIn("helper_ctest::SignalRecorder accepted", source)
        self.assertIn("helper_ctest::SignalRecorder finished", source)
        self.assertIn('finished[0][3].toString().contains("checksum")', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QTest>|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_generation_host_has_no_unused_qt_concurrent_dependency(self):
        cmake = (ROOT / "cmake/Generation.cmake").read_text()
        host = re.search(r"target_link_libraries\(iiSocietyHelperGenerationHost\s+([^)]*)\)", cmake)
        self.assertIsNotNone(host)
        self.assertNotIn("Qt6::Concurrent", host.group(1))
        self.assertIn("find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Gui)", cmake)
        config = (ROOT / "cmake/iiSocietyHelperConfig.cmake.in").read_text()
        generation = config.split('if("GenerationHost" IN_LIST _helper_components)', 1)[1]
        self.assertNotIn("COMPONENTS Gui Concurrent", generation)
        self.assertNotIn("QtConcurrent", (ROOT / "src/Generation/Host.cpp").read_text())

    def test_both_targets_do_not_link_qttest(self):
        for filename, target in (("cmake/Generation.cmake", "iiSocietyHelperGenerationTests"),
                                 ("tests/consumer/CMakeLists.txt", "iiSocietyHelper_installed_generation")):
            cmake = (ROOT / filename).read_text()
            link = re.search(rf"target_link_libraries\({target}\s+([^)]*)\)", cmake)
            self.assertIsNotNone(link)
            self.assertNotIn("Qt6::Test", link.group(1))
            self.assertIn(f'target_link_options({target} BEFORE PRIVATE', cmake)
            self.assertIn('"LINKER:-rpath,$<TARGET_FILE_DIR:iiSharedCanvas::Core>"', cmake)
        self.assertIn("Generation.ctest_contract", (ROOT / "cmake/Generation.cmake").read_text())


if __name__ == "__main__":
    unittest.main()
