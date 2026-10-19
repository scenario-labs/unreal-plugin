# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
"""Short films: cameras and shots in a Level Sequence with Scenario music, rendered with
Movie Render Queue and encoded to MP4 with ffmpeg when it is installed."""
import glob
import json
import math
import os
import random
import shutil
import subprocess
import sys
import time

import unreal

from . import common

SHOT_TYPES = ("aerial", "crane_down", "orbit", "walk", "detail", "rise")
AUTO_SHOTS = (("aerial", 5.0), ("crane_down", 4.0), ("orbit", 5.0), ("walk", 4.0), ("detail", 3.0), ("rise", 5.0))
FOCAL = {"aerial": 24.0, "crane_down": 28.0, "orbit": 35.0, "walk": 24.0, "detail": 50.0, "rise": 28.0}
RESOLUTIONS = {"720p": (1280, 720), "1080p": (1920, 1080)}

# Render state survives a reload of this module (the kit reloads in development).
RENDERS = getattr(sys, "_scenario_kit_renders", None)
if RENDERS is None:
    RENDERS = {}
    sys._scenario_kit_renders = RENDERS


def _require_sequencer():
    if not hasattr(unreal, "MovieSceneSequenceExtensions") or not hasattr(unreal, "LevelSequence"):
        raise common.KitError("Films need the Sequencer Scripting plugin: Edit > Plugins, enable it, restart the editor.")


def _subject(focus):
    """Center (cm), footprint radius, height and actor (or None) of what the film is about."""
    if isinstance(focus, (list, tuple)) and len(focus) >= 2:
        z = float(focus[2]) * 100.0 if len(focus) > 2 else 150.0
        return (float(focus[0]) * 100.0, float(focus[1]) * 100.0, z), 300.0, 300.0, None
    candidates = []
    if isinstance(focus, str) and focus:
        candidates = [a for a in common.actors().get_all_level_actors() if a.get_actor_label() == focus]
        if not candidates:
            raise common.KitError(f"focus: no actor labeled {focus} in the open level.")
    candidates = candidates or common.tagged("ScenarioKit.Landmark") or common.tagged("ScenarioKit.Game")
    if candidates:
        origin, extent = candidates[0].get_actor_bounds(False)
        return (origin.x, origin.y, origin.z), max(extent.x, extent.y, 200.0), max(extent.z * 2.0, 200.0), candidates[0]
    return (0.0, 0.0, 150.0), 300.0, 300.0, None


def _clear_angle(preferred, camera_at, target, ignore, samples=(0.0,)):
    """The angle nearest preferred from which the camera sees target at every sample point of its move.
    camera_at(angle, t) gives the camera position; foliage or a building in the way rules an angle out."""
    best, best_clear = preferred, -1
    for step in range(13):
        angle = preferred + (step + 1) // 2 * 0.35 * (1 if step % 2 else -1)
        clear = sum(1 for t in samples if common.trace(camera_at(angle, t), target, ignore) is None)
        if clear == len(samples):
            return angle
        if clear > best_clear:
            best, best_clear = angle, clear
    return best


def _half_size():
    grounds = common.tagged("ScenarioKit.Ground")
    if grounds:
        return float(common.tag_value(grounds[0], "ScenarioKit.Size", "120")) * 50.0
    xs = []
    for actor in common.all_of(unreal.StaticMeshActor):
        origin, extent = actor.get_actor_bounds(False)
        if extent.x < 100000:
            xs += [abs(origin.x) + extent.x, abs(origin.y) + extent.y]
    return max(xs) if xs else 5000.0


def _ease(t):
    return 0.5 * t + 0.5 * (t * t * (3.0 - 2.0 * t))


