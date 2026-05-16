#!/usr/bin/env bash
# fetch_claude_usage.sh — Fetch Claude Code session and weekly usage percentages.
#
# Launches `claude /usage` via expect, captures CLI output, and extracts
# session and weekly usage percentages by parsing ANSI-stripped text.
#
# Requirements:
#   - expect (usually pre-installed on macOS, `apt install expect` on Linux)
#   - perl (for ANSI stripping)
#   - claude CLI binary
#
# Environment variables:
#   CLAUDE_BIN  - Path to claude binary (default: auto-detect via PATH or ~/.local/bin)
#
# Output (stdout):
#   JSON: {"session":{"percent":N},"weekly":{"percent":N}}
#
# Exit codes:
#   0 - Success (JSON always printed, even on parse failure with 0% fallback)
#   1 - Claude binary not found

# Find claude binary
if [ -n "$CLAUDE_BIN" ]; then
    :
elif command -v claude &>/dev/null; then
    CLAUDE_BIN=$(command -v claude)
elif [ -x "$HOME/.local/bin/claude" ]; then
    CLAUDE_BIN="$HOME/.local/bin/claude"
else
    echo '{"session":{"percent":0},"weekly":{"percent":0}}'
    exit 1
fi

TMPFILE=$(mktemp /tmp/claude_usage.XXXXXX)

export CLAUDECODE=

# Pass /usage as the initial command — skips welcome screen, much faster
/usr/bin/expect -c "
set timeout 10
log_file -noappend \"$TMPFILE\"
spawn \"$CLAUDE_BIN\" \"/usage\"
# Wait for Extra usage section which appears after both percentages
expect {Extra}
# Data captured, kill immediately
set pid [exp_pid]
exec kill \$pid
" >/dev/null

# Strip ANSI escape sequences and parse percentages
pcts=$(perl -pe '
    s/\e\[[0-9;?]*[a-zA-Z]/ /g;
    s/\e\][^\a\e]*(?:\a|\e\\)//g;
    s/\e[^\[].//g;
    s/[\x00-\x09\x0b-\x1f]//g;
    s/ +/ /g;
' "$TMPFILE" | grep -oE '[0-9]+% used' | head -2)

session_pct=$(echo "$pcts" | sed -n '1s/% used//p')
weekly_pct=$(echo "$pcts" | sed -n '2s/% used//p')

rm -f "$TMPFILE"

# Fallback if parsing failed
session_pct=${session_pct:-0}
weekly_pct=${weekly_pct:-0}

echo "{\"session\":{\"percent\":${session_pct}},\"weekly\":{\"percent\":${weekly_pct}}}"
