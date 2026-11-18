from pathlib import Path
import re
import unittest
ROOT=Path(__file__).resolve().parents[1]
CASES=['differentProductsUseTheSameSuiteStorageWithoutAnExplicitContainer', 'modelsAndGeneratedImagesWorkWithoutASocietyProcessAndSurviveAdoption', 'anUnavailableSuiteRootDoesNotCreatePrivateFallbackStorage', 'logicalFilesMigrateToAnAuthenticatedHostAndModelsDownloadOverTls']
class ApplicationStorageCTestContract(unittest.TestCase):
    def test_all_cases_are_preserved(self):
        source=(ROOT/"tests/application_storage.cpp").read_text()
        for case in CASES:self.assertIn(f'runner.run("{case}"',source)
        self.assertIn('"--offline"',source)
        self.assertNotRegex(source,r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|<QTest>|<QSignalSpy>|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
    def test_producer_and_consumer_are_qttest_free(self):
        for path,target in (("CMakeLists.txt","iiSocietyHelper_application_storage_tests"),("tests/consumer/CMakeLists.txt","iiSocietyHelper_installed_application_storage")):
            text=(ROOT/path).read_text();link=re.search(r"target_link_libraries\("+target+r"\s+([^)]*)\)",text)
            self.assertIsNotNone(link);self.assertNotIn("Qt6::Test",link.group(1));self.assertIn("application_storage_offline",text)
if __name__=="__main__":unittest.main()
