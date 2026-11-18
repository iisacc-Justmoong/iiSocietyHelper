from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "statelessCanvasExportRetainsDestinationRulesAndDriveState",
    "statelessMediaExportDoesNotOpenOrChangeTheSelectedDrive",
    "nativeCanvasMediaExportUsesFullReadOnlyFrame:legacy19",
    "nativeCanvasMediaExportUsesFullReadOnlyFrame:snapshot",
    "nativeCanvasMediaExportUsesFullReadOnlyFrame:working",
    "nativeCanvasMediaExportUsesFullReadOnlyFrame:readonly-working",
    "nativeCanvasMediaExportUsesFullReadOnlyFrame:live-working",
    "backgroundMirrorCoalescesWithoutBlockingInputAndRejectsOldSelections",
    "richInformationUsesTheSelectedContainerAndPreservesOriginals",
    "nativeMediaExportUsesProviderAndPreservesFailedDestinations",
    "partialMonitorRetainsFallbackForChangesBeyondItsScanLimit",
    "containerDaemonUpdatesTwoGuiConsumersAndSurvivesSelectionChanges",
    "maskStacksSurviveStorageOwnerAndPreview",
    "remoteAliasesExistWithoutLocalPayloadOrPlaceholderDirectories",
    "containerAliasesFollowHelperSynchronizationAndNeverWritePlaceholders",
    "helperOwnsCanvasSavePreviewAndVersionConflict",
    "deferredStorageDoesNotDiscoverTheDefaultDrive",
    "preparedStorageSurvivesItsWorkerAndDoesNotRescanOnAdoption",
    "invalidPreparedStoragePreservesTheSelectedDrive",
    "generatedPngIsPublishedToThePinnedDriveWithoutRunningSociety",
    "defaultObservationBelongsToTheSelectedPackageAndLookupIsReadOnly",
    "modelsTrackOwnerChangesWithoutRestart",
    "nativeIoAcrossIndependentApplications",
    "missingStorageRecoversAndDoesNotInterruptObservation",
    "qmlContextUsesTheSameNativePaths",
    "diagnosticOnlyReadsStorageAndReportsMissingDefaults",
    "selectionIsPinnedUntilExplicitRefresh",
    "errorsRejectInvalidPathsAndReplacedDrives",
    "shutdownPreparationRetiresMonitorAndStopsAdmission",
)

class FilesystemCTestContract(unittest.TestCase):
    def test_original_cases_rows_and_fixtures_remain(self):
        source = (ROOT / "tests/filesystem.cpp").read_text()
        self.assertEqual(re.findall(r'runner.run\("([^\"]+)"', source), list(CASES))
        self.assertEqual(source.count("FileSystemTests tests; FlushDeferredDeletes cleanup; tests.init("), len(CASES))
        self.assertIn("helper_ctest::SignalRecorder", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QTest>|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_offline_selection(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        link = re.search(r"target_link_libraries\(iiSocietyHelper_filesystem_tests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("iiSocietyHelper.filesystem.offline", cmake)
        self.assertIn("iiSocietyHelper.filesystem_ctest_contract", cmake)

if __name__ == "__main__":
    unittest.main()
