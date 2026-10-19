# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
"""Light, sky and surfaces: time-of-day presets, the Scenario sky sphere and
world-aligned copies of Scenario materials (no stretching on scaled meshes)."""
import re

import unreal

from . import common

# Per time of day: sun angle (used when the sky shows no clear sun), sun in lux and its
# color temperature (or a color at night), sky light, fog density and colors (ambient and
# toward the sun), exposure bias, light shafts and the sun's disc size (softer shadows low down).
# Tuned from the engine's Basic level (6 lux, real-time sky light, fog 0.044).
PRESETS = {
    "day": dict(pitch=-48, yaw=-25, lux=6.5, temperature=5900, sky=1.0, fog=0.03, fog_color=(0.42, 0.52, 0.66),
                sun_fog=(0.8, 0.75, 0.62), clouds=True, bias=0.4, shafts=False, angle=0.6),
    "morning": dict(pitch=-20, yaw=55, lux=6.0, temperature=5000, sky=1.0, fog=0.05, fog_color=(0.5, 0.52, 0.58),
                    sun_fog=(0.95, 0.75, 0.5), clouds=True, bias=0.8, shafts=True, angle=1.0),
    "golden": dict(pitch=-9, yaw=105, lux=8.5, temperature=4300, sky=1.1, fog=0.045, fog_color=(0.52, 0.42, 0.34),
                   sun_fog=(1.0, 0.62, 0.32), clouds=True, bias=1.3, shafts=True, angle=1.5),
    "dusk": dict(pitch=-3, yaw=120, lux=4.0, temperature=3300, sky=1.4, fog=0.05, fog_color=(0.3, 0.24, 0.3),
                 sun_fog=(0.9, 0.42, 0.25), clouds=True, bias=1.8, shafts=True, angle=2.0),
    "night": dict(pitch=-40, yaw=200, lux=0.35, color=(0.6, 0.72, 1.0), sky=0.5, fog=0.04, fog_color=(0.02, 0.03, 0.06),
                  sun_fog=(0.04, 0.05, 0.09), clouds=False, bias=2.4, atmosphere=False, shafts=False, angle=0.5),
    "overcast": dict(pitch=-62, yaw=10, lux=2.5, temperature=6800, sky=1.5, fog=0.07, fog_color=(0.55, 0.57, 0.6),
                     sun_fog=(0.2, 0.2, 0.2), clouds=True, bias=1.0, shafts=False, angle=3.0),
}

SKY_SPHERE = "/Engine/BasicShapes/Sphere"
SKY_SCALE = 2000.0  # the basic sphere has a 50 cm radius: the sky sits 1 km away
WORLD_MASTER = "M_ScenarioKit_WorldAligned"
WAT = "/Engine/Functions/Engine_MaterialFunctions01/Texturing/WorldAlignedTexture"
WAN = "/Engine/Functions/Engine_MaterialFunctions01/Texturing/WorldAlignedNormal"
SUN_CLEAR = 3.0  # panorama sun brightness over the sky's median above which the sky's sun leads
_panorama_suns = {}


def _set(obj, name, value):
    """Sets a property that some engine versions name or expose differently; False when it is not there."""
    try:
        obj.set_editor_property(name, value)
        return True
    except Exception:
        return False


def _tag(actor, key, value):
    tags = [t for t in actor.tags if not str(t).startswith(key + "=")]
    actor.set_editor_property("tags", tags + [unreal.Name(f"{key}={value}")])


def current_time_of_day():
    sun = common.first_of(unreal.DirectionalLight)
    name = common.tag_value(sun, "ScenarioKit.TimeOfDay") if sun else None
    return name if name in PRESETS else None


def current_mist():
    fog = common.first_of(unreal.ExponentialHeightFog)
    try:
        return float(common.tag_value(fog, "ScenarioKit.Mist", "0")) if fog else 0.0
    except ValueError:
        return 0.0


