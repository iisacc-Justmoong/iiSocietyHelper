"""The legacy package link must not replace data or escape resources."""
from pathlib import Path
import subprocess
import sys
import ii_file_provider as provider

project = Path(__file__).resolve().parents[1]
cmake, owner = sys.argv[1], Path(sys.argv[2]).resolve()
resources, build = owner / "resources", owner / "build"
provider.mkdir(resources, exist_ok=False)
provider.mkdir(build, exist_ok=False)
def run():
    return subprocess.run([cmake, "-DHELPER=" + str(project / "cmake/ResourceLinks.cmake"),
        "-DRESOURCE_ROOT=" + str(resources), "-DBUILD_ROOT=" + str(build),
        "-P", str(project / "tests/resource_links.cmake")], capture_output=True, text=True)
assert run().returncode == 0 and not list(build.iterdir())
target = resources / "dependencies/quit-fix-stage"
provider.mkdir(target, parents=True, exist_ok=False)
assert run().returncode == 0
link = build / "quit-fix-stage"
assert link.is_symlink() and link.resolve() == target.resolve()
assert run().returncode == 0
link.unlink()
provider.write_text(link, "preserve")
assert run().returncode != 0 and link.read_text() == "preserve"
link.unlink()
link.symlink_to(owner / "outside", target_is_directory=True)
assert run().returncode != 0 and link.is_symlink()
link.unlink()
target.rmdir()
target.symlink_to(owner / "outside", target_is_directory=True)
assert run().returncode != 0 and target.is_symlink()
