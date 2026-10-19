# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
"""Light, sky and surfaces: time-of-day presets, the Scenario sky sphere and
world-aligned copies of Scenario materials (no stretching on scaled meshes)."""
import unreal

from . import common

# Sun angle, light in lux, sky light, fog density and exposure bias per time of day.
# Tuned from the engine's Basic level (6 lux, real-time sky light, fog 0.044).
PRESETS = {
    "day": dict(pitch=-48, yaw=-25, lux=6.0, color=(1.0, 0.97, 0.92), sky=1.0, fog=0.03, clouds=True, bias=0.4),
    "morning": dict(pitch=-20, yaw=55, lux=5.0, color=(1.0, 0.9, 0.78), sky=1.0, fog=0.05, clouds=True, bias=0.8),
    "golden": dict(pitch=-9, yaw=105, lux=5.0, color=(1.0, 0.78, 0.55), sky=1.1, fog=0.045, clouds=True, bias=1.3),
    "dusk": dict(pitch=-3, yaw=120, lux=3.0, color=(1.0, 0.6, 0.45), sky=1.4, fog=0.05, clouds=True, bias=1.8),
    "night": dict(pitch=-40, yaw=200, lux=0.35, color=(0.6, 0.72, 1.0), sky=0.5, fog=0.04, clouds=False, bias=2.4, atmosphere=False),
    "overcast": dict(pitch=-62, yaw=10, lux=2.5, color=(0.92, 0.95, 1.0), sky=1.5, fog=0.07, clouds=True, bias=1.0),
}

SKY_SPHERE = "/Engine/BasicShapes/Sphere"
SKY_SCALE = 2000.0  # the basic sphere has a 50 cm radius: the sky sits 1 km away
WORLD_MASTER = "M_ScenarioKit_WorldAligned"
WAT = "/Engine/Functions/Engine_MaterialFunctions01/Texturing/WorldAlignedTexture"
WAN = "/Engine/Functions/Engine_MaterialFunctions01/Texturing/WorldAlignedNormal"


def apply_time_of_day(name):
    if name not in PRESETS:
        raise common.KitError(f"time_of_day must be one of {', '.join(PRESETS)}.")
    p = PRESETS[name]
    sun = common.first_of(unreal.DirectionalLight)
    if sun:
        sun.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=float(p["pitch"]), yaw=float(p["yaw"])), False)
        light = sun.light_component
        light.set_intensity(p["lux"])
        light.set_light_color(unreal.LinearColor(*p["color"], 1.0))
        light.set_editor_property("atmosphere_sun_light", p.get("atmosphere", True))
    sky_light = common.first_of(unreal.SkyLight)
    if sky_light:
        sky_light.light_component.set_intensity(p["sky"])
    fog = common.first_of(unreal.ExponentialHeightFog)
    if fog:
        fog.component.set_fog_density(p["fog"])
    if not p["clouds"]:
        common.remove(common.all_of(unreal.VolumetricCloud))
    post = _post_volume()
    settings = post.settings
    settings.set_editor_property("override_auto_exposure_bias", True)
    settings.set_editor_property("auto_exposure_bias", p["bias"])
    post.set_editor_property("settings", settings)


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
    """Puts the Scenario sky around the level and lights the level from it."""
    material = sky_material(path)
    common.remove(common.tagged("ScenarioKit.Sky"))
    sphere = unreal.load_asset(SKY_SPHERE)
    sky = common.spawn(sphere, (0.0, 0.0, 0.0), yaw, label="Scenario Sky", folder="Scenario/Lighting", tags=("ScenarioKit.Sky",))
    sky.set_actor_scale3d(unreal.Vector(SKY_SCALE, SKY_SCALE, SKY_SCALE))
    component = sky.static_mesh_component
    component.set_material(0, material)
    component.set_cast_shadow(False)
    component.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
    _hide_engine_skies(keep=sky)
    light_from_sky()
    return sky


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
    name = f"{source.get_name()}_World{int(round(tile_m * 100))}"
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
