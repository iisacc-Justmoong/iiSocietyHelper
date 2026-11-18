from pathlib import Path
import re
import unittest
ROOT=Path(__file__).resolve().parents[1]
class LocalTransportContract(unittest.TestCase):
    def test_test_sources_are_qttest_free(self):
        for source,case in (("tests/client/resident_client.cpp", "routesOnlyOwnedEventsAndCancels"), ("tests/sync/socket_transfer.cpp", "tlsBinaryMirrorResumeUploadAndRevocation")):
            text=(ROOT/source).read_text()
            self.assertIn(f'runner.run("{case}"', text)
            self.assertNotRegex(text,r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|<QTest>|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
    def test_source_and_installed_targets_do_not_link_qttest(self):
        files={"CMakeLists.txt":["iiSocietyHelper_resident_client_tests"], "cmake/SyncTests.cmake":["iiSocietyHelper_sync_socket_tests"], "tests/consumer/CMakeLists.txt":["iiSocietyHelper_installed_delivery", "iiSocietyHelper_installed_objects", "iiSocietyHelper_installed_account"]}
        for path, targets in files.items():
            cmake=(ROOT/path).read_text()
            for target in targets:
                link=re.search(r"target_link_libraries\("+target+r"\s+([^)]*)\)",cmake)
                self.assertIsNotNone(link)
                self.assertNotIn("Qt6::Test",link.group(1))
if __name__=="__main__":unittest.main()
