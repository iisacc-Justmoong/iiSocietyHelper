from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "transientOperationLockKeepsTheOriginalRequestPending",
    "hostSelectionRetriesTheLocalOperationLockWithoutBlockingTheCaller",
    "failedStartupCanRetryTheSameContainer",
    "remoteDownloadsOpenWriteAndCommitAsynchronously",
    "inspectionsAreQueuedReadOnlyAndDiscardSupersededContainers",
    "asynchronousShutdownDoesNotWaitForAnActiveScan",
    "processHandoffDrainsAnActiveFileScan",
    "filesProjectionSupportsCreateReadAndPaginationWithoutExposingOtherSections",
    "endpointRequiresTheAuthenticatedPeerAndRejectsTokenMutation",
    "aClientSynchronizesBothDirectionsOverRealLocalTls",
    "anIdleHostArchivesExternalFilesAfterPublishingItsNamespace",
)


class ControllerCTestContract(unittest.TestCase):
    def test_cases_warning_and_callback_observations_remain(self):
        source = (ROOT / "tests/sync/controller.cpp").read_text()
        self.assertEqual(re.findall(r'runner.run\("([^"]+)"', source), list(CASES))
        self.assertIn("helper_ctest::ExpectedWarning", source)
        self.assertIn("expectedWarning.seen()", source)
        for name in ("inspectionOnOwner", "verificationOnOwner", "allTransportsLocal"):
            self.assertIn(name, source)
        self.assertIn("restoreTrace", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QTest>|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_offline_selection(self):
        cmake = (ROOT / "cmake/SyncTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiSocietyHelper_sync_controller_tests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("iiSocietyHelper.Sync.Controller.Offline", cmake)
        self.assertIn("iiSocietyHelper.Sync.Controller.ctest_contract", cmake)
        self.assertIn("iiSocietyHelper.qt_warning_capture", cmake)


if __name__ == "__main__":
    unittest.main()
