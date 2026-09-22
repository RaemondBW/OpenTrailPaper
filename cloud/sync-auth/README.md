# sync-auth

The confidential half of Strava and RideWithGPS sign-in for the OpenTrailPaper
companion apps, as a Cloud Run service. Full design and setup:
[`docs/sync-accounts.md`](../../docs/sync-accounts.md).

    npm test                      # fake providers, no network, no secrets
    cp .env.example .env          # local run; .env is gitignored
    set -a; . ./.env; set +a; npm start
    ./deploy.sh <gcp-project>     # Cloud Run + Secret Manager
