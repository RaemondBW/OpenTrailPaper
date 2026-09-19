# Strava and RideWithGPS accounts

The companion apps can upload a ride's `.fit` straight to Strava or RideWithGPS
(Settings > Accounts, then "Upload to …" on a ride's page). Both providers
use OAuth 2.0 with a **client secret**, and RideWithGPS also wants an **API
key** on every request. Neither can go inside an app: anything in an IPA or
APK is readable, and a leaked secret lets anyone act as the app. So a small
service holds them, and the phones hold only their own user's tokens.

```
 phone                         sync-auth (Cloud Run)                provider
 ─────                         ─────────────────────                ────────
 Connect ──► /v1/auth/<p>/start ──302──────────────────────────► consent page
                                 /v1/auth/<p>/callback  ◄──code──┘
                                    │ exchange (client secret here)
                                    │ tokens sealed in a 120 s AES-GCM handoff
 opentrailpaper://sync/<p>?handoff=…&state=…  ◄──302──┘
 POST /v1/auth/handoff {handoff} ──► tokens (Keychain / EncryptedSharedPreferences)

 Upload to Strava:      phone ──bearer──► strava.com/api/v3/uploads
 Upload to RideWithGPS: phone ──bearer──► /v1/rwgps/trips ──+ API key──► ridewithgps.com
```

The service is **stateless**: no database, no user records. It knows the
secrets (from Secret Manager) and nothing else. `state` is HMAC-signed so the
callback only accepts codes for a flow this service started, and the app
checks the same `state` on the way back so a stray redirect cannot plant a
token. The handoff is opaque and expires in two minutes.

Source: [`cloud/sync-auth/`](../cloud/sync-auth/) (Node 20, no dependencies,
`npm test` runs it against fake providers). Apps:
`companion-ios/Sources/SyncAccounts.swift`,
`companion-android/.../data/SyncAccounts.kt`.

## Deploying (once)

You need `gcloud` and a Google Cloud project with billing.

```
cd cloud/sync-auth
./deploy.sh <gcp-project> [region]
```

The script prompts (without echo) for each provider value, stores them in
Secret Manager, creates a service account whose only right is reading those
secrets, and deploys the service with them mounted as environment variables.
It prints the service URL and the two values to register with the providers:

- **Strava** — <https://www.strava.com/settings/api>: *Authorization
  Callback Domain* = the service host (`sync-auth-….a.run.app`). Note the
  Client ID and Client Secret; the app is registered once, for both phone
  platforms.
- **RideWithGPS** — <https://ridewithgps.com/settings/developers>: an API key
  for the app plus an OAuth client whose redirect URI is
  `https://<service host>/v1/auth/ridewithgps/callback`.

Re-running `deploy.sh` redeploys the code and leaves the secrets alone;
`--rotate` prompts for new values. The handoff key is generated on the
machine running the script and never shown.

## Pointing the apps at it

The service URL is public and safe to commit. It is the one thing the apps
need:

| App     | Where                                                          |
|---------|----------------------------------------------------------------|
| iOS     | `companion-ios/project.yml` → `OTP_SYNC_SERVICE_URL`            |
| Android | `local.properties` → `sync.url=…`, or `OTP_SYNC_SERVICE_URL` in CI |

With it empty the Accounts card says uploads are unavailable and the share
button keeps working.

## What must never be committed

- `STRAVA_CLIENT_SECRET`, `RWGPS_CLIENT_SECRET`, `RWGPS_API_KEY`,
  `HANDOFF_KEY` — Secret Manager only. `cloud/sync-auth/.env` is gitignored
  for local runs.
- The Strava client ID is not secret, but there is no reason to have it
  anywhere but the service either.

If a secret does leak: rotate it with the provider, then `deploy.sh --rotate`.
Users stay signed in (their tokens are unaffected by a client-secret change;
a new RWGPS API key needs no app update because the key never left the
service).

## Running locally

```
cd cloud/sync-auth
cp .env.example .env         # fill in test-app credentials
set -a; . ./.env; set +a
npm start                    # http://localhost:8080
```

The providers must be able to reach the callback, so for a real end-to-end
test either deploy or expose the port with a tunnel and set `BASE_URL` to it.
`npm test` needs neither.
