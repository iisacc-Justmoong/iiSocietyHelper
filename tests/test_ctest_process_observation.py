from pathlib import Path
import re
import unittest
ROOT=Path(__file__).resolve().parents[1]
class ProcessObservationCTestContract(unittest.TestCase):
    def test_cases_payloads_and_skip_are_preserved(self):
        text=(ROOT/"tests/process_observation.cpp").read_text()
        for case in ("threeProcessesDiscoverCrashAndRestart","suspendedPeerExpiresAndReturns"):
            self.assertIn(f'runner.run("{case}"',text)
        self.assertIn("helper_ctest::skip(",text);self.assertIn("qvariant_cast<DepartureReason>(departed.last()[1])",text)
        self.assertNotRegex(text,r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|<QTest>|<QSignalSpy>|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
    def test_target_does_not_link_qttest(self):
        text=(ROOT/"CMakeLists.txt").read_text();link=re.search(r"target_link_libraries\(iiSocietyHelper_process_tests\s+([^)]*)\)",text)
        self.assertIsNotNone(link);self.assertNotIn("Qt6::Test",link.group(1));self.assertNotIn("process-observation-qt.txt",text)
if __name__=="__main__":unittest.main()
