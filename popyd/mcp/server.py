#!/usr/bin/env python3
"""popy MCP server — wraps the `popy` CLI for AI agents.

Speaks JSON-RPC 2.0 over stdio (newline-delimited), the canonical MCP
transport. Exposes five tools:

  popy_fetch       — Mode A primary path: download a URL straight into the
                     quarantine (_popy + sidecar). The original-extension
                     filename never exists on disk.
  popy_list        — list quarantined files
  popy_read_text   — return the text contents of a quarantined file (raw
                     bytes; the agent decides what to do with them)
  popy_release     — release a quarantined file to a destination path
  popy_delete      — delete a quarantined file

Every tool subprocess-calls the `popy` binary with `--json`. Stdlib only —
no third-party MCP SDK, so the install footprint is one Python script.

Configuration:
  POPY_BIN    path to the `popy` binary (default: looked up in $PATH)
"""
import json
import os
import shutil
import subprocess
import sys


POPY = os.environ.get("POPY_BIN") or shutil.which("popy") or "popy"


def call_popy(*args: str) -> dict | list | None:
    """Run `popy <args> --json` and parse stdout as JSON. Raises on non-zero."""
    cmd = [POPY, *args, "--json"]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        msg = proc.stderr.strip() or f"popy exited {proc.returncode}"
        raise RuntimeError(msg)
    out = proc.stdout.strip()
    return json.loads(out) if out else None


def call_popy_raw(*args: str) -> bytes:
    """Run `popy <args>` (no --json) and return stdout bytes. Raises on non-zero."""
    cmd = [POPY, *args]
    proc = subprocess.run(cmd, capture_output=True)
    if proc.returncode != 0:
        msg = proc.stderr.decode("utf-8", "replace").strip() or "popy failed"
        raise RuntimeError(msg)
    return proc.stdout


# ---- Tool schemas -----------------------------------------------------------
TOOLS = [
    {
        "name": "popy_fetch",
        "description": (
            "Download a URL into the quarantine. Bytes stream straight into "
            "<stage>/<uuid>/<name>_popy mode 0000 — the original-extension "
            "filename never exists on disk. Use this instead of curl/wget for "
            "any download whose contents you have not yet inspected."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "url":  {"type": "string", "description": "https:// or http:// URL"},
                "out":  {"type": "string", "description": "override the saved filename"},
                "mime": {"type": "string", "description": "expected MIME; rejects on mismatch"},
            },
            "required": ["url"],
        },
    },
    {
        "name": "popy_list",
        "description": "List quarantined files (most-recent first). Returns metadata only.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "popy_read_text",
        "description": (
            "Return the raw contents of a quarantined file as text. Resolves "
            "by full UUID, UUID prefix (>=4 chars), or filename. Use this for "
            "text/markdown/JSON files; binary content will be returned with "
            "U+FFFD substitutions."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {"file": {"type": "string", "description": "id or name"}},
            "required": ["file"],
        },
    },
    {
        "name": "popy_release",
        "description": (
            "Release a quarantined file to a destination path (writing the "
            "original bytes, mode 0644, no _popy suffix). Removes from "
            "quarantine on success."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "file":  {"type": "string"},
                "to":    {"type": "string", "description": "absolute destination path"},
                "force": {"type": "boolean", "default": False},
            },
            "required": ["file", "to"],
        },
    },
    {
        "name": "popy_delete",
        "description": "Delete a quarantined file (unlink, no overwrite).",
        "inputSchema": {
            "type": "object",
            "properties": {"file": {"type": "string"}},
            "required": ["file"],
        },
    },
]


def call_tool(name: str, args: dict) -> dict:
    """Dispatch a tools/call invocation. Returns the MCP `content` payload."""
    if name == "popy_fetch":
        cli_args = ["fetch", args["url"]]
        if args.get("out"):  cli_args += ["--out",  args["out"]]
        if args.get("mime"): cli_args += ["--mime", args["mime"]]
        return _wrap_json(call_popy(*cli_args))
    if name == "popy_list":
        return _wrap_json(call_popy("list"))
    if name == "popy_read_text":
        # Read bytes via the CLI without --json; decode replacing invalid utf-8.
        body = call_popy_raw("read", args["file"], "--mode", "raw")
        return {"content": [{"type": "text", "text": body.decode("utf-8", "replace")}]}
    if name == "popy_release":
        cli_args = ["release", args["file"], "--to", args["to"]]
        if args.get("force"): cli_args.append("--force")
        return _wrap_json(call_popy(*cli_args))
    if name == "popy_delete":
        return _wrap_json(call_popy("delete", args["file"]))
    raise ValueError(f"unknown tool: {name}")


def _wrap_json(obj):
    return {"content": [{"type": "text", "text": json.dumps(obj, indent=2)}]}


# ---- JSON-RPC dispatch ------------------------------------------------------
def handle(req: dict) -> dict | None:
    method = req.get("method")
    if method == "initialize":
        return {
            "protocolVersion": "2024-11-05",
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "popy", "version": "0.1.0"},
        }
    if method == "notifications/initialized":
        return None  # notification — no response
    if method == "tools/list":
        return {"tools": TOOLS}
    if method == "tools/call":
        params = req.get("params", {})
        return call_tool(params["name"], params.get("arguments", {}))
    raise ValueError(f"unknown method: {method}")


def main() -> None:
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except json.JSONDecodeError:
            continue
        try:
            result = handle(req)
            if result is None:
                continue  # notification — silent
            resp = {"jsonrpc": "2.0", "id": req.get("id"), "result": result}
        except Exception as e:
            resp = {
                "jsonrpc": "2.0",
                "id": req.get("id"),
                "error": {"code": -32000, "message": str(e)},
            }
        sys.stdout.write(json.dumps(resp) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
