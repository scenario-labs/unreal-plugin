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


if __name__ == "__main__":
    unittest.main()