def _path(kind, subject, radius, height, half, front, rng, actor=None, focused=False):
    """Returns f(t) -> (camera position, look-at point) for t in 0..1, in centimeters.
    focused: the shot names its subject, so a detail shot frames it instead of a random prop."""
    sx, sy, sz = subject
    ground = sz - height * 0.5
    eye = (sx, sy, ground + height * 0.55)

    def around(angle, distance, z):
        return (sx + math.cos(angle) * distance, sy + math.sin(angle) * distance, z)

    if kind == "aerial":
        d = max(half * 0.8, radius * 6.0)
        return lambda t: (around(front - 0.7 + 0.35 * t, d, ground + d * (0.38 - 0.06 * t)), eye)
    if kind == "crane_down":
        return lambda t: (around(front + 0.45, radius * (3.6 - 1.4 * t), ground + height * (2.6 - 2.0 * t) + 160.0), eye)
    if kind == "orbit":
        # Far enough for a 35 mm lens to hold the whole subject; start where the arc sees it most.
        d, z = max(radius * 3.6, 900.0), ground + height * 0.45 + 150.0
        start = _clear_angle(front + 1.0, lambda a, t: around(a + 1.7 * t, d, z), eye, [actor] if actor else [],
                             samples=(0.0, 0.25, 0.5, 0.75, 1.0))
        return lambda t: (around(start + 1.7 * t, d, z), eye)
    if kind == "walk":
        start = common.first_of(unreal.PlayerStart)
        if start:
            p = start.get_actor_location()
            begin = (p.x, p.y)
        else:
            begin = (sx + math.cos(front) * half * 0.6, sy + math.sin(front) * half * 0.6)
        dist = math.hypot(begin[0] - sx, begin[1] - sy)
        angle = math.atan2(begin[1] - sy, begin[0] - sx)
        near = radius + 700.0
        far = min(max(dist, near + 1500.0), near + 4000.0)
        return lambda t: (around(angle + 0.05 * t, far + (near - far) * t, ground + 165.0), (sx, sy, ground + height * 0.5))
    if kind == "detail":
        props = [] if focused else (common.tagged("ScenarioKit.Prop") or common.tagged("ScenarioKit.Game"))
        if focused and actor is not None:
            props = [actor]
        if props:
            prop = rng.choice(props)
            origin, extent = prop.get_actor_bounds(False)
            r = max(extent.x, extent.y, 80.0)
            target = (origin.x, origin.y, origin.z)
            z = origin.z + extent.z * 0.4 + 40.0

            def camera_at(angle, t):
                return (origin.x + math.cos(angle) * r * (3.4 - 1.2 * t), origin.y + math.sin(angle) * r * (3.4 - 1.2 * t), z)

            # Seen from the side people arrive from when it can be; never through bamboo or a wall.
            facing = rng.uniform(0, 2 * math.pi)
            if focused:
                start = common.first_of(unreal.PlayerStart)
                p = start.get_actor_location() if start else None
                facing = math.atan2(p.y - origin.y, p.x - origin.x) if p else front
            a = _clear_angle(facing, camera_at, target, [prop], samples=(0.0, 0.5, 1.0))
            return lambda t: (camera_at(a, t), target)
        return lambda t: (around(front - 0.3, radius * (1.8 - 0.4 * t), ground + 120.0), (sx, sy, ground + 120.0))
    if kind == "rise":
        return lambda t: (around(front + 0.15 * t, radius * (1.8 + 2.6 * t), ground + height * (0.4 + 2.2 * t)), eye)
    raise common.KitError(f"Shot type must be one of {', '.join(SHOT_TYPES)}.")


def _channels(section):
    named = {}
    for channel in section.get_all_channels():
        named[str(channel.channel_name)] = channel
    order = ["Location.X", "Location.Y", "Location.Z", "Rotation.X", "Rotation.Y", "Rotation.Z"]
    if all(k in named for k in order):
        return [named[k] for k in order]
    return list(section.get_all_channels())[:6]


def _sequence_path(name):
    base = common.slug(name, "Film")
    for n in range(1, 100):
        candidate = base if n == 1 else f"{base}{n}"
        path = f"{common.ROOT}/Films/{candidate}/{candidate}"
        if not common.assets().does_asset_exist(path):
            return path, candidate
    raise common.KitError("Too many films with this name.")