def apply_time_of_day(name, mist=None):
    """Light, fog and exposure for a time of day. mist None keeps the level's mist. Returns where the sun came from."""
    if name not in PRESETS:
        raise common.KitError(f"time_of_day must be one of {', '.join(PRESETS)}.")
    p = PRESETS[name]
    sun = common.first_of(unreal.DirectionalLight)
    if sun:
        _tag(sun, "ScenarioKit.TimeOfDay", name)
        light = sun.light_component
        light.set_intensity(p["lux"])
        if p.get("temperature"):
            _set(light, "use_temperature", True)
            _set(light, "temperature", float(p["temperature"]))
            light.set_light_color(unreal.LinearColor(1.0, 1.0, 1.0, 1.0))
        else:
            _set(light, "use_temperature", False)
            light.set_light_color(unreal.LinearColor(*p["color"], 1.0))
        light.set_editor_property("atmosphere_sun_light", p.get("atmosphere", True))
        _set(light, "light_source_angle", float(p["angle"]))
        _set(light, "enable_light_shaft_occlusion", bool(p["shafts"]))
        _set(light, "enable_light_shaft_bloom", bool(p["shafts"]))
        if p["shafts"]:
            _set(light, "bloom_scale", 0.2)
            _set(light, "bloom_threshold", 4.0)
            _set(light, "occlusion_mask_darkness", 0.4)
    sky_light = common.first_of(unreal.SkyLight)
    if sky_light:
        sky_light.light_component.set_intensity(p["sky"])
    set_mist(current_mist() if mist is None else mist, name)
    if not p["clouds"]:
        common.remove(common.all_of(unreal.VolumetricCloud))
    post = _post_volume()
    settings = post.settings
    settings.set_editor_property("override_auto_exposure_bias", True)
    settings.set_editor_property("auto_exposure_bias", p["bias"])
    post.set_editor_property("settings", settings)
    sun_source = aim_sun(name)
    recapture()
    return sun_source


def set_mist(mist, time_of_day=None):
    """Height fog for the time of day, thickened by mist from 0 (clear air) to 1 (dense ground mist).
    Above 0 the fog is volumetric, so light shafts and lantern glow show in it."""
    fog = common.first_of(unreal.ExponentialHeightFog)
    if fog is None:
        fog = common.spawn(unreal.ExponentialHeightFog, (0.0, 0.0, 0.0), label="Scenario Fog", folder="Scenario/Lighting")
    m = max(0.0, min(float(mist or 0.0), 1.0))
    p = PRESETS[time_of_day or current_time_of_day() or "day"]
    _tag(fog, "ScenarioKit.Mist", round(m, 2))
    # Height fog is densest at the actor's height: keep it on a kit world's ground (z 0).
    if common.tagged("ScenarioKit.Ground"):
        location = fog.get_actor_location()
        fog.set_actor_location(unreal.Vector(location.x, location.y, 0.0), False, False)
    c = fog.component
    c.set_fog_density(p["fog"] + m * 0.095)
    c.set_fog_height_falloff(0.2 + m * 0.5)
    c.set_fog_max_opacity(1.0 - m * 0.1)
    c.set_start_distance(m * 250.0)
    ambient = unreal.LinearColor(*p["fog_color"], 1.0)
    toward_sun = unreal.LinearColor(*p["sun_fog"], 1.0)
    if not _set(c, "fog_inscattering_luminance", ambient):
        c.set_fog_inscattering_color(ambient)
    if not _set(c, "directional_inscattering_luminance", toward_sun):
        c.set_directional_inscattering_color(toward_sun)
    _set(c, "directional_inscattering_exponent", 6.0)
    c.set_volumetric_fog(m > 0.0)
    if m > 0.0:
        c.set_volumetric_fog_scattering_distribution(0.2 + m * 0.625)
        c.set_volumetric_fog_extinction_scale(1.0 + m * 2.5)
        c.set_volumetric_fog_distance(6000.0 + m * 2500.0)
    return m


