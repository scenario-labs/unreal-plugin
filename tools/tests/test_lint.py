# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
"""Tests of tools/lint.py. Secret-looking values are built at run time so this file passes the lint itself."""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
import lint  # noqa: E402

HEADER = lint.HEADER
DASH = chr(0x2014)


def lanes(**overrides):
    data = {
        "version": 1,
        "lanes": [{
            "id": "image", "title": "Image", "capability": "txt2img", "importer": "texture",
            "folder": "Images", "hint": "Textures.", "starter_prompts": ["a chest"],
            "models": [{"id": "model_a", "name": "A"}],
        }],
        "presets": {"model_a": {}},
    }
    data.update(overrides)
    return json.dumps(data)


class HeaderTests(unittest.TestCase):
    def test_source_file_needs_the_header(self):
        self.assertEqual(lint.check_header("Plugins/Scenario/Source/A.cpp", "#include \"A.h\"\n")[0][0], 1)

    def test_header_present(self):
        self.assertEqual(lint.check_header("Source/Host/Host.Build.cs", HEADER + "\nusing UnrealBuildTool;\n"), [])

    def test_other_files_are_not_checked(self):
        self.assertEqual(lint.check_header("tools/lint.py", "#!/usr/bin/env python3\n"), [])


class TextTests(unittest.TestCase):
    def test_em_dash_is_reported_with_its_line(self):
        found = lint.check_text("docs/a.md", "fine\nnot fine " + DASH + " here\n")
        self.assertEqual(found, [(2, "em dash: use a comma, colon or period")])

    def test_api_key_and_basic_credential(self):
        key = "api_" + "A1b2C3d4E5f6G7h8I9j0K1"
        basic = "Basic " + "QWxhZGRpbjpvcGVuIHNlc2FtZQ" + "QWxhZGRpbjpvcGVuIHNlc2FtZQ=="
        messages = [m for _, m in lint.check_text("x.cpp", key + "\n" + basic + "\n")]
        self.assertIn("looks like a Scenario API key", messages)
        self.assertIn("looks like a Basic credential", messages)

    def test_private_key_and_signed_url(self):
        text = "-----BEGIN " + "PRIVATE KEY-----\nhttps://cdn/a.png?X-Amz-" + "Signature=" + "0123456789abcdef0123\n"
        messages = [m for _, m in lint.check_text("x.md", text)]
        self.assertEqual(messages, ["private key", "signed URL signature"])

    def test_synthetic_values_and_the_marker_pass(self):
        fake = "Bearer " + "eyJhbGciOiJIUzI1NiJ9.payload.sig"
        real_looking = "Bearer " + "Zk9xR2Vb3sLq8TnW4yHc7uJm1pAe5dKf"
        self.assertEqual(lint.check_text("x.cpp", fake + "\n"), [])
        self.assertEqual(lint.check_text("x.cpp", real_looking + "  // " + lint.ALLOW_MARKER + "\n"), [])
        self.assertEqual(len(lint.check_text("x.cpp", real_looking + "\n")), 1)

    def test_home_paths(self):
        self.assertEqual(lint.check_text("a.sh", "UE=/Users/Shared/Epic Games/UE_5.8\n"), [])
        self.assertEqual(lint.check_text("a.md", "open /Users/you/Project\n"), [])
        self.assertEqual(len(lint.check_text("a.md", "/Users/" + "jdoe/Project\n")), 1)


class AsciiTests(unittest.TestCase):
    def test_windows_scripts_must_be_ascii(self):
        self.assertEqual(lint.check_ascii("tools/install/Install-Scenario.ps1", "Write-Host 'ok'\nWrite-Host 'caf" + chr(0xE9) + "'\n")[0][0], 2)
        self.assertEqual(lint.check_ascii("Install-Windows.bat", "@echo off\n"), [])

    def test_other_files_may_hold_any_text(self):
        self.assertEqual(lint.check_ascii("README.md", "caf" + chr(0xE9) + "\n"), [])


