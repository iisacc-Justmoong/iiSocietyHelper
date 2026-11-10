from pathlib import Path
import subprocess
import sys

source, workspace, cmake = map(Path, sys.argv[1:])
old = workspace / "native" / "iiFileProviderDevice.h"
current = workspace / "qt" / "iiFileProviderDevice.h"
old.parent.mkdir(parents=True)
current.parent.mkdir(parents=True)
old.write_text("old ABI\n")
current.write_text("new ABI\n")
command = [str(cmake), f"-DPROJECT_SOURCE={source}", f"-DNATIVE_HEADER={old}",
           f"-DQT_HEADER={current}", "-P", str(source / "tests/provider_header_alignment.cmake")]
mismatch = subprocess.run(command, capture_output=True, text=True)
assert mismatch.returncode != 0 and "different" in mismatch.stderr
current.write_text(old.read_text())
matching = subprocess.run(command, capture_output=True, text=True)
assert matching.returncode == 0, matching.stderr
