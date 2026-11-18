from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ('unchangedHostBindingPreservesItsDurableFile', 'inlineCommitRejectsCorruptionAndPreservesThePreviousFile', 'metadataPagesReadACommittedWalSnapshotWhileTheOperationLockIsHeld', 'unchangedPeerCursorsDoNotCreateAnotherDurableRevision', 'failedNamespaceMigrationRollsBackTheWholeNewJournal', 'maintenancePreservesLeasedCapabilityProbeAndRetainedHardLink', 'leaseSentinelNeverBecomesAReplicatedObject', 'presentationsAreRegeneratedWhileUserClassificationStillReplicates', 'maintenancePreservesActiveTransferAndRemovesCompletedOrClosedOrphans', 'providerRestoreKeepsTemporaryLeaseWhileJournalLockIsReleased', 'formerDefaultNamesAcceptRemoteDeletionReplacementAndReplay', 'hostAdoptionArchivesUserDirectoriesAndLeavesFilesEmpty', 'manifestHashAndOrderingAreCheckedWithoutWritingFileBytes', 'sectionInventoryAndLocalBoundary', 'chunksResumeAndBadDataCannotReplaceExistingFiles', 'deletionIsPersistentAndTheSameClockHasOneValue', 'corruptCompleteTransferPreservesDestinationAndCanRetry', 'redirectedFilesAndReplacedContainerFailClosed', 'redirectedJournalCannotWriteLocksOutsideContainer', 'filenameAliasesCannotOverwriteDistinctNames', 'manifestPagesKeepAStableCursorWhileNewChangesWaitForNextCycle')

class ReplicaCTestContract(unittest.TestCase):
    def test_cases_and_skip_remain_without_qttest(self):
        source = (ROOT / "tests/sync/replica.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        self.assertIn("helper_ctest::skip(", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QTest>|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_source_and_installed_targets_are_qttest_free(self):
        for file, target in (("cmake/SyncTests.cmake", "iiSocietyHelper_sync_replica_tests"),
                             ("tests/consumer/CMakeLists.txt", "iiSocietyHelper_installed_sync")):
            cmake = (ROOT / file).read_text()
            link = re.search(r"target_link_libraries\(" + target + r"\s+([^)]*)\)", cmake)
            self.assertIsNotNone(link)
            self.assertNotIn("Qt6::Test", link.group(1))
            self.assertIn("SKIP_RETURN_CODE 77", cmake)
        self.assertIn("Sync.Replica.ctest_contract", (ROOT / "cmake/SyncTests.cmake").read_text())

if __name__ == "__main__":
    unittest.main()