def _panorama_texture():
    """The panorama of the Scenario sky in the level, or None."""
    for sky in common.tagged("ScenarioKit.Sky") + scenario_skies():
        material = sky.static_mesh_component.get_material(0)
        if material is None:
            continue
        if isinstance(material, unreal.MaterialInstance):
            try:
                texture = unreal.MaterialEditingLibrary.get_material_instance_texture_parameter_value(material, "Panorama")
            except Exception:
                texture = None
            if isinstance(texture, unreal.Texture2D):
                return texture
    return None


def panorama_sun(texture):
    """(azimuth, elevation, strength) of the sun in a panorama, or None."""
    key = texture.get_path_name()
    if key not in _panorama_suns:
        found = None
        finder = getattr(unreal, "ScenarioScriptLibrary", None)
        if finder is not None and hasattr(finder, "find_panorama_sun"):
            # (azimuth, elevation, strength), or None when the texture has no source image.
            result = finder.find_panorama_sun(texture)
            if result and len(result) == 3:
                found = tuple(float(v) for v in result)
        _panorama_suns[key] = found
    return _panorama_suns[key]


def aim_sun(time_of_day=None):
    """Points the sun from the sky's own sun when the panorama shows a clear one, else from the preset."""
    sun = common.first_of(unreal.DirectionalLight)
    if sun is None:
        return {"source": "none", "note": "The level has no directional light."}
    p = PRESETS[time_of_day or current_time_of_day() or "day"]
    pitch, yaw = float(p["pitch"]), float(p["yaw"])
    report = {"source": "time_of_day preset"}
    texture = _panorama_texture()
    found = panorama_sun(texture) if texture is not None else None
    if found is not None:
        azimuth, elevation, strength = found
        if strength >= SUN_CLEAR:
            # Light travels away from the sun: pitch down by its elevation, yaw opposite its azimuth.
            pitch, yaw = -max(4.0, min(elevation, 80.0)), azimuth + 180.0
            report = {"source": "sky panorama", "azimuth": round(azimuth, 1), "elevation": round(elevation, 1),
                      "strength": round(strength, 1)}
        else:
            report = {"source": "time_of_day preset", "note": f"the sky shows no clear sun (strength {strength:.1f}, needs {SUN_CLEAR})"}
    sun.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=pitch, yaw=yaw), False)
    report["pitch"], report["yaw"] = round(pitch, 1), round(yaw % 360.0, 1)
    return report


def recapture():
    """Updates the sky light from the current sky and sun."""
    sky_light = common.first_of(unreal.SkyLight)
    if sky_light is not None:
        sky_light.light_component.recapture_sky()


def _post_volume():
    for volume in common.all_of(unreal.PostProcessVolume):
        if volume.unbound:
            return volume
    volume = common.spawn(unreal.PostProcessVolume, label="Scenario Post Process", folder="Scenario/Lighting")
    volume.set_editor_property("unbound", True)
    return volume


