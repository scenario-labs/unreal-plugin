# Troubleshooting

Start with **Settings > Diagnostics** in the Scenario Studio. **Copy diagnostics** copies the plugin, engine and OS versions, the sign-in and spending state, job counts and the last error. It holds no key, token or prompt. It does include your team and project ids and a local folder path: fine for Scenario support, but remove those lines before posting in a public issue. The Output Log (**Window > Output Log**, filter `LogScenario`) has the details.

## Install

| Symptom | Cause and fix |
|---|---|
| macOS: "the plugin's libraries cannot be opened" or "is damaged" | The zip downloaded from GitHub is quarantined by Gatekeeper; version 0.2 zips are not signed. Close the editor, run `xattr -dr com.apple.quarantine "<Project>/Plugins/Scenario"`, reopen. |
| "The following modules are missing or built with a different engine version" | A source copy, or a zip built for another engine version. Answer **Yes** to rebuild (needs a C++ toolchain: Xcode on macOS, Visual Studio on Windows), or download the zip for your engine version. |
| No **Scenario** button in the toolbar | Check **Edit > Plugins > Scenario** is enabled, then restart. The Studio is also in **Window > Scenario Studio**. |
| The plugin is enabled but nothing loads | The Output Log lists the failing module. The plugin needs the Interchange and glTF Exporter plugins, which ship with Unreal and are enabled with it. |
| Unreal 5.8: Python Editor Script Plugin, Editor Scripting Utilities, File Sandbox and Toolset Registry are enabled after installing | The plugin enables Epic's Toolset Registry when the engine has it, so agents on Epic's Unreal MCP can use Scenario; the other three are the registry's own dependencies. On 5.8 the plugin's `ScenarioToolsets` module needs the registry: keep it enabled. |

## Sign-in

| Symptom | Cause and fix |
|---|---|
| The browser opens but Unreal keeps waiting | The browser could not reach the editor's local sign-in address, often a different browser profile or a remote session. Use **Copy link** in the right profile, or **Sign in with a code**. |
| "Scenario did not accept this API key and secret." | Copy both values again from scenario.com (API keys page). An API key belongs to one project. |
| "Your Scenario session ended. Sign in again." (HTTP 401) | The session expired or the key was revoked. Sign in again; jobs and imported assets stay. |
| The Studio opens on another project than the one you chose | `SCENARIO_API_KEY` and `SCENARIO_API_SECRET` are set in the editor's environment: they take precedence over your sign-in and select the key's project. On macOS, an editor started from the Dock or Finder sees variables set with `launchctl setenv`; check with `launchctl getenv SCENARIO_API_KEY`, and clear them with `launchctl unsetenv` before restarting the editor. |
| macOS: a dialog asks for the "login" keychain password when you run the commandlet | The keychain trusts the app that saved your sign-in, the editor; `UnrealEditor-Cmd` is another app. Click **Always Allow** once. With `-unattended` there is no dialog: the saved sign-in is skipped, so set `SCENARIO_API_KEY` and `SCENARIO_API_SECRET` for that run. |
| Credentials stored in a file, not the keychain | On Linux, version 0.2 uses a file only you can read. On macOS and Windows, it means the system store refused the write: check the Output Log. |

## Prices and generation

| Message or state | Meaning and fix |
|---|---|
| **Choose a project** | Your account has several projects. Pick one in the Studio, or with **Switch project** on **Settings**. |
| **Retry price** on the button | The price check failed (network, or settings Scenario refused). The line under the button gives the reason ("Price unavailable: ..."); fix the field it names. |
| "This account cannot do that in the selected Scenario project." (HTTP 403) | Your role in that project cannot run this model, or your team blocked it. Ask a team admin, or switch project. |
| "Not enough Creative Units for this run." | Top up or change plan on scenario.com, or pick a cheaper model or lower settings. |
| "Scenario is limiting requests right now." (HTTP 429) | Too many requests at once. Wait a moment; price checks resume by themselves. A paid request refused this way did not run. |
| "Could not reach Scenario." / **Offline** banner | No connection. Running jobs continue on Scenario and resume here when you are back. |
| **Blocked by Scenario's content rules** | Moderation refused the prompt or a reference. Change it. |
| "SCENARIO_NO_SPEND is set: prices only, nothing is sent." | `SCENARIO_NO_SPEND=1` is set for this editor. Prices are checked; nothing is sent. Remove the variable and restart. |
| "Automatic price checks are off for this session because a price check returned a job." | A safety stop: a free price check should never start a job. Check the price by hand; report it if it happens again. |
| "An identical request may still be running." under **Generate** | The same request is not confirmed yet. Click **Check in Jobs**, then **Check** on that job, or **Mark as not sent** in its **...** menu if it is not on Scenario. |

