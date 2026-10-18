---
name: scenario-unreal
description: Generate images, video, 3D models, PBR materials, audio and skies with Scenario and land them in an open Unreal Editor through the Scenario for Unreal Engine plugin's local MCP server (scenario-unreal). Use when the user wants Scenario assets in an Unreal project, or asks to price, generate, import, place or save Scenario results in Unreal.
---

# Scenario for Unreal Engine

The Scenario plugin runs a local MCP server inside the Unreal Editor. Its tools price and run Scenario models, then import results as native Unreal assets and place them in the open level. Everything goes through the editor that is open on the user's machine.

## Setup

- Server: `http://127.0.0.1:9877/mcp` (if 9877 is taken, the plugin uses the next free port up to 9886; Scenario > Connection > Agents shows the real one).
- A bearer token is required by default. The Connect buttons in Scenario > Connection > Agents write it into Claude Code, Codex, Cursor or Claude Desktop. Never ask the user to paste the token into chat.
- Port 8000 is Epic's own Unreal MCP server, not this one. Epic's tools edit actors and Blueprints; they do not run Scenario jobs.
- If a call answers `Auth: Not signed in`, ask the user to sign in from the Scenario button in the level editor toolbar.

## Tool order

1. `scenario_status`: signed in, project, permissions (change_project, run_python), spending rules.
2. `scene_summary` (and `object_detail`, `screenshot_viewport`) to understand the level before generating for it.
3. `list_models` with a `lane` (image, video, 3d, material, audio, environment, render). The lane's picks come first and are the safe default. `recommend_models` asks Scenario for a pick.
4. `model_schema`: parameter names, options, defaults, and which change the price. File fields take Scenario asset ids.
5. `estimate_cost` with `model_id` and `parameters`: free, returns `quote_id` and `cu_cost_exact`.
6. `generate` with `quote_id` and `approved_cost` equal to `cu_cost_exact`, character for character. Returns `job_id`.
7. `wait_for_job` until `delivered` is true (it returns `still_running` after about 150 s; call it again).
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
- When "Allow agents to change this project" is off, `apply_result_application` returns `pending_approval`: the user sees Apply and Decline in the editor. Check `job_status` (`imported`) later; do not call apply again.
- Level changes are one Ctrl+Z each. Imported assets are new files; `remove_scenario_items` removes what the plugin placed or imported, after Unreal's reference check.
- Read the `checks` in the apply result (scale, triangles, bones, materials) and tell the user about any line starting with `Check:`.

## Lane facts

- 3D: request GLB. Interchange imports GLB with PBR materials, skeletons and morph targets. Expect centimeters in Unreal; check the reported bounds.
- Materials: the material lane returns PBR maps imported with texture settings per role (sRGB off for normal, roughness, metallic, height) and wired into a material instance. Unreal expects roughness: if a model returns a smoothness map instead, version 0.1 imports it as it is, so tell the user it needs inverting (roughness = 1 - smoothness).
- Skies: environment results are 2:1 equirectangular panoramas. `.hdr` files become cubemaps for an HDRI backdrop; PNG or JPG go on an unlit sky sphere.
- Audio: ask for WAV when the model offers a format. Unreal imports it as a Sound Wave; `place` adds an ambient sound.
- Render lane (restyle a view): `capture_reference` with `source: "viewport"`, `upload_reference`, wait for `reference_upload_status` to be `imported`, then pass its `asset_id` in the model's image field.
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

## Common mistakes

- Calling Epic's Unreal MCP tools (port 8000) for Scenario jobs.
- Putting a `reference_id` or a local path in a file field: file fields take Scenario `asset_id` values (from `upload_reference` once `imported`, `search_assets`, or a finished job's files).
- Assuming the level is saved after placing: it is not until `save_changes` with `include_levels: true`.
- Re-running `generate` after a timeout instead of checking `job_status` or `recover_local_job`.
- Approving a different number than `cu_cost_exact` ("12.0" for "12").

For general Unreal editing (Blueprints, Sequencer, lighting), use Epic's Unreal MCP or your Unreal skills, and come back to these tools for Scenario generation and landing.
