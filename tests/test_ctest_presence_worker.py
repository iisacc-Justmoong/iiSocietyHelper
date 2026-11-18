from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class PresenceWorkerCTestContract(unittest.TestCase):
    def test_all_worker_cases_are_registered_without_qttest(self):
        source = (ROOT / "tests/presence_worker.cpp").read_text()
        for case in (
            "slowIoDoesNotBlockTheApplicationLoop",
            "pendingUpdatesAreBoundedAndKeepTheLatest",
            "drainPreservesAQueuedCommit",
            "closingDuringBlockedIoDoesNotWaitOrCallDestroyedOwner",
            "queuedCallbackIsDiscardedWhenGateCloses",
            "detachedWorkQuiescesBeforeStaticDestruction",
        ):
            self.assertIn(f'runner.run("{case}"', source)
        self.assertNotRegex(source, r"\bQTest\b|\bQTEST_|<QTest>")

    def test_worker_target_does_not_link_qttest(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        link = re.search(r"target_link_libraries\(iiSocietyHelper_presence_worker_tests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))


if __name__ == "__main__":
    unittest.main()
