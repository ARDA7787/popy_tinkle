#!/usr/bin/env python3
"""popy MCP server — wraps the `popy` CLI for AI agents.

Speaks JSON-RPC 2.0 over stdio (newline-delimited), the canonical MCP
transport. Exposes five tools:

  popy_fetch       — Mode A primary path: download a URL straight into the
                     quarantine (_popy + HMAC-signed sidecar). The file is
                     mode 0000 from its first byte; the original-extension
                     filename never exists on disk.
  popy_list        — list quarantined files
  popy_read_text   — return the SANITIZED text contents of a quarantined
                     file (sidecar signature + content hash verified first;
                     control chars and invalid UTF-8 neutralised; capped)
  popy_verify      — verify a quarantined file's sidecar signature and
                     re-hash its content
  popy_release     — hash-verified, symlink-safe release to a destination
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
            "Download a URL into the quarantine. The file is mode 0000 from "
            "its very first byte (anonymous O_TMPFILE on Linux, a 0000 .part "
            "elsewhere) and only gets its final <stage>/<uuid>/<name>_popy "
            "name after its HMAC-signed sidecar is on disk — the "
            "original-extension filename never exists. Use this instead of "
            "curl/wget for any download whose contents you have not yet "
            "inspected."
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
            "Return the sanitized text contents of a quarantined file. The "
            "sidecar HMAC signature, size, and full content SHA-256 are "
            "verified before any byte is returned; C0/C1 control characters "
            "are stripped (except newline/tab) and invalid UTF-8 becomes "
            "U+FFFD. Content that looks binary (NUL in the head) is refused. "
            "Resolves by full UUID, UUID prefix (>=4 chars), or filename. "
            "Output is capped at max_bytes."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "file": {"type": "string", "description": "id or name"},
                "max_bytes": {
                    "type": "integer",
                    "default": 200000,
                    "description": "maximum bytes to read (truncates longer files)",
                },
            },
            "required": ["file"],
        },
    },
    {
        "name": "popy_verify",
        "description": (
            "Verify a quarantined file without opening it for use: checks "
            "the sidecar's HMAC signature and re-hashes the content against "
            "the recorded SHA-256. Returns signature/content status and an "
            "overall ok flag."
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
            "Release a quarantined file to a destination path. The sidecar "
            "signature is verified and the copied bytes are re-hashed "
            "against the recorded SHA-256 before the destination is "
            "committed atomically (a symlink planted at the destination is "
            "never followed). Writes mode 0644, no _popy suffix; removes "
            "from quarantine on success."
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
        # Verified + sanitized text via the CLI (never --mode raw here; raw
        # stays an explicit human debug path).
        max_bytes = int(args.get("max_bytes") or 200_000)
        body = call_popy_raw("read", args["file"], "--mode", "text",
                             "--max-bytes", str(max_bytes))
        return {"content": [{"type": "text", "text": body.decode("utf-8", "replace")}]}
    if name == "popy_verify":
        # `popy verify` exits 1 on failure but still prints its JSON verdict;
        # surface the verdict rather than a bare error where we can.
        proc = subprocess.run([POPY, "verify", args["file"], "--json"],
                              capture_output=True, text=True)
        out = proc.stdout.strip()
        if out:
            return _wrap_json(json.loads(out))
        raise RuntimeError(proc.stderr.strip() or "popy verify failed")
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
