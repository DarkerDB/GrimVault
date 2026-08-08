#!/usr/bin/env bash
#
# Vendor the ddb-tooltips dist into web/tooltips/ for the WebView2 Augment.
#
#    tools/build/sync-tooltips.sh [path-to-ddb-tooltips-checkout]
#
# Default source is the katforge realm checkout. Copies dist/ (tooltip.min.js,
# tooltip.css, assets/) plus a VERSION stamp read from package.json. Run
# `npm run build` in the tooltips package first if dist/ is stale.

set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
src="${1:-$HOME/.katforge/realms/darkerdb.com/tooltips}"
dst="$repo/web/tooltips"

[[ -f "$src/dist/tooltip.min.js" ]] || {
   echo "no dist at $src/dist — run 'npm run build' there first" >&2
   exit 1
}

version="$(sed -n 's/.*"version":[[:space:]]*"\([^"]*\)".*/\1/p' "$src/package.json" | head -1)"

rm -rf "$dst"
mkdir -p "$dst"
cp "$src/dist/tooltip.min.js" "$src/dist/tooltip.css" "$dst/"
cp -r "$src/dist/assets" "$dst/assets"
printf '%s\n' "$version" > "$dst/VERSION"

echo "vendored ddb-tooltips $version -> $dst"
