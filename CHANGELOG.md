# Changelog

## 0.2.0 (unreleased)

- Unreal 5.8: Scenario toolset and agent skill in Epic's Toolset Registry, so agents on Epic's Unreal MCP can price, run and land Scenario jobs. Every paid call from there answers `pending_approval` and waits for Approve in the editor.
- `job_status` and `wait_for_job` take the `approval_id` of a spend waiting for the person.
- Agent tool search (Editor Preferences, off by default): `tools/list` returns four tools that find, describe and call the others.
- The Agent page shows whether Epic's Unreal MCP server is running, and on which port.

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
