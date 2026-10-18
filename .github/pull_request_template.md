<!--
The PR title becomes the squash commit header: type(scope): summary.
Use an imperative summary, no trailing period, and at most 120 characters.
Scopes: core, editor, ui, agent, lanes, docs, tools, tests, ci, deps,
release, repo (see AGENTS.md).
Example: fix(editor): keep the selection after placing a material

Keep one concern per PR and the description short. Tick only checks you did;
leave the rest unticked without comment. Never claim review, results or
authorization that has not happened.
-->

## What does this PR change?

<!--
One to three sentences: the problem and what changes. Add a before/after only
for a behavior change. Name any action needed after merge and any known gap.
Closes #n for complete issue scope; Refs #n otherwise, saying what remains.
Show a visible change with screenshots of the Studio tab or the composer,
before/after for UI.
-->

## Validation

- [ ] `tools/build.sh --host` and `tools/test.sh` pass; the engine version and OS are listed below.
- [ ] `python3 tools/lint.py` and `python3 -m unittest discover -s tools/tests` pass.
- [ ] Packaging changes: the `BuildPlugin` output installs into a blank project with no errors or warnings, and its contents were inspected.
- [ ] Editor or UI changes were tried in a full Unreal Editor session, with before/after screenshots for UI.
- [ ] Live status is stated: `tools/contract_check.py` (free) when calls to Scenario change; any paid run had prior authorization for its project and budget.

Commands and results:

<!-- One line per command run and its result. Keep the line above: it stops
GitHub merging this list into the checklist. -->

## Public content

- [ ] No credentials, tokens, signed URL query strings, or private account, team, project, job or asset IDs are included.
- [ ] No personal paths, machine details, agent session IDs or spend anecdotes are included.

## Rules

- [ ] The spending rules in `AGENTS.md` still hold (price check first, single-use quotes, paid calls sent once, `SCENARIO_NO_SPEND`).
- [ ] No unrelated manual version or changelog edits; documentation matches the resulting behavior.

## Authorship and licensing

- [ ] Human review of the complete diff is recorded before merge.
- [ ] Adopted third-party code or assets keep their compatible licenses and attribution; original contributions are licensed under MIT, the repository license.

<!-- If an agent contributed, name the harness and keep accurate co-author
trailers on commits and in the final squash message. -->
