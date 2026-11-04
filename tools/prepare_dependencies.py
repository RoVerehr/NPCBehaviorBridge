"""Fetch or verify the pinned public dependency sources, including nested modules."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def git(*args):
    return subprocess.check_output(["git", *map(str, args)], text=True).strip()


def main(check_only=False):
    lock = json.loads((ROOT / "dependencies.lock.json").read_text(encoding="utf-8"))
    for dependency in lock["dependencies"]:
        path = ROOT / "vendor" / dependency["directory"]
        if not path.exists() or not any(path.iterdir()):
            if check_only:
                raise RuntimeError(f"Missing dependency: {path}")
            path.parent.mkdir(parents=True, exist_ok=True)
            git("clone", "--filter=blob:none", "--no-checkout", dependency["repository"], path)
            git("-C", path, "fetch", "--depth", "1", "origin", dependency["commit"])
            git("-C", path, "checkout", "--detach", dependency["commit"])
        # Do not mistake the parent project's repository for a dependency checkout.
        if Path(git("-C", path, "rev-parse", "--show-toplevel")).resolve() != path.resolve():
            raise RuntimeError(f"Not a dependency checkout: {path}")
        actual = git("-C", path, "rev-parse", "HEAD")
        if actual != dependency["commit"]:
            raise RuntimeError(f"Dependency differs from lock; left unchanged: {path}")
        if not check_only:
            git("-C", path, "submodule", "update", "--init", "--recursive", "--depth", "1")
        states = git("-C", path, "submodule", "status", "--recursive")
        if any(line.startswith(("-", "+", "U")) for line in states.splitlines()):
            raise RuntimeError(f"Uninitialized or mismatched nested dependency: {path}")
        print(dependency["directory"], actual)
    for nested in lock.get("nested_dependencies", []):
        path = ROOT / "vendor" / nested["parent"] / nested["path"]
        if git("-C", path, "rev-parse", "HEAD") != nested["commit"]:
            raise RuntimeError(f"Nested dependency differs from lock: {path}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="Verify only; do not fetch or change checkouts")
    main(parser.parse_args().check)
