#!/usr/bin/env bash
# Deploy sync-auth to Cloud Run with its secrets in Secret Manager.
#
#   ./deploy.sh <gcp-project> [region]
#
# First run: creates the secrets (prompting for each value without echo) and
# the service. Later runs: redeploys the code; secrets are left alone unless
# you pass --rotate, which prompts for new values. Nothing here writes a
# secret to disk, the shell history, or the build.
set -euo pipefail

PROJECT=${1:?usage: deploy.sh <gcp-project> [region] [--rotate]}
REGION=${2:-us-central1}
ROTATE=false
[[ "${3:-}" == "--rotate" || "${2:-}" == "--rotate" ]] && ROTATE=true
[[ "$REGION" == "--rotate" ]] && REGION=us-central1
SERVICE=sync-auth

gcloud config set project "$PROJECT" >/dev/null
gcloud services enable run.googleapis.com secretmanager.googleapis.com \
    cloudbuild.googleapis.com artifactregistry.googleapis.com >/dev/null

secret() {   # secret <NAME> <prompt>   - create or (with --rotate) add a version
    local name=$1 prompt=$2 value
    if gcloud secrets describe "$name" >/dev/null 2>&1 && ! $ROTATE; then
        echo "  $name: exists"
        return
    fi
    if [[ "$name" == HANDOFF_KEY ]]; then
        value=$(openssl rand -hex 32)
        echo "  $name: generated"
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
secret HANDOFF_KEY          ""

# The service runs as its own account with exactly one right: reading these.
SA="$SERVICE@$PROJECT.iam.gserviceaccount.com"
gcloud iam service-accounts describe "$SA" >/dev/null 2>&1 || \
    gcloud iam service-accounts create "$SERVICE" --display-name "sync-auth runtime" >/dev/null
for s in STRAVA_CLIENT_ID STRAVA_CLIENT_SECRET RWGPS_CLIENT_ID RWGPS_CLIENT_SECRET RWGPS_API_KEY HANDOFF_KEY; do
    gcloud secrets add-iam-policy-binding "$s" --member="serviceAccount:$SA" \
        --role=roles/secretmanager.secretAccessor >/dev/null
done

echo "deploying $SERVICE to $REGION..."
gcloud run deploy "$SERVICE" \
    --source "$(dirname "$0")" \
    --region "$REGION" \
    --service-account "$SA" \
    --allow-unauthenticated \
    --min-instances 0 --max-instances 3 --memory 256Mi --cpu 1 \
    --set-secrets "STRAVA_CLIENT_ID=STRAVA_CLIENT_ID:latest,STRAVA_CLIENT_SECRET=STRAVA_CLIENT_SECRET:latest,RWGPS_CLIENT_ID=RWGPS_CLIENT_ID:latest,RWGPS_CLIENT_SECRET=RWGPS_CLIENT_SECRET:latest,RWGPS_API_KEY=RWGPS_API_KEY:latest,HANDOFF_KEY=HANDOFF_KEY:latest"

URL=$(gcloud run services describe "$SERVICE" --region "$REGION" --format 'value(status.url)')
HOST=${URL#https://}
cat <<MSG

Deployed: $URL

Register these with the providers (once):
  Strava     Authorization Callback Domain:  $HOST
  RideWithGPS OAuth redirect URI:            $URL/v1/auth/ridewithgps/callback

Point the apps at it (not a secret, safe to commit):
  companion-ios/project.yml     OTP_SYNC_SERVICE_URL: $URL
  companion-android             sync.url=$URL   (local.properties or OTP_SYNC_SERVICE_URL in CI)
MSG
