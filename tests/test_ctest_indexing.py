from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "derivedCompanionsDoNotBecomePublicDirectoryChanges",
    "watchingALargeLibraryLeavesRoomForFilesAndSockets",
    "openingWaitsForABriefConcurrentIndexCommit",
    "smallObjectsAreDurableAndReadableWhileLargeHashIsCancelled",
    "recreationDuringAHashDoesNotBecomeAFalseDeletion",
    "controllerServesCommittedMetadataBeforeLargeHashCompletes",
)


class IndexingCTestContract(unittest.TestCase):
    def test_cases_and_platform_skip_remain(self):
        source = (ROOT / "tests/sync/indexing.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        self.assertIn("helper_ctest::skip(", source)
        self.assertIn("helper_ctest::processFor(1)", source)
        self.assertIn('recreated = tryPut(root.filePath("Files/recreated"), "revived")', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QTest>|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_timeout(self):
        cmake = (ROOT / "cmake/SyncTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiSocietyHelper_sync_indexing_tests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("Sync.Indexing.ctest_contract", cmake)
        self.assertIn("iiSocietyHelper.Sync.Indexing PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77", cmake)


if __name__ == "__main__":
    unittest.main()
