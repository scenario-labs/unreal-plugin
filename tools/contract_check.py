#!/usr/bin/env python3
# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
"""Free live contract check (no Creative Units spent).

Checks that the hosted Scenario MCP still answers the way the plugin expects:
sign-in with an API key, workspace listing, the schema and the dry-run price of
every lane model in Plugins/Scenario/Resources/Lanes.json, recommendations per
lane, model thumbnails through search, Prompt Spark pricing and the usage read.
Only reads and dry runs are sent. Paid calls are refused by this script itself.

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
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
LANES = ROOT / "Plugins/Scenario/Resources/Lanes.json"
ENDPOINT = os.environ.get("SCENARIO_MCP_URL", "https://mcp.scenario.com/mcp?toolsets=full")
PROTOCOL = "2025-03-26"
USER_AGENT = "ScenarioUnreal/contract-check"
PAID_TOOLS = {"model_run", "workflow_run", "prompt_spark", "upload_asset", "upload_asset_complete", "job_cancel"}


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


class Mcp:
    def __init__(self, key, secret):
        self.auth = "Basic " + base64.b64encode(f"{key}:{secret}".encode()).decode()
        self.session = ""
        self.next_id = 1

    def _post(self, payload):
        data = json.dumps(payload).encode()
        headers = {
            "Authorization": self.auth,
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
            "MCP-Protocol-Version": PROTOCOL,
            "User-Agent": USER_AGENT,
        }
        if self.session:
            headers["Mcp-Session-Id"] = self.session
        request = urllib.request.Request(ENDPOINT, data=data, headers=headers, method="POST")
        try:
            with urllib.request.urlopen(request, timeout=120) as response:
                body = response.read().decode("utf-8", "replace")
                self.session = response.headers.get("mcp-session-id", self.session)
                return response.status, response.headers.get("content-type", ""), body
        except urllib.error.HTTPError as error:
            return error.code, error.headers.get("content-type", ""), error.read().decode("utf-8", "replace")

    @staticmethod
    def _messages(content_type, body):
        if "text/event-stream" in content_type or body.lstrip().startswith(("data:", "event:")):
            out, lines = [], []
            for line in body.replace("\r\n", "\n").split("\n") + [""]:
                if not line:
                    if lines:
                        try:
                            out.append(json.loads("\n".join(lines)))
                        except ValueError:
                            pass
                        lines = []
                elif line.startswith("data:"):
                    lines.append(line[5:].lstrip(" ") if line[5:6] == " " else line[5:])
            return out
        try:
            parsed = json.loads(body) if body.strip() else []
        except ValueError:
            return []
        return parsed if isinstance(parsed, list) else [parsed]

    def rpc(self, method, params=None, notify=False):
        payload = {"jsonrpc": "2.0", "method": method}
        if not notify:
            payload["id"] = self.next_id
            self.next_id += 1
        if params:
            payload["params"] = params
        status, content_type, body = self._post(payload)
        if status >= 300:
            raise RuntimeError(f"{method}: HTTP {status}: {body[:300]}")
        if notify:
            return None
        found = None
        for message in self._messages(content_type, body):
            if isinstance(message, dict) and message.get("id") == payload["id"] and ("result" in message or "error" in message):
                found = message
        if not found:
            raise RuntimeError(f"{method}: no reply")
        if "error" in found:
            raise RuntimeError(f"{method}: {json.dumps(found['error'])[:300]}")
        return found["result"]

    def initialize(self):
        self.rpc("initialize", {"protocolVersion": PROTOCOL, "capabilities": {}, "clientInfo": {"name": "scenario-unreal-contract-check", "version": "0.2.0"}})
        self.rpc("notifications/initialized", notify=True)

    def call(self, tool, arguments):
        if tool in PAID_TOOLS and not arguments.get("dry_run"):
            raise RuntimeError(f"refused: {tool} without dry_run would spend")
        result = self.rpc("tools/call", {"name": tool, "arguments": arguments})
        if result.get("isError"):
            text = next((c.get("text", "") for c in result.get("content", []) if c.get("type") == "text"), "")
            raise RuntimeError(text[:400] or "tool error")
        if isinstance(result.get("structuredContent"), dict):
            return result["structuredContent"]
        for item in result.get("content", []):
            if item.get("type") == "text":
                try:
                    return json.loads(item["text"])
                except ValueError:
                    return {"text": item["text"]}
        return {}


def job_id_in(value):
    if not isinstance(value, dict):
        return ""
    for key in ("job_id", "jobId"):
        if value.get(key):
            return str(value[key])
    for key in ("job", "inference"):
        inner = value.get(key)
        if isinstance(inner, dict) and (inner.get("id") or inner.get("jobId")):
            return str(inner.get("id") or inner.get("jobId"))
    return ""


def price_params(schema, lane, presets):
    """Minimal parameters for a dry run: a prompt, presets, schema defaults are left to Scenario."""
    params = dict(presets)
    missing_files = []
    capabilities = schema.get("capabilities", [])
    if capabilities and not any(c.startswith("txt") for c in capabilities):
        # Image-to-3D, 3D edit and riggers need an input asset even when no field is marked required.
        inputs = [p.get("name") for p in schema.get("parameters", []) if p.get("type") in ("file", "file_array")]
        return params, inputs[:1] or ["an input asset"]
    for parameter in schema.get("parameters", []):
        name = parameter.get("name")
        kind = parameter.get("type")
        required = parameter.get("required") in (True, "true")
        if parameter.get("prompt") or name in ("prompt", "text"):
            if required or name == "prompt":
                params.setdefault(name, lane["starter_prompts"][0])
        elif required and kind in ("file", "file_array"):
            missing_files.append(name)
    return params, missing_files


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
    mcp = Mcp(key, secret)
    report = {"started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "credential_source": source, "models": [], "recommend": [], "checks": {}}
    ok = True
    started = time.time()
    mcp.initialize()
    teams = mcp.call("teams_list", {})
    report["checks"]["teams_list"] = f"{len(teams.get('teams', []))} team(s)"

    catalog = {}
    token = None
    for _ in range(40):
        page = mcp.call("models_list", {"limit": 100, "filters": {"privacy": "public"}, **({"page_token": token} if token else {})})
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
                ok = False
                seen[model["id"]] = {"state": row["state"]}
                report["models"].append(row)
                continue
            if "sc:legacy" in tags or "deprecated" in tags:
                replacement = next((t.split(":", 1)[1] for t in listed.get("tags", []) if t.startswith("deprecated:")), "")
                row["state"] = "LEGACY"
                row["note"] = "replace with " + replacement if replacement else "marked legacy"
                ok = False
                seen[model["id"]] = {"state": row["state"], "note": row["note"]}
                report["models"].append(row)
                continue
            try:
                schema = mcp.call("model_schema_get", {"model_id": model["id"]})
                row["name"] = schema.get("name", "")
                row["fields"] = len(schema.get("parameters", []))
                row["capabilities"] = schema.get("capabilities", [])
                params, missing = price_params(schema, lane, config["presets"].get(model["id"], {}))
                if missing:
                    row["state"] = "needs-input"
                    row["note"] = "needs " + ", ".join(missing)
                else:
                    estimate = mcp.call("model_run", {"model_id": model["id"], "parameters": params, "dry_run": True})
                    if job_id_in(estimate):
                        row["state"] = "ALARM"
                        row["note"] = "dry run returned a job " + job_id_in(estimate)
                        ok = False
                    elif "creativeUnitsCost" in estimate:
                        row["state"] = "priced"
                        row["cu"] = estimate["creativeUnitsCost"]
                    else:
                        row["state"] = "no-price"
                        row["note"] = json.dumps(estimate)[:160]
                        ok = False
            except Exception as error:  # noqa: BLE001 - every failure is reported, the run continues
                row["state"] = "error"
                row["note"] = str(error)[:200]
                ok = False
            seen[model["id"]] = {k: v for k, v in row.items() if k not in ("lane", "mode", "model", "listed_name")}
            report["models"].append(row)
        if lane.get("recommend", True):
            try:
                rec = mcp.call("recommend", {"capability": lane["capability"], "prompt": lane["starter_prompts"][0], "priority": "quality", "limit": 3})
                ranked = rec.get("ranked") or rec.get("models") or rec.get("results") or []
                first = ranked[0].get("model_id") or ranked[0].get("id") if ranked and isinstance(ranked[0], dict) else ""
                report["recommend"].append({"lane": lane["id"], "count": len(ranked), "first": first})
            except Exception as error:  # noqa: BLE001
                report["recommend"].append({"lane": lane["id"], "error": str(error)[:200]})

    try:
        found = mcp.call("search", {"target": "models", "query": "GPT Image", "public": True, "include_urls": True, "limit": 3})
        hits = found.get("results") or found.get("hits") or []
        report["checks"]["search_models_thumbnails"] = sum(1 for h in hits if isinstance(h, dict) and h.get("thumbnail"))
        report["checks"]["search_models_keys"] = sorted(hits[0].keys()) if hits and isinstance(hits[0], dict) else []
    except Exception as error:  # noqa: BLE001
        report["checks"]["search_models_thumbnails"] = "error: " + str(error)[:200]
    try:
        spark = mcp.call("prompt_spark", {"model_id": "model_openai-gpt-image-2-5-flare", "prompt": "a treasure chest", "dry_run": True})
        report["checks"]["prompt_spark_dry_run"] = {k: spark.get(k) for k in ("creativeUnitsCost", "dry_run") if k in spark} or json.dumps(spark)[:200]
        if job_id_in(spark):
            ok = False
            report["checks"]["prompt_spark_dry_run"] = "ALARM: job returned"
    except Exception as error:  # noqa: BLE001
        report["checks"]["prompt_spark_dry_run"] = "error: " + str(error)[:200]
    try:
        project = next((p.get("id") for t in teams.get("teams", []) for p in t.get("projects", [])), "")
        usage = mcp.call("usage", {"project_id": project} if project else {})
        report["checks"]["usage_totalCU_present"] = "totalCU" in (usage.get("totals") or {})
    except Exception as error:  # noqa: BLE001
        report["checks"]["usage_totalCU_present"] = "error: " + str(error)[:200]
    try:
        assets = mcp.call("search", {"target": "assets", "limit": 2, "sort_by": ["createdAt:desc"], "filter": "kind EXISTS"})
        report["checks"]["search_assets_kind_exists"] = len(assets.get("assets", []))
    except Exception as error:  # noqa: BLE001
        report["checks"]["search_assets_kind_exists"] = "error: " + str(error)[:200]
    try:
        flows = mcp.call("workflows_list", {"privacy": "public", "tags": ["featured"], "limit": 5})
        report["checks"]["workflows_featured"] = len(flows.get("workflows", []))
    except Exception as error:  # noqa: BLE001
        report["checks"]["workflows_featured"] = "error: " + str(error)[:200]
    try:
        flows = mcp.call("workflows_list", {"status": "ready", "limit": 5})
        report["checks"]["workflows_ready"] = len(flows.get("workflows", []))
    except Exception as error:  # noqa: BLE001
        report["checks"]["workflows_ready"] = "error: " + str(error)[:200]

    report["seconds"] = round(time.time() - started, 1)
    report["ok"] = ok
    for row in report["models"]:
        cu = f"{row['cu']:g} CU" if isinstance(row.get("cu"), (int, float)) else ""
        print(f"  {row['lane']:11s} {row.get('mode', ''):8s} {row['model']:45s} {row.get('state', ''):12s} {cu:9s} {row.get('name', '')[:28]:28s} {row.get('note', '')[:60]}")
    for rec in report["recommend"]:
        print(f"  recommend {rec['lane']:11s} {rec.get('count', '-')} first={rec.get('first', '')} {rec.get('error', '')}")
    for name, value in report["checks"].items():
        print(f"  {name}: {value}")
    print("ok:", ok, f"({report['seconds']} s)")
    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(report, indent=2))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