## Unconfirmed jobs and paying twice

A job shows **Not confirmed** when the connection dropped while the request was being sent: Scenario may or may not have received it. The plugin never sends a paid request twice by itself, and it blocks an identical request until that job is checked.

1. Click **Check** on the job, or **Check now** in the banner above the list. When Scenario has exactly one matching job, it is linked and continues normally.
2. If nothing matches after a minute, check your project on scenario.com.
3. Only when you are sure it is not there, click **Mark as not sent** in the job's **...** menu. Sending again is then allowed.

## Downloads and imports

| Symptom | Cause and fix |
|---|---|
| "The download link expired. Getting a fresh one resumes the download." | Signed links last a limited time. Click **Resume download**; partial files resume. |
| Download stopped when the editor closed | It resumes at the next start. **Resume download** on the job forces it. |
| "Scenario no longer has this job." / "...this file." | The job or asset was deleted on Scenario, or is too old. Nothing to recover. |
| "Could not write the download to disk." | Check free space and write permissions on `<Project>/Saved/Scenario/`. |
| **Import failed** | The notification and the job's **Details** show the importer's message; the Output Log (filter `LogInterchange`) has more. The downloaded file stays in `Saved/Scenario/Downloads/`. The notification has **Try again**. The job then shows **Download stopped**; **Resume download** fetches the files again before the next import. |
| A mesh is huge or tiny | The model ignored real-world scale. Turn on **Unreal-ready**, or scale the actor; the import checks that agents read (`asset_detail`) give the size in centimeters. |
| A material looks too shiny, or too rough | The model's map holds the opposite of what its name says. The import uses maps named smoothness as roughness, because PATINA labels its roughness map that way; turn **Invert smoothness** on in Project Settings > Plugins > Scenario and import again for a model whose smoothness map is real smoothness. |
| Scenario materials render black after turning Substrate on or off | The PBR parent follows the renderer the editor started with. Restart the editor after changing **Substrate materials** in Project Settings > Rendering; new imports then use the matching parent (`M_Scenario_PBR` or `M_Scenario_PBR_Substrate`). |
| A video or HDR sky is imported but not placed; agents get "Enable the Media Plate plugin..." / "Enable the HDRI Backdrop plugin..." | The asset was imported but cannot be placed without that engine plugin. Enable it in **Edit > Plugins**, restart, then **Place again** in the job's **...** menu. |

## Agents

See [Troubleshooting agents](MCP_GUIDE.md#7-troubleshooting-agents) in the Scenario MCP guide.

- **Other servers in the editor.** Epic's Unreal MCP server and Remote Control's web server have no token. Epic's MCP server listens on this computer only unless the engine's HTTP listener settings say otherwise; Remote Control's web interface binds all network interfaces (`0.0.0.0`) by default. Anything that reaches them over the network can drive the editor. Keep them on loopback (`127.0.0.1`) or behind a firewall, and turn Remote Control off when you do not use it. The Scenario MCP server always listens on `127.0.0.1` and checks its token, and paid calls through Epic's server always wait for your approval.
- **Two entries in one config file.** Epic's setup for its server writes its own `unreal-mcp` entry into the project's `.mcp.json`, `.cursor/mcp.json` or similar, and rewrites that entry when run again. **Set up** on the Agent page writes only the `scenario-unreal` entry, in your user configuration, and keeps every other entry. Both servers can stay connected.

## Where things are

| What | Where |
|---|---|
| Job list, downloads, captures | `<Project>/Saved/Scenario/` (**Open state folder** on **Settings**) |
| Logs | Output Log; `<Project>/Saved/Logs/` |
| Project settings | `Config/DefaultEditor.ini`, section `[/Script/ScenarioEditor.ScenarioProjectSettings]` |
| Your preferences | Editor Preferences > Plugins > Scenario |
| Credentials | System credential store (see [Privacy](PRIVACY.md)) |

## Getting help

- Plugin bugs: [open an issue](https://github.com/scenario-labs/unreal-plugin/issues/new/choose) with **Copy diagnostics** (ids and paths removed) and the steps to reproduce. Never paste a key, secret or signed link.
- Account, Creative Units and billing: [help.scenario.com](https://help.scenario.com) or support@scenario.com.
- Security: see [SECURITY.md](../SECURITY.md).