class ResourceTests(unittest.TestCase):
    def test_valid_lanes(self):
        self.assertEqual(lint.check_lanes(lanes()), [])

    def test_lanes_problems(self):
        broken = json.loads(lanes())
        broken["lanes"][0]["models"] = [{"id": "gpt", "name": ""}]
        broken["presets"] = {"model_b": {}}
        messages = [m for _, m in lint.check_lanes(json.dumps(broken))]
        self.assertIn("lane image: a model has no name", messages)
        self.assertIn("lane image: model id gpt does not start with model_", messages)
        self.assertIn("presets: model_b is not a lane model", messages)

    def test_invalid_json(self):
        self.assertTrue(lint.check_lanes("{")[0][1].startswith("invalid JSON"))
        self.assertTrue(lint.check_trained("{")[0][1].startswith("invalid JSON"))

    def test_valid_trained(self):
        data = {"version": 1, "base_probes": ["model_a", "model_b"], "base_names": {"a-1": "model_a"}, "default_scale": 0.75}
        self.assertEqual(lint.check_trained(json.dumps(data)), [])

    def test_trained_problems(self):
        data = {"version": 1, "base_probes": ["model_a", "b", "model_a"], "base_names": {"c-1": "model_c"}, "default_scale": "high"}
        messages = [m for _, m in lint.check_trained(json.dumps(data))]
        self.assertIn("base_probes: b does not start with model_", messages)
        self.assertIn("base_probes: duplicate id", messages)
        self.assertIn("base_names: c-1 maps to model_c, which base_probes does not list", messages)
        self.assertIn("default_scale must be a finite number of 0 or more", messages)
        self.assertEqual(lint.check_trained(json.dumps({"version": 1})), [
            (1, "missing key base_probes"), (1, "missing key base_names"), (1, "missing key default_scale"),
            (1, "base_probes must be a non-empty list")])

    def test_trained_root_and_scale(self):
        for root in ("null", "[]", "\"x\""):
            self.assertEqual(lint.check_trained(root), [(1, "must be a JSON object")])
        for scale in ("1e309", "NaN", "-Infinity", "-1"):
            text = '{"version": 1, "base_probes": ["model_a"], "base_names": {}, "default_scale": %s}' % scale
            self.assertEqual(lint.check_trained(text), [(1, "default_scale must be a finite number of 0 or more")], scale)
        for scale in ("0", "1" + "0" * 400):
            text = '{"version": 1, "base_probes": ["model_a"], "base_names": {}, "default_scale": %s}' % scale
            self.assertEqual(lint.check_trained(text), [], scale[:8])

    def test_uplugin_modules_are_editor_only(self):
        plugin = {key: "x" for key in lint.UPLUGIN_KEYS}
        plugin["VersionName"] = "0.1.0"
        plugin["Modules"] = [{"Name": "ScenarioCore", "Type": "Runtime"}, {"Name": "ScenarioGameplay", "Type": "Runtime"}]
        messages = [m for _, m in lint.check_uplugin(json.dumps(plugin))]
        self.assertEqual(messages, ["module ScenarioCore must be Type Editor (only ScenarioGameplay ships in a game)"])

    def test_skill_frontmatter(self):
        self.assertEqual(lint.check_skill("---\nname: a\ndescription: b\n---\n# A\n"), [])
        self.assertEqual(lint.check_skill("---\nname: a\n---\n"), [(1, "frontmatter has no description")])
        self.assertEqual(len(lint.check_skill("# no frontmatter\n")), 1)


class RepositoryTests(unittest.TestCase):
    def test_env_file_and_missing_resources(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            (root / ".env").write_text("SCENARIO_API_SECRET=\n")
            problems = lint.lint(root)
        self.assertIn(".env:1: .env files must not be committed", problems)
        self.assertIn(lint.UPLUGIN + ":1: file is missing", problems)

    def test_unreadable_required_resource(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            trained = root / lint.TRAINED
            trained.parent.mkdir(parents=True)
            trained.write_bytes(b'{"version": 1}\xff')
            (root / "notes.bin").write_bytes(b"\0\xff")
            problems = lint.lint(root)
        self.assertIn(lint.TRAINED + ":1: not UTF-8 text", problems)
        self.assertNotIn(lint.TRAINED + ":1: file is missing", problems)
        self.assertFalse([p for p in problems if p.startswith("notes.bin")])


if __name__ == "__main__":
    unittest.main()
