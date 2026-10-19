# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
"""Walkable worlds: a new level with light, ground, walls and a player start, then
Scenario props, a landmark, a path, a sky and ambient sound laid out on it."""
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
LIGHT_LIMIT = 48
AREAS = ("anywhere", "edges", "center", "around_landmark", "path_sides", "on_path")
# Props that need exact spots go first, whatever their order in the call.
AREA_ORDER = ("on_path", "path_sides", "around_landmark", "center", "anywhere", "edges")
# Scenario 3D models (glTF) face local +Y; Unreal's yaw 0 faces +X.
FRONT_YAW = -90.0
PROP_LIGHT = {"temperature_k": 2200.0, "intensity_cd": 4.0, "radius_m": 4.5, "height": 0.63}


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


def build_world(name, time_of_day="day", size_m=120, ground_material=None, ground_tile_m=4.0, sky=None, game_mode="auto",
                mist=None):
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

    sun = looks.apply_time_of_day(time_of_day, mist or 0.0)
    if sky:
        sun = looks.set_sky(sky)
    looks.recapture()
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
        "mist": looks.current_mist(),
        "sun": sun,
        "next": "dress_world with props, a landmark, a path, a ground material, a sky and ambient sound.",
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
    """Free space on a kit world: circles taken by things, plus areas kept clear.

    The path runs from the player start to path_end (the landmark, or the world's center
    when a path strip was laid without one). Without either, nothing is kept clear for it."""

    def __init__(self, half, start, landmark=None, landmark_radius=0.0, path_end=None, path_width=500.0):
        self.half = half
        self.start = start
        self.landmark = landmark
        self.landmark_radius = landmark_radius
        self.path_end = path_end if path_end is not None else landmark
        self.path_width = path_width
        self.taken = []  # (x, y, radius)

    def clear(self, x, y, radius, gap=150.0, keep_path=True):
        if abs(x) > self.half - radius or abs(y) > self.half - radius:
            return False
        if math.hypot(x - self.start[0], y - self.start[1]) < 600.0 + radius:
            return False
        if self.landmark is not None and math.hypot(x - self.landmark[0], y - self.landmark[1]) < self.landmark_radius + 300.0 + radius:
            return False
        if keep_path and self.path_end is not None:
            if _segment_distance(x, y, self.start[0], self.start[1], self.path_end[0], self.path_end[1]) < self.path_width / 2 + radius:
                return False
        return all(math.hypot(x - tx, y - ty) >= radius + tr + gap for tx, ty, tr in self.taken)

    def path_line(self):
        """Start, end and length of the path (to the world's center when there is no landmark)."""
        ax, ay = self.start
        bx, by = self.path_end if self.path_end is not None else (0.0, 0.0)
        return ax, ay, bx, by, math.hypot(bx - ax, by - ay)

    def path_slots(self, area, count, radius, gap, wide_along_x):
        """Evenly spaced spots along the path: both sides (path_sides) or on it, crossing it (on_path).
        Returns ([(x, y, keep_path, yaw or None)], note when fewer than count fit)."""
        ax, ay, bx, by, length = self.path_line()
        if count <= 0:
            return [], None
        if length < 1.0:
            return [], "the player start is on the landmark spot: there is no path to follow"
        ux, uy = (bx - ax) / length, (by - ay) / length
        begin = 600.0 + radius + 50.0
        end = length - ((self.landmark_radius + 300.0 + radius + 50.0) if self.landmark is not None else radius)
        if end < begin:
            return [], f"the path is {length / 100.0:.0f} m long: too short for this prop between the player start and the landmark"
        usable = end - begin
        wanted = math.ceil(count / 2) if area == "path_sides" else count
        fit = int(usable // (2.0 * radius + gap)) + 1
        n = min(wanted, fit)
        note = None
        if n < wanted:
            what = "pairs" if area == "path_sides" else "spots"
            note = (f"room for {n} {what} along {usable / 100.0:.0f} m of path at spacing_m {gap / 100.0:g} and this height: "
                    "lower spacing_m or height_m, or use a bigger size_m in build_world")
        if n == 1:
            # One gate marks the entrance; one pair of side props sits halfway.
            along = [begin] if area == "on_path" else [begin + usable * 0.5]
        else:
            along = [begin + usable * i / (n - 1) for i in range(n)]
        slots = []
        if area == "on_path":
            # The widest horizontal side crosses the path, so the player walks through the gate.
            yaw = math.degrees(math.atan2(uy, ux)) + (90.0 if wide_along_x else 0.0)
            for d in along:
                slots.append((ax + ux * d, ay + uy * d, False, yaw))
        else:
            nx, ny = -uy, ux
            offset = self.path_width / 2 + radius + 50.0
            for d in along:
                for side in (1, -1):
                    slots.append((ax + ux * d + nx * offset * side, ay + uy * d + ny * offset * side, False, None))
        return slots, note

    def candidates(self, rng, area, radius):
        h = self.half
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
            yield x, y, True, None


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
        return math.degrees(math.atan2(focus[1] - y, focus[0] - x)) + float(spec.get("front_yaw", FRONT_YAW))
    return rng.uniform(0.0, 360.0)


def _lay_path(spec, start, end, landmark_r):
    """A flat strip from just behind the player start to the landmark's edge, in a world-aligned material."""
    if isinstance(spec, str):
        spec = {"material": spec}
    if not spec.get("material"):
        raise common.KitError("path needs material: a Scenario material result (stone, gravel, planks).")
    width = max(1.0, min(float(spec.get("width_m", 3.5)), 20.0)) * 100.0
    tile_m = float(spec.get("tile_m", 2.0))
    ax, ay = start
    bx, by = end
    length = math.hypot(bx - ax, by - ay)
    if length < 300.0:
        raise common.KitError("path: the player start is too close to the landmark spot for a path.")
    ux, uy = (bx - ax) / length, (by - ay) / length
    begin, stop = -150.0, length - landmark_r * 0.7
    middle = (begin + stop) * 0.5
    cube = unreal.load_asset("/Engine/BasicShapes/Cube")
    # 2 cm thick on the ground: no step to trip on, and no fight with the ground's surface.
    strip = common.spawn(cube, (ax + ux * middle, ay + uy * middle, 1.0), math.degrees(math.atan2(uy, ux)),
                         label="Scenario Path", folder="Scenario/World",
                         tags=("ScenarioKit.Path", f"ScenarioKit.PathWidth={int(width)}"))
    strip.set_actor_scale3d(unreal.Vector((stop - begin) / 100.0, width / 100.0, 0.02))
    component = strip.static_mesh_component
    component.set_collision_profile_name("NoCollision")
    component.set_cast_shadow(False)
    component.set_material(0, looks.world_material(spec["material"], tile_m, spec.get("tint")))
    return strip, width, {"material": common.package(spec["material"]), "width_m": width / 100.0,
                          "length_m": round((stop - begin) / 100.0, 1), "tile_m": tile_m}


def _prop_light(actor, spec):
    """A warm point light inside a placed prop (lanterns, braziers, windows)."""
    cfg = dict(PROP_LIGHT)
    if isinstance(spec, dict):
        cfg.update({k: v for k, v in spec.items() if k in cfg})
    origin, extent = actor.get_actor_bounds(False)
    z = origin.z - extent.z + 2.0 * extent.z * float(cfg["height"])
    light = common.spawn(unreal.PointLight, (origin.x, origin.y, z), label=f"{actor.get_actor_label()} Light",
                         folder=str(actor.get_folder_path()), tags=("ScenarioKit.PropLight",))
    component = light.get_component_by_class(unreal.PointLightComponent)
    component.set_mobility(unreal.ComponentMobility.MOVABLE)
    looks._set(component, "intensity_units", unreal.LightUnits.CANDELAS)
    component.set_intensity(float(cfg["intensity_cd"]))
    looks._set(component, "use_temperature", True)
    looks._set(component, "temperature", float(cfg["temperature_k"]))
    component.set_attenuation_radius(float(cfg["radius_m"]) * 100.0)
    looks._set(component, "source_radius", 4.0)
    # Many small lights: no shadows (the prop would also shadow its own flame).
    component.set_cast_shadows(False)
    return light


def _ground_material(ground, ground_material, ground_tile_m, ground_tint, report):
    source = ground_material
    tile_m = ground_tile_m
    if not source:
        current = ground.static_mesh_component.get_material(0)
        if current is None or not isinstance(current, unreal.MaterialInstance):
            report["warnings"].append("ground_tint needs a ground_material (the ground has none yet).")
            return
        source = current.get_path_name()
        if tile_m is None:
            try:
                tile_m = unreal.MaterialEditingLibrary.get_material_instance_vector_parameter_value(current, "TileSize").r / 100.0
            except Exception:
                tile_m = None
    ground.static_mesh_component.set_material(0, looks.world_material(source, tile_m or 4.0, ground_tint))
    report["ground_material"] = common.package(source)
    if ground_tint:
        report["ground_tint"] = list(ground_tint)


def dress_world(props=(), landmark=None, path=None, ground_material=None, ground_tile_m=None, ground_tint=None, sky=None,
                ambient=None, ambient_volume=0.5, time_of_day=None, mist=None, seed=7, replace=True):
    """Lays Scenario assets out on a world made by build_world."""
    ground, size_m = _kit_world()
    half = size_m * 50.0
    rng = random.Random(seed)
    if replace:
        common.remove(common.tagged("ScenarioKit.Prop") + common.tagged("ScenarioKit.PropLight") + common.tagged("ScenarioKit.Landmark"))
    if path:
        common.remove(common.tagged("ScenarioKit.Path"))
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
            front = landmark.get("front_yaw")
            yaw = math.degrees(math.atan2(start[1] - landmark_xy[1], start[0] - landmark_xy[0])) + float(FRONT_YAW if front is None else front)
        actor = common.place_mesh(mesh, landmark_xy[0], landmark_xy[1], yaw, scale, ground=0.0,
                                  label=f"Landmark {mesh.get_name()}", folder="Scenario/World",
                                  tags=("ScenarioKit.Landmark", f"ScenarioKit.Radius={int(landmark_r)}"))
        report["landmark"] = {"actor": actor.get_actor_label(), "at_m": [landmark_xy[0] / 100.0, landmark_xy[1] / 100.0],
                              "height_m": landmark.get("height_m", 6.0), "radius_m": round(landmark_r / 100.0, 1),
                              "yaw": round(yaw % 360.0, 1)}
    else:
        existing = common.tagged("ScenarioKit.Landmark")
        if existing:
            loc = existing[0].get_actor_location()
            landmark_xy = (loc.x, loc.y)
            landmark_r = float(common.tag_value(existing[0], "ScenarioKit.Radius", "300"))

    path_end = landmark_xy if landmark_xy is not None else (0.0, 0.0)
    path_width = None
    if path:
        _, strip_width, report["path"] = _lay_path(path, start, path_end, landmark_r)
        path_width = strip_width + 100.0
    else:
        strips = common.tagged("ScenarioKit.Path")
        if strips:
            path_width = float(common.tag_value(strips[0], "ScenarioKit.PathWidth", "350")) + 100.0
    keep_clear = landmark_xy is not None or path_width is not None
    layout = Layout(half, start, landmark_xy, landmark_r, path_end if keep_clear else None, path_width or 500.0)
    if not replace:
        for actor in common.tagged("ScenarioKit.Prop"):
            loc = actor.get_actor_location()
            layout.taken.append((loc.x, loc.y, float(common.tag_value(actor, "ScenarioKit.Radius", "100"))))

    specs = list(props or ())
    for index, spec in enumerate(specs):
        if spec.get("area", "anywhere") not in AREAS:
            raise common.KitError(f"props[{index}].area must be one of {', '.join(AREAS)}.")
    order = sorted(range(len(specs)), key=lambda i: (AREA_ORDER.index(specs[i].get("area", "anywhere")), i))
    entries = [None] * len(specs)
    total = 0
    lights = 0 if replace else len(common.tagged("ScenarioKit.PropLight"))
    for index in order:
        spec = specs[index]
        mesh = common.load_mesh(spec.get("mesh"), f"props[{index}]")
        area = spec.get("area", "anywhere")
        count = max(0, min(int(spec.get("count", 6)), PROP_LIMIT - total))
        gap = float(spec.get("spacing_m", 1.5)) * 100.0
        slot_scale = common.fit_scale(mesh, _height(spec, random.Random(0)))
        slot_radius = common.footprint(mesh, slot_scale)
        note = None
        keep_radius = None
        if area in ("path_sides", "on_path"):
            lo, hi = common.mesh_box(mesh)
            if area == "on_path":
                # A gate is wide across the path but thin along it: side props may stand just before and after it.
                keep_radius = max(min(hi.x - lo.x, hi.y - lo.y) * slot_scale * 0.5, 50.0)
            slots, note = layout.path_slots(area, count, slot_radius, gap, (hi.x - lo.x) >= (hi.y - lo.y))
            if area == "on_path":
                across = max(hi.x - lo.x, hi.y - lo.y) * slot_scale
                strip = (path_width - 100.0) if path_width else 350.0
                if across < strip + 50.0:
                    report["warnings"].append(f"props[{index}] is {across / 100.0:.1f} m wide, the path {strip / 100.0:.1f} m: "
                                              "raise height_m or narrow the path so the player walks through it.")
        else:
            slots = layout.candidates(rng, area, slot_radius)
        placed, blocked, lit = 0, 0, 0
        light_spec = spec.get("light")
        for x, y, keep_path, fixed_yaw in slots:
            if placed >= count:
                break
            scale = slot_scale if area == "on_path" else common.fit_scale(mesh, _height(spec, rng))
            radius = common.footprint(mesh, scale)
            if not layout.clear(x, y, radius, gap, keep_path):
                blocked += 1
                continue
            if fixed_yaw is not None:
                yaw = fixed_yaw + float(spec.get("front_yaw", 0.0))
            else:
                yaw = _yaw(spec, rng, x, y, landmark_xy or (0.0, 0.0))
            actor = common.place_mesh(mesh, x, y, yaw, scale, ground=0.0, label=f"{mesh.get_name()} {placed + 1}",
                                      folder=f"Scenario/Props/{mesh.get_name()}",
                                      tags=("ScenarioKit.Prop", f"ScenarioKit.Radius={int(radius)}"),
                                      lift=-float(spec.get("sink_m", 0.0)) * 100.0)
            layout.taken.append((x, y, keep_radius or radius))
            placed += 1
            if light_spec and lights < LIGHT_LIMIT:
                _prop_light(actor, light_spec)
                lights += 1
                lit += 1
        total += placed
        entry = {"mesh": mesh.get_path_name().split(".")[0], "placed": placed, "asked": count, "area": area}
        if light_spec:
            entry["lights"] = lit
            if lit < placed:
                entry["light_note"] = f"lights stop at {LIGHT_LIMIT} per world"
        if placed < count:
            if area in ("path_sides", "on_path"):
                parts = [note] if note else []
                if blocked:
                    parts.append(f"{blocked} spots by the path were taken by other props, the landmark or the player start")
                entry["note"] = "; ".join(parts) or "not enough room along the path"
            else:
                entry["note"] = "not enough free room: lower height_m or spacing_m, or use a bigger size_m in build_world"
        entries[index] = entry
    report["props"] = entries

    if ground_material or ground_tint:
        _ground_material(ground, ground_material, ground_tile_m, ground_tint, report)
    if time_of_day:
        report["sun"] = looks.apply_time_of_day(time_of_day, mist)
        report["time_of_day"] = time_of_day
    elif mist is not None:
        looks.set_mist(mist)
    if mist is not None:
        report["mist"] = looks.current_mist()
    if sky:
        report["sun"] = looks.set_sky(sky)
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
    looks.recapture()
    if common.mesh_fixes:
        report["mesh_fixes"] = list(common.mesh_fixes)
        common.mesh_fixes.clear()
    if landmark_xy is not None:
        common.frame_view((landmark_xy[0], landmark_xy[1], 300.0), max(landmark_r * 4.0, half * 0.5), max(landmark_r * 1.2, 600.0))
    else:
        common.frame_view((0.0, 0.0, 200.0), half * 0.9, size_m * 12.0)
    common.save_level_if_kit()
    report["total_props"] = total
    report["prop_lights"] = len(common.tagged("ScenarioKit.PropLight"))
    report["next"] = "screenshot_viewport (pass camera to look from anywhere), then make_game, make_film, or play_level."
    return report


def _play_status():
    """During Play: the player character. Unreal's editor scripting reads no level actors while playing."""
    status = {"playing": True, "note": "Stop Play (play_level stop) to see what the kit made in the level."}
    game = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()
    pawn = unreal.GameplayStatics.get_player_pawn(game, 0) if game else None
    if pawn is not None:
        status["pawn"] = pawn.get_class().get_name()
        status["pawn_at_m"] = common.summary_vec(pawn.get_actor_location())
    else:
        status["pawn"] = None
        status["pawn_note"] = ("No player character yet. Check again in a few seconds; if it stays empty, the game mode "
                               "has no default pawn or the player start is blocked.")
    return status


def kit_status():
    """What the kit has made in the open level, or the player character during Play."""
    if common.playing():
        return _play_status()
    level = common.level_path()
    status = {"level": level, "kit_world": bool(common.tagged("ScenarioKit.Ground"))}
    if status["kit_world"]:
        status["size_m"] = float(common.tag_value(common.tagged("ScenarioKit.Ground")[0], "ScenarioKit.Size", "0"))
    status["props"] = len(common.tagged("ScenarioKit.Prop"))
    status["prop_lights"] = len(common.tagged("ScenarioKit.PropLight"))
    status["landmark"] = [a.get_actor_label() for a in common.tagged("ScenarioKit.Landmark")]
    status["path"] = bool(common.tagged("ScenarioKit.Path"))
    status["sky"] = len(looks.scenario_skies()) > 0
    status["time_of_day"] = looks.current_time_of_day()
    status["mist"] = looks.current_mist()
    status["pickups"] = len(common.all_of(unreal.ScenarioPickup)) if hasattr(unreal, "ScenarioPickup") else 0
    status["goal"] = len(common.all_of(unreal.ScenarioGoal)) if hasattr(unreal, "ScenarioGoal") else 0
    status["game_rules"] = bool(common.all_of(unreal.ScenarioGameRules)) if hasattr(unreal, "ScenarioGameRules") else False
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    status["films"] = sorted(str(d.package_name) for d in registry.get_assets_by_path(unreal.Name(f"{common.ROOT}/Films"), recursive=True)
                             if str(d.asset_class_path.asset_name) == "LevelSequence")
    status["playing"] = False
    return status


def play_level(action="start"):
    """Starts or stops Play in the editor, so the person can walk the level."""
    if action == "stop":
        common.levels().editor_request_end_play()
        return {"playing": False}
    if common.playing():
        return {"playing": True, "note": "already playing"}
    common.levels().editor_request_begin_play()
    return {"playing": True,
            "note": "Play starts on the next frame. Call kit_status after a few seconds: it reports the player character "
                    "(pawn, pawn_at_m) once it has spawned. The person clicks in the viewport to control it; Esc stops."}
