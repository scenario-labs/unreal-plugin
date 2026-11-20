#!/usr/bin/env python3
# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
"""Free live contract check (no Creative Units spent).

Checks that Scenario's REST API still answers the way the plugin expects:
the API key reads its project, the public catalog lists, every lane model in
Plugins/Scenario/Resources/Lanes.json loads its inputs and prices with a dry
run, Prompt Spark prices, and the asset search, workflow lists and usage read
still answer. Only reads and dry runs are sent: this script refuses any other
request itself, before it leaves the machine.

Credentials: SCENARIO_API_KEY and SCENARIO_API_SECRET, or on macOS a Keychain
item named by SCENARIO_KEYCHAIN_SERVICE (read with /usr/bin/security; the
secret is never printed).

Usage:
  python3 tools/contract_check.py [--json report.json] [--lane image]
"""
import argparse
import base64
import json
import os
import pathlib
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
LANES = ROOT / "Plugins/Scenario/Resources/Lanes.json"
API = os.environ.get("SCENARIO_API_URL", "https://api.cloud.scenario.com/v1").rstrip("/")
USER_AGENT = "ScenarioUnreal/contract-check"
# Free requests that are not GETs: anything else that is not a dry run is refused.
FREE_POSTS = {"/search/assets"}


def keychain_credentials(service):
    if sys.platform != "darwin" or not service:
        return None, None
    found = subprocess.run(["/usr/bin/security", "find-generic-password", "-s", service], capture_output=True, text=True)
    account = None
    for line in found.stdout.splitlines():
        line = line.strip()
        if line.startswith('"acct"<blob>='):
            account = line.split("=", 1)[1].strip().strip('"')
    if not account:
        return None, None
    secret = subprocess.run(["/usr/bin/security", "find-generic-password", "-s", service, "-a", account, "-w"], capture_output=True, text=True)
    value = secret.stdout.strip()
    return (account, value) if value else (None, None)


class Api:
    def __init__(self, key, secret):
        self.auth = "Basic " + base64.b64encode(f"{key}:{secret}".encode()).decode()

    def request(self, verb, path, query=None, body=None):
        """One request; returns (status, JSON object). Refuses anything that could spend."""
        query = {k: v for k, v in (query or {}).items() if v not in (None, "")}
        if verb != "GET" and query.get("dryRun") != "true" and path not in FREE_POSTS:
            raise RuntimeError(f"refused: {verb} {path} without dryRun=true could spend")
        url = API + path + ("?" + urllib.parse.urlencode(query) if query else "")
        data = json.dumps(body).encode() if body is not None else None
        headers = {"Authorization": self.auth, "Accept": "application/json", "User-Agent": USER_AGENT}
        if data is not None:
            headers["Content-Type"] = "application/json"
        request = urllib.request.Request(url, data=data, headers=headers, method=verb)
        try:
            with urllib.request.urlopen(request, timeout=120) as response:
                status, text = response.status, response.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as error:
            text = error.read().decode("utf-8", "replace")
            try:
                reason = json.loads(text).get("reason") or json.loads(text).get("error") or text
            except ValueError:
                reason = text
            raise RuntimeError(f"{verb} {path}: HTTP {error.code}: {str(reason)[:300]}") from None
        try:
            parsed = json.loads(text) if text.strip() else {}
        except ValueError as error:
            raise RuntimeError(f"{verb} {path}: reply is not JSON") from error
        return status, parsed if isinstance(parsed, dict) else {}

    def get(self, path, **query):
        return self.request("GET", path, query)[1]


def job_id_in(value):
    if not isinstance(value, dict):
        return ""
    for key in ("job_id", "jobId"):
        if value.get(key):
            return str(value[key])
    for key in ("job", "inference"):
        inner = value.get(key)
        if isinstance(inner, dict) and (inner.get("jobId") or inner.get("id")):
            return str(inner.get("jobId") or inner.get("id"))
    return ""


def required(parameter):
    value = parameter.get("required")
    return bool(value.get("always")) if isinstance(value, dict) else value in (True, "true")


def price_params(model, lane, presets):
    """Minimal inputs for a dry run: a prompt and the lane's presets; defaults are left to Scenario."""
    params = dict(presets)
    missing_files = []
    capabilities = model.get("capabilities", [])
    inputs = [p for p in model.get("inputs", []) if isinstance(p, dict) and p.get("name")]
    if capabilities and not any(c.startswith("txt") for c in capabilities):
        # Image-to-3D, 3D edit and riggers need an input asset even when no field is marked required.
        files = [p["name"] for p in inputs if p.get("type") in ("file", "file_array")]
        return params, files[:1] or ["an input asset"]
    for parameter in inputs:
        name = parameter["name"]
        if parameter.get("prompt") or name in ("prompt", "text"):
            if required(parameter) or name == "prompt":
                params.setdefault(name, lane["starter_prompts"][0])
        elif required(parameter) and parameter.get("type") in ("file", "file_array"):
            missing_files.append(name)
    return params, missing_files