def make_film(name, duration_s=24.0, shots=None, music=None, music_volume=0.9, sfx=(), focus=None, fps=24, seed=3):
    """Builds a Level Sequence: cameras flying through the open level, cuts, fades and music."""
    _require_sequencer()
    rng = random.Random(seed)
    duration_s = max(6.0, min(float(duration_s), 120.0))
    subject, radius, height, subject_actor = _subject(focus)
    half = _half_size()
    start = common.first_of(unreal.PlayerStart)
    front = math.atan2(start.get_actor_location().y - subject[1], start.get_actor_location().x - subject[0]) if start else -math.pi / 2

    plan = []
    if shots:
        for i, shot in enumerate(shots):
            kind = shot.get("type")
            if kind not in SHOT_TYPES:
                raise common.KitError(f"shots[{i}].type must be one of {', '.join(SHOT_TYPES)}.")
            plan.append((kind, float(shot.get("duration_s", 4.0)), shot.get("focus")))
    else:
        plan = [(kind, length, None) for kind, length in AUTO_SHOTS]
    total = sum(p[1] for p in plan)
    plan = [(kind, length * duration_s / total, shot_focus) for kind, length, shot_focus in plan]

    path, final = _sequence_path(name)
    folder = path.rsplit("/", 1)[0]
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    sequence = tools.create_asset(final, folder, unreal.LevelSequence, unreal.LevelSequenceFactoryNew())
    fps = int(fps)
    sequence.set_display_rate(unreal.FrameRate(fps, 1))
    end_frame = int(round(duration_s * fps))
    sequence.set_playback_start(0)
    sequence.set_playback_end(end_frame)

    cuts = sequence.add_track(unreal.MovieSceneCameraCutTrack)
    frame = 0
    shots_report = []
    for index, (kind, length, shot_focus) in enumerate(plan):
        frames = int(round(length * fps)) if index < len(plan) - 1 else end_frame - frame
        s_subject, s_radius, s_height, s_actor = _subject(shot_focus) if shot_focus else (subject, radius, height, subject_actor)
        move = _path(kind, s_subject, s_radius, s_height, half, front, rng, s_actor, bool(shot_focus))
        pos0, look0 = move(0.0)
        pitch0, yaw0 = common.look_at(pos0, look0)
        camera = common.spawn(unreal.CineCameraActor, pos0, yaw0, label=f"{final} {index + 1} {kind}",
                              folder=f"Scenario/Films/{final}", tags=(f"ScenarioKit.Film={final}",), pitch=pitch0)
        component = camera.get_cine_camera_component()
        component.set_editor_property("current_focal_length", FOCAL[kind])
        focus_settings = component.get_editor_property("focus_settings")
        if kind == "detail":
            focus_settings.set_editor_property("focus_method", unreal.CameraFocusMethod.MANUAL)
            focus_settings.set_editor_property("manual_focus_distance", math.dist(pos0, look0))
            component.set_editor_property("current_aperture", 2.8)
        else:
            focus_settings.set_editor_property("focus_method", unreal.CameraFocusMethod.DISABLE)
        component.set_editor_property("focus_settings", focus_settings)

        binding = sequence.add_possessable(camera)
        track = binding.add_track(unreal.MovieScene3DTransformTrack)
        section = track.add_section()
        section.set_range(frame, frame + frames)
        channels = _channels(section)
        keys = max(4, int(length * 2))
        previous_yaw = None
        for k in range(keys + 1):
            t = k / keys
            pos, look = move(_ease(t))
            pitch, yaw = common.look_at(pos, look)
            if previous_yaw is not None:
                while yaw - previous_yaw > 180.0:
                    yaw -= 360.0
                while yaw - previous_yaw < -180.0:
                    yaw += 360.0
            previous_yaw = yaw
            at = unreal.FrameNumber(frame + int(round(t * frames)))
            for channel, value in zip(channels, (pos[0], pos[1], pos[2], 0.0, pitch, yaw)):
                channel.add_key(at, value)

        cut = cuts.add_section()
        cut.set_range(frame, frame + frames)
        cut.set_camera_binding_id(sequence.get_binding_id(binding))
        shots_report.append({"shot": index + 1, "type": kind, "seconds": round(frames / fps, 1)})
        frame += frames

    fade = sequence.add_track(unreal.MovieSceneFadeTrack)
    fade_section = fade.add_section()
    fade_section.set_range(0, end_frame)
    fade_channel = fade_section.get_all_channels()[0]
    for at, value in ((0, 1.0), (int(fps * 0.8), 0.0), (end_frame - int(fps * 1.2), 0.0), (end_frame, 1.0)):
        fade_channel.add_key(unreal.FrameNumber(at), value)

    audio_report = []
    if music:
        sound = common.load_sound(music, "music")
        audio = sequence.add_track(unreal.MovieSceneAudioTrack)
        section = audio.add_section()
        section.set_sound(sound)
        section.set_range(0, end_frame)
        audio_report.append({"sound": common.package(music), "at_s": 0.0, "volume": float(music_volume)})
    for i, effect in enumerate(sfx or ()):
        sound = common.load_sound(effect.get("sound"), f"sfx[{i}]")
        at = int(float(effect.get("at_s", 0.0)) * fps)
        length = int(max(sound.get_editor_property("duration"), 0.5) * fps) if isinstance(sound, unreal.SoundWave) else fps * 3
        track = sequence.add_track(unreal.MovieSceneAudioTrack)
        section = track.add_section()
        section.set_sound(sound)
        section.set_range(at, min(at + length, end_frame))
        audio_report.append({"sound": common.package(effect.get("sound")), "at_s": at / fps, "volume": float(effect.get("volume", 1.0))})

    meta = {"level": common.level_path(), "duration_s": duration_s, "fps": fps, "audio": audio_report}
    _write_meta(final, meta)
    common.assets().save_loaded_asset(sequence)
    common.save_level_if_kit()
    return {"film": final, "sequence": path, "level": meta["level"], "duration_s": duration_s, "shots": shots_report,
            "audio": audio_report, "next": "render_film, then film_status until state is done."}


