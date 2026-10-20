# Changelog

## Unreleased

- Imports use the plugin's own Interchange pipelines: 1 unit = 1 cm, simple collision, Nanite from Project Settings (now on by default, skipped for meshes with translucent slots), no generated lightmap UVs. The result's checks report each rule per mesh.
- Smoothness maps get an inverted roughness copy wired into the material instance; the original is kept. New setting: Invert smoothness.
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
