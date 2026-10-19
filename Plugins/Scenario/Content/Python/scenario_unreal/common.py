# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
"""Helpers shared by the kit: asset lookup, spawning, mesh fitting, ground traces."""
import math
import re

import unreal

ROOT = "/Game/Scenario"
TAG = "ScenarioKit"


class KitError(Exception):
    """A problem the caller can fix (a missing asset, the wrong level). The message says how."""


def actors():
    return unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def levels():
    return unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)


def assets():
    return unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)


def world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()


def level_path():
    current = world()
    return current.get_path_name().split(".")[0] if current else ""


def slug(text, fallback="Untitled"):
    """'misty bamboo temple' -> 'MistyBambooTemple' (asset and folder names)."""
    words = re.findall(r"[A-Za-z0-9]+", text or "")
    name = "".join(w[:1].upper() + w[1:] for w in words)[:40]
    if name[:1].isdigit():
        name = "S" + name
    return name or fallback


def package(path):
    """'/Game/A/B.B', "StaticMesh'/Game/A/B.B'" or '/Game/A/B' -> '/Game/A/B'."""
    if not path:
        return ""
    text = str(path).strip()
    quoted = re.search(r"'(/[^']+)'", text)
    if quoted:
        text = quoted.group(1)
    return text.split(".")[0].rstrip("/")


def free_level_path(folder, name):
    """A level path under folder/name that does not exist yet."""
    for n in range(1, 100):
        candidate = name if n == 1 else f"{name}{n}"
        path = f"{folder}/{candidate}/{candidate}"
        if not assets().does_asset_exist(path):
            return path, candidate
    raise KitError(f"Too many levels named {name} in {folder}.")


def _assets_in(folder, class_names):
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    found = []
    for data in registry.get_assets_by_path(unreal.Name(folder), recursive=True):
        if str(data.asset_class_path.asset_name) in class_names:
            found.append(data)
    found.sort(key=lambda d: str(d.package_name))
    return found


def load(path, class_names, what, folder_fallback=False):
    """Loads an asset of one of class_names.

    With folder_fallback, a folder or another asset from the same generation also works:
    meshes and materials are imported in one folder per generation, so the first mesh of
    that folder is the one meant.
    """
    p = package(path)
    if not p:
        raise KitError(f"{what}: no asset path given.")
    asset = unreal.load_asset(p) if assets().does_asset_exist(p) else None
    if asset is not None and asset.get_class().get_name() in class_names:
        return asset
    if folder_fallback:
        folders = [p] if assets().does_directory_exist(p) else []
        parent = p.rsplit("/", 1)[0]
        # A date folder holds many generations: never guess inside it.
        if asset is not None and not re.search(r"/\d{4}-\d{2}-\d{2}$", parent):
            folders.append(parent)
        for folder in folders:
            found = _assets_in(folder, class_names)
            if found:
                return found[0].get_asset()
    kind = asset.get_class().get_name() if asset is not None else "nothing"
    raise KitError(
        f"{what}: {p} is {kind}, expected {' or '.join(class_names)}. "
        "Use a path from created_assets in the apply_result_application result.")


def load_mesh(path, what="mesh"):
    return load(path, ("StaticMesh",), what, folder_fallback=True)


def load_material(path, what="material"):
    return load(path, ("MaterialInstanceConstant", "Material"), what, folder_fallback=True)


def load_sound(path, what="sound"):
    return load(path, ("SoundWave", "SoundCue", "MetaSoundSource"), what)


def spawn(what, location=(0.0, 0.0, 0.0), yaw=0.0, label=None, folder=None, tags=(), pitch=0.0, roll=0.0):
    """Places an actor from a class or an asset, tagged as made by the kit."""
    loc = unreal.Vector(*[float(v) for v in location])
    rot = unreal.Rotator(roll=float(roll), pitch=float(pitch), yaw=float(yaw))
    if isinstance(what, unreal.StaticMesh):
        # From the class, then the mesh: spawning from the asset needs a level viewport.
        actor = actors().spawn_actor_from_class(unreal.StaticMeshActor, loc, rot)
        if actor is not None:
            actor.static_mesh_component.set_static_mesh(what)
    elif isinstance(what, type):
        actor = actors().spawn_actor_from_class(what, loc, rot)
    else:
        actor = actors().spawn_actor_from_object(what, loc, rot)
    if actor is None:
        raise KitError(f"Could not place {label or what} in the level.")
    if label:
        actor.set_actor_label(label)
    if folder:
        actor.set_folder_path(folder)
    actor.set_editor_property("tags", list(actor.tags) + [unreal.Name(TAG)] + [unreal.Name(t) for t in tags])
    return actor


