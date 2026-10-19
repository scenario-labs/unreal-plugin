# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
"""Games: a collect-and-reach game on any level (Scenario Pickup, Goal and Game Rules
actors), and a copy of an Epic template level dressed with Scenario assets."""
import math
import random

import unreal

from . import common, looks, world

TEMPLATES = {
    "third_person": "/Game/ThirdPerson/Lvl_ThirdPerson",
    "platforming": "/Game/Variant_Platforming/Lvl_Platforming",
    "combat": "/Game/Variant_Combat/Lvl_Combat",
    "first_person": "/Game/FirstPerson/Lvl_FirstPerson",
    "shooter": "/Game/Variant_Shooter/Lvl_Shooter",
    "horror": "/Game/Variant_Horror/Lvl_Horror",
}
SWAP_ALIASES = {
    "boxes": "/Game/LevelPrototyping/Meshes/SM_ChamferCube",
    "cubes": "/Game/LevelPrototyping/Meshes/SM_Cube",
    "cylinders": "/Game/LevelPrototyping/Meshes/SM_Cylinder",
}
GAME_TAG = "ScenarioKit.Game"


def _require_gameplay():
    if not hasattr(unreal, "ScenarioPickup"):
        raise common.KitError("The Scenario Gameplay module is not loaded: enable the Scenario plugin's runtime module and restart the editor.")


def _blockers():
    """2D boxes (min x, min y, max x, max y) of everything a pickup or prop should not sit inside."""
    boxes = []
    for actor in common.actors().get_all_level_actors():
        if isinstance(actor, (unreal.PlayerStart, unreal.Light, unreal.SkyLight, unreal.ExponentialHeightFog, unreal.PostProcessVolume,
                              unreal.SkyAtmosphere, unreal.AmbientSound, unreal.WorldSettings)):
            continue
        if common.has_tag(actor, "ScenarioKit.Ground") or common.has_tag(actor, "ScenarioKit.Wall") or common.has_tag(actor, "ScenarioKit.Sky"):
            continue
        origin, extent = actor.get_actor_bounds(True)
        if extent.x <= 0 or extent.x > 3000 or extent.y > 3000:
            continue  # nothing to collide with, or a floor
        boxes.append((origin.x - extent.x, origin.y - extent.y, origin.x + extent.x, origin.y + extent.y, origin.z + extent.z))
    return boxes


def _inside(boxes, x, y, margin):
    return any(b[0] - margin <= x <= b[2] + margin and b[1] - margin <= y <= b[3] + margin for b in boxes)


def _floor(x, y):
    """(z, actor) of a floor the player can stand on at (x, y), or None (walls, lava, water, nothing)."""
    hit = common.trace_down(x, y)
    if not hit or hit[1].z < 0.85:
        return None
    name = hit[2].get_class().get_name() if hit[2] else ""
    if "Lava" in name or "Kill" in name or "Water" in name:
        return None
    return hit[0].z, hit[2]


def _room(x, y, z, clearance, height):
    """True when the floor is flat for clearance cm around (x, y) and nothing is above it for height cm."""
    for dx, dy in ((clearance, 0), (-clearance, 0), (0, clearance), (0, -clearance)):
        floor = _floor(x + dx, y + dy)
        if floor is None or abs(floor[0] - z) > 30.0:
            return False
    return common.trace((x, y, z + 20.0), (x, y, z + height)) is None


