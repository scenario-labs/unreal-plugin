# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
"""Walkable worlds: a new level with light, ground, walls and a player start, then
Scenario props, a landmark, a sky and ambient sound laid out on it."""
import math
import random

import unreal

from . import common, looks

BASIC_TEMPLATE = "/Engine/Maps/Templates/Template_Default"
GAME_MODES = (
    "/Game/ThirdPerson/Blueprints/BP_ThirdPersonGameMode",
    "/Game/FirstPerson/Blueprints/BP_FirstPersonGameMode",
    "/Game/TopDown/Blueprints/BP_TopDownGameMode",
)
PROP_LIMIT = 300
AREAS = ("anywhere", "edges", "center", "around_landmark", "path_sides")


def _check_open_level():
    dirty = [p.get_name() for p in unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages()]
    if not dirty:
        return
    if common.save_level_if_kit():
        return
    raise common.KitError(
        f"The open level ({common.level_path()}) has unsaved changes. Ask the person to save or discard them, "
        "or call save_changes with include_levels, then try again.")


def _game_mode(choice):
    candidates = GAME_MODES if choice in (None, "", "auto") else (common.package(choice),)
    for path in candidates:
        if common.assets().does_asset_exist(path):
            cls = unreal.EditorAssetLibrary.load_blueprint_class(path)
            if cls:
                return cls, path
    if choice not in (None, "", "auto"):
        raise common.KitError(f"game_mode: no Blueprint game mode at {choice}.")
    return None, None


def build_world(name, time_of_day="day", size_m=120, ground_material=None, ground_tile_m=4.0, sky=None, game_mode="auto"):
    """Creates and opens a new level: light for the time of day, a ground, invisible walls
    around the play area and a player start. Returns where things are."""
    size_m = max(40.0, min(float(size_m), 600.0))
    _check_open_level()
    path, final = common.free_level_path(f"{common.ROOT}/Worlds", common.slug(name, "World"))
    if not common.levels().new_level_from_template(path, BASIC_TEMPLATE):
        raise common.KitError(f"Unreal could not create the level {path}.")

    for actor in common.all_of(unreal.StaticMeshActor):
        mesh = actor.static_mesh_component.static_mesh
        if mesh and mesh.get_name() == "SM_Template_Map_Floor":
            common.remove([actor])

    half = size_m * 50.0
    cube = unreal.load_asset("/Engine/BasicShapes/Cube")
    ground_size = max(size_m * 4.0, 600.0)
    ground = common.spawn(cube, (0.0, 0.0, -50.0), label="Scenario Ground", folder="Scenario/World",
                          tags=("ScenarioKit.Ground", f"ScenarioKit.Size={int(size_m)}"))
    ground.set_actor_scale3d(unreal.Vector(ground_size, ground_size, 1.0))
    if ground_material:
        ground.static_mesh_component.set_material(0, looks.world_material(ground_material, ground_tile_m))

    for i, (x, y, sx, sy) in enumerate(((0, half, size_m, 1), (0, -half, size_m, 1), (half, 0, 1, size_m), (-half, 0, 1, size_m))):
        wall = common.spawn(cube, (x, y, 500.0), label=f"Scenario Edge {i + 1}", folder="Scenario/World", tags=("ScenarioKit.Wall",))
        wall.set_actor_scale3d(unreal.Vector(float(sx), float(sy), 10.0))
        wall.static_mesh_component.set_editor_property("visible", False)

    start = common.first_of(unreal.PlayerStart) or common.spawn(unreal.PlayerStart, label="Player Start")
    start.set_actor_location(unreal.Vector(0.0, -half * 0.6, 120.0), False, False)
    start.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=0.0, yaw=90.0), False)

    settings = common.world().get_world_settings()
    mode, mode_path = _game_mode(game_mode)
    if mode:
        settings.set_editor_property("default_game_mode", mode)
    settings.set_editor_property("kill_z", -3000.0)

    looks.apply_time_of_day(time_of_day)
    if sky:
        looks.set_sky(sky)
    common.frame_view((0.0, 0.0, 200.0), size_m * 50.0 * 0.9, size_m * 12.0)
    common.save_level()
    return {
        "level": path,
        "size_m": size_m,
        "play_area_m": [-size_m / 2, size_m / 2],
        "player_start_m": common.summary_vec(start.get_actor_location()),
        "landmark_spot_m": [0.0, 0.0],
        "game_mode": mode_path or "project default (no template game mode found)",
        "time_of_day": time_of_day,
        "next": "dress_world with props, a landmark, a ground material, a sky and ambient sound.",
    }