def has_tag(actor, tag):
    return any(str(t) == tag for t in actor.tags)


def tagged(tag=None):
    """Actors the kit placed, optionally with one more tag."""
    return [a for a in actors().get_all_level_actors() if has_tag(a, TAG) and (tag is None or has_tag(a, tag))]


def tag_value(actor, key, default=None):
    """Reads 'key=value' tags."""
    for t in actor.tags:
        text = str(t)
        if text.startswith(key + "="):
            return text[len(key) + 1:]
    return default


def remove(actor_list):
    if actor_list:
        actors().destroy_actors(actor_list)


def first_of(cls):
    for actor in actors().get_all_level_actors():
        if isinstance(actor, cls):
            return actor
    return None


def all_of(cls):
    return [a for a in actors().get_all_level_actors() if isinstance(a, cls)]


def mesh_box(mesh):
    box = mesh.get_bounding_box()
    return box.min, box.max


def fit_scale(mesh, height_m=None, size_m=None):
    """Uniform scale that gives the mesh this height (or this longest side), in meters."""
    lo, hi = mesh_box(mesh)
    ext = (hi.x - lo.x, hi.y - lo.y, hi.z - lo.z)
    if height_m:
        return float(height_m) * 100.0 / max(ext[2], 1.0)
    if size_m:
        return float(size_m) * 100.0 / max(max(ext), 1.0)
    return 1.0


def footprint(mesh, scale):
    """Radius of the mesh's ground footprint, in centimeters."""
    lo, hi = mesh_box(mesh)
    return max(hi.x - lo.x, hi.y - lo.y) * scale * 0.5


MASK_WORDS = ("metallic", "roughness", "occlusion", "orm", "specular")
COLOR_WORDS = ("normal", "basecolor", "albedo", "emissive", "diffuse")
mesh_fixes = []  # what ensure_mesh changed since the tool's report last read it
_checked = set()


def _shape_count(body):
    geometry = body.get_editor_property("agg_geom")
    count = 0
    for name in ("sphere_elems", "box_elems", "sphyl_elems", "convex_elems", "tapered_capsule_elems"):
        try:
            count += len(geometry.get_editor_property(name))
        except Exception:
            pass
    return count


def ensure_mesh(mesh):
    """Safety net for meshes imported before the plugin set them up: mask textures (metallic,
    roughness, occlusion) become linear, and a mesh with no collision gets its own triangles
    as collision, so the player walks against it and through its openings. Once per mesh."""
    path = mesh.get_path_name()
    if path in _checked or not path.startswith("/Game/"):
        return
    _checked.add(path)
    changed = []
    for slot in mesh.get_editor_property("static_materials"):
        material = slot.get_editor_property("material_interface")
        if not isinstance(material, unreal.MaterialInstance):
            continue
        for value in material.get_editor_property("texture_parameter_values"):
            name = str(value.get_editor_property("parameter_info").get_editor_property("name")).lower()
            texture = value.get_editor_property("parameter_value")
            if texture is None or any(w in name for w in COLOR_WORDS) or not any(w in name for w in MASK_WORDS):
                continue
            if texture.get_editor_property("srgb"):
                texture.set_editor_property("srgb", False)
                assets().save_loaded_asset(texture, False)
                changed.append(f"{texture.get_name()} set to linear")
    body = mesh.get_editor_property("body_setup")
    try:
        bare = body is not None and _shape_count(body) == 0 and \
            body.get_editor_property("collision_trace_flag") != unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE
    except Exception:
        bare = False
    if bare:
        body.set_editor_property("collision_trace_flag", unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE)
        mesh.set_editor_property("body_setup", body)
        mesh.modify()
        try:
            mesh.post_edit_change()
        except Exception:
            pass
        assets().save_loaded_asset(mesh, False)
        changed.append("collision from its triangles")
    if changed:
        mesh_fixes.append(f"{mesh.get_name()}: {', '.join(changed)}")


