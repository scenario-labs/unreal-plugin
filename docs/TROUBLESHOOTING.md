# Troubleshooting

Start with **Connection > Diagnostics** in the Scenario Studio. **Copy diagnostics** copies the plugin, engine and OS versions, the sign-in and spending state, job counts and the last error. It holds no key, token or prompt. It does include your team and project ids and a local folder path: fine for Scenario support, but remove those lines before posting in a public issue. The Output Log (**Window > Output Log**, filter `LogScenario`) has the details.

## Install

| Symptom | Cause and fix |
|---|---|
| macOS: "the plugin's libraries cannot be opened" or "is damaged" | The zip downloaded from GitHub is quarantined by Gatekeeper; version 0.1 zips are not signed. Close the editor, run `xattr -dr com.apple.quarantine "<Project>/Plugins/Scenario"`, reopen. |
| "The following modules are missing or built with a different engine version" | A source copy, or a zip built for another engine version. Answer **Yes** to rebuild (needs a C++ toolchain: Xcode on macOS, Visual Studio on Windows), or download the zip for your engine version. |
| No **Scenario** button in the toolbar | Check **Edit > Plugins > Scenario** is enabled, then restart. The Studio is also in **Window > Scenario**. |
| The plugin is enabled but nothing loads | The Output Log lists the failing module. The plugin needs the Interchange and glTF Exporter plugins, which ship with Unreal and are enabled with it. |

## Sign-in

| Symptom | Cause and fix |
|---|---|
| The browser opens but Unreal keeps waiting | The browser could not reach the editor's local sign-in address, often a different browser profile or a remote session. Use **Copy link** in the right profile, or **Sign in with a code**. |
| "Scenario did not accept this API key and secret." | Copy both values again from scenario.com (API keys page). An API key belongs to one project. |
| "Your Scenario session ended. Sign in again." (HTTP 401) | The session expired or the key was revoked. Sign in again; jobs and imported assets stay. |
| Credentials stored in a file, not the keychain | On Linux, version 0.1 uses a file only you can read. On macOS and Windows, it means the system store refused the write: check the Output Log. |

## Prices and generation

| Message or state | Meaning and fix |
|---|---|
| **Choose a project** | Your account has several projects. Pick one in **Connection > Team and project**. |
| **Price unavailable (Retry)** | The price check failed (network, or settings Scenario refused). Hover the button for the reason; fix the field it names. |
| "This account cannot do that in the selected Scenario project." (HTTP 403) | Your role in that project cannot run this model, or your team blocked it. Ask a team admin, or switch project. |
| "Not enough Creative Units for this run." | Top up or change plan on scenario.com, or pick a cheaper model or lower settings. |
| "Scenario is limiting requests right now." (HTTP 429) | Too many requests at once. Wait a moment; price checks resume by themselves. A paid request refused this way did not run. |
| "Could not reach Scenario." / **Offline** in the header | No connection. Running jobs continue on Scenario and resume here when you are back. |
| **Blocked by Scenario's content rules** | Moderation refused the prompt or a reference. Change it. |
| **Spending off** | `SCENARIO_NO_SPEND=1` is set for this editor. Prices are checked; nothing is sent. Remove the variable and restart. |
| "Automatic price checks are off for this session because a price check returned a job." | A safety stop: a free price check should never start a job. Check the price by hand; report it if it happens again. |
| **Waiting for Scenario...** on the button | An identical request may already be running. Open **Jobs**, **Check Scenario** on it, or **Mark as not sent** if it is not there. |

## Unknown jobs and paying twice

A job is **Unknown** when the connection dropped while the request was being sent: Scenario may or may not have received it. The plugin never sends a paid request twice by itself, and it blocks an identical request while one is Unknown.

1. Click **Check Scenario** on the card. When Scenario has exactly one matching job, it is linked and continues normally.
2. If nothing matches after a minute, check your project on scenario.com.
3. Only when you are sure it is not there, click **Mark as not sent**. Sending again is then allowed.

## Downloads and imports

| Symptom | Cause and fix |
|---|---|
| "The download link expired. Getting a fresh one resumes the download." | Signed links last a limited time. Click **Download again**; partial files resume. |
| Download stopped when the editor closed | It resumes at the next start. **Resume** on the card forces it. |
| "Scenario no longer has this job." / "...this file." | The job or asset was deleted on Scenario, or is too old. Nothing to recover. |
| "Could not write the download to disk." | Check free space and write permissions on `<Project>/Saved/Scenario/`. |
| **Import failed** | **Details** shows the importer's message; the Output Log (filter `LogInterchange`) has more. The downloaded file stays in `Saved/Scenario/Downloads/` and **Import** can be retried. |
| A mesh is huge or tiny | The model ignored real-world scale. Turn on **Unreal-ready**, or scale the actor; the card's `Check:` lines give the size in centimeters. |
| A material looks too shiny | The model returned a smoothness map; invert it into roughness (version 0.1 does not). |
| "Enable the Media Plate plugin..." / "Enable the HDRI Backdrop plugin..." | The asset was imported but cannot be placed without that engine plugin. Enable it in **Edit > Plugins**, restart, then **Import and place** again. |

## Agents

See [Troubleshooting agents](MCP_GUIDE.md#7-troubleshooting-agents) in the Scenario MCP guide.

## Where things are

| What | Where |
|---|---|
| Job list, downloads, captures | `<Project>/Saved/Scenario/` (**Open state folder** in Connection) |
| Logs | Output Log; `<Project>/Saved/Logs/` |
| Project settings | `Config/DefaultEditor.ini`, section `[/Script/ScenarioEditor.ScenarioProjectSettings]` |
| Your preferences | Editor Preferences > Plugins > Scenario |
| Credentials | System credential store (see [Privacy](PRIVACY.md)) |

## Getting help

- Plugin bugs: [open an issue](https://github.com/scenario-labs/unreal-plugin/issues/new/choose) with **Copy diagnostics** (ids and paths removed) and the steps to reproduce. Never paste a key, secret or signed link.
- Account, Creative Units and billing: [help.scenario.com](https://help.scenario.com) or support@scenario.com.
- Security: see [SECURITY.md](../SECURITY.md).
