# Scenario MCP guide

The plugin runs a local Scenario MCP server inside the Unreal Editor. An agent connected to it (Claude Code, Codex, Cursor, Claude Desktop or any MCP client) can read your level, price and run Scenario models and workflows, then import and place the results, in the editor that is open on your machine.

## 1. Connect an agent

1. Open **Connection > Agents** in the Scenario Studio (toolbar menu: **More > Agents and diagnostics**).
2. Check that the server says it is running, with its address (`http://127.0.0.1:9877/mcp` unless that port was taken).
3. Click **Connect** next to your agent. The plugin writes the server and its token into the agent's configuration, after saving a timestamped backup of the file (`<file>.scenario-backup-YYYYMMDD-HHMMSS`). A file it cannot read is left untouched.
4. Restart the agent, or reload its MCP servers.
5. Optional: **Install the agent skill** copies the `scenario-unreal` skill to `~/.claude/skills/`. It teaches Claude the tool order and the spending rules.

| Agent | What Connect writes |
|---|---|
| Claude Code | `claude mcp add --transport http --scope user scenario-unreal <url> --header "Authorization: Bearer <token>"`, or the same entry in `~/.claude.json` when the `claude` command is not found |
| Codex | `[mcp_servers.scenario-unreal]` with `url` and `http_headers` in `~/.codex/config.toml` |
| Cursor | `mcpServers.scenario-unreal` with `url` and `headers` in `~/.cursor/mcp.json` |
| Claude Desktop | A stdio bridge, `npx -y mcp-remote <url> --allow-http`, with the header passed through the `SCENARIO_UNREAL_AUTH` environment variable (needs Node.js) |

For any other client, **Copy setup** puts the URL, the header and ready-made snippets on the clipboard. A typical JSON entry:

```json
{
  "mcpServers": {
    "scenario-unreal": {
      "type": "http",
      "url": "http://127.0.0.1:9877/mcp",
      "headers": { "Authorization": "Bearer <token>" }
    }
  }
}
```

Never paste the token into a chat. **New token** replaces it; connected agents then need **Connect** again.

The editor must be open, signed in to Scenario, with a team and project chosen. Epic's own Unreal MCP server (port 8000 when enabled) is a different server: its tools edit actors and Blueprints and do not run Scenario jobs. Both can be connected at once.

## 2. Permissions

| Switch (Connection > Agents, or Editor Preferences > Plugins > Scenario) | Default | Off means | On means |
|---|---|---|---|
| Allow agents to change this project | Off | `apply_result_application` returns `pending_approval` and you see **Apply** and **Decline** in the editor; `save_changes` and `remove_scenario_items` are refused | The agent imports, places and saves what the plugin created |
| Allow agents to run Python | Off | `execute_python` is refused | Unreal Python runs inside one undo transaction (needs the switch above and the Python Editor Script Plugin) |
| Approve each agent spend | Off | An agent spends with a valid price quote; a notification shows each spend | Each paid call waits for **Approve** or **Decline** in the editor (100 seconds) |

Reading the level (summaries, object details, screenshots, assets, the output log) and checking prices are always allowed. Agents never see your Scenario credentials.

## 3. Spending

Every paid call follows the same contract as the Studio:

1. `estimate_cost` (or `estimate_workflow`, `estimate_prompt`) is free and returns a `quote_id` and `cu_cost_exact`.
2. `generate` (or `run_workflow`, `approve_prompt`) needs that `quote_id` and `approved_cost` equal to `cu_cost_exact`, character for character.
3. A quote is single use, bound to the exact model, settings and project, and expires after 120 seconds. Any change needs a new estimate.
4. A paid call is sent once. If the connection drops, the job becomes `unknown` and an identical request is blocked: call `recover_local_job` with `action: "check"`, never `generate` again. Only you can mark a request as not sent.
5. With `SCENARIO_NO_SPEND=1` in the editor's environment, every paid call is refused before anything is sent.

The plugin sets no Creative Unit cap of its own. The activity log in **Connection > Agents** lists every agent call with its client, duration, result and the CU it spent.

## 4. Tools

Tool descriptions that spend start with `SPENDS`. Tools are annotated as read-only, destructive or idempotent for clients that use MCP annotations.

**Status and level**

| Tool | Does |
|---|---|
| `scenario_status` | Signed in, team and project, permissions, spending rules, server address |
| `scene_summary` | Level, World Partition state, selection, camera, actor counts by class, a page of actors with ids and tags |
| `object_detail` | One actor: transform, bounds, components, materials per slot, Scenario provenance |
| `screenshot_viewport` | PNG of the active level viewport, returned as an image |
| `find_assets`, `asset_detail` | Search the Content Browser; details and mesh checks for one asset |
| `output_log` | Recent warnings and errors, or Scenario and Interchange messages |
| `select_objects` | Selects actors in the level |
| `unreal_api_help` | Reflection help for an Unreal class, for writing Python |
| `execute_python` | Runs Unreal Python (permission needed) |

