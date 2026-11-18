from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]

class CTestBoundary(unittest.TestCase):
    def test_cpp_suites_and_cmake_do_not_require_qttest(self):
        pattern = re.compile(r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QTest>|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
        sources = [p for p in (ROOT / "tests").rglob("*") if p.is_file() and p.suffix in (".cpp", ".h", ".mm")]
        for path in sources:
            with self.subTest(path=path.relative_to(ROOT)):
                self.assertFalse(pattern.search(path.read_text()), f"QtTest source remains: {path}")
        for path in (ROOT / "CMakeLists.txt", ROOT / "cmake/SyncTests.cmake", ROOT / "tests/consumer/CMakeLists.txt"):
            with self.subTest(path=path.relative_to(ROOT)):
                text = path.read_text()
                self.assertFalse("Qt6::Test" in text, f"QtTest link remains: {path}")
                self.assertFalse(re.search(r"find_package\(Qt6[^\n]*COMPONENTS[^\n]*\bTest\b", text), f"QtTest package search remains: {path}")

if __name__ == "__main__":
    unittest.main()