def _walkable_spots(rng, count, clearance=150.0, height=250.0, tries=3000):
    """Spread-out spots on floors the player can reach. Kit worlds keep clear of the props;
    other levels (templates) are probed with traces: flat floor around, free room above."""
    grounds = common.tagged("ScenarioKit.Ground")
    boxes = _blockers() if grounds else []
    if grounds:
        half = float(common.tag_value(grounds[0], "ScenarioKit.Size", "120")) * 50.0 * 0.9
        lo, hi = (-half, -half), (half, half)
    else:
        xs, ys = [], []
        for actor in common.all_of(unreal.StaticMeshActor):
            origin, extent = actor.get_actor_bounds(False)
            if extent.x < 100000:
                xs += [origin.x - extent.x, origin.x + extent.x]
                ys += [origin.y - extent.y, origin.y + extent.y]
        if not xs:
            raise common.KitError("No floor found in the open level.")
        lo, hi = (min(xs), min(ys)), (max(xs), max(ys))
    start = common.first_of(unreal.PlayerStart)
    start_xy = (start.get_actor_location().x, start.get_actor_location().y) if start else (0.0, 0.0)
    spots = []
    for _ in range(tries):
        x, y = rng.uniform(lo[0], hi[0]), rng.uniform(lo[1], hi[1])
        if math.hypot(x - start_xy[0], y - start_xy[1]) < 500.0:
            continue
        if grounds and _inside(boxes, x, y, clearance):
            continue
        floor = _floor(x, y)
        if floor is None:
            continue
        if not grounds and not _room(x, y, floor[0], clearance, height):
            continue
        spots.append((x, y, floor[0]))
        if len(spots) >= max(count * 12, 60):
            break
    if not spots:
        raise common.KitError("No free floor found in the open level.")
    # Farthest-point picks spread the spots over the whole level.
    chosen = [max(spots, key=lambda s: math.hypot(s[0] - start_xy[0], s[1] - start_xy[1]) * rng.uniform(0.7, 1.0))]
    while len(chosen) < min(count, len(spots)):
        chosen.append(max(spots, key=lambda s: min(math.hypot(s[0] - c[0], s[1] - c[1]) for c in chosen)))
    return chosen, start_xy


def _set_look(component, mesh, height_m):
    scale = common.fit_scale(mesh, height_m)
    lo, hi = common.mesh_box(mesh)
    component.set_static_mesh(mesh)
    component.set_relative_scale3d(unreal.Vector(scale, scale, scale))
    component.set_relative_location(unreal.Vector(-(lo.x + hi.x) * 0.5 * scale, -(lo.y + hi.y) * 0.5 * scale, -(lo.z + hi.z) * 0.5 * scale), False, False)
    return scale, lo, hi