def _kit_world():
    grounds = common.tagged("ScenarioKit.Ground")
    if not grounds:
        raise common.KitError("The open level was not made by build_world. Call build_world first, or dress_template for a template level.")
    size_m = float(common.tag_value(grounds[0], "ScenarioKit.Size", "120"))
    return grounds[0], size_m


def _segment_distance(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    length = dx * dx + dy * dy
    t = 0.0 if length == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / length))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


class Layout:
    """Free space on a kit world: circles taken by things, plus areas kept clear."""

    def __init__(self, half, start, landmark=None, landmark_radius=0.0, path_width=500.0):
        self.half = half
        self.start = start
        self.landmark = landmark
        self.landmark_radius = landmark_radius
        self.path_width = path_width
        self.taken = []  # (x, y, radius)

    def clear(self, x, y, radius, gap=150.0, keep_path=True):
        if abs(x) > self.half - radius or abs(y) > self.half - radius:
            return False
        if math.hypot(x - self.start[0], y - self.start[1]) < 600.0 + radius:
            return False
        if self.landmark is not None:
            if math.hypot(x - self.landmark[0], y - self.landmark[1]) < self.landmark_radius + 300.0 + radius:
                return False
            if keep_path and _segment_distance(x, y, self.start[0], self.start[1], self.landmark[0], self.landmark[1]) < self.path_width / 2 + radius:
                return False
        return all(math.hypot(x - tx, y - ty) >= radius + tr + gap for tx, ty, tr in self.taken)

    def candidates(self, rng, area, radius):
        h = self.half
        if area == "path_sides" and self.landmark is not None:
            ax, ay = self.start
            bx, by = self.landmark
            length = math.hypot(bx - ax, by - ay)
            nx, ny = -(by - ay) / length, (bx - ax) / length
            offset = self.path_width / 2 + radius + 50.0
            steps = max(2, int(length / max(radius * 2 + 250.0, 300.0)))
            for i in range(1, steps):
                t = i / steps
                for side in (1, -1):
                    yield ax + (bx - ax) * t + nx * offset * side, ay + (by - ay) * t + ny * offset * side, False
            return
        for _ in range(4000):
            if area == "edges":
                edge = rng.uniform(0.72, 0.96) * h
                along = rng.uniform(-0.96, 0.96) * h
                side = rng.randrange(4)
                x, y = [(along, edge), (along, -edge), (edge, along), (-edge, along)][side]
            elif area == "center":
                angle, r = rng.uniform(0, 2 * math.pi), math.sqrt(rng.random()) * 0.45 * h
                x, y = math.cos(angle) * r, math.sin(angle) * r
            elif area == "around_landmark" and self.landmark is not None:
                angle = rng.uniform(0, 2 * math.pi)
                r = self.landmark_radius + 400.0 + radius + rng.uniform(0, 1200.0)
                x, y = self.landmark[0] + math.cos(angle) * r, self.landmark[1] + math.sin(angle) * r
            else:
                x, y = rng.uniform(-h, h), rng.uniform(-h, h)
            yield x, y, True


def _height(spec, rng):
    value = spec.get("height_m", 1.5)
    if isinstance(value, (list, tuple)) and len(value) == 2:
        return rng.uniform(float(value[0]), float(value[1]))
    return float(value)


