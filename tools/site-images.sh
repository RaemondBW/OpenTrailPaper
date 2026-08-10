#!/bin/sh
# Puts the site's generated images where the site expects them.
#
# The device screens are rendered by tools/preview/render_preview.sh into
# tools/preview/out, and the app screenshots live in companion-ios/. Neither is
# duplicated into docs/ — docs/img/* is gitignored apart from the icons — so the
# repo carries one copy of each image and the gallery cannot drift from it.
#
# The catch is that this made `docs/` un-servable on its own: the deploy
# assembled the images in CI, so anyone previewing the site locally saw whatever
# stale copies happened to be in docs/img, which is exactly how the tour ended
# up showing a menu screen several firmware versions old. Both paths now go
# through this one script.
#
#   sh tools/site-images.sh              # refresh docs/img for a local preview
#   sh tools/site-images.sh _site/img    # assemble for deploy (see pages.yml)
#
# Run render_preview.sh first if the UI has changed.
set -e
cd "$(dirname "$0")/.."

DEST=${1:-docs/img}
mkdir -p "$DEST"

for f in dashboard map summary menu sensors nav_banner nav_prompt; do
    cp "tools/preview/out/$f.png" "$DEST/$f.png"
done

for f in route ride settings; do
    cp "companion-ios/$f.png" "$DEST/app_$f.png"
done

echo "site images → $DEST"
