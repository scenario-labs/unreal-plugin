#!/usr/bin/env python3
# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
"""Repository checks that need no engine (stdlib only).

- Every .h, .cpp and .cs file under Plugins/ and Source/ starts with the MIT
  copyright header.
- No em dash (U+2014) in any text file.
- Plugins/Scenario/Scenario.uplugin and Resources/Lanes.json are valid JSON
  with the keys the plugin reads.
- Resources/AgentSkill/SKILL.md has `name` and `description` frontmatter.
- No secret-looking strings (API keys, Basic or Bearer credentials, private
  keys, GitHub tokens, signed URL signatures), no personal home paths, and no
  tracked .env files.

Files: tracked plus untracked files that git does not ignore (or every file
when git is unavailable). Prints `path:line: message` per problem and exits 1
when there is any.

Usage: python3 tools/lint.py [--root DIR]
"""
import argparse
import json
import pathlib
import re
import subprocess
import sys

HEADER = "// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License."
HEADER_SUFFIXES = {".h", ".cpp", ".cs"}
HEADER_ROOTS = ("Plugins/", "Source/")
BINARY_SUFFIXES = {
    ".png", ".jpg", ".jpeg", ".gif", ".webp", ".ico", ".icns", ".exr", ".hdr",
    ".uasset", ".umap", ".zip", ".gz", ".glb", ".fbx", ".wav", ".mp3", ".ogg",
    ".mp4", ".mov", ".pdf", ".ttf", ".otf", ".woff", ".woff2", ".dylib", ".dll", ".so",
}
EM_DASH = chr(0x2014)
UPLUGIN = "Plugins/Scenario/Scenario.uplugin"
LANES = "Plugins/Scenario/Resources/Lanes.json"
SKILL = "Plugins/Scenario/Resources/AgentSkill/SKILL.md"
UPLUGIN_KEYS = ("FileVersion", "Version", "VersionName", "FriendlyName", "Description", "Category", "CreatedBy", "Modules")
LANE_KEYS = ("id", "title", "capability", "importer", "folder", "hint", "starter_prompts", "models")
MODEL_KEYS = ("id", "name")

SECRET_PATTERNS = [
    (re.compile(r"\bapi_[A-Za-z0-9]{20,}\b"), "looks like a Scenario API key"),
    (re.compile(r"\bBasic\s+[A-Za-z0-9+/]{24,}={0,2}"), "looks like a Basic credential"),
    (re.compile(r"\bBearer\s+[A-Za-z0-9\-._~+/]{24,}=*"), "looks like a Bearer token"),
    (re.compile(r"-----BEGIN (?:RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----"), "private key"),
    (re.compile(r"\bgh[pousr]_[A-Za-z0-9]{36,}\b"), "looks like a GitHub token"),
    (re.compile(r"X-Amz-Signature=[0-9a-fA-F]{16,}|[?&]Signature=[A-Za-z0-9%~_\-]{16,}"), "signed URL signature"),
]
# Synthetic values in tests and docs: a match containing one of these words is not a secret.
PLACEHOLDER_WORDS = ("payload", "example", "fake", "dummy", "test", "xxxx", "placeholder", "redacted", "your")
# Put this on a line to accept a deliberate match the rules above cannot tell apart.
ALLOW_MARKER = "lint: allow-secret"
# A home folder with a real user name. Shared folders and obvious placeholders are fine.
HOME_PATH = re.compile(r"(?:/Users/|/home/|[A-Za-z]:\\Users\\)(?!Shared\b|you\b|me\b|user\b|username\b|name\b|runner\b|<)([A-Za-z0-9._-]+)")


