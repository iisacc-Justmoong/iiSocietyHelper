from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "asynchronousLifecycleKeepsOwnerResponsive",
    "asynchronousStartCanBeStoppedBeforeIoCompletes",
    "asynchronousStopPreservesSynchronousDeliveryAndAllowsRestart",
    "helperCanDeferStorageDiscovery",
    "transientUnreadableRecordDoesNotWithdrawAnObservedPeer",
    "presenceSurvivesAnOpenProviderReadSnapshot",
    "presenceResponsiveness",
    "stoppedWorkCannotRepublishIntoTheNextSession",
    "mutualDiscoveryAndDistinctInstances",
    "staleAndMalformedRecordsNeverBecomePeers",
    "invalidConfigurationIsReported",
    "redirectedDirectoryStopsObservation",
    "independentDirectoriesDoNotMix",
    "resumedObserverRequiresFreshEvidence",
)

class ObservationCTestContract(unittest.TestCase):
    def test_original_cases_and_signals_remain(self):
        source = (ROOT / "tests/observation.cpp").read_text()
        self.assertEqual(re.findall(r'runner.run\("([^\"]+)"', source), list(CASES))
        for name in ("started", "stopped", "appeared", "updated", "departed", "errors"):
            self.assertIn(f"helper_ctest::SignalRecorder {name}", source)
        self.assertIn("helper_ctest::processFor(3000)", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QTest>|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_offline_selection(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        link = re.search(r"target_link_libraries\(iiSocietyHelper_observation_tests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("iiSocietyHelper.observation.offline", cmake)
        self.assertIn("iiSocietyHelper.observation_ctest_contract", cmake)

if __name__ == "__main__":
    unittest.main()
