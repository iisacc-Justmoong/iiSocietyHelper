from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "requestDeadlineTracksOutstandingWork",
    "incomingSnapshotDeadlineTracksOnlyAuthorizedWork",
    "timestampedSnapshotIsAnalyzedBeforeAnyBytesAndReceiptSurvivesReopen",
    "hostEditAfterAnalysisPreservesBothFiles",
    "snapshotPagesKeepOneIdentityAndOmissionNeverDeletesHostFiles",
    "integerQueriesDeliverSuccessFailureAndCancellationOnTheReceiverLoop",
    "closingTheProducerCancelsQueuedReceiverCalls",
    "invalidCompletionReceiptFailsAndRetryConvergesWithoutDuplicateConflicts",
)
ROWS = ("clock-behind", "clock-ahead")


class SnapshotCTestContract(unittest.TestCase):
    def test_cases_rows_and_callback_failures_remain(self):
        source = (ROOT / "tests/sync/snapshot.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        for row in ROWS:
            self.assertIn(f'runner.run("transferMustMatchTheAnalyzedSnapshotAndAllEntriesMustComplete:{row}"', source)
        self.assertIn("std::rethrow_exception(inspectionFailure)", source)
        self.assertIn("queryStatus", source)
        self.assertIn("helper_ctest::SignalRecorder done", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QTest>|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_offline_selection(self):
        cmake = (ROOT / "cmake/SyncTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiSocietyHelper_sync_snapshot_tests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("iiSocietyHelper.Sync.Snapshot.Offline", cmake)
        self.assertIn("iiSocietyHelper.Sync.Snapshot.ctest_contract", cmake)


if __name__ == "__main__":
    unittest.main()