def repo_files(root: pathlib.Path) -> list[str]:
    try:
        out = subprocess.run(
            ["git", "-C", str(root), "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
            capture_output=True, check=True,
        ).stdout.decode()
        names = [n for n in out.split("\0") if n]
        return sorted({n for n in names if (root / n).is_file()})
    except (OSError, subprocess.CalledProcessError):
        skip = {".git", "Binaries", "Intermediate", "Saved", "DerivedDataCache", "Build", "dist"}
        return sorted(
            p.relative_to(root).as_posix() for p in root.rglob("*")
            if p.is_file() and not any(part in skip for part in p.relative_to(root).parts)
        )


def read_text(path: pathlib.Path) -> str | None:
    if path.suffix.lower() in BINARY_SUFFIXES:
        return None
    data = path.read_bytes()
    if b"\0" in data[:4096]:
        return None
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def check_header(name: str, text: str) -> list[tuple[int, str]]:
    if not name.startswith(HEADER_ROOTS) or pathlib.PurePosixPath(name).suffix not in HEADER_SUFFIXES:
        return []
    first = text.lstrip("﻿").split("\n", 1)[0].rstrip("\r")
    return [] if first == HEADER else [(1, f"first line must be the copyright header: {HEADER}")]


def check_text(name: str, text: str) -> list[tuple[int, str]]:
    problems = []
    for number, line in enumerate(text.splitlines(), 1):
        if EM_DASH in line:
            problems.append((number, "em dash: use a comma, colon or period"))
        if ALLOW_MARKER not in line:
            for pattern, message in SECRET_PATTERNS:
                for match in pattern.finditer(line):
                    if not any(word in match.group(0).lower() for word in PLACEHOLDER_WORDS):
                        problems.append((number, message))
                        break
        home = HOME_PATH.search(line)
        if home:
            problems.append((number, f"personal home path ({home.group(0)})"))
    return problems


def check_uplugin(text: str) -> list[tuple[int, str]]:
    try:
        data = json.loads(text)
    except ValueError as error:
        return [(getattr(error, "lineno", 1), f"invalid JSON: {error}")]
    problems = [(1, f"missing key {key}") for key in UPLUGIN_KEYS if key not in data]
    for module in data.get("Modules", []):
        if module.get("Type") != "Editor":
            problems.append((1, f"module {module.get('Name')} must be Type Editor (nothing ships in a game)"))
    if not re.fullmatch(r"\d+\.\d+\.\d+", str(data.get("VersionName", ""))):
        problems.append((1, "VersionName must be X.Y.Z"))
    return problems


def check_lanes(text: str) -> list[tuple[int, str]]:
    try:
        data = json.loads(text)
    except ValueError as error:
        return [(getattr(error, "lineno", 1), f"invalid JSON: {error}")]
    problems = [(1, f"missing key {key}") for key in ("version", "lanes", "presets") if key not in data]
    lanes = data.get("lanes", [])
    if not isinstance(lanes, list) or not lanes:
        return problems + [(1, "lanes must be a non-empty list")]
    seen_lanes, model_ids = set(), set()
    for lane in lanes:
        lane_id = lane.get("id", "?")
        for key in LANE_KEYS:
            if key not in lane:
                problems.append((1, f"lane {lane_id}: missing key {key}"))
        if lane_id in seen_lanes:
            problems.append((1, f"lane {lane_id}: duplicate id"))
        seen_lanes.add(lane_id)
        if not lane.get("starter_prompts"):
            problems.append((1, f"lane {lane_id}: starter_prompts is empty"))
        if not lane.get("models"):
            problems.append((1, f"lane {lane_id}: models is empty"))
        for model in lane.get("models", []):
            for key in MODEL_KEYS:
                if not model.get(key):
                    problems.append((1, f"lane {lane_id}: a model has no {key}"))
            if model.get("id") and not str(model["id"]).startswith("model_"):
                problems.append((1, f"lane {lane_id}: model id {model['id']} does not start with model_"))
            model_ids.add(model.get("id"))
    for model_id in data.get("presets", {}):
        if model_id not in model_ids:
            problems.append((1, f"presets: {model_id} is not a lane model"))
    return problems


def check_skill(text: str) -> list[tuple[int, str]]:
    match = re.match(r"---\n(.*?)\n---\n", text, re.S)
    if not match:
        return [(1, "SKILL.md must start with --- frontmatter ---")]
    fields = dict(line.split(":", 1) for line in match.group(1).splitlines() if ":" in line)
    return [(1, f"frontmatter has no {key}") for key in ("name", "description") if not fields.get(key, "").strip()]


def lint(root: pathlib.Path) -> list[str]:
    problems = []
    names = repo_files(root)
    for name in names:
        base = pathlib.PurePosixPath(name).name
        if base == ".env" or base.startswith(".env."):
            problems.append(f"{name}:1: .env files must not be committed")
            continue
        text = read_text(root / name)
        if text is None:
            continue
        found = check_header(name, text) + check_text(name, text)
        if name == UPLUGIN:
            found += check_uplugin(text)
        elif name == LANES:
            found += check_lanes(text)
        elif name == SKILL:
            found += check_skill(text)
        problems += [f"{name}:{line}: {message}" for line, message in found]
    for required in (UPLUGIN, LANES, SKILL):
        if required not in names:
            problems.append(f"{required}:1: file is missing")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    problems = lint(args.root.resolve())
    for problem in problems:
        print(problem)
    print(f"lint: {len(problems)} problem(s)" if problems else "lint: ok")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
