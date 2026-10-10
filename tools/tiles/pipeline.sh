#!/usr/bin/env bash
# The planet run, one step at a time — what .github/workflows/prebuilt-tiles.yml
# calls, and runnable locally against a directory instead of R2.
#
#   pipeline.sh plan     <workdir> [--only id,id,…] [--jobs N]
#   pipeline.sh interior <workdir> <job>        build + upload the job's regions
#   pipeline.sh border   <workdir> <job>        border cells, from <workdir>/strips
#   pipeline.sh merge    <workdir>              index.json, meta, deletions
#
# Storage: with R2_BUCKET set, objects go to Cloudflare R2 through rclone,
# configured from R2_ACCOUNT_ID / R2_ACCESS_KEY_ID / R2_SECRET_ACCESS_KEY.
# With LOCAL_BUCKET=<dir> instead, the "bucket" is that directory (tests:
# serve it with `python3 -m http.server --directory <dir>`). DRY_RUN=1 builds
# everything but writes nothing to the bucket.
#
# Workdir layout:
#   plan.json                     regions + jobs (plan.mjs)
#   frags/<job>/…                 this run's fragments, per job (artifacts)
#   strips/<region>.strip.ndjson.gz, <region>.todo.json   (artifacts)
#   out/                          tiles waiting for upload (deleted after)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
cmd=${1:?usage: pipeline.sh plan|interior|border|merge <workdir> …}
W=$(mkdir -p "${2:?workdir}" && cd "$2" && pwd)
shift 2
UA="OpenTrailPaper tile builder (github.com/RaemondBW/OpenTrailPaper)"
WORKERS=${WORKERS:-$(node -e 'console.log(require("os").availableParallelism())')}
log() { echo "[$(date -u +%H:%M:%S)] $*" >&2; }

# ---- storage ----------------------------------------------------------------
if [ -n "${R2_BUCKET:-}" ]; then
    export RCLONE_CONFIG_R2_TYPE=s3 RCLONE_CONFIG_R2_PROVIDER=Cloudflare
    export RCLONE_CONFIG_R2_ACCESS_KEY_ID="${R2_ACCESS_KEY_ID:?}" RCLONE_CONFIG_R2_SECRET_ACCESS_KEY="${R2_SECRET_ACCESS_KEY:?}"
    export RCLONE_CONFIG_R2_ENDPOINT="https://${R2_ACCOUNT_ID:?}.r2.cloudflarestorage.com"
    export RCLONE_CONFIG_R2_NO_CHECK_BUCKET=true RCLONE_CONFIG_R2_ACL=private
    REMOTE="r2:$R2_BUCKET"
elif [ -n "${LOCAL_BUCKET:-}" ]; then
    mkdir -p "$LOCAL_BUCKET"; REMOTE=""
else
    echo "set R2_BUCKET (+ R2_* credentials) or LOCAL_BUCKET" >&2; exit 2
fi

# Cache-Control by kind. Tile URLs carry ?v=<hash> from the index, so a
# changed tile is a new URL to the CDN; the week covers clients that ask
# without it (and is how long a stale copy can live at most).
cache_for() {
    case "$1" in
        *.ebm|*.poi)   echo "public, max-age=604800" ;;
        */index.json)  echo "public, max-age=3600" ;;
        *)             echo "public, max-age=300" ;;
    esac
}
type_for() { case "$1" in *.json) echo "application/json" ;; *) echo "application/octet-stream" ;; esac; }

