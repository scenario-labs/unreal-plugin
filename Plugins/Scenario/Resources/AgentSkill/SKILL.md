---
name: scenario-unreal
description: Generate images, video, 3D models, PBR materials, audio and skies with Scenario and land them in an open Unreal Editor through the Scenario for Unreal Engine plugin's local MCP server (scenario-unreal). Use when the user wants Scenario assets in an Unreal project, or asks to price, generate, import, place or save Scenario results in Unreal.
---

# Scenario for Unreal Engine

The Scenario plugin runs a local MCP server inside the Unreal Editor. Its tools price and run Scenario models, then import results as native Unreal assets and place them in the open level. Everything goes through the editor that is open on the user's machine.

## Setup

- Server: `http://127.0.0.1:9877/mcp` (if 9877 is taken, the plugin uses the next free port up to 9886; the Agent page of the Scenario Studio shows the real one).
- A bearer token is required by default. The Set up buttons on the Scenario Studio's Agent page write it into Claude Code, Codex, Cursor or Claude Desktop. Never ask the user to paste the token into chat.
- Port 8000 is Epic's own Unreal MCP server (Unreal 5.8), not this one. Epic's toolsets edit actors, materials, Blueprints and the rest of the editor. On 5.8 the plugin also registers a Scenario toolset there (`ScenarioToolsets.ScenarioToolset`, reached with `call_tool`): same price, quote and landing rules, and every paid call waits for the person's Approve in the editor (see Money rules). Use this server when you have it; use Epic's registry when it is the only one connected.
- With Agent tool search on (Editor Preferences, off by default), `tools/list` shows only `scenario_status`, `find_tools`, `describe_tools` and `call_tool`: find a tool, describe it, then call it by name.
- If a call answers `Auth: Not signed in`, ask the user to sign in from the Scenario button in the level editor toolbar.

## Which tool for which job

| Job | Use |
| --- | --- |
| Make an asset (image, video, 3D, material, audio, sky), price it, import it, place it | these Scenario tools |
| Edit actors, Blueprints, materials beyond the imported instance, Niagara, Sequencer, PCG, lighting, level layout | Epic's toolsets on Epic's Unreal MCP (`list_toolsets`, `describe_toolset`, `call_tool`) |
| Know how to do it well in the engine (budgets, numbers, traps, review loops) | the Unreal Engine expert skills (`scenario-unreal-expert` and its domain skills) |
| Something no tool covers | `execute_python` (needs the "Run Python" permission), then the engine's Python |

Scenario tools never edit what they did not create. The result of a Scenario job is a normal asset: after it lands, hand it to Epic's toolsets or the expert skills.

## Tool order

1. `scenario_status`: signed in, project, permissions (change_project, run_python), spending rules.
2. `scene_summary` (and `object_detail`, `screenshot_viewport`) to understand the level before generating for it.
3. `list_models` with a `lane` (image, video, 3d, material, audio, environment, render). The lane's picks come first and are the safe default. `recommend_models` asks Scenario for a pick.
4. `model_schema`: parameter names, options, defaults, and which change the price. File fields take Scenario asset ids.
5. `estimate_cost` with `model_id` and `parameters`: free, returns `quote_id` and `cu_cost_exact`.
6. `generate` with `quote_id` and `approved_cost` equal to `cu_cost_exact`, character for character. Returns `job_id`.
7. `wait_for_job` until `delivered` is true (it returns `still_running` after 45 s by default; call it again. Keep `timeout_seconds` under 60: many clients cut a call at 60 s).
8. `prepare_result_application`, show the user the `review` text when it matters, then `apply_result_application`.
9. `save_changes` when the user wants the result saved. Nothing is saved for you except the imported asset files.

Workflows follow the same shape: `list_workflows`, `workflow_schema`, `estimate_workflow`, `run_workflow`.

## Money rules

- Estimate and generate are separate calls. Estimates are free; `generate`, `run_workflow` and `approve_prompt` spend Creative Units (CU).
- A quote is single use, bound to the exact model, parameters and project, and lasts 120 seconds. Any change needs a new estimate.
- Pass the quote's `cu_cost_exact` as `approved_cost`. Your own reading of a price is not an approval; the server checks the quote.
- Paid calls are never retried. If a job's state is `unknown` or `submitting`, call `recover_local_job` with `action: "check"`. Never send the same request again to "try once more": it may pay twice. Only the user can mark a request as not sent.
- `Declined` means the user refused this spend in the editor. Do not retry it.
- The plugin sets no CU cap. Tell the user what you are about to spend when it is more than a few CU, and prefer cheap models and low settings for tests.

