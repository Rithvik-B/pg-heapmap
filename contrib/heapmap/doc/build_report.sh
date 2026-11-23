#!/usr/bin/env bash
# Build doc/report.pdf from doc/report.html with the repository URL filled in.
#   doc/build_report.sh https://github.com/<user>/pg-heapmap
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
URL="${1:?repository URL}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
cp -r "$HERE/figures" "$TMP/"
sed "s|REPO_URL|$URL|g" "$HERE/report.html" > "$TMP/report.html"
CHROME="$(command -v google-chrome || command -v chromium || command -v chromium-browser)"
"$CHROME" --headless --disable-gpu --no-pdf-header-footer --print-to-pdf="$HERE/report.pdf" "file://$TMP/report.html" 2>/dev/null
echo "wrote $HERE/report.pdf"