def _yaw(spec, rng, x, y, focus):
    mode = spec.get("yaw", "random")
    if isinstance(mode, (int, float)):
        return float(mode)
    if mode == "face_center" and focus is not None:
        return math.degrees(math.atan2(focus[1] - y, focus[0] - x)) + float(spec.get("front_yaw", 0.0))
    return rng.uniform(0.0, 360.0)


def dress_world(props=(), landmark=None, ground_material=None, ground_tile_m=4.0, sky=None, ambient=None,
                ambient_volume=0.5, time_of_day=None, seed=7, replace=True):
    """Lays Scenario assets out on a world made by build_world."""
    ground, size_m = _kit_world()
    half = size_m * 50.0
    rng = random.Random(seed)
    if replace:
        common.remove(common.tagged("ScenarioKit.Prop") + common.tagged("ScenarioKit.Landmark"))
    start_actor = common.first_of(unreal.PlayerStart)
    start = (start_actor.get_actor_location().x, start_actor.get_actor_location().y) if start_actor else (0.0, -half * 0.6)

    report = {"level": common.level_path(), "size_m": size_m, "props": [], "warnings": []}
    landmark_xy, landmark_r = None, 0.0
    if landmark:
        mesh = common.load_mesh(landmark.get("mesh"), "landmark")
        scale = common.fit_scale(mesh, landmark.get("height_m", 6.0))
        at = landmark.get("at_m", [0.0, 0.0])
        landmark_xy = (float(at[0]) * 100.0, float(at[1]) * 100.0)
        landmark_r = common.footprint(mesh, scale)
        if landmark_r > half * 0.5:
            report["warnings"].append(f"The landmark is {round(landmark_r / 50.0)} m wide: lower height_m if it fills the area.")
        yaw = landmark.get("yaw")
        if yaw is None:
            yaw = math.degrees(math.atan2(start[1] - landmark_xy[1], start[0] - landmark_xy[0])) + float(landmark.get("front_yaw", 0.0))
        actor = common.place_mesh(mesh, landmark_xy[0], landmark_xy[1], yaw, scale, ground=0.0,
                                  label=f"Landmark {mesh.get_name()}", folder="Scenario/World",
                                  tags=("ScenarioKit.Landmark", f"ScenarioKit.Radius={int(landmark_r)}"))
        report["landmark"] = {"actor": actor.get_actor_label(), "at_m": [landmark_xy[0] / 100.0, landmark_xy[1] / 100.0],
                              "height_m": landmark.get("height_m", 6.0), "radius_m": round(landmark_r / 100.0, 1)}
    else:
        existing = common.tagged("ScenarioKit.Landmark")
        if existing:
            loc = existing[0].get_actor_location()
            landmark_xy = (loc.x, loc.y)
            landmark_r = float(common.tag_value(existing[0], "ScenarioKit.Radius", "300"))

    layout = Layout(half, start, landmark_xy, landmark_r)
    if not replace:
        for actor in common.tagged("ScenarioKit.Prop"):
            loc = actor.get_actor_location()
            layout.taken.append((loc.x, loc.y, float(common.tag_value(actor, "ScenarioKit.Radius", "100"))))

    total = 0
    for index, spec in enumerate(props or ()):
        mesh = common.load_mesh(spec.get("mesh"), f"props[{index}]")
        area = spec.get("area", "anywhere")
        if area not in AREAS:
            raise common.KitError(f"props[{index}].area must be one of {', '.join(AREAS)}.")
        count = max(0, min(int(spec.get("count", 6)), PROP_LIMIT - total))
        gap = float(spec.get("spacing_m", 1.5)) * 100.0
        placed = 0
        candidates = layout.candidates(rng, area, common.footprint(mesh, common.fit_scale(mesh, _height(spec, random.Random(0)))))
        for x, y, keep_path in candidates:
            if placed >= count:
                break
            scale = common.fit_scale(mesh, _height(spec, rng))
            radius = common.footprint(mesh, scale)
            if not layout.clear(x, y, radius, gap, keep_path):
                continue
            yaw = _yaw(spec, rng, x, y, landmark_xy or (0.0, 0.0))
            common.place_mesh(mesh, x, y, yaw, scale, ground=0.0, label=f"{mesh.get_name()} {placed + 1}",
                              folder=f"Scenario/Props/{mesh.get_name()}",
                              tags=("ScenarioKit.Prop", f"ScenarioKit.Radius={int(radius)}"), lift=-float(spec.get("sink_m", 0.0)) * 100.0)
            layout.taken.append((x, y, radius))
            placed += 1
        total += placed
        entry = {"mesh": mesh.get_path_name().split(".")[0], "placed": placed, "asked": count, "area": area}
        if placed < count:
            entry["note"] = "not enough free room: lower height_m or spacing_m, or use a bigger size_m in build_world"
        report["props"].append(entry)

    if ground_material:
        ground.static_mesh_component.set_material(0, looks.world_material(ground_material, ground_tile_m))
        report["ground_material"] = common.package(ground_material)
    if time_of_day:
        looks.apply_time_of_day(time_of_day)
        report["time_of_day"] = time_of_day
    if sky:
        looks.set_sky(sky)
        report["sky"] = common.package(sky)
    elif looks.scenario_skies():
        looks.light_from_sky()
    if ambient:
        sound = common.load_sound(ambient, "ambient")
        if isinstance(sound, unreal.SoundWave) and not sound.get_editor_property("looping"):
            sound.set_editor_property("looping", True)
            common.assets().save_loaded_asset(sound)
        common.remove(common.tagged("ScenarioKit.Ambient"))
        player = common.spawn(unreal.AmbientSound, (0.0, 0.0, 200.0), label="Scenario Ambient", folder="Scenario/World", tags=("ScenarioKit.Ambient",))
        audio = player.audio_component
        audio.set_sound(sound)
        audio.set_editor_property("volume_multiplier", float(ambient_volume))
        audio.set_editor_property("allow_spatialization", False)
        report["ambient"] = common.package(ambient)
    if landmark_xy is not None:
        common.frame_view((landmark_xy[0], landmark_xy[1], 300.0), max(landmark_r * 4.0, half * 0.5), max(landmark_r * 1.2, 600.0))
    else:
        common.frame_view((0.0, 0.0, 200.0), half * 0.9, size_m * 12.0)
    common.save_level_if_kit()
    report["total_props"] = total
    report["next"] = "screenshot_viewport to check, then make_game, make_film, or play_level."
    return report


