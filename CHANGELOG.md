# Changelog

## 0.2.1 (2026-10-22)

- Uploads and thumbnail downloads send the plugin's User-Agent. Before, Unreal's default User-Agent went with them, and it names the project.
- `docs/PRIVACY.md` is checked against the code and no longer a draft: every host the plugin contacts, what each one receives, what is stored where (including `workspace.json`, the agent configuration files the Set up buttons write, and imported videos copied to `Content/Movies`), and the Toolset Registry route.
- The docs site has a new page template and in-page search, written for this repository.

## 0.2.0 (2026-10-21)

- Unreal 5.8: Scenario toolset and agent skill in Epic's Toolset Registry, so agents on Epic's Unreal MCP can price, run and land Scenario jobs. Every paid call from there answers `pending_approval` and waits for Approve in the editor.
- `job_status` and `wait_for_job` take the `approval_id` of a spend waiting for the person.
- Agent tool search (Editor Preferences, off by default): `tools/list` returns four tools that find, describe and call the others.
- The Agent page shows whether Epic's Unreal MCP server is running, and on which port.
- Imports use the plugin's own Interchange pipelines: 1 unit = 1 cm, simple collision, Nanite from Project Settings (now on by default, skipped for meshes with translucent slots), no generated lightmap UVs. The result's checks report each rule per mesh.
- Maps named smoothness keep their own role. New setting **Invert smoothness** (off by default, because PATINA labels a roughness map "texture-smoothness"): when on, the import adds an inverted roughness copy wired into the material instance and keeps the original.
- The PBR parent material follows Substrate: a Substrate slab graph when `r.Substrate` is on, the legacy graph otherwise. The import notice names it.
- `scene_summary` and placements report World Partition, one file per actor and data layers; only loaded actors are listed.
- `screenshot_viewport` returns a frame check (black, white, uniform, clipped or crushed frames) and says when the viewport has a fixed exposure.

## 0.1.0 (unreleased)

First version.

- Studio tab (Create, Workflows, Library, Jobs, Results, Connection) and a quick composer in the level viewport.
- Seven lanes: Image, Video, 3D (text, image, edit), Material, Audio, Environment (sky and HDRI), Render (restyle the viewport).
- Model picker over every Scenario model and the team's trained models; recommendations; Prompt Spark; workflows.
- Prices checked before every paid request; single-use quotes; a job list that survives restarts, with resumable downloads and recovery of uncertain requests.
- Imports as native assets with provenance, placement in one undo step, import checks, context menus in the Content Browser and the level.
- Sign-in with the browser, a device code or an API key; credentials in the system credential store.
- Local Scenario MCP server for agents, with Connect buttons for Claude Code, Codex, Cursor and Claude Desktop, permissions, approvals and an activity log; the `scenario-unreal` agent skill.
- Python, Blueprint and command-line access to the same actions.
- Kit tools for agents: a walkable world, a game (pickups, goal, timer, music, HUD) or a dressed Epic template level, and a short film rendered with Movie Render Queue, from Scenario results.
- Scenario Gameplay runtime module: pickup, goal and game rules actors.
