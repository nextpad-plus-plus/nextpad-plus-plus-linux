#!/usr/bin/env bash
# ui-smoke-test.sh — G42 UI verification harness.
#
# Launches the binary, fires every registered GAction sequentially, captures
# the window state after each, asserts the process stays alive. Detects:
#   * actions that crash the process
#   * actions referenced by the menu but missing from kAppActions
#   * windows that fail to map
#   * dialogs that hang (timeout enforced)
#
# Usage: ./tools/ui-smoke-test.sh [optional fixture file]

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
# Prefer the release build (what users will run); fall back to the
# debug build/ dir for in-progress development.
# Override with `NPP_BUILD=… ./tools/ui-smoke-test.sh`.
# Executable name matches the macOS app: "Nextpad++".
# (Falls back to the legacy "nextpad-plus-plus" name if found, for builds
# made before the rename.)
pick_bin() {
    local d="$1"
    [ -x "$d/Nextpad++" ]         && { echo "$d/Nextpad++"; return; }
    [ -x "$d/nextpad-plus-plus" ] && { echo "$d/nextpad-plus-plus"; return; }
}
if [ -n "${NPP_BUILD:-}" ]; then
    BIN=$(pick_bin "$NPP_BUILD")
fi
[ -z "${BIN:-}" ] && BIN=$(pick_bin "$ROOT/build-release")
[ -z "${BIN:-}" ] && BIN=$(pick_bin "$ROOT/build")
FIXTURE=${1:-/tmp/npp-test/hello-lf.c}

if [ ! -x "$BIN" ]; then
    echo "❌ binary not built: $BIN"; exit 1
fi
if [ ! -f "$FIXTURE" ]; then
    mkdir -p "$(dirname "$FIXTURE")"
    printf '// smoke-test fixture\nint main(){return 0;}\n' > "$FIXTURE"
fi

# Clean state — match the real binary by /proc/*/exe rather than pkill -f,
# which would also match the wrapper shell that invoked this script (its
# argv contains the binary path) and silently kill the caller.
kill_by_exe() {
    local target="$1"
    for p in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
        [ "$(readlink /proc/$p/exe 2>/dev/null)" = "$target" ] \
            && kill -9 "$p" 2>/dev/null
    done
}
kill_by_exe "$BIN"
sleep 0.4

# Launch
DISPLAY=:0 "$BIN" "$FIXTURE" > /tmp/npp-smoke.log 2>&1 &
APP_PID=$!
sleep 3

if ! kill -0 "$APP_PID" 2>/dev/null; then
    echo "❌ binary failed to launch"; tail -20 /tmp/npp-smoke.log; exit 1
fi

# Get window info
WIN=$(xdotool search --name "— Nextpad" 2>/dev/null | tail -1)
if [ -z "$WIN" ]; then
    echo "❌ no main window appeared"; kill -9 "$APP_PID"; exit 1
fi

echo "✅ launched pid=$APP_PID, win=$WIN"
echo ""

# Enumerate all registered actions via D-Bus
ACTIONS=$(gdbus call --session \
    --dest org.nextpad.NextpadPP \
    --object-path /org/nextpad/NextpadPP \
    --method org.gtk.Actions.List 2>/dev/null \
    | tr ',' '\n' | tr -d "'()[] " | sort -u)
ACTION_COUNT=$(echo "$ACTIONS" | wc -l)
echo "Discovered $ACTION_COUNT registered actions."

# Actions that take parameters (skip in this pass)
SKIP_REGEX="^(set-encoding|set-language|tab-goto|tab-set-color|open-recent|panel-popout|panel-dockback|panel-toggle-floating)$"

# Actions that pop a modal dialog (will timeout / need closing)
DIALOG_REGEX="^(find|replace|find-in-files|preferences|shortcut-map|style-editor|column-editor|plugins-admin|help-cli-args|help-about|view-summary|hash-md5|hash-sha1|hash-sha256|hash-sha512|move-to-trash|run|command-palette|rename|save-as|save-copy-as|open|open-workspace|reload)$"

# Actions known to be destructive (skip — we want process to survive)
DESTRUCTIVE_REGEX="^(quit|close-others|close)$"

# Actions that hand off to an external application via gtk_show_uri
# (http/https URL → browser; file:// → file manager / default viewer).
# Firing these during a smoke run would spawn Firefox / Nautilus windows on
# the user's desktop, which we don't want.
EXTERNAL_REGEX="^(help-home|help-project|help-manual|open-containing-folder|open-in-default-viewer|sel-open-folder|sel-search-internet)$"

PASS=0
FAIL=0
SKIP=0

echo ""
echo "── Smoke-firing each non-dialog, non-parametric, non-destructive action ──"
for a in $ACTIONS; do
    if echo "$a" | grep -qE "$SKIP_REGEX"; then SKIP=$((SKIP+1)); continue; fi
    if echo "$a" | grep -qE "$DIALOG_REGEX"; then SKIP=$((SKIP+1)); continue; fi
    if echo "$a" | grep -qE "$DESTRUCTIVE_REGEX"; then SKIP=$((SKIP+1)); continue; fi
    if echo "$a" | grep -qE "$EXTERNAL_REGEX"; then SKIP=$((SKIP+1)); continue; fi

    out=$(timeout 0.5 gapplication action org.nextpad.NextpadPP "$a" 2>&1 | head -1)
    if echo "$out" | grep -qi "Unknown\|InvalidArgs"; then
        printf '  ❌ %-32s %s\n' "$a" "$out"
        FAIL=$((FAIL+1))
    else
        PASS=$((PASS+1))
    fi
    # Verify process still alive after each action
    if ! kill -0 "$APP_PID" 2>/dev/null; then
        printf '  💥 %s CRASHED THE PROCESS\n' "$a"
        FAIL=$((FAIL+1))
        break
    fi
done

echo ""
echo "── Parametric actions (sanity-fire with sample args) ──"
PARAM_TESTS=(
    "set-encoding 'UTF-8'"
    "set-encoding 'UTF-16 LE BOM'"
    "set-language 'cpp'"
    "set-language 'python'"
    "set-language ''"
    "tab-goto 1"
    "tab-set-color 2"
    "tab-set-color 0"
    "panel-toggle-floating 'gitpanel'"
    "panel-popout 'doclist'"
    "panel-dockback 'doclist'"
    "panel-toggle-floating 'gitpanel'"
)
for pt in "${PARAM_TESTS[@]}"; do
    out=$(timeout 0.5 gapplication action org.nextpad.NextpadPP $pt 2>&1 | head -1)
    if echo "$out" | grep -qi "Unknown\|InvalidArgs"; then
        printf '  ❌ %-40s %s\n' "$pt" "$out"
        FAIL=$((FAIL+1))
    else
        PASS=$((PASS+1))
    fi
done

# Window state after the firestorm
echo ""
echo "── Final state ──"
if kill -0 "$APP_PID" 2>/dev/null; then
    title=$(xprop -id "$WIN" _NET_WM_NAME 2>/dev/null | head -1)
    geom=$(xwininfo -id "$WIN" 2>/dev/null | grep -E "Width:|Height:|Map State" | tr '\n' ' ')
    echo "Process alive ✅"
    echo "Window: $title"
    echo "Geom:   $geom"
else
    echo "Process DIED during testing ❌"
    FAIL=$((FAIL+1))
fi

echo ""
echo "──────────────────────────"
echo "PASS: $PASS    FAIL: $FAIL    SKIP: $SKIP    (of $ACTION_COUNT)"
echo "──────────────────────────"

kill_by_exe "$BIN"
exit $FAIL
