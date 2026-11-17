#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 - <<'PY'
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from urllib.parse import unquote, urlsplit

root = Path.cwd()
indexes = [
    "AGENTS.md", "README.md", "README.zh-CN.md", "docs/README.md",
    "docs/research/README.md", "docs/integrations/README.md",
    "docs/project/README.md", "docs/archive/README.md",
    "tests/README.md", "tests/fixtures/README.md", "local/README.md",
]
for name in indexes:
    path = root / name
    for target in re.findall(r"\[[^\]\n]+\]\(([^)\s]+)\)", path.read_text()):
        link = urlsplit(target)
        if link.scheme or link.netloc or not link.path:
            continue
        destination = path.parent / unquote(link.path)
        assert destination.exists(), f"{name}: broken link {target}"

for path in root.glob("docs/archive/plans/*.tasks.json"):
    metadata = json.loads(path.read_text())
    assert (root / metadata["planPath"]).is_file(), f"{path}: missing planPath"

assert not (root / "CLAUDE.md").exists(), "use a single tool-independent AGENTS.md"

for category in ("abl", "pe", "avb", "recovery"):
    assert (root / "tests/fixtures" / category).is_dir(), category
raw_abls = list(root.glob("tests/fixtures/abl/*-abl.img"))
assert raw_abls, "missing raw ABL fixture pool"
for path in root.glob("tests/fixtures/recovery/*.img"):
    relative = path.relative_to(root).as_posix()
    attribute = subprocess.check_output(["git", "check-attr", "filter", "--", relative], text=True)
    assert attribute.strip().endswith(": lfs"), f"{relative}: missing Git LFS rule"
ignored = subprocess.run(["git", "check-ignore", "--no-index", "--quiet", "local/firmware/private-test.img"])
assert ignored.returncode == 0, "private firmware directory must be ignored"
readme = subprocess.run(["git", "check-ignore", "--no-index", "--quiet", "local/README.md"])
assert readme.returncode == 1, "local README must remain trackable"

with tempfile.TemporaryDirectory(prefix="gbl-local-inputs-") as temporary:
    workspace = Path(temporary)
    (workspace / "scripts").mkdir()
    binaries = workspace / "bin"
    binaries.mkdir()
    for name in ("extract-canoe-fixtures.sh", "extract-pe-from-fv.sh"):
        shutil.copy2(root / "scripts" / name, workspace / "scripts" / name)
    for name, content in {
        "cargo": "#!/bin/sh\nexit 0\n",
        "gbl": '#!/bin/sh\n[ "$1" = unwrap ] || exit 1\ncp "$2" "$3"\n',
    }.items():
        binary = binaries / name
        binary.write_text(content)
        binary.chmod(0o755)
    firmware = workspace / "local/firmware"
    for source in ("canoe-stock-A.07_2024_02_05/abl_a.img", "infiniti-EU-16.0.5.703/abl.bin"):
        path = firmware / source
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"private-firmware-fixture")
    environment = dict(os.environ, PATH=str(binaries) + os.pathsep + os.environ["PATH"])
    environment.pop("MYRON_PE", None)
    for name in ("extract-canoe-fixtures.sh", "extract-pe-from-fv.sh"):
        subprocess.run(["bash", "scripts/" + name], cwd=workspace, env=environment, check=True, capture_output=True)
    for output in ("fixtures/canoe-A.07/abl_a.bin", "pe/infiniti-EU-16.0.5.703.efi"):
        assert (firmware / output).read_bytes() == b"private-firmware-fixture", output
    assert not (workspace / "tests/fixtures").exists(), "private extraction must not overwrite curated fixtures"

retired_paths = ("tests/images/", ".re-notes/", "docs/superpowers/", "docs/notes/", "CLAUDE.md", ".claude/hooks/")
for directory in ("crates", "GblChainloadPkg", "scripts", "tests", "tools"):
    for current, directories, files in os.walk(root / directory):
        directories[:] = [name for name in directories if name not in (".last", "__pycache__")]
        for name in files:
            path = Path(current) / name
            if path.name == "102_repository_layout.sh" or path.suffix not in (".c", ".h", ".rs", ".sh", ".py", ".md"):
                continue
            content = path.read_text()
            for retired in retired_paths:
                assert retired not in content, f"{path.relative_to(root)}: stale path {retired}"
print(f"ok 102_repository_layout — indexes, task metadata, {len(raw_abls)} ABLs, LFS and private inputs")
PY