def make_game(title, pickups, goal=None, time_limit_s=None, music=None, music_volume=0.6, win_sound=None,
              lose_sound=None, objective=None, accent=(1.0, 0.78, 0.3), seed=11, replace=True):
    """Turns the open level into a game: pickups to collect, an optional goal, a timer, music and a HUD."""
    _require_gameplay()
    if replace:
        common.remove(common.tagged(GAME_TAG))
    rng = random.Random(seed)
    mesh = common.load_mesh(pickups.get("mesh"), "pickups.mesh")
    count = max(1, min(int(pickups.get("count", 8)), 40))
    height_m = float(pickups.get("height_m", 0.7))
    hover = float(pickups.get("hover_m", 1.0)) * 100.0
    sound = common.load_sound(pickups["sound"], "pickups.sound") if pickups.get("sound") else None
    spots, start_xy = _walkable_spots(rng, count)
    color = unreal.LinearColor(*accent, 1.0)
    for i, (x, y, z) in enumerate(spots):
        pickup = common.spawn(unreal.ScenarioPickup, (x, y, z + hover), rng.uniform(0, 360), label=f"Pickup {i + 1}",
                              folder="Scenario/Game", tags=(GAME_TAG,))
        _set_look(pickup.get_editor_property("mesh"), mesh, height_m)
        pickup.get_editor_property("glow").set_light_color(color)
        if sound:
            pickup.set_editor_property("pickup_sound", sound)

    report = {"level": common.level_path(), "pickups": len(spots), "pickup_mesh": common.package(pickups.get("mesh"))}
    if len(spots) < count:
        report["warning"] = f"Only {len(spots)} free spots for {count} pickups."

    if goal is not None:
        goal_xy = None
        landmarks = common.tagged("ScenarioKit.Landmark")
        if goal.get("at_m"):
            goal_xy = (float(goal["at_m"][0]) * 100.0, float(goal["at_m"][1]) * 100.0)
        elif landmarks:
            loc = landmarks[0].get_actor_location()
            radius = float(common.tag_value(landmarks[0], "ScenarioKit.Radius", "300"))
            angle = math.atan2(start_xy[1] - loc.y, start_xy[0] - loc.x)
            goal_xy = (loc.x + math.cos(angle) * (radius + 350.0), loc.y + math.sin(angle) * (radius + 350.0))
        else:
            far, _ = _walkable_spots(random.Random(seed + 1), 1)
            goal_xy = (far[0][0], far[0][1])
        gz = common.ground_z(goal_xy[0], goal_xy[1])
        goal_actor = common.spawn(unreal.ScenarioGoal, (goal_xy[0], goal_xy[1], gz + 150.0), label="Goal", folder="Scenario/Game", tags=(GAME_TAG,))
        if goal.get("mesh"):
            goal_mesh = common.load_mesh(goal["mesh"], "goal.mesh")
            g_height = float(goal.get("height_m", 3.0))
            scale, lo, hi = _set_look(goal_actor.get_editor_property("mesh"), goal_mesh, g_height)
            component = goal_actor.get_editor_property("mesh")
            # Rest the mesh on the ground: the actor sits 1.5 m up.
            component.set_relative_location(unreal.Vector(-(lo.x + hi.x) * 0.5 * scale, -(lo.y + hi.y) * 0.5 * scale, -lo.z * scale - 150.0), False, False)
            half_x = max(150.0, (hi.x - lo.x) * 0.5 * scale + 50.0)
            half_y = max(150.0, (hi.y - lo.y) * 0.5 * scale + 50.0)
            goal_actor.get_editor_property("trigger").set_box_extent(unreal.Vector(half_x, half_y, 150.0), True)
        goal_actor.get_editor_property("glow").set_light_color(color)
        goal_actor.set_editor_property("needs_all_pickups", bool(goal.get("needs_all_pickups", True)))
        report["goal_m"] = [round(goal_xy[0] / 100.0, 1), round(goal_xy[1] / 100.0, 1)]

    rules = common.spawn(unreal.ScenarioGameRules, (start_xy[0], start_xy[1], 300.0), label="Game Rules", folder="Scenario/Game", tags=(GAME_TAG,))
    limit = float(time_limit_s) if time_limit_s is not None else max(60.0, 25.0 * len(spots) + 30.0)
    rules.set_editor_property("title", title)
    rules.set_editor_property("pickup_name", pickups.get("name", "pickups"))
    if objective:
        rules.set_editor_property("objective", objective)
    rules.set_editor_property("time_limit_seconds", limit)
    rules.set_editor_property("accent_color", color)
    rules.set_editor_property("music_volume", float(music_volume))
    for prop, value, what in (("music", music, "music"), ("win_sound", win_sound, "win_sound"), ("lose_sound", lose_sound, "lose_sound")):
        if value:
            rules.set_editor_property(prop, common.load_sound(value, what))
    report.update({"title": title, "time_limit_s": limit, "music": common.package(music) if music else None})
    common.save_level_if_kit()
    report["next"] = "play_level to try it: walk into each pickup, then the goal."
    return report


def _template_path(template):
    path = TEMPLATES.get(template, common.package(template))
    if not common.assets().does_asset_exist(path):
        have = [k for k, v in TEMPLATES.items() if common.assets().does_asset_exist(v)]
        raise common.KitError(f"No template level {template}. This project has: {', '.join(have) or 'none'}.")
    return path


def _swap_meshes(target_path, mesh):
    count = 0
    lo2, hi2 = common.mesh_box(mesh)
    new_max = max(hi2.x - lo2.x, hi2.y - lo2.y, hi2.z - lo2.z, 1.0)
    for actor in common.actors().get_all_level_actors():
        for component in actor.get_components_by_class(unreal.StaticMeshComponent):
            current = component.static_mesh
            if not current or current.get_path_name().split(".")[0] != target_path:
                continue
            lo, hi = common.mesh_box(current)
            old_scale = component.get_editor_property("relative_scale3d")
            old_max = max((hi.x - lo.x) * old_scale.x, (hi.y - lo.y) * old_scale.y, (hi.z - lo.z) * old_scale.z)
            scale = old_max / new_max
            old_center = unreal.Vector((lo.x + hi.x) * 0.5 * old_scale.x, (lo.y + hi.y) * 0.5 * old_scale.y, lo.z * old_scale.z)
            new_center = unreal.Vector((lo2.x + hi2.x) * 0.5 * scale, (lo2.y + hi2.y) * 0.5 * scale, lo2.z * scale)
            component.set_static_mesh(mesh)
            component.set_editor_property("override_materials", [])
            component.set_relative_scale3d(unreal.Vector(scale, scale, scale))
            delta = (old_center - new_center).rotate(component.get_world_rotation())
            component.set_world_location(component.get_world_location() + delta, False, False)
            count += 1
    return count


