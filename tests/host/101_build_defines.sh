#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
for tool in cc python3; do
  command -v "$tool" >/dev/null || { echo "SKIP: 101 — missing $tool"; exit 0; }
done
python3 - <<'PY'
from pathlib import Path
import subprocess
import tempfile

names = {
    "BOARD_BOOTLOADER_PRODUCT_NAME": "canoe",
    "GBL_CHAINLOAD_VERSION": "2.3.4",
    "GBL_BUILD_NAME": "gbl-chainload-auto-debug-verbose",
}
flags = []
for line in Path("GblChainloadPkg/GblChainloadPkg.dsc").read_text().splitlines():
    if line.strip().startswith("GCC:") and any(
        f"-D{name}=" in line for name in ("PRODUCT_NAME", "GBL_CHAINLOAD_VERSION", "GBL_BUILD_NAME")
    ):
        flag = line.split("=", 1)[1].strip()
        for name, value in names.items():
            flag = flag.replace(f"$({name})", value)
        flags.append(flag)
assert len(flags) == 3
source = "PRODUCT_NAME\nGBL_CHAINLOAD_VERSION\nGBL_BUILD_NAME\n"
expected = "".join(f'"{value}"\n' for value in names.values())
command = "cc " + " ".join(flags) + " -E -P -x c -"
direct = subprocess.run(command, shell=True, input=source, capture_output=True, text=True, check=True)
assert direct.stdout == expected, direct.stdout
with tempfile.TemporaryDirectory(prefix="gbl-defines-") as temporary:
    response = Path(temporary) / "cc_resp.txt"
    response.write_text(" ".join(flags).replace(chr(92), "/"))
    result = subprocess.run(
        ["cc", f"@{response}", "-E", "-P", "-x", "c", "-"],
        input=source, capture_output=True, text=True, check=True,
    )
    assert result.stdout == expected, result.stdout
print("ok 101_build_defines — shell and EDK2-normalized GCC response-file quoting")
PY
