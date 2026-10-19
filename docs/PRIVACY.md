# Privacy

Draft, 2026-10-18. This page describes what Scenario for Unreal Engine sends, stores and keeps local. It will be checked against the code before the first release.

## What goes to Scenario

The plugin talks to Scenario's hosted service at `mcp.scenario.com` over HTTPS. It sends:

- **Your account or key identity**: an OAuth access token after "Sign in with Scenario", or your API key and secret as a Basic authorization header.
- **What you ask for**: the model, your prompt, the settings in the form, and the workspace (team and project) you chose.
- **Price checks**: while you edit the form, the plugin asks Scenario for the price of the current settings. These requests contain the same prompt and settings as a generation and cost nothing.
- **Files you choose as inputs**: images, meshes, audio or video from your project or your disk, captures of the level viewport, and meshes exported for a 3D edit. They are uploaded only when you, or an agent connected to the editor, add them as an input.
- **Workflow inputs and Prompt Spark text**, when you run them.
- **A User-Agent header** with the plugin version, the engine version and the operating system. It contains no personal or project data.

Scenario processes these under your Scenario account terms and privacy policy. Generated results and uploaded inputs are stored in your Scenario project, as they are when you use the web app.

## What the plugin never sends

- Your level, actors, asset names, file listings or project settings, unless you or an allowed agent send one of them as an input.
- Your OAuth tokens or API secret to any host other than Scenario's service.
- Credentials of any kind with file downloads. Results are downloaded only from Scenario's content hosts, and uploads go only to the presigned storage addresses Scenario returns.

## What stays on your machine

| Data | Where | Committed to source control? |
|---|---|---|
| OAuth refresh token, API secret | macOS Keychain or Windows Credential Manager; on Linux, a per-user file readable only by you | No |
| OAuth access token | Memory only | No |
| API key id, chosen team and project ids, your editor preferences | Your per-user editor settings | No |
| Job list (ids, model, prompt, prices, states) | `<Project>/Saved/Scenario/` | No, `Saved/` is excluded by Unreal's default ignore rules |
| Downloaded originals | `<Project>/Saved/Scenario/Downloads/` | No |
| Model catalog cache | `<Project>/Saved/Scenario/` | No |
| Imported assets | `/Game/Scenario/...` in your project | Yes, like any asset you import |
| Provenance on each imported asset: asset id, job id, model, prompt, settings, price, date, workspace ids | Editor-only metadata inside the asset | Yes, with the asset. A Project Setting leaves the prompt out. The metadata is removed when the game is cooked, so it never ships in a build |

Only the Scenario Gameplay module (pickup, goal and game rules actors) is compiled into a packaged game. It has no network code and holds no Scenario data.

## Agents

The local agent server listens only on your machine (`127.0.0.1`). It refuses requests from web pages and other hosts, and requires a token that the Connect buttons write into your agent's configuration.

What an agent reads through it (level summaries, screenshots, asset details, job results) goes to that agent's own model provider, under your agreement with that provider. The plugin does not control what the provider does with it.

An agent can spend Creative Units only with a price quote it asked for first: the plugin checks that the paid call carries that exact quote, and shows each agent spend in the editor and in the Agents activity log. An optional setting asks for your approval in the editor before each agent spend. By default, an agent cannot change your project and cannot run Python. You can change each of these in Editor Preferences > Plugins > Scenario.

## Telemetry

Off by default. After your first finished generation, the plugin asks once whether you want to share anonymous usage statistics; ignoring the question leaves it off. You can change the answer in Editor Preferences > Plugins > Scenario.

Version 0.1 sends nothing even when you agree: no collection endpoint exists yet. The events the plugin is ready to send hold only these fields:

| Field | Example |
|---|---|
| Event name | `job_finished` |
| Lane and job kind | `3d`, `model` |
| Job state | `success`, `failed` |
| Who started it | `person`, `agent` or `script`, and the agent client's name such as `claude-code` |
| Plugin version, engine version, OS family | `0.1.0`, `5.8.0`, `Mac` |

Events never include prompts, file contents, asset, level or model names, team, project, job or asset ids, or credentials. This page will be updated before any endpoint is turned on.
