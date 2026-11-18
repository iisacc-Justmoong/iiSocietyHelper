from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]

class BootstrapCTestContract(unittest.TestCase):
    def test_data_rows_and_offline_selection_are_preserved(self):
        source = (ROOT / "tests/sync/bootstrap.cpp").read_text()
        for name in ("pagesRoundTripAndDetectMixedRefresh", "rejectsCredentialsFilesAndPublicEndpoints", "digestDoesNotAuthorizeUntrustedMetadata", "stop.invalid-endpoint", "stop.disabled"):
            self.assertIn(f'runner.run("{name}"', source)
        self.assertIn("--offline", source)
        self.assertIn("stopIsIdempotentWithoutStartingRadio(false)", source)
        self.assertIn("stopIsIdempotentWithoutStartingRadio(true)", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy|QAbstractItemModelTester)\b|\bQTEST_|<QtTest|<QTest>|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_does_not_link_qttest(self):
        cmake = (ROOT / "cmake/SyncTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiSocietyHelper_sync_bootstrap_tests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("iiSocietyHelper.Sync.Bootstrap.Offline", cmake)

if __name__ == "__main__":
    unittest.main()
