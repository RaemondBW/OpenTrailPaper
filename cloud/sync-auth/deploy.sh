#!/usr/bin/env bash
# Deploy sync-auth to Cloud Run with its secrets in Secret Manager.
#
#   ./deploy.sh <gcp-project> [region] [--rotate [SECRET ...]]
#
# First run: creates the secrets (prompting for each value without echo, or
# as TODO placeholders when not on a terminal) and the service. Later runs:
# redeploys the code; secrets are left alone unless you pass --rotate, which
# prompts for new values - all of them, or only the names given after it,
# e.g. `--rotate STRAVA_CLIENT_ID STRAVA_CLIENT_SECRET`. Nothing here writes
# a secret to disk, the shell history, or the build.
set -euo pipefail

PROJECT=${1:?usage: deploy.sh <gcp-project> [region] [--rotate [SECRET ...]]}
shift
REGION=us-central1
ROTATE=false
ROTATE_ONLY=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --rotate) ROTATE=true ;;
        [A-Z_]*) $ROTATE && ROTATE_ONLY+=("$1") || REGION=$1 ;;
        *) REGION=$1 ;;
    esac
    shift
done
SERVICE=sync-auth

gcloud config set project "$PROJECT" >/dev/null
gcloud services enable run.googleapis.com secretmanager.googleapis.com \
    cloudbuild.googleapis.com artifactregistry.googleapis.com >/dev/null

secret() {   # secret <NAME> <prompt>   - create or (with --rotate) add a version
    local name=$1 prompt=$2 value
    if gcloud secrets describe "$name" >/dev/null 2>&1; then
        local wanted=false
        if $ROTATE; then
            if [[ ${#ROTATE_ONLY[@]} -eq 0 ]]; then wanted=true; fi
            for n in "${ROTATE_ONLY[@]:-}"; do [[ "$n" == "$name" ]] && wanted=true; done
        fi
        if ! $wanted; then echo "  $name: exists"; return; fi
    fi
    if [[ "$name" == HANDOFF_KEY ]]; then
        value=$(openssl rand -hex 32)
        echo "  $name: generated"
    elif [[ ! -t 0 ]]; then
        # Non-interactive first deploy: a placeholder the service refuses at
        # runtime (503 "not configured") until `deploy.sh --rotate` fills it.
        value=TODO
        echo "  $name: placeholder (fill in with --rotate)"
    else
        read -r -s -p "  $prompt: " value; echo
        [[ -n "$value" ]] || { echo "empty, aborting"; exit 1; }
    fi
    if gcloud secrets describe "$name" >/dev/null 2>&1; then
        printf '%s' "$value" | gcloud secrets versions add "$name" --data-file=- >/dev/null
    else
        printf '%s' "$value" | gcloud secrets create "$name" --replication-policy=automatic --data-file=- >/dev/null
    fi
}

echo "secrets:"
secret STRAVA_CLIENT_ID     "Strava client ID"
secret STRAVA_CLIENT_SECRET "Strava client secret"
secret RWGPS_CLIENT_ID      "RideWithGPS OAuth client ID"
secret RWGPS_CLIENT_SECRET  "RideWithGPS OAuth client secret"
secret RWGPS_API_KEY        "RideWithGPS API key"
secret INTERVALS_CLIENT_ID  "Intervals.icu OAuth client ID"
secret INTERVALS_CLIENT_SECRET "Intervals.icu OAuth client secret"
secret HANDOFF_KEY          ""

# The service runs as its own account with exactly one right: reading these.
SA="$SERVICE@$PROJECT.iam.gserviceaccount.com"
gcloud iam service-accounts describe "$SA" >/dev/null 2>&1 || \
    gcloud iam service-accounts create "$SERVICE" --display-name "sync-auth runtime" >/dev/null
for s in STRAVA_CLIENT_ID STRAVA_CLIENT_SECRET RWGPS_CLIENT_ID RWGPS_CLIENT_SECRET RWGPS_API_KEY INTERVALS_CLIENT_ID INTERVALS_CLIENT_SECRET HANDOFF_KEY; do
    gcloud secrets add-iam-policy-binding "$s" --member="serviceAccount:$SA" \
        --role=roles/secretmanager.secretAccessor >/dev/null
done

# Not secrets: who the apps are. App Check tokens must come from this Firebase
# project; the Android App Link is claimable only by these signing certs (the
# Play app-signing cert from Play Console > App integrity, plus the sideload
# key CI signs with). Override with ANDROID_CERT_SHA256 in the environment.
PROJECT_NUMBER=$(gcloud projects describe "$PROJECT" --format 'value(projectNumber)')
ANDROID_CERT_SHA256=${ANDROID_CERT_SHA256:-"BC:F2:78:B6:50:AE:6F:55:0A:69:55:61:43:90:77:B0:A8:11:A8:1D:73:F9:AB:9C:8C:13:6B:C3:B1:E4:FA:E4,1B:F3:F1:43:09:60:8D:EF:C3:57:79:7A:13:20:D5:D7:F7:D9:09:9C:A1:80:73:E7:E1:D1:56:F6:56:A2:E6:5C"}
BASE_URL=${BASE_URL:-https://sync.opentrailpaper.com}

echo "deploying $SERVICE to $REGION..."
gcloud run deploy "$SERVICE" \
    --source "$(dirname "$0")" \
    --region "$REGION" \
    --service-account "$SA" \
    --allow-unauthenticated \
    --min-instances 0 --max-instances 3 --memory 256Mi --cpu 1 \
    --set-env-vars "^|^APP_CHECK_PROJECT_NUMBER=$PROJECT_NUMBER|ANDROID_CERT_SHA256=$ANDROID_CERT_SHA256|BASE_URL=$BASE_URL" \
    --set-secrets "STRAVA_CLIENT_ID=STRAVA_CLIENT_ID:latest,STRAVA_CLIENT_SECRET=STRAVA_CLIENT_SECRET:latest,RWGPS_CLIENT_ID=RWGPS_CLIENT_ID:latest,RWGPS_CLIENT_SECRET=RWGPS_CLIENT_SECRET:latest,RWGPS_API_KEY=RWGPS_API_KEY:latest,INTERVALS_CLIENT_ID=INTERVALS_CLIENT_ID:latest,INTERVALS_CLIENT_SECRET=INTERVALS_CLIENT_SECRET:latest,HANDOFF_KEY=HANDOFF_KEY:latest"

RUN_URL=$(gcloud run services describe "$SERVICE" --region "$REGION" --format 'value(status.url)')
HOST=${BASE_URL#https://}
cat <<MSG

Deployed: $RUN_URL, serving as $BASE_URL once the domain mapping is in place:
  gcloud domains verify ${HOST#*.}        # once: Search Console, a TXT record
  gcloud beta run domain-mappings create --service $SERVICE --domain $HOST --region $REGION
  then in DNS: $HOST  CNAME  ghs.googlehosted.com   (DNS only, not proxied)

Register these with the providers (once):
  Strava     Authorization Callback Domain:  $HOST
  RideWithGPS OAuth redirect URI:            $BASE_URL/v1/auth/ridewithgps/callback
  Intervals.icu OAuth redirect URI:          $BASE_URL/v1/auth/intervals/callback

The apps already point at $BASE_URL (project.yml / build.gradle.kts).
MSG
