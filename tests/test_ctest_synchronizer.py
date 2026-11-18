from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "accountHostReplacementPreservesOldEditsAndRejectsWrongContainer",
    "boundedWindowHandlesReorderedReplies:serial-download",
    "boundedWindowHandlesReorderedReplies:window-download",
    "boundedWindowHandlesReorderedReplies:window-upload",
    "boundedWindowHandlesReorderedReplies:legacy-download",
    "smallChangesAndDeletionDoNotWaitForBulkTransfer:download",
    "smallChangesAndDeletionDoNotWaitForBulkTransfer:upload",
    "exchangesEveryManifestPageBeforeAnyFileBytes",
    "rejectedManifestAcknowledgementPreventsByteTransfer",
    "firstJoinAdoptsOneLogicalDriveAndPreservesFormerContentsPrivately",
    "interruptedInitialDownloadResumesWithoutUploadingLegacyFiles",
    "recoveryResumesAfterIdentityCommitAndOnlySomeEntriesMoved",
    "bootstrapCatchesHostChangesBeforePublishingTheMirror",
    "aPrimaryClaimIsPersistentAndCannotPromoteAMirror",
    "emptyHostAndHostDriveReplacementNeverMergeIndependentContents",
    "smallUploadsNegotiateInlineCommitAndRetainTheLegacyFallback",
    "anIdleProviderLeaseSentinelDoesNotPreventDirectoryDeletion",
    "bothDirectionsDirectoriesDeletionAndIncrementalRequests",
    "concurrentEditsAndDeleteAgainstEditKeepBothResults",
    "lostAcknowledgementAndRestartDoNotResendCompletedBytes",
    "directoryAndLongFilenameConflictsPreserveData",
)

class SynchronizerCTestContract(unittest.TestCase):
    def test_cases_and_callback_checks_remain(self):
        source = (ROOT / "tests/sync/synchronizer.cpp").read_text()
        self.assertEqual(re.findall(r'runner.run\("([^\"]+)"', source), list(CASES))
        self.assertIn("helper_ctest::SignalRecorder", source)
        self.assertIn("callbackFailure", source)
        self.assertIn("scopeMatches", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QTest>|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_offline_selection(self):
        cmake = (ROOT / "cmake/SyncTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiSocietyHelper_sync_sync_tests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("iiSocietyHelper.Sync.Synchronizer.Offline", cmake)
        self.assertIn("iiSocietyHelper.Sync.Synchronizer.ctest_contract", cmake)

if __name__ == "__main__":
    unittest.main()