def _films_dir(name):
    saved = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir())
    return os.path.join(saved, "Scenario", "Films", name)


def _write_meta(name, meta):
    folder = _films_dir(name)
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "film.json"), "w", encoding="utf-8") as handle:
        json.dump(meta, handle, indent=1)


def _read_meta(name):
    try:
        with open(os.path.join(_films_dir(name), "film.json"), encoding="utf-8") as handle:
            return json.load(handle)
    except OSError:
        return None


def _film_name(film):
    if film:
        p = common.package(film)
        return p.rsplit("/", 1)[-1] if p.startswith("/") else common.slug(film, "Film")
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    films = [d for d in registry.get_assets_by_path(unreal.Name(f"{common.ROOT}/Films"), recursive=True)
             if str(d.asset_class_path.asset_name) == "LevelSequence"]
    if not films:
        raise common.KitError("No film yet: call make_film first.")
    newest = max(films, key=lambda d: os.path.getmtime(_films_dir(str(d.asset_name))) if os.path.isdir(_films_dir(str(d.asset_name))) else 0)
    return str(newest.asset_name)


def _ffmpeg():
    for candidate in (shutil.which("ffmpeg"), "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg",
                      "C:\\ffmpeg\\bin\\ffmpeg.exe"):
        if candidate and os.path.isfile(candidate):
            return candidate
    return None


def _sound_file(path, out_dir):
    sound = common.load_sound(path, "audio")
    data = sound.get_editor_property("asset_import_data")
    if data is not None:
        source = data.get_first_filename()
        if source and os.path.isfile(source):
            return source
    export = os.path.join(out_dir, "audio")
    os.makedirs(export, exist_ok=True)
    unreal.AssetToolsHelpers.get_asset_tools().export_assets([common.package(path)], export)
    found = glob.glob(os.path.join(export, "**", "*.[wW][aA][vV]"), recursive=True)
    return found[0] if found else None