def sky_material(path):
    """A sky material from a Scenario environment result: its material, or a panorama texture."""
    asset = unreal.load_asset(common.package(path)) if path else None
    if asset is None:
        raise common.KitError(f"sky: nothing at {path}. Pass the material or texture from the environment result.")
    if isinstance(asset, unreal.MaterialInterface):
        return asset
    if isinstance(asset, unreal.Texture2D):
        parent = unreal.load_asset(f"{common.ROOT}/_Shared/M_Scenario_SkySphere")
        if parent is None:
            raise common.KitError("sky: apply the environment result once with purpose environment, then pass its material.")
        name = "MI_Sky_" + asset.get_name()
        folder = common.package(path).rsplit("/", 1)[0]
        existing = f"{folder}/{name}"
        if common.assets().does_asset_exist(existing):
            return unreal.load_asset(existing)
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        mi = tools.create_asset(name, folder, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
        unreal.MaterialEditingLibrary.set_material_instance_parent(mi, parent)
        unreal.MaterialEditingLibrary.set_material_instance_texture_parameter_value(mi, "Panorama", asset)
        common.assets().save_loaded_asset(mi)
        return mi
    raise common.KitError(f"sky: {path} is a {asset.get_class().get_name()}, expected a material or a texture.")


def set_sky(path, yaw=0.0):
    """Puts the Scenario sky around the level, points the sun from it and lights the level from it.
    Returns where the sun came from. (The sky material maps by world direction: yaw does not turn it.)"""
    material = sky_material(path)
    common.remove(common.tagged("ScenarioKit.Sky"))
    sphere = unreal.load_asset(SKY_SPHERE)
    sky = common.spawn(sphere, (0.0, 0.0, 0.0), yaw, label="Scenario Sky", folder="Scenario/Lighting", tags=("ScenarioKit.Sky",))
    sky.set_actor_scale3d(unreal.Vector(SKY_SCALE, SKY_SCALE, SKY_SCALE))
    component = sky.static_mesh_component
    component.set_material(0, material)
    component.set_cast_shadow(False)
    # The player spawns inside the sphere: no collision at all, and nothing to step on.
    component.set_collision_profile_name("NoCollision")
    component.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
    _set(component, "can_character_step_up_on", unreal.CanBeCharacterBase.ECB_NO)
    _hide_engine_skies(keep=sky)
    sun_source = aim_sun()
    light_from_sky()
    return sun_source


def _hide_engine_skies(keep):
    for actor in common.all_of(unreal.StaticMeshActor):
        if actor == keep:
            continue
        mesh = actor.static_mesh_component.static_mesh
        name = mesh.get_name() if mesh else ""
        if name == "SM_SkySphere" or (name == "Sphere" and actor.get_actor_scale3d().x >= 500.0):
            actor.set_is_temporarily_hidden_in_editor(True)
            actor.set_actor_hidden_in_game(True)
    common.remove(common.all_of(unreal.VolumetricCloud))


def scenario_skies():
    """Sky spheres in the level placed by the kit or by the plugin's environment placement."""
    found = []
    for actor in common.all_of(unreal.StaticMeshActor):
        mesh = actor.static_mesh_component.static_mesh
        if mesh and mesh.get_name() == "Sphere" and actor.get_actor_scale3d().x >= 500.0 and not actor.is_hidden_ed():
            found.append(actor)
    return found


def light_from_sky():
    """The sky light captures the sky sphere (static capture: real-time capture ignores unlit spheres)."""
    sky_light = common.first_of(unreal.SkyLight)
    if sky_light is None:
        sky_light = common.spawn(unreal.SkyLight, (0.0, 0.0, 500.0), label="Sky Light", folder="Scenario/Lighting")
    component = sky_light.light_component
    component.set_real_time_capture(False)
    component.set_editor_property("source_type", unreal.SkyLightSourceType.SLS_CAPTURED_SCENE)
    component.set_editor_property("sky_distance_threshold", 50000.0)
    component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    component.recapture_sky()
    fog = common.first_of(unreal.ExponentialHeightFog)
    if fog:
        fog.component.set_editor_property("fog_cutoff_distance", 90000.0)


def _world_master():
    path = f"{common.ROOT}/Kit/{WORLD_MASTER}"
    if common.assets().does_asset_exist(path):
        return unreal.load_asset(path)
    mel = unreal.MaterialEditingLibrary
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(WORLD_MASTER, f"{common.ROOT}/Kit", unreal.Material, unreal.MaterialFactoryNew())

    def node(cls, x, y):
        return mel.create_material_expression(material, cls, x, y)

    size = node(unreal.MaterialExpressionVectorParameter, -1400, 200)
    size.set_editor_property("parameter_name", "TileSize")
    size.set_editor_property("default_value", unreal.LinearColor(400.0, 400.0, 400.0, 0.0))
    size3 = node(unreal.MaterialExpressionComponentMask, -1150, 200)
    for channel, on in (("r", True), ("g", True), ("b", True), ("a", False)):
        size3.set_editor_property(channel, on)
    mel.connect_material_expressions(size, "", size3, "")

    white = unreal.load_asset(f"{common.ROOT}/_Shared/T_Scenario_WhiteMask")
    maps = [
        ("BaseColor", unreal.load_asset("/Engine/EngineResources/DefaultTexture"), unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, WAT, 0),
        ("Normal", unreal.load_asset("/Engine/EngineMaterials/DefaultNormal"), unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL, WAN, 400),
    ]
    if white is not None:
        maps.append(("Roughness", white, unreal.MaterialSamplerType.SAMPLERTYPE_MASKS, WAT, 800))
    outputs = {}
    for name, texture, sampler, function, y in maps:
        param = node(unreal.MaterialExpressionTextureObjectParameter, -1150, y)
        param.set_editor_property("parameter_name", name)
        param.set_editor_property("texture", texture)
        param.set_editor_property("sampler_type", sampler)
        call = node(unreal.MaterialExpressionMaterialFunctionCall, -800, y)
        call.set_material_function(unreal.load_asset(function))
        mel.connect_material_expressions(param, "", call, "TextureObject")
        mel.connect_material_expressions(size3, "", call, "TextureSize")
        outputs[name] = call

    tint = node(unreal.MaterialExpressionVectorParameter, -800, -250)
    tint.set_editor_property("parameter_name", "Tint")
    tint.set_editor_property("default_value", unreal.LinearColor(1.0, 1.0, 1.0, 1.0))
    multiply = node(unreal.MaterialExpressionMultiply, -400, 0)
    mel.connect_material_expressions(outputs["BaseColor"], "XYZ Texture", multiply, "A")
    mel.connect_material_expressions(tint, "", multiply, "B")
    mel.connect_material_property(multiply, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(outputs["Normal"], "XYZ Texture", unreal.MaterialProperty.MP_NORMAL)
    if "Roughness" in outputs:
        mask = node(unreal.MaterialExpressionComponentMask, -400, 800)
        for channel, on in (("r", True), ("g", False), ("b", False), ("a", False)):
            mask.set_editor_property(channel, on)
        mel.connect_material_expressions(outputs["Roughness"], "XYZ Texture", mask, "")
        mel.connect_material_property(mask, "", unreal.MaterialProperty.MP_ROUGHNESS)
    else:
        rough = node(unreal.MaterialExpressionConstant, -400, 800)
        rough.set_editor_property("r", 0.8)
        mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.recompile_material(material)
    common.assets().save_loaded_asset(material)
    return material


def world_material(path, tile_m=4.0, tint=None):
    """A world-aligned copy of a Scenario PBR material: same maps, one tile every tile_m meters."""
    source = common.load_material(path)
    if isinstance(source, unreal.Material) or source.get_class().get_name() != "MaterialInstanceConstant":
        return source
    mel = unreal.MaterialEditingLibrary
    maps = {}
    for name in ("BaseColor", "Normal", "Roughness"):
        try:
            value = mel.get_material_instance_texture_parameter_value(source, name)
        except Exception:
            value = None
        if value is not None:
            maps[name] = value
    if "BaseColor" not in maps:
        # Not a Scenario PBR material: use it as it is.
        return source
    folder = source.get_path_name().rsplit("/", 1)[0]
    # A world-aligned copy can be the source too (a new tint for the ground): name from the original.
    base = re.sub(r"_World\d+(_[0-9A-F]{6})?$", "", source.get_name())
    name = f"{base}_World{int(round(tile_m * 100))}"
    if tint:
        name += "_" + "".join(f"{max(0, min(int(round(float(c) * 255)), 255)):02X}" for c in tint[:3])
    target = f"{folder}/{name}"
    if common.assets().does_asset_exist(target):
        mi = unreal.load_asset(target)
    else:
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        mi = tools.create_asset(name, folder, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
        mel.set_material_instance_parent(mi, _world_master())
    for key, value in maps.items():
        mel.set_material_instance_texture_parameter_value(mi, key, value)
    size = float(tile_m) * 100.0
    mel.set_material_instance_vector_parameter_value(mi, "TileSize", unreal.LinearColor(size, size, size, 0.0))
    if tint:
        mel.set_material_instance_vector_parameter_value(mi, "Tint", unreal.LinearColor(*tint, 1.0))
    common.assets().save_loaded_asset(mi)
    return mi