def kit_status():
    """What the kit has made in the open level."""
    level = common.level_path()
    status = {"level": level, "kit_world": bool(common.tagged("ScenarioKit.Ground"))}
    if status["kit_world"]:
        status["size_m"] = float(common.tag_value(common.tagged("ScenarioKit.Ground")[0], "ScenarioKit.Size", "0"))
    status["props"] = len(common.tagged("ScenarioKit.Prop"))
    status["landmark"] = [a.get_actor_label() for a in common.tagged("ScenarioKit.Landmark")]
    status["sky"] = len(looks.scenario_skies()) > 0
    status["pickups"] = len(common.all_of(unreal.ScenarioPickup)) if hasattr(unreal, "ScenarioPickup") else 0
    status["goal"] = len(common.all_of(unreal.ScenarioGoal)) if hasattr(unreal, "ScenarioGoal") else 0
    status["game_rules"] = bool(common.all_of(unreal.ScenarioGameRules)) if hasattr(unreal, "ScenarioGameRules") else False
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    status["films"] = sorted(str(d.package_name) for d in registry.get_assets_by_path(unreal.Name(f"{common.ROOT}/Films"), recursive=True)
                             if str(d.asset_class_path.asset_name) == "LevelSequence")
    status["playing"] = common.playing()
    return status


def play_level(action="start"):
    """Starts or stops Play in the editor, so the person can walk the level."""
    if action == "stop":
        common.levels().editor_request_end_play()
        return {"playing": False}
    if common.playing():
        return {"playing": True, "note": "already playing"}
    common.levels().editor_request_begin_play()
    return {"playing": True, "note": "Play starts on the next frame. The person clicks in the viewport to control the character; Esc stops."}
