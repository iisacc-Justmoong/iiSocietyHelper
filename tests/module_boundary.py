"""Check the consolidated package boundaries without loading installed SDKs."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
LEGACY = ("iiSocietyGeneration", "iiSocietyMonitor", "iiSocietySync")


class UnifiedPackageTests(unittest.TestCase):
    def test_no_legacy_package_or_target_dependency(self):
        files = [ROOT / "CMakeLists.txt", *sorted((ROOT / "cmake").glob("*.cmake*"))]
        for path in files:
            source = path.read_text()
            for name in LEGACY:
                self.assertNotRegex(source, rf"(?:find_package|find_dependency)\(\s*{name}\b", str(path))
                self.assertNotIn(name + "::", source, str(path))

    def test_public_features_have_one_owner(self):
        for feature, header in [("Sync", "Replica.h"), ("Monitoring", "Monitor.h"), ("Generation", "Remote.h")]:
            path = ROOT / "src/iiSocietyHelper" / feature / header
            self.assertTrue(path.is_file(), str(path))
            self.assertIn("namespace iiSocietyHelper::" + feature, path.read_text())

    def test_monitoring_public_headers_are_native(self):
        directory = ROOT / "src/iiSocietyHelper/Monitoring"
        headers = list(directory.glob("*.h"))
        self.assertTrue(headers)
        for path in headers:
            self.assertNotRegex(path.read_text(), r"#\s*include\s*[<\"](?:Q|Qt|iiSocietyHelper/Export)")

    def test_core_umbrella_exposes_remote_without_host_inference(self):
        source = (ROOT / "src/iiSocietyHelper.h").read_text()
        self.assertIn("iiSocietyHelper/Generation/Remote.h", source)
        self.assertIn("iiSocietyHelper/Sync/Sync.h", source)
        self.assertNotIn("Generation/Host.h", source)


if __name__ == "__main__":
    unittest.main()
