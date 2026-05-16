#!/bin/sh
# check-backend-isolation.sh
#
# Enforces the migration contract: NO file under src/ except sci_backend.h
# may reference a backend-specific Scintilla symbol. As long as this passes,
# swapping the GTK4 Scintilla backend is a one-file edit (src/sci_backend.h).
#
# Exit 0 = isolated. Exit 1 = a leak was found.
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../src"

# Symbols specific to the bugaevc backend. A future official backend would add
# its own names here. None of these may appear outside sci_backend.h.
pattern='scintilla_view\|ScintillaView\|SCINTILLA_VIEW\|SCINTILLA_IS_VIEW'

leaks=$(grep -rln "$pattern" "$src" 2>/dev/null | grep -v '/sci_backend\.h$' || true)

if [ -n "$leaks" ]; then
    echo "FAIL: backend-specific symbols leaked outside src/sci_backend.h:"
    echo "$leaks" | sed 's/^/  /'
    echo
    echo "Fix: route these through the canonical API in src/sci_backend.h."
    exit 1
fi

echo "OK: Scintilla backend is isolated to src/sci_backend.h."