def render_film(film=None, resolution="720p", quality="draft"):
    """Starts a Movie Render Queue render of the film in the editor. Returns at once; poll film_status."""
    _require_sequencer()
    if not hasattr(unreal, "MoviePipelineQueueSubsystem"):
        raise common.KitError("Rendering needs the Movie Render Queue plugin: Edit > Plugins, enable it, restart the editor.")
    name = _film_name(film)
    meta = _read_meta(name) or {}
    sequence_path = f"{common.ROOT}/Films/{name}/{name}"
    if not common.assets().does_asset_exist(sequence_path):
        raise common.KitError(f"No film {name}: call make_film first.")
    if meta.get("level") and meta["level"] != common.level_path():
        raise common.KitError(f"Film {name} was made in {meta['level']}; open that level first.")
    subsystem = unreal.get_editor_subsystem(unreal.MoviePipelineQueueSubsystem)
    if subsystem.is_rendering():
        raise common.KitError("A render is already running. Poll film_status until it is done.")
    if common.playing():
        common.levels().editor_request_end_play()
        raise common.KitError("Play was running and is stopping now. Call render_film again in a few seconds.")
    width, height = RESOLUTIONS.get(resolution, RESOLUTIONS["720p"])
    out_dir = os.path.join(_films_dir(name), time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(out_dir, exist_ok=True)

    queue = subsystem.get_queue()
    queue.delete_all_jobs()
    job = queue.allocate_new_job(unreal.MoviePipelineExecutorJob)
    job.set_editor_property("job_name", name)
    job.set_editor_property("sequence", unreal.SoftObjectPath(f"{sequence_path}.{name}"))
    level = common.level_path()
    job.set_editor_property("map", unreal.SoftObjectPath(f"{level}.{level.rsplit('/', 1)[-1]}"))
    config = job.get_configuration()
    output = config.find_or_add_setting_by_class(unreal.MoviePipelineOutputSetting)
    output.set_editor_property("output_directory", unreal.DirectoryPath(out_dir))
    output.set_editor_property("output_resolution", unreal.IntPoint(width, height))
    output.set_editor_property("file_name_format", "{sequence_name}.{frame_number}")
    output.set_editor_property("zero_pad_frame_numbers", 4)
    output.set_editor_property("override_existing_output", True)
    config.find_or_add_setting_by_class(unreal.MoviePipelineDeferredPassBase)
    config.find_or_add_setting_by_class(unreal.MoviePipelineImageSequenceOutput_JPG)
    aa = config.find_or_add_setting_by_class(unreal.MoviePipelineAntiAliasingSetting)
    aa.set_editor_property("spatial_sample_count", 1)
    aa.set_editor_property("temporal_sample_count", 4 if quality == "high" else 1)
    aa.set_editor_property("engine_warm_up_count", 48)
    aa.set_editor_property("use_camera_cut_for_warm_up", False)

    executor = unreal.MoviePipelinePIEExecutor()
    state = {"film": name, "state": "rendering", "output_dir": out_dir, "frames": int(round(meta.get("duration_s", 24) * meta.get("fps", 24))),
             "fps": meta.get("fps", 24), "duration_s": meta.get("duration_s", 24), "audio": meta.get("audio", []),
             "resolution": [width, height], "started": time.time(), "executor": executor}
    RENDERS[name] = state

    def finished(_executor, success):
        state["state"] = "encoding" if success else "failed"
        if not success:
            state["error"] = "Movie Render Queue reported a failure: see the Output Log (LogMovieRenderPipeline)."
        _save_status(state)
        if success:
            _encode(state)

    executor.on_executor_finished_delegate.add_callable(finished)
    subsystem.render_queue_with_executor_instance(executor)
    _save_status(state)
    return {"film": name, "state": "rendering", "frames": state["frames"], "resolution": [width, height], "output_dir": out_dir,
            "note": "A render window opens in the editor; editing waits until it ends.", "next": "film_status every 20 seconds."}


def _save_status(state):
    public = {k: v for k, v in state.items() if k not in ("executor", "process")}
    with open(os.path.join(state["output_dir"], "status.json"), "w", encoding="utf-8") as handle:
        json.dump(public, handle, indent=1)


def _encode(state):
    frames = sorted(glob.glob(os.path.join(state["output_dir"], "*.jpeg")) + glob.glob(os.path.join(state["output_dir"], "*.jpg")))
    if not frames:
        state["state"] = "failed"
        state["error"] = "The render wrote no frames."
        _save_status(state)
        return
    ffmpeg = _ffmpeg()
    if not ffmpeg:
        state["state"] = "done"
        state["note"] = "ffmpeg is not installed: the film is the image sequence in output_dir (and the music asset)."
        _save_status(state)
        return
    pattern = os.path.join(state["output_dir"], "*" + os.path.splitext(frames[0])[1])
    mp4 = os.path.join(state["output_dir"], f"{state['film']}.mp4")
    duration = float(state["duration_s"])
    command = [ffmpeg, "-y", "-loglevel", "error", "-framerate", str(state["fps"]), "-pattern_type", "glob", "-i", pattern]
    inputs = []
    for entry in state.get("audio", []):
        source = _sound_file(entry["sound"], state["output_dir"])
        if source:
            inputs.append((source, entry))
    for source, _ in inputs:
        command += ["-i", source]
    command += ["-c:v", "libx264", "-preset", "medium", "-crf", "18", "-pix_fmt", "yuv420p"]
    if inputs:
        parts, labels = [], []
        for i, (_, entry) in enumerate(inputs):
            delay = int(float(entry["at_s"]) * 1000)
            parts.append(f"[{i + 1}:a]adelay={delay}|{delay},volume={entry['volume']},apad[a{i}]")
            labels.append(f"[a{i}]")
        fade_at = max(0.0, duration - 2.5)
        mix = f"{''.join(labels)}amix=inputs={len(labels)}:normalize=0,afade=t=in:d=0.4,afade=t=out:st={fade_at}:d=2.5[aout]"
        command += ["-filter_complex", ";".join(parts + [mix]), "-map", "0:v", "-map", "[aout]", "-c:a", "aac", "-b:a", "192k"]
    command += ["-t", f"{duration}", "-movflags", "+faststart", mp4]
    log = open(os.path.join(state["output_dir"], "ffmpeg.log"), "w", encoding="utf-8")
    state["process"] = subprocess.Popen(command, stdout=log, stderr=log)
    state["mp4"] = mp4
    _save_status(state)


def film_status(film=None):
    """Where a render is: rendering (with frames done), encoding, done (with the MP4 path) or failed."""
    name = _film_name(film)
    state = RENDERS.get(name)
    if state is None:
        runs = sorted(glob.glob(os.path.join(_films_dir(name), "*", "status.json")))
        if not runs:
            return {"film": name, "state": "not_rendered", "next": "render_film"}
        with open(runs[-1], encoding="utf-8") as handle:
            saved = json.load(handle)
        if saved.get("state") in ("rendering", "encoding"):
            saved["state"] = "interrupted"
            saved["note"] = "The editor restarted during this render: call render_film again."
        return saved
    if state["state"] == "rendering":
        done = len(glob.glob(os.path.join(state["output_dir"], "*.jpeg")) + glob.glob(os.path.join(state["output_dir"], "*.jpg")))
        state["frames_done"] = done
    process = state.get("process")
    if state["state"] == "encoding" and process is not None and process.poll() is not None:
        if process.returncode == 0 and os.path.isfile(state.get("mp4", "")):
            state["state"] = "done"
            state["size_mb"] = round(os.path.getsize(state["mp4"]) / 1e6, 1)
        else:
            state["state"] = "failed"
            state["error"] = "ffmpeg failed: see ffmpeg.log in output_dir."
        _save_status(state)
    result = {k: v for k, v in state.items() if k not in ("executor", "process")}
    result["elapsed_s"] = round(time.time() - state["started"])
    return result
