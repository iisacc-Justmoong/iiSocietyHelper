from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "pinnedReadsDoNotContendWithTheMutationLock",
    "richInformationIsBoundedVersionedAndIndependentOfPayloadReads",
    "legacyShareHidesDerivedDocumentsWhileServingOriginalBytes",
    "abandonedDirectWriteUsesTheExistingMaintenanceContract",
    "liveAccessDoesNotRequireTheIndexOrLocalPayload",
    "pagingIsBoundedAndRevisionChecked",
    "rejectsEscapesPrivateStateAndOtherContainers",
    "controllerRequiresPeerScopeAndExactRequestToken",
)


class HostDriveCTestContract(unittest.TestCase):
    def test_cases_and_wait_are_preserved(self):
        source = (ROOT / "tests/sync/host_drive.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        self.assertIn("3000", source)
        self.assertIn("CTEST_WAIT_DEFAULT", source)
        self.assertIn("society_test::root", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QTest>|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_offline_selection(self):
        cmake = (ROOT / "cmake/SyncTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiSocietyHelper_sync_host_drive_tests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("iiSocietyHelper.Sync.HostDrive.Offline", cmake)
        self.assertIn("iiSocietyHelper.Sync.HostDrive.ctest_contract", cmake)


if __name__ == "__main__":
    unittest.main()
