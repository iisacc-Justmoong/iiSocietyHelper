from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ['valueTypesSurviveJsonQueueEncodingExactly', 'qobjectSnapshotsCaptureNestedStoredPropertiesWithoutPointers', 'accountUsesItsCompleteCanonicalSnapshotAndCanBeReadAgain', 'rejectsUnsupportedPointersCyclesDepthAndSize', 'rejectsInvalidVersionTypeBase64AndTruncatedStreams', 'objectFailuresNeverCreatePartialOutboxMessages', 'getterRestartAndDeletionCannotQueueUnderAnotherSender', 'foreignThreadObjectIsRejected', 'capturesNestedValuesBeforeLaterGettersDestroyThem', 'errorNotificationMayDestroyHelperWithoutAStaleSignal', 'qmlCanQueueTheQObjectDirectly', 'senderAndReceiverProcessesRestoreObjectsAfterSenderExit']

class ObjectsCTestContract(unittest.TestCase):
    def test_all_cases_use_ctest_and_register_metatype(self):
        source = (ROOT / "tests/objects.cpp").read_text()
        for name in CASES:
            self.assertIn(f'runner.run("{name}"', source)
        self.assertIn("tests.initTestCase()", source)
        self.assertIn("helper_ctest::SignalCounter queued", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy|QAbstractItemModelTester)\b|\bQTEST_|<QtTest|<QTest>|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_does_not_link_qttest(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        link = re.search(r"target_link_libraries\(iiSocietyHelper_object_tests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))

if __name__ == "__main__":
    unittest.main()
