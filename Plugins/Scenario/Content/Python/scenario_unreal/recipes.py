# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
"""Entry point for the kit tools declared in Resources/Kit.json.

The plugin runs run(name, base64 JSON arguments) and reads the line starting with
MARKER. A KitError becomes {"error": message}: a problem the caller can fix.
"""
import base64
import importlib
import inspect
import json
import os
import traceback

MARKER = "SCENARIO_KIT_RESULT "

TOOLS = {
    "build_world": ("world", "build_world"),
    "dress_world": ("world", "dress_world"),
    "kit_status": ("world", "kit_status"),
    "play_level": ("world", "play_level"),
    "make_game": ("game", "make_game"),
    "dress_template": ("game", "dress_template"),
    "make_film": ("film", "make_film"),
    "render_film": ("film", "render_film"),
    "film_status": ("film", "film_status"),
}


def _modules():
    from . import common, looks, world, game, film
    modules = [common, looks, world, game, film]
    if os.environ.get("SCENARIO_KIT_RELOAD") == "1":
        for module in modules:
            importlib.reload(module)
    return {m.__name__.rsplit(".", 1)[-1]: m for m in modules}


def run(name, encoded):
    result = None
    try:
        modules = _modules()
        common = modules["common"]
        args = json.loads(base64.b64decode(encoded).decode("utf-8")) if encoded else {}
        if name not in TOOLS:
            raise common.KitError(f"Unknown kit tool {name}.")
        module, function = TOOLS[name]
        target = getattr(modules[module], function)
        try:
            inspect.signature(target).bind(**args)
        except TypeError as error:
            raise common.KitError(f"Invalid arguments for {name}: {error}.")
        result = target(**args)
    except Exception as error:  # every failure goes back to the caller as one line
        kit_error = type(error).__name__ == "KitError"
        result = {"error": str(error) if kit_error else f"{type(error).__name__}: {error}"}
        if not kit_error:
            result["trace"] = traceback.format_exc()[-1500:]
    print(MARKER + json.dumps(result, default=str))