# put <srcdir> <listfile>: upload the listed keys (paths relative to srcdir).
put() {
    local src=$1 list=$2
    [ -s "$list" ] || return 0
    if [ -n "${DRY_RUN:-}" ]; then log "dry run: would upload $(wc -l < "$list") objects"; return 0; fi
    if [ -n "$REMOTE" ]; then
        # One rclone call per Cache-Control class; --no-traverse/--no-check-dest:
        # no listing, no HEAD per object (each would be a billed operation).
        for kind in tile index other; do
            case $kind in
                tile)  grep -E '\.(ebm|poi)$' "$list" > "$list.$kind" || true ;;
                index) grep -E '/index\.json$' "$list" > "$list.$kind" || true ;;
                other) grep -vE '\.(ebm|poi)$|/index\.json$' "$list" > "$list.$kind" || true ;;
            esac
            [ -s "$list.$kind" ] || continue
            local first; first=$(head -1 "$list.$kind")
            rclone copy "$src" "$REMOTE" --files-from "$list.$kind" --no-traverse --no-check-dest \
                --transfers 64 --checkers 16 --retries 5 --low-level-retries 20 --stats 60s --stats-one-line \
                --header-upload "Cache-Control: $(cache_for "$first")" \
                --header-upload "Content-Type: $(type_for "$first")"
        done
    else
        rsync -a --files-from="$list" "$src/" "$LOCAL_BUCKET/"
    fi
    log "uploaded $(wc -l < "$list") objects"
}
# get <key> <dest>: fetch one object; false when absent.
get() {
    if [ -n "$REMOTE" ]; then rclone copyto "$REMOTE/$1" "$2" 2>/dev/null
    else [ -f "$LOCAL_BUCKET/$1" ] && cp "$LOCAL_BUCKET/$1" "$2"; fi
}
# get_dir <prefix> <dest>
get_dir() {
    mkdir -p "$2"
    if [ -n "$REMOTE" ]; then rclone copy "$REMOTE/$1" "$2" --transfers 32 || true
    elif [ -d "$LOCAL_BUCKET/$1" ]; then cp -R "$LOCAL_BUCKET/$1/." "$2/"; fi
}
# del <listfile>
del() {
    local list=$1
    [ -s "$list" ] || return 0
    if [ -n "${DRY_RUN:-}" ]; then log "dry run: would delete $(wc -l < "$list") objects"; return 0; fi
    if [ -n "$REMOTE" ]; then rclone delete "$REMOTE" --files-from "$list" --no-traverse
    else (cd "$LOCAL_BUCKET" && xargs rm -f < "$list"); fi
    log "deleted $(wc -l < "$list") objects"
}

jq_regions() { node -e 'const p=require(process.argv[1]);const j=p.jobs.find(j=>j.name===process.argv[2]);console.log((j?j.regions:[]).join("\n"))' "$W/plan.json" "$1"; }
region_url() { node -e 'const p=require(process.argv[1]);console.log(p.regions.find(r=>r.id===process.argv[2]).url)' "$W/plan.json" "$1"; }
safe() { echo "${1//\//_}"; }

case "$cmd" in
plan)
    curl -fsSL -A "$UA" -o "$W/index-v1.json" https://download.geofabrik.de/index-v1.json
    node "$here/plan.mjs" --index "$W/index-v1.json" --out "$W/plan.json" "$@"
    ;;

interior)
    job=${1:?job}
    mkdir -p "$W/frags/$job" "$W/strips" "$W/prev" "$W/dem"
    for r in $(jq_regions "$job"); do
        s=$(safe "$r")
        log "== $r"
        url=$(region_url "$r")
        pbf="$W/pbf/$s.osm.pbf"; mkdir -p "$W/pbf"
        curl -fsSL --retry 5 --retry-delay 10 -A "$UA" -o "$pbf" "$url"
        get "v1/regions/$s.json" "$W/prev/$s.json" || true
        rm -rf "$W/out"
        node --max-old-space-size=12000 "$here/build_region.mjs" --phase interior --region "$r" --regions "$W/plan.json" \
            --pbf "$pbf" --source-url "$url" --out "$W/out" --prev-dir "$W/prev" --strip-dir "$W/strips" \
            --dem-cache "$W/dem" --tmp "$W/tmp" --workers "$WORKERS"
        put "$W/out" "$W/out/upload.txt"
        cp "$W/out/v1/regions/$s.json" "$W/frags/$job/"
        rm -rf "$W/out" "$W/tmp" "$pbf"
        # The DEM cache is shared by neighbouring regions; keep it under ~5 GB.
        if [ "$(du -sm "$W/dem" | cut -f1)" -gt 5000 ]; then find "$W/dem" -name '*.tif' -delete; fi
    done
    ;;

border)
    job=${1:?job}
    mkdir -p "$W/frags/$job" "$W/prev"
    todo=$(jq_regions "$job" | paste -sd, -)
    for r in $(jq_regions "$job"); do get "v1/regions/$(safe "$r").border.json" "$W/prev/$(safe "$r").border.json" || true; done
    rm -rf "$W/out"
    node --max-old-space-size=12000 "$here/build_region.mjs" --phase border --todo "$todo" --regions "$W/plan.json" \
        --strip-dir "$W/strips" --out "$W/out" --prev-dir "$W/prev" --dem-cache "$W/dem" --tmp "$W/tmp" --workers "$WORKERS"
    put "$W/out" "$W/out/upload.txt"
    cp "$W/out"/v1/regions/*.border.json "$W/frags/$job/" 2>/dev/null || true
    rm -rf "$W/out" "$W/tmp"
    ;;

merge)
    rm -rf "$W/old" "$W/merged"
    get_dir v1/regions "$W/old"
    node "$here/merge_index.mjs" --new "$W/frags" --old "$W/old" --regions "$W/plan.json" --out "$W/merged"
    put "$W/merged" "$W/merged/upload.txt"
    del "$W/merged/delete.txt"
    ;;

*) echo "unknown command $cmd" >&2; exit 2 ;;
esac