def place_mesh(mesh, x, y, yaw=0.0, scale=1.0, ground=None, label=None, folder=None, tags=(), lift=0.0):
    """Places a mesh centered on (x, y) with its lowest point on the ground."""
    ensure_mesh(mesh)
    lo, hi = mesh_box(mesh)
    cx, cy = (lo.x + hi.x) * 0.5 * scale, (lo.y + hi.y) * 0.5 * scale
    r = math.radians(yaw)
    ox, oy = cx * math.cos(r) - cy * math.sin(r), cx * math.sin(r) + cy * math.cos(r)
    base = ground_z(x, y) if ground is None else ground
    z = base - lo.z * scale + lift
    actor = spawn(mesh, (x - ox, y - oy, z), yaw, label, folder, tags)
    actor.set_actor_scale3d(unreal.Vector(scale, scale, scale))
    return actor


def trace(start, end, ignore=()):
    """(point, normal, actor) of the first surface between two points (cm), or None."""
    hit = unreal.SystemLibrary.line_trace_single(
        world(), unreal.Vector(*start), unreal.Vector(*end),
        unreal.TraceTypeQuery.ECC_VISIBILITY, False, list(ignore), unreal.DrawDebugTrace.NONE, True)
    if hit is None:
        return None
    if isinstance(hit, tuple):
        hit = next((h for h in hit if isinstance(h, unreal.HitResult)), None)
        if hit is None:
            return None
    parts = hit.to_tuple()
    if not parts[0]:
        return None
    return parts[5], parts[7], parts[9]


def trace_down(x, y, top=50000.0, bottom=-50000.0, ignore=()):
    """(point, normal, actor) of the first surface under (x, y), or None."""
    return trace((x, y, top), (x, y, bottom), ignore)


def ground_z(x, y, ignore=()):
    hit = trace_down(x, y, ignore=ignore)
    return hit[0].z if hit else 0.0


def look_at(source, target):
    """(pitch, yaw) in degrees for a camera at source looking at target."""
    dx, dy, dz = target[0] - source[0], target[1] - source[1], target[2] - source[2]
    yaw = math.degrees(math.atan2(dy, dx))
    pitch = math.degrees(math.atan2(dz, math.hypot(dx, dy)))
    return pitch, yaw


def playing():
    """True while Play in the editor runs (safe without a level editor, unlike is_in_play_in_editor)."""
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world() is not None


def save_level():
    """Saves the open level without revision-control prompts."""
    path = level_path()
    if not unreal.EditorLoadingAndSavingUtils.save_map(world(), path):
        raise KitError(f"Unreal could not save {path}.")


def save_level_if_kit():
    """Saves the open level when the kit made it (under /Game/Scenario). Other levels stay unsaved."""
    if level_path().startswith(ROOT + "/"):
        save_level()
        return True
    return False


def save_assets(paths):
    for path in paths:
        if path:
            assets().save_asset(package(path), only_if_is_dirty=False)


def summary_vec(v):
    return [round(v.x / 100.0, 2), round(v.y / 100.0, 2), round(v.z / 100.0, 2)]


def frame_view(target, distance, height, from_xy=None):
    """Points the level viewport at target (cm) from distance away and height up, from the player start side."""
    import math as _m
    if from_xy is None:
        start = first_of(unreal.PlayerStart)
        from_xy = (start.get_actor_location().x, start.get_actor_location().y) if start else (target[0], target[1] - distance)
    angle = _m.atan2(from_xy[1] - target[1], from_xy[0] - target[0])
    eye = (target[0] + _m.cos(angle) * distance, target[1] + _m.sin(angle) * distance, target[2] + height)
    pitch, yaw = look_at(eye, target)
    try:
        unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).set_level_viewport_camera_info(
            unreal.Vector(*eye), unreal.Rotator(roll=0.0, pitch=pitch, yaw=yaw))
    except Exception:
        pass  # no level viewport (commandlet)
