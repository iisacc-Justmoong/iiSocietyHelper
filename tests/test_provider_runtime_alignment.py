from pathlib import Path
import subprocess
import sys

source, workspace, cmake, build = map(Path, sys.argv[1:])
selected = workspace / "selected.dylib"
candidate = workspace / "candidate.dylib"
selected.write_bytes(b"selected provider")
candidate.write_bytes(b"stale provider")
command = [str(cmake), f"-DPROJECT_SOURCE={source}", f"-DSELECTED={selected}",
           f"-DCANDIDATE={candidate}", "-P", str(source / "tests/provider_runtime_alignment.cmake")]
mismatch = subprocess.run(command, capture_output=True, text=True)
assert mismatch.returncode != 0 and "different" in mismatch.stderr
candidate.write_bytes(selected.read_bytes())
matching = subprocess.run(command, capture_output=True, text=True)
assert matching.returncode == 0, matching.stderr
candidate.unlink()
absent = subprocess.run(command, capture_output=True, text=True)
assert absent.returncode == 0, absent.stderr

# Also exercise the imported-target/configuration lookup used by the package.
provider = workspace / "provider" / "libprovider.dylib"
backend = workspace / "backend" / "libbackend.dylib"
collocated = backend.parent / provider.name
provider.parent.mkdir()
backend.parent.mkdir()
provider.write_bytes(b"selected provider")
backend.write_bytes(b"backend")
collocated.write_bytes(b"stale provider")
configure = [str(cmake), "-S", str(source / "tests/provider_runtime_targets"), "-B", str(build),
             f"-DHELPER_SOURCE={source}", f"-DSELECTED={provider}",
             f"-DBACKEND={backend}", f"-DRUNTIME_NAME={provider.name}"]
mismatch = subprocess.run(configure, capture_output=True, text=True)
assert mismatch.returncode != 0 and "different" in mismatch.stderr
collocated.write_bytes(provider.read_bytes())
matching = subprocess.run(configure, capture_output=True, text=True)
assert matching.returncode == 0, matching.stderr

# Different Qt adapter ABI names cannot safely coexist even when their native
# provider names do not collide.
dependencies = workspace / "otool.txt"
qt_command = [str(cmake), f"-DPROJECT_SOURCE={source}", f"-DDEPENDENCIES={dependencies}",
              "-DEXPECTED_NAME=libiiFileProvider.0.8.dylib", "-P",
              str(source / "tests/provider_qt_alignment.cmake")]
dependencies.write_text("\t@rpath/libiiFileProvider.0.4.dylib (compatibility version 0.4.0)\n")
wrong_abi = subprocess.run(qt_command, capture_output=True, text=True)
assert wrong_abi.returncode != 0 and "incompatible" in wrong_abi.stderr
dependencies.write_text("\t@rpath/libiiFileProvider.0.8.dylib (compatibility version 0.8.0)\n"
                        "\t@rpath/libiiFileProviderNative.0.5.dylib (compatibility version 0.5.0)\n")
right_abi = subprocess.run(qt_command, capture_output=True, text=True)
assert right_abi.returncode == 0, right_abi.stderr
