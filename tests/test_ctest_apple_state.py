from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AppleStateCTestContract(unittest.TestCase):
    def test_cases_and_callback_ownership_remain(self):
        source = (ROOT / "tests/client/apple_state.mm").read_text()
        for case in ("adHocProcessesShareTheSelectedContainerStateLocation", "nativeKeychainRoundTrip"):
            self.assertIn(f'runner.run("{case}"', source)
        self.assertIn("callbackOnOwner", source)
        self.assertIn("society_test::root", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QTest>|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_contract_are_qttest_free(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        link = re.search(r"target_link_libraries\(iiSocietyHelper_apple_state_tests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("iiSocietyHelper.apple_state_ctest_contract", cmake)
        self.assertIn("HELPER_TEST_DIRECTORY", cmake[link.end():link.end()+400])


if __name__ == "__main__":
    unittest.main()
