from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "automaticPairingIdleDoesNotPoll",
    "automaticPairingRetriesAtDeadline",
    "applicationsOnTheRegisteredHostUseItsNativeDriveWithoutASelfPeer",
    "directHostConnectionRequiresASignedInAccount",
    "stateCreatedBeforeSelectionSavesAfterThePackageAppears",
    "stateRemovalReportsLegacyFailure",
    "sessionRecordRevisionTracksReplacementAndRevocation",
    "stateRemovalAcceptsMissingLegacyEntry",
    "stateRemovalReportsTombstoneFailure",
    "savedHostRoutesReconnectWithoutBonjourAndRequireFreshProof",
    "hostConnectionIsPersistedAndBoundToTheAccountDrive",
    "nativeDiscoveryLifecycleWithoutNetwork",
    "localServiceEnforcesDeviceAndCurrentEntitlement",
    "pairingRequestedDuringRestoreIsRetriedAfterRestore",
    "pairingRequestedDuringRestoreIsDiscardedOnLogout",
    "accountProofRejectsTamperingExpiryAndCrossAccount",
    "accountSessionAutomaticallyFetchesItsDriveLocation",
    "downloadsAndUploadsWithoutASocietyApplicationProcess:saved-server",
    "downloadsAndUploadsWithoutASocietyApplicationProcess:automatic-local-network",
    "downloadsAndUploadsWithoutASocietyApplicationProcess:gateway-relay",
    "downloadsAndUploadsWithoutASocietyApplicationProcess:gateway-lan",
)

class SessionCTestContract(unittest.TestCase):
    def test_original_cases_rows_and_signal_observation(self):
        source = (ROOT / "tests/client/client.cpp").read_text()
        self.assertEqual(re.findall(r'runner.run\("([^\"]+)"', source), list(CASES))
        for name in ("otherResponses", "transferResponses", "ready", "localReady", "responses", "retired", "boundChanges", "synced", "done"):
            self.assertIn(f"helper_ctest::SignalRecorder {name}", source)
        self.assertIn("helper_ctest::waitFor", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QTest>|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_offline_selection(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        link = re.search(r"target_link_libraries\(iiSocietyHelper_session_tests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("iiSocietyHelper.session.offline", cmake)
        self.assertIn("iiSocietyHelper.session_ctest_contract", cmake)

if __name__ == "__main__":
    unittest.main()