## Landing results

- `prepare_result_application` purposes: `import` (Content Browser only), `place` (in front of the camera by default, or `at_location`, `at_pivot`), `material` (on `actor_ids` or the selection, `slot`), `environment` (sky), `media_plate` (video), `mesh_edit` (at the original actor's pivot).
- When the "Change this project" permission is off, `apply_result_application` returns `pending_approval`: the user sees Approve and Decline in the editor. Check `job_status` (`imported`) later; do not call apply again.
- Level changes are one Ctrl+Z each. Imported assets are new files; `remove_scenario_items` removes what the plugin placed or imported, after Unreal's reference check.
- Read the `checks` in the apply result (scale, triangles, bones, materials) and tell the user about any line starting with `Check:`.

## Lane facts

- 3D: request GLB. Interchange imports GLB with PBR materials, skeletons and morph targets. The plugin's own import pipeline sets 1 unit = 1 cm, simple collision and no generated lightmap UVs, and builds Nanite when every material slot is opaque or masked (Project Settings: Nanite, Mesh collision). The apply result's `checks` report each rule per mesh; check the reported bounds.
- Materials: the material lane returns PBR maps imported with texture settings per role (sRGB off for normal, roughness, metallic, height) and wired into a material instance. A map named smoothness is used as roughness (PATINA labels its roughness map that way); with the Invert smoothness setting on, an inverted roughness copy is wired instead. The result's checks say which, and name the parent material (legacy or Substrate).
- Skies: environment results are 2:1 equirectangular panoramas. `.hdr` files become cubemaps for an HDRI backdrop; PNG or JPG go on an unlit sky sphere.
- Audio: ask for WAV when the model offers a format. Unreal imports it as a Sound Wave; `place` adds an ambient sound.
- Open worlds: `scene_summary` and placements report World Partition, one file per actor and data layers, and list only loaded actors. Load the cells you need before judging what is in the level.
- Frames: `screenshot_viewport` returns a frame check (black, white, uniform, clipped or crushed). Treat a flagged frame as a problem to fix, not a result to show; a fixed exposure is named when the viewport has one.
- Restyle lane (`render`): `capture_reference` with `source: "viewport"`, `upload_reference`, wait for `reference_upload_status` to be `imported`, then pass its `asset_id` in the model's image field.
- Unreal-ready prompts: when `model_schema` returns `unreal_ready_prompt`, add it to 3D prompts (single object, neutral pose, clean topology, no ground plane).

## Example

User: "Put a mossy stone well next to the selected house."

1. `scene_summary` to see the selection and camera.
2. `list_models` `{ "lane": "3d" }`, take the first lane pick; `model_schema` on it.
3. `estimate_cost` `{ "model_id": "<pick>", "parameters": { "prompt": "mossy medieval stone well, <unreal_ready_prompt>" } }` returns `quote_id: q_1a2b`, `cu_cost_exact: "12"`.
4. Tell the user: "This costs 12 CU." Then `generate` `{ "quote_id": "q_1a2b", "approved_cost": "12" }`.
5. `wait_for_job` `{ "job_id": "<job_id>" }` until `delivered`.
6. `prepare_result_application` `{ "job_id": "<job_id>", "purpose": "place", "placement": "at_location", "location": [x, y, z] }` with a point beside the house from `object_detail`.
7. `apply_result_application`, report the created asset and actor, and ask before `save_changes`.

## One prompt to a world, a game or a film

The kit tools lay Scenario results out for you: `build_world`, `dress_world`, `make_game`, `dress_template`, `make_film`, `render_film`, `film_status`, `kit_status`, `play_level`. They need the "Change this project" permission. Distances are in meters.

Work in this order so generation time overlaps with building:

1. `scenario_status` and `kit_status`.
2. Pick a theme and write every prompt. Then `estimate_cost` for each asset and `generate` them all at once (each quote lasts 120 s, so generate right after its estimate). Tell the person the total first.
3. While the jobs run: `build_world` (or `dress_template`).
4. `wait_for_job` each job, then `prepare_result_application` with purpose `import` and `apply_result_application`. Keep the paths from `created_assets`: the static mesh for 3D (its import folder also works), the material instance for materials, the material or texture for skies, the sound wave for audio.
5. Lay out with the kit tool, then look with `screenshot_viewport` and its `camera` (the player's eye from the start, an aerial view) so the person's viewport stays put. Fix what looks wrong (`front_yaw`, `height_m`, `seed`), and tell the person what to do next (press Play, open the MP4). After `play_level`, call `kit_status` a few seconds later: it reports the player character once it has spawned.

Assets per recipe (lane picks from `list_models` are good defaults):

| Recipe | Generate | Then |
| --- | --- | --- |
| Walkable world | 1 sky (environment), 1 ground material, 1 path material, 1 landmark and 3 or 4 props (3D), 1 ambient loop (audio, sound effects model, 20 to 30 s) | `build_world`, `dress_world` with `landmark`, `path`, `props`, `ground_material`, `sky`, `mist`, `ambient` |
| Game you build | the world assets, plus 1 small pickup (3D), 1 pickup sound, 1 music track; optionally a goal mesh | world recipe, then `make_game` with `title`, `pickups` (mesh, count, name, sound), `goal`, `music`, `time_limit_s` |
| Template game | 1 floor material, 1 block material, 1 crate or prop (3D), 1 sky, 1 music track | `dress_template` with `template` (platforming, combat...), `floor_material`, `block_material`, `swaps` ([{"replace": "boxes", "mesh": ...}]), `sky`, `music` |
| Short film | the world assets (or an existing world), 1 music track as long as the film | world recipe, `make_film` with `music`, then `render_film` and `film_status` every 20 s until `done`; give the person the `mp4` path |

Prompt tips:

- 3D: one object, centered, no ground plane, no text ("weathered red torii gate, single object"). Add `unreal_ready_prompt` from `model_schema` when it is there. Sizes come from `height_m`, not the prompt.
- Materials: "seamless tileable <surface>, top-down, even lighting".
- Skies: describe the horizon and light ("360 panorama, misty mountain valley at golden hour, soft clouds"); match `time_of_day`.
- Music: give the length the model asks for (film length, or 60 s or more for a game loop) and the mood.

Layout tips:

- Landmark 4 to 10 m high; props 0.5 to 4 m (use `[min, max]` for variety); `path_sides` for things lining the path (lanterns, fences), evenly spaced; `on_path` for gates and arches the player walks through (torii, archways): they straddle the path; `edges` for tall things framing the area (trees, cliffs).
- `path` lays a paved strip from the player start to the landmark: `{ "material": <material>, "width_m": 3.5 }`.
- Lanterns, braziers and lit windows: add `"light": true` to the prop (a warm 2200 K light inside each, 48 at most per world).
- `mist` from 0 to 1 (0.6 to 0.9 for a misty morning or a shrine at golden hour) thickens the ground fog and makes it volumetric, so light shafts and lantern glow show.
- With a `sky`, the sun is aimed from the sun in the panorama when it shows a clear one; the result's `sun` says which (sky panorama or time_of_day preset).
- The landmark faces the player start by default (Scenario 3D models face +Y, so `front_yaw` defaults to -90). If it faces away, call `dress_world` again with `landmark.front_yaw` 90 (or 0, 180); `replace` true keeps the layout clean.
- `size_m` 80 to 150 is a good walk. More than 120 props slows the editor.
- A film needs the world first. 20 to 30 s with six shots is the default; renders take a few minutes at 720p draft.

## Common mistakes

- Calling Epic's own toolsets (port 8000) for Scenario jobs, or this server for editing what Scenario did not make. On 5.8, the Scenario toolset inside Epic's registry is the exception: it is ours.
- Putting a `reference_id` or a local path in a file field: file fields take Scenario `asset_id` values (from `upload_reference` once `imported`, `search_assets`, or a finished job's files).
- Assuming the level is saved after placing: it is not until `save_changes` with `include_levels: true`.
- Re-running `generate` after a timeout instead of checking `job_status` or `recover_local_job`.
- Approving a different number than `cu_cost_exact` ("12.0" for "12").
- In `execute_python`, building `unreal.Rotator(pitch, yaw, roll)` positionally: the order is roll, pitch, yaw. Use keywords: `unreal.Rotator(roll=0.0, pitch=-10.0, yaw=90.0)`.
- Moving the person's viewport to look at something: pass `camera` to `screenshot_viewport` instead.

For general Unreal editing (Blueprints, Sequencer, lighting), use Epic's Unreal MCP or the Unreal Engine expert skills, and come back to these tools for Scenario generation and landing.
