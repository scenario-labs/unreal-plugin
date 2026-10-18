# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
"""Assembles a demo project from Epic's Third Person template on this machine.

The demo shows an agent turning one prompt into a walkable world, a small game
or a short film through the plugin's local Scenario MCP server. The project is
made from the engine's own template files (Epic content stays on this machine,
nothing from it is in this repository) and links the plugin from this
repository, so plugin changes apply without copying.

Usage:
  python3 tools/make_demo_project.py <destination folder> [--name ScenarioDemo]
      [--variants Platforming,Combat] [--engine "/Users/Shared/Epic Games/UE_5.8"]

What the Project Browser does for the template is reproduced here: copy the
template, install its shared content packs (Characters, Input,
LevelPrototyping and the chosen variants) under /Game/<pack>, and rename the
project. It also enables the plugins the demo uses: Scenario, Python,
Sequencer scripting and Movie Render Queue.
"""
import argparse
import json
import os
import pathlib
import re
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEMO_PLUGINS = ["Scenario", "PythonScriptPlugin", "SequencerScripting", "MovieRenderPipeline", "EditorScriptingUtilities", "MediaPlate", "HDRIBackdrop"]


def read_manifest(path):
    text = re.sub(r",\s*([}\]])", r"\1", path.read_text(encoding="utf-8-sig"))
    return json.loads(text)


def copy_tree(src, dst):
    for item in src.rglob("*"):
        if item.is_file():
            target = dst / item.relative_to(src)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(item, target)


def install_pack(engine, pack_dir, content):
    """Copies a shared content pack where the Project Browser puts it."""
    manifest = read_manifest(pack_dir / "FeaturePack" / "manifest.json")
    folder = manifest.get("AdditionalFiles", {}).get("DestinationFilesFolder") or pack_dir.name
    copy_tree(pack_dir / "Content", content / folder)
    # One-file-per-actor data follows the level's package path: /Game/__ExternalActors__/<folder>/<level>.
    for external in ("__ExternalActors__", "__ExternalObjects__"):
        if (pack_dir / external).is_dir():
            copy_tree(pack_dir / external, content / external / folder)
    return folder


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("destination")
    parser.add_argument("--name", default="ScenarioDemo")
    parser.add_argument("--variants", default="Platforming,Combat")
    parser.add_argument("--engine", default=os.environ.get("UE_ROOT", "/Users/Shared/Epic Games/UE_5.8"))
    args = parser.parse_args()

    engine = pathlib.Path(args.engine)
    template = engine / "Templates" / "TP_ThirdPersonBP"
    resources = engine / "Templates" / "TemplateResources"
    if not template.is_dir():
        sys.exit(f"No Third Person template under {engine}. Install Unreal with templates or pass --engine.")
    project = pathlib.Path(args.destination).resolve() / args.name
    if project.exists():
        sys.exit(f"{project} exists already; choose another destination or name.")

    project.mkdir(parents=True)
    for part in ("Config", "Content"):
        copy_tree(template / part, project / part)

    defs = (template / "Config" / "TemplateDefs.ini").read_text(encoding="utf-8-sig")
    packs = re.findall(r'^SharedContentPacks=\(MountName="([^"]+)",DetailLevels=\("([^"]+)"\)\)', defs, re.M)
    for variant in [v.strip() for v in args.variants.split(",") if v.strip()]:
        packs.append((f"Variant_{variant}", "Standard"))
    installed = []
    for mount, level in packs:
        pack_dir = resources / level / mount
        if not pack_dir.is_dir():
            pack_dir = resources / ("Standard" if level == "High" else "High") / mount
        if not pack_dir.is_dir():
            print(f"skipped pack {mount}: not installed with this engine")
            continue
        installed.append(install_pack(engine, pack_dir, project / "Content"))
    descriptor = json.loads((template / "TP_ThirdPersonBP.uproject").read_text(encoding="utf-8-sig"))
    plugins = {p["Name"]: p for p in descriptor.get("Plugins", [])}
    for name in DEMO_PLUGINS:
        plugins[name] = {"Name": name, "Enabled": True}
    descriptor["Plugins"] = list(plugins.values())
    descriptor["Description"] = "Scenario for Unreal demo: one prompt to a walkable world, a small game or a short film."
    (project / f"{args.name}.uproject").write_text(json.dumps(descriptor, indent="\t") + "\n")

    game = project / "Config" / "DefaultGame.ini"
    text = game.read_text(encoding="utf-8-sig") if game.exists() else ""
    if "ProjectName=" in text:
        text = re.sub(r"ProjectName=.*", f"ProjectName={args.name}", text)
    else:
        text += f"\n[/Script/EngineSettings.GeneralProjectSettings]\nProjectName={args.name}\n"
    game.write_text(text)

    (project / "Plugins").mkdir()
    os.symlink(ROOT / "Plugins" / "Scenario", project / "Plugins" / "Scenario")
    print(f"Demo project: {project / (args.name + '.uproject')}")
    print(f"Packs: {', '.join(installed)}")


if __name__ == "__main__":
    main()
