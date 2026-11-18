from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "targetedTransferCannotPublishAfterTheMirroredVersionChanges",
    "validatedHostDoesNotWaitForPendingUploads",
    "firstConnectionDoesNotWaitForThePhotoLibrary",
    "metadataIsVisibleWhileAnUnrelatedConflictWaitsForBytes",
    "selectedDownloadFinishesBeforeUnrelatedConflictReconciliation",
    "newUploadIsAcknowledgedBeforeUnrelatedConflictReconciliation",
    "previewBatchesYieldToSelectedDownloads",
    "cancellationDuringTransferLeavesNoPublishedOriginal",
    "previewsAndCancellationDoNotFetchOriginals",
    "metadataThenSelectedModelAndGeneratedUpload",
    "staleRequestAndRemoteDeletion",
)


class OnDemandCTestContract(unittest.TestCase):
    def test_cases_signals_and_cancellation_remain(self):
        source = (ROOT / "tests/sync/on_demand.cpp").read_text()
        self.assertEqual(re.findall(r'runner.run\("([^"]+)"', source), list(CASES))
        for name in ("done", "validated", "finished"):
            self.assertIn(f"helper_ctest::SignalRecorder {name}", source)
        self.assertIn("cancellationAttempted", source)
        self.assertIn("cancellationsSucceeded", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QTest>|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_offline_selection(self):
        cmake = (ROOT / "cmake/SyncTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiSocietyHelper_sync_on_demand_tests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("iiSocietyHelper.Sync.OnDemand.Offline", cmake)
        self.assertIn("iiSocietyHelper.Sync.OnDemand.ctest_contract", cmake)


if __name__ == "__main__":
    unittest.main()
