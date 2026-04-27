#!/usr/bin/env bash
# popy PreToolUse hook for Claude Code's Bash tool.
#
# Refuses any Bash command that downloads from the network (curl, wget, http,
# httpie) and tells the agent to use the popy_fetch MCP tool instead. This
# is the security control: AI agents cannot pull arbitrary bytes onto disk
# bypassing the quarantine.
#
# Wire this into ~/.claude/settings.json — see dist/claude-hooks.json for the
# exact stanza. Exit codes: 0 = allow, 2 = block (Claude Code shows the
# stderr message to the model so it can correct itself).
#
# Heuristic only — we look for the binary as the first non-flag word. Won't
# catch `bash -c '... curl ...'` or `eval $cmd`. Honest limit, documented.

set -euo pipefail

# Read tool invocation from stdin (Claude Code passes the tool call as JSON).
input="$(cat)"

# Extract the command. python3 is in macOS by default and on every Linux distro.
cmd="$(printf '%s' "$input" | python3 -c \
    'import json,sys; print(json.load(sys.stdin)["tool_input"].get("command",""))')"

# First non-flag, non-redirection word.
first="$(printf '%s\n' "$cmd" | awk '{
    for (i=1; i<=NF; i++) if ($i !~ /^[-<>|&]/) { print $i; exit }
}')"

case "${first##*/}" in
    curl|wget|http|https|httpie|aria2c|axel)
        url="$(printf '%s\n' "$cmd" | grep -oE 'https?://[^[:space:]\"'"'"']+' | head -1)"
        printf 'popy: refusing direct download. Use the `popy_fetch` MCP tool ' >&2
        printf 'so the file lands quarantined as <name>_popy mode 0000 ' >&2
        printf 'instead of in your filesystem.\n' >&2
        if [ -n "$url" ]; then
            printf '\nSuggested call:\n  popy_fetch(url=%s)\n' "$url" >&2
            printf '\nOr from a shell: popy fetch %s\n' "$url" >&2
        fi
        exit 2
        ;;
esac

exit 0