def dress_template(name, template="platforming", floor_material=None, block_material=None, tile_m=4.0, sky=None,
                   time_of_day=None, music=None, music_volume=0.5, swaps=(), props=(), seed=5):
    """Copies an Epic template level and dresses the copy with Scenario materials, sky, music and props.
    The template's own gameplay (character, enemies, platforms) stays as it is."""
    source = _template_path(template)
    world._check_open_level()
    path, final = common.free_level_path(f"{common.ROOT}/Games", common.slug(name, "Game"))
    if not common.levels().new_level_from_template(path, source):
        raise common.KitError(f"Unreal could not copy {source} to {path}.")
    report = {"level": path, "template": source, "materials_replaced": 0, "swapped": []}

    floor = looks.world_material(floor_material, tile_m) if floor_material else None
    blocks = looks.world_material(block_material, tile_m) if block_material else None
    if floor or blocks:
        for actor in common.actors().get_all_level_actors():
            for component in actor.get_components_by_class(unreal.StaticMeshComponent):
                for slot in range(component.get_num_materials()):
                    current = component.get_material(slot)
                    name_now = current.get_name() if current else ""
                    replacement = floor if "PrototypeGrid" in name_now or "Template_Map_Floor" in name_now else blocks if "ColWay" in name_now else None
                    if replacement:
                        component.set_material(slot, replacement)
                        report["materials_replaced"] += 1

    for index, swap in enumerate(swaps or ()):
        target = SWAP_ALIASES.get(swap.get("replace"), common.package(swap.get("replace")))
        mesh = common.load_mesh(swap.get("mesh"), f"swaps[{index}].mesh")
        report["swapped"].append({"replace": target, "with": mesh.get_path_name().split(".")[0], "count": _swap_meshes(target, mesh)})

    if time_of_day:
        looks.apply_time_of_day(time_of_day)
    if sky:
        looks.set_sky(sky)
    if music:
        sound = common.load_sound(music, "music")
        if isinstance(sound, unreal.SoundWave) and not sound.get_editor_property("looping"):
            sound.set_editor_property("looping", True)
            common.assets().save_loaded_asset(sound)
        player = common.spawn(unreal.AmbientSound, (0.0, 0.0, 300.0), label="Scenario Music", folder="Scenario", tags=("ScenarioKit.Ambient",))
        player.audio_component.set_sound(sound)
        player.audio_component.set_editor_property("volume_multiplier", float(music_volume))
        player.audio_component.set_editor_property("allow_spatialization", False)

    rng = random.Random(seed)
    placed_props = []
    for index, spec in enumerate(props or ()):
        mesh = common.load_mesh(spec.get("mesh"), f"props[{index}].mesh")
        count = max(0, min(int(spec.get("count", 6)), 60))
        scale = common.fit_scale(mesh, world._height(spec, rng))
        radius = common.footprint(mesh, scale)
        spots, _ = _walkable_spots(rng, count, clearance=radius + 100.0, height=max(250.0, radius * 2.0))
        placed = 0
        boxes = []
        for x, y, z in spots:
            if placed >= count:
                break
            if _inside(boxes, x, y, radius + 100.0):
                continue
            actor = common.place_mesh(mesh, x, y, rng.uniform(0, 360), scale, ground=z, label=f"{mesh.get_name()} {placed + 1}",
                                      folder=f"Scenario/Props/{mesh.get_name()}", tags=("ScenarioKit.Prop", f"ScenarioKit.Radius={int(radius)}"))
            origin, extent = actor.get_actor_bounds(True)
            boxes.append((origin.x - extent.x, origin.y - extent.y, origin.x + extent.x, origin.y + extent.y, origin.z + extent.z))
            placed += 1
        placed_props.append({"mesh": mesh.get_path_name().split(".")[0], "placed": placed, "asked": count})
    report["props"] = placed_props
    if common.mesh_fixes:
        report["mesh_fixes"] = list(common.mesh_fixes)
        common.mesh_fixes.clear()
    mode = common.world().get_world_settings().get_editor_property("default_game_mode")
    report["game_mode"] = mode.get_name() if mode else "project default"
    common.save_level()
    report["next"] = "play_level to try it, or make_game to add pickups and a goal on top."
    return report
