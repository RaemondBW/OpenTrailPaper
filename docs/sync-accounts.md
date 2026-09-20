# Strava, Intervals.icu and RideWithGPS accounts

The companion apps can upload a ride's `.fit` straight to Strava, Intervals.icu
or RideWithGPS (Settings > Accounts, then "Upload to …" on a ride's page). All
three use OAuth 2.0 with a **client secret**, and RideWithGPS also wants an
**API key** on every request. Neither can go inside an app: anything in an IPA or
APK is readable, and a leaked secret lets anyone act as the app. So a small
service at `https://sync.opentrailpaper.com` holds them, and the phones hold
only their own user's tokens.

```
 phone (attested)                 sync-auth (Cloud Run)                     provider
 ────────────────                 ─────────────────────                     ────────
 POST /v1/auth/<p>/begin {state} ──App Check──► ticket ──► {url}
 open url ──► /v1/auth/<p>/start?ticket ──302───────────────────────────► consent page
                                   /v1/auth/<p>/callback  ◄────────code───┘
                                      │ exchange (client secret stays here)
                                      │ tokens sealed in a 120 s AES-GCM handoff
 https://sync.opentrailpaper.com/app/sync/<p>?handoff=…&state=…  ◄──302──┘
   (a Universal Link / App Link: only the signed apps can claim it)
 POST /v1/auth/handoff {handoff} ──App Check──► tokens (Keychain / EncryptedSharedPreferences)

 Upload to Strava:        phone ──bearer──► strava.com/api/v3/uploads
 Upload to Intervals.icu: phone ──bearer──► intervals.icu/api/v1/athlete/0/activities?device_name=OpenTrailPaper
 Upload to RideWithGPS:   phone ──bearer + App Check──► /v1/rwgps/trips ──+ API key──► ridewithgps.com
```

## Only these apps

Two independent layers, both enforced by the service:

1. **App Check.** Every call to the service carries a Firebase App Check token
   in `X-Firebase-AppCheck`: on iOS from App Attest (the Secure Enclave
   attests the app's identity to Apple), on Android from Play Integrity
   (Google attests the APK's signature and the device). The service verifies
   the token against Firebase's public keys, its issuer and audience
   (project `opentrailpaper`, number 357305860460) and expiry. No token, no
   ticket to start a sign-in, no way to redeem a handoff, no proxied upload.
   The consent page itself is opened in a browser, which cannot carry a
   header, so `/start` accepts only a *ticket* that `/begin` issues to an
   attested app.
2. **Signed-app links.** The sign-in return is an https link on the
   service's own host. iOS opens `sync.opentrailpaper.com/app/sync/*` only in
   the app whose Apple team + bundle id the service's
   `/.well-known/apple-app-site-association` names; Android does the same
   for the package + signing certificates in `/.well-known/assetlinks.json`.
   If nothing claims the link (no app installed, or a build signed with an
   unlisted key) the service shows a page whose button retries on the
   `opentrailpaper://` scheme, and layer 1 still applies to whatever answers.

What the Firebase side needs is an app *identity* in each build (app id,
API key, project id, sender id). Those are identifiers, not secrets: the API
key is restricted in Google Cloud to the App Check and Installations APIs
and to this bundle id / these signing certificates, and a token is only
minted for an app that passes attestation. They sit in
`companion-ios/project.yml` and `companion-android/app/build.gradle.kts`.

Simulators and emulators cannot attest. There the apps install Firebase's
*debug provider*, which prints a debug token to the console once; register
it under Firebase console > App Check > Apps > Manage debug tokens and that
one device is accepted. (Android does this for every debug build, iOS only
on the simulator.)

The service is **stateless**: no database, no user records. It knows the
secrets (from Secret Manager) and nothing else. `state` is HMAC-signed so
the callback only accepts codes for a flow this service started, and the
app checks the same `state` on the way back. The handoff is opaque and
expires in two minutes.

Source: [`cloud/sync-auth/`](../cloud/sync-auth/) (Node 20, no dependencies,
`npm test` runs it against fake providers and a fake Firebase). Apps:
`companion-ios/Sources/SyncAccounts.swift`,
`companion-android/.../data/SyncAccounts.kt`.

## What exists (2026-09-19)

- Google Cloud project `opentrailpaper` (357305860460), billing linked,
  Cloud Run + Secret Manager enabled.
- Firebase on that project with the iOS app (`1:…:ios:b875…`, team
  G5JFC849XY, App Attest on) and the Android app (`1:…:android:8ada…`, Play
  Integrity on, upload + sideload certificate fingerprints registered).
- The service on Cloud Run (`us-central1`), reachable as
  `https://sync.opentrailpaper.com`: `opentrailpaper.com` is Google-verified,
  the domain mapping is up with its certificate, and Cloudflare holds
  `sync` CNAME `ghs.googlehosted.com` (proxied; that turned out to work, the
  certificate provisioned through it).
- Strava (client 280563) and RideWithGPS secrets are in Secret Manager; the
  attested flow through the domain reaches both consent pages, and the
  RideWithGPS API key is accepted. Intervals.icu's two values are still
  **placeholders**, so its sign-in answers 503 "not configured".

## Finishing the setup

1. **Intervals.icu.** Apply for an OAuth app at
   <https://intervals.icu/oauth/apply> (logged in as the owning athlete; apps
   are managed at <https://intervals.icu/settings/apps>) with redirect URI
   `https://sync.opentrailpaper.com/v1/auth/intervals/callback`. The service
   asks for scope `ACTIVITY:WRITE`; tokens do not expire and there is no
   refresh token. Then, from your own terminal (values prompted without echo,
   straight into Secret Manager, and the service redeployed):

   ```
   cd cloud/sync-auth && ./deploy.sh opentrailpaper --rotate INTERVALS_CLIENT_ID INTERVALS_CLIENT_SECRET
   ```

   The upload itself needs nothing from the service: the phone posts the
   FIT to `athlete/0/activities` with the user's bearer token, `device_name=
   OpenTrailPaper` and an `external_id` of the ride's filename (Intervals
   answers 201 for a new activity, 200 for one it already had).
2. **Strava** — <https://www.strava.com/settings/api>: confirm
   *Authorization Callback Domain* is `sync.opentrailpaper.com`. The access
   and refresh tokens shown on that page are your own account's and are not
   used; riders get theirs through Connect.
3. **Play app-signing certificate.** Play re-signs releases with its own
   key. Copy its SHA-256 from Play Console > App integrity into
   `ANDROID_CERT_SHA256` (deploy.sh) and add it to the Firebase Android app,
   or App Links and Play Integrity will only recognise sideloaded builds.
4. **Debug tokens** for any other simulator / emulator you test on (above).
   This Mac's iPhone 16 Pro Max simulator and the `scanner` emulator are
   registered.

Re-running `deploy.sh` redeploys the code and leaves the secrets alone.

## What must never be committed

- `STRAVA_CLIENT_SECRET`, `RWGPS_CLIENT_SECRET`, `RWGPS_API_KEY`,
  `INTERVALS_CLIENT_SECRET`, `HANDOFF_KEY` — Secret Manager only. `cloud/sync-auth/.env` is gitignored
  for local runs.

If a secret does leak: rotate it with the provider, then `deploy.sh
--rotate`. Users stay signed in (their tokens are unaffected by a
client-secret change; a new RWGPS API key needs no app update because the
key never left the service).

## Running locally

```
cd cloud/sync-auth
cp .env.example .env         # test-app credentials; leave APP_CHECK_PROJECT_NUMBER unset
set -a; . ./.env; set +a
npm start                    # http://localhost:8080, accepts unattested callers
```

The providers must be able to reach the callback, so for a real end-to-end
test either deploy or expose the port with a tunnel and set `BASE_URL` to it.
`npm test` needs neither.
