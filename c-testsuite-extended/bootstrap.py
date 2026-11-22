"""Build mcc with GCC, rebuild twice with mcc, and compare the results."""
import argparse
from hashlib import sha256
import os
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory


ROOT = Path(__file__).resolve().parent.parent


def make(build, compiler, goals, flags=None):
    options = [f"BUILD={build}", f"CC={compiler}"]
    if flags:
        options.append(f"WARN={flags}")
    env = {key: value for key, value in os.environ.items()
           if key not in {"MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES"}}
    subprocess.run(["make", "-s", "-j4", *options, *goals], check=True, cwd=ROOT, env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=["c11", "gnu11"], default="c11")
    args = parser.parse_args()
    toolchain = os.environ.get("MCC_TOOLCHAIN") or "gcc"
    flags = "-std=" + args.mode
    if args.mode == "c11":
        flags += " -pedantic-errors"

    with TemporaryDirectory(prefix="mcc-bootstrap-") as directory:
        stage1, stage2, stage3 = [Path(directory) / f"stage{i}" for i in range(1, 4)]
        print("Stage 1: build with GCC", flush=True)
        make(stage1, toolchain, ["all"])
        print(f"Stage 2: build with stage 1 ({flags})", flush=True)
        make(stage2, stage1 / "mcc", ["all"], flags)
        print(f"Stage 3: build with stage 2 ({flags})", flush=True)
        make(stage3, stage2 / "mcc", ["all"], flags)

        second = (stage2 / "mcc").read_bytes()
        third = (stage3 / "mcc").read_bytes()
        if second != third:
            print(f"Stage 2 SHA-256: {sha256(second).hexdigest()}")
            print(f"Stage 3 SHA-256: {sha256(third).hexdigest()}")
            return 1
        print(f"Stages 2 and 3 match (SHA-256 {sha256(third).hexdigest()}).", flush=True)

        goals = ["check", "check-extended"]
        if (ROOT / "c-testsuite/tests/single-exec").is_dir():
            goals += ["check-strict", "check-suite"]
        else:
            print("Upstream c-testsuite checkout absent. Running local tests.", flush=True)
        make(stage3, stage2 / "mcc", goals, flags)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode) from None