def check(report, name, run):
    """A read the plugin depends on: reported, without failing the run."""
    try:
        report["checks"][name] = run()
    except Exception as error:  # noqa: BLE001 - every failure is reported, the run continues
        report["checks"][name] = "error: " + str(error)[:200]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--json", help="write the report as JSON to this path")
    parser.add_argument("--lane", help="only this lane")
    args = parser.parse_args()

    key = os.environ.get("SCENARIO_API_KEY", "").strip()
    secret = os.environ.get("SCENARIO_API_SECRET", "").strip()
    source = "environment"
    if not (key and secret):
        key, secret = keychain_credentials(os.environ.get("SCENARIO_KEYCHAIN_SERVICE", ""))
        source = "keychain"
    if not (key and secret):
        print("Set SCENARIO_API_KEY and SCENARIO_API_SECRET (or SCENARIO_KEYCHAIN_SERVICE on macOS).")
        return 2

    config = json.loads(LANES.read_text())
    api = Api(key, secret)
    report = {"started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "credential_source": source, "models": [], "checks": {}}
    failures = []
    started = time.time()

    # An API key cannot list teams: like the plugin, it reads its own project.
    projects = api.get("/projects", pageSize=100).get("projects", [])
    project = next((p for p in projects if isinstance(p, dict) and p.get("id")), {})
    report["checks"]["key_project"] = "found" if project else "none"
    if not project:
        failures.append(False)

    catalog = {}
    token = None
    for _ in range(20):
        page = api.get("/models", privacy="public", pageSize=500, paginationToken=token)
        catalog.update({row["id"]: row for row in page.get("models", []) if isinstance(row, dict) and row.get("id")})
        token = page.get("nextPaginationToken")
        if not token:
            break
    report["checks"]["catalog_public_models"] = len(catalog)

    seen = {}
    for lane in config["lanes"]:
        if args.lane and lane["id"] != args.lane:
            continue
        for model in lane["models"]:
            row = {"lane": lane["id"], "mode": model.get("mode", ""), "model": model["id"], "listed_name": model["name"]}
            if model["id"] in seen:
                row.update(seen[model["id"]])
                report["models"].append(row)
                continue
            listed = catalog.get(model["id"])
            tags = [t.lower() for t in (listed or {}).get("tags", [])]
            if listed is None:
                row["state"] = "not-in-catalog"
                failures.append(False)
                seen[model["id"]] = {"state": row["state"]}
                report["models"].append(row)
                continue
            if "sc:legacy" in tags or "deprecated" in tags:
                replacement = next((t.split(":", 1)[1] for t in listed.get("tags", []) if t.startswith("deprecated:")), "")
                row["state"] = "LEGACY"
                row["note"] = "replace with " + replacement if replacement else "marked legacy"
                failures.append(False)
                seen[model["id"]] = {"state": row["state"], "note": row["note"]}
                report["models"].append(row)
                continue
            try:
                record = api.get("/models/" + urllib.parse.quote(model["id"], safe=""))
                record = record.get("model") if isinstance(record.get("model"), dict) else record
                row["name"] = record.get("name", "")
                row["fields"] = len(record.get("inputs", []))
                row["capabilities"] = record.get("capabilities", [])
                params, missing = price_params(record, lane, config["presets"].get(model["id"], {}))
                if missing:
                    row["state"] = "needs-input"
                    row["note"] = "needs " + ", ".join(missing)
                else:
                    status, estimate = api.request("POST", "/generate/custom/" + urllib.parse.quote(model["id"], safe=""), {"dryRun": "true"}, params)
                    if job_id_in(estimate):
                        row["state"] = "ALARM"
                        row["note"] = "dry run returned a job " + job_id_in(estimate)
                        failures.append(False)
                    elif "creativeUnitsCost" in estimate:
                        row["state"] = "priced"
                        row["cu"] = estimate["creativeUnitsCost"]
                        row["status"] = status
                    else:
                        row["state"] = "no-price"
                        row["note"] = json.dumps(estimate)[:160]
                        failures.append(False)
            except Exception as error:  # noqa: BLE001 - every failure is reported, the run continues
                row["state"] = "error"
                row["note"] = str(error)[:200]
                failures.append(False)
            seen[model["id"]] = {k: v for k, v in row.items() if k not in ("lane", "mode", "model", "listed_name")}
            report["models"].append(row)

    def spark():
        body = {"mode": "contextual-v2", "modelId": "model_openai-gpt-image-2-5-flare", "prompt": "a treasure chest", "numResults": 1}
        status, reply = api.request("POST", "/generate/prompt", {"dryRun": "true"}, body)
        if job_id_in(reply):
            raise RuntimeError("ALARM: a dry run returned a job")
        return {"status": status, "creativeUnitsCost": reply.get("creativeUnitsCost")}

    check(report, "prompt_spark_dry_run", spark)
    check(report, "search_assets_kind_exists", lambda: len(api.request("POST", "/search/assets", None, {"filter": "kind EXISTS", "sortBy": ["createdAt:desc"], "limit": 2})[1].get("hits", [])))
    check(report, "assets_list", lambda: len(api.get("/assets", pageSize=2, sortBy="createdAt", sortDirection="desc").get("assets", [])))
    check(report, "workflows_featured", lambda: len(api.get("/workflows", privacy="public", tags="featured", pageSize=5, full="false").get("workflows", [])))
    check(report, "workflows_ready", lambda: len(api.get("/workflows", status="ready", pageSize=5, full="false").get("workflows", [])))
    check(report, "jobs_list", lambda: "jobs" in api.get("/jobs", pageSize=2))
    check(report, "usage_consumption_present", lambda: "consumption" in api.get("/usages", projectIds=project.get("id", ""), type="consumption", timeRange="last-thirty-one-days"))

    ok = not failures
    report["seconds"] = round(time.time() - started, 1)
    report["ok"] = ok
    for row in report["models"]:
        cu = f"{row['cu']:g} CU" if isinstance(row.get("cu"), (int, float)) else ""
        print(f"  {row['lane']:11s} {row.get('mode', ''):8s} {row['model']:45s} {row.get('state', ''):12s} {cu:9s} {row.get('name', '')[:28]:28s} {row.get('note', '')[:60]}")
    for name, value in report["checks"].items():
        print(f"  {name}: {value}")
    print("ok:", ok, f"({report['seconds']} s)")
    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(report, indent=2))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
