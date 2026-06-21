"""Bootstrap the USD sidecar virtualenv used for .usdc and validation."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import traceback
import venv
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
VENV = TOOLS / ".venv-usd"
REQS = TOOLS / "requirements.txt"


def venv_python() -> Path:
	if sys.platform.startswith("win"):
		return VENV / "Scripts" / "python.exe"
	return VENV / "bin" / "python"


def run(cmd: list[str]) -> None:
	print("+ " + " ".join(cmd), flush=True)
	subprocess.check_call(cmd)


def write_status(path: str | None, payload: dict) -> None:
	if not path:
		return
	target = Path(path)
	target.parent.mkdir(parents=True, exist_ok=True)
	target.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def setup(force: bool = False) -> dict:
	if not REQS.is_file():
		raise RuntimeError(f"requirements file not found: {REQS}")
	if force and VENV.exists():
		shutil.rmtree(VENV)
	py = venv_python()
	if not py.is_file():
		print(f"Creating virtualenv: {VENV}", flush=True)
		venv.EnvBuilder(with_pip=True, clear=False).create(VENV)
	run([str(py), "-m", "pip", "install", "-r", str(REQS)])
	return {
		"ok": True,
		"venv": str(VENV),
		"python": str(py),
		"requirements": str(REQS),
	}


def main() -> int:
	parser = argparse.ArgumentParser()
	parser.add_argument("--force", action="store_true",
		help="delete and rebuild tools/.venv-usd")
	parser.add_argument("--status-json",
		help="write setup status JSON for TouchDesigner polling")
	args = parser.parse_args()

	try:
		status = setup(force=args.force)
		write_status(args.status_json, status)
		print("USD sidecar is ready: " + status["python"], flush=True)
		return 0
	except BaseException as exc:
		status = {
			"ok": False,
			"error": str(exc),
			"traceback": traceback.format_exc(),
		}
		write_status(args.status_json, status)
		print(status["traceback"], file=sys.stderr, flush=True)
		if isinstance(exc, KeyboardInterrupt):
			return 130
		return 1


if __name__ == "__main__":
	raise SystemExit(main())