**Models and generation**

| Tool | Does |
|---|---|
| `list_models` | Models for a lane (`image`, `video`, `3d`, `material`, `audio`, `environment`, `render`); the lane's picks first |
| `model_schema` | Parameters, options, defaults, which change the price, and the Unreal-ready prompt for 3D |
| `recommend_models` | Asks Scenario for the best models for a request |
| `estimate_cost` | Free price check; returns the quote |
| `generate` | SPENDS: runs the quoted request; returns `job_id` |
| `job_status`, `wait_for_job` | State and files of a job; `wait_for_job` returns `still_running` after about 150 seconds, call it again |
| `list_local_jobs`, `list_generations` | This project's job list; recent jobs on Scenario from any tool |
| `cancel_job` | Asks Scenario to stop a job |
| `recover_local_job` | `check` an `unknown` request against Scenario, or `dismiss` a row |
| `recover_cloud_job` | Links a job from `list_generations` into this project and downloads it, free |

**Workflows, prompts, library and references**

| Tool | Does |
|---|---|
| `list_workflows`, `workflow_schema`, `estimate_workflow`, `run_workflow` | Same shape as models; `run_workflow` SPENDS |
| `estimate_prompt`, `approve_prompt`, `read_prompt_result` | Prompt Spark: price, run (SPENDS), read the proposals |
| `list_assets`, `search_assets`, `list_collections` | Your Scenario project's library |
| `capture_reference` | Renders the viewport or exports actors to GLB, locally |
| `upload_reference`, `reference_upload_status` | Uploads a capture or a project file to Scenario; wait for `imported`, then pass its `asset_id` in a file field |

**Landing results**

| Tool | Does |
|---|---|
| `prepare_result_application` | Plans an import: `import`, `place` (`at_location`, `at_pivot`, in front of the camera by default), `material` (on `actor_ids` or the selection), `environment`, `media_plate`, `mesh_edit`; returns a `review` text |
| `apply_result_application` | Applies the plan, or returns `pending_approval` when changes are off; returns what changed and the import checks |
| `save_changes` | Saves what the plugin created; `include_levels: true` also saves the level |
| `remove_scenario_items` | Removes actors the plugin placed and assets it imported, after Unreal's reference check |

## 5. A session, step by step

Ask: "Put a mossy stone well next to the selected house."

1. `scenario_status`, then `scene_summary` to find the selection and the camera.
2. `list_models` `{ "lane": "3d" }` and `model_schema` on the first pick.
3. `estimate_cost` `{ "model_id": "<pick>", "parameters": { "prompt": "mossy medieval stone well, <unreal_ready_prompt>" } }` returns a `quote_id` and `cu_cost_exact`.
4. The agent tells you the price, then calls `generate` `{ "quote_id": "...", "approved_cost": "<cu_cost_exact>" }`.
5. `wait_for_job` until `delivered` is true.
6. `prepare_result_application` with `purpose: "place"`, `placement: "at_location"` and a point beside the house from `object_detail`.
7. `apply_result_application`. With changes off, you click **Apply** in the editor.
8. The agent reports the asset and actor it created and asks before `save_changes`.

## 6. Security

- The server listens on `127.0.0.1` only. It refuses requests whose `Host` is not a loopback name and any request from a web page (`Origin` header), so a website cannot drive it.
- The bearer token is random, stored in the system credential store, and compared in constant time. Turn **Agent token** off only for a client that cannot send headers.
- Tool results never contain Scenario credentials. Error messages have signed URLs and authorization headers removed.
- What an agent reads through the server (level summaries, screenshots, results) goes to that agent's model provider, under your agreement with that provider.

## 7. Troubleshooting agents

| Symptom | Fix |
|---|---|
| The agent does not list `scenario-unreal` tools | Restart the agent after **Connect**. Check the server is running in **Connection > Agents**. |
| HTTP 401 | The token changed: click **Connect** again for that agent. |
| Connection refused | The editor is closed, or the server is off (**Agent server** in Editor Preferences). |
| The port is not 9877 | Another program held 9877, so the server took the next free port; **Connect** writes the right one. |
| `Auth: Not signed in` | Sign in from the Scenario toolbar button. |
| `Declined` | You declined the spend or the change in the editor. The agent must not retry it. |
| `pending_approval` | Waiting for **Apply** in the editor. The agent should check `job_status` later, not apply again. |

More: [Troubleshooting](TROUBLESHOOTING.md).
