# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
"""Tests of tools/build_icons.py."""
import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
import build_icons  # noqa: E402


class SpacedPathTest(unittest.TestCase):
    def test_compact_arc_flags_are_split(self):
        self.assertEqual(build_icons.spaced_path("M19.5 12a7.5 7.5 0 11-2.2-5.3"), "M 19.5 12 a 7.5 7.5 0 1 1 -2.2 -5.3")

    def test_repeated_arcs_keep_their_flags(self):
        self.assertEqual(build_icons.spaced_path("a1 1 0 010 2 1 1 0 100-2"), "a 1 1 0 0 1 0 2 1 1 0 1 0 0 -2")

    def test_leading_dots_and_signs(self):
        self.assertEqual(build_icons.spaced_path("M.5-.5l2.2.8-2.2.8z"), "M .5 -.5 l 2.2 .8 -2.2 .8 z")

    def test_every_icon_has_spaced_arcs(self):
        # Each arc segment reads as seven separate tokens once spaced.
        for name, body in {**build_icons.ICONS, **build_icons.FILLED}.items():
            for data in re.findall(r'd="([^"]*)"', build_icons.svg("", body)):
                for segment in re.findall(r"[aA]([^a-zA-Z]*)", data):
                    self.assertEqual(len(segment.split()) % 7, 0, name)


if __name__ == "__main__":
    unittest.main()
