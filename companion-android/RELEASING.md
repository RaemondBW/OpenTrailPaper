# Releasing the Android companion to Google Play

Everything in the repo is ready to build a release bundle. What is not in the
repo, and deliberately cannot be, is the upload key and the Play credentials.
This is the sequence.

## 1. Create the upload key (once, ever)

```sh
keytool -genkeypair -v \
  -keystore ~/keys/opentrailpaper-upload.jks \
  -keyalg RSA -keysize 2048 -validity 10000 \
  -alias upload
```

Keep it **outside the repo** and back it up somewhere you would still have after
losing this machine. `*.jks`, `*.keystore` and `keystore.properties` are
gitignored so it cannot be committed by accident.

With Play App Signing (on by default for new apps) Google holds the *app*
signing key and this is only the *upload* key, so a lost upload key can be reset
by Google support rather than orphaning the app. That is a recovery path, not a
backup strategy.

## 2. Point the build at it

Create `companion-android/keystore.properties` — gitignored:

```properties
storeFile=/Users/you/keys/opentrailpaper-upload.jks
storePassword=…
keyAlias=upload
keyPassword=…
```

CI can supply the same four as `OTP_KEYSTORE_FILE`, `OTP_KEYSTORE_PASSWORD`,
`OTP_KEY_ALIAS`, `OTP_KEY_PASSWORD` instead.

With neither present the release build still runs and comes out **unsigned** —
useful for checking that a minified build compiles, useless for uploading. Play
rejects an unsigned bundle, which is the right place to find out.

## 3. Build the bundle

Neither the JDK nor the SDK is on `PATH` on this machine:

```sh
cd companion-android
export JAVA_HOME=/opt/homebrew/opt/openjdk@17
export ANDROID_HOME=/opt/homebrew/share/android-commandlinetools
./gradlew :app:bundleRelease
```

Output: `app/build/outputs/bundle/release/app-release.aab` (~6.3 MB).

Confirm it is signed before uploading:

```sh
unzip -l app/build/outputs/bundle/release/app-release.aab | grep -E 'META-INF/.*\.(RSA|EC)'
```

Nothing printed means unsigned — step 2 did not take.

### Versioning

`versionCode`/`versionName` are `10`/`0.3`, kept in step with
`companion-ios/project.yml` on purpose. **Every upload needs a `versionCode`
strictly higher than the last one Play has seen**, so the second release must
bump it — and by this repo's convention the iOS side moves with it.

## 4. Play Console, one-time setup

A closed test still needs most of the store paperwork done.

1. Developer account (US$25 once) at <https://play.google.com/console>.
2. **Create app** → name, default language, "App", free.
3. **App content**, all required before any track can go live:
   - **Privacy policy URL** — a reachable page. `docs/` is already published
     via GitHub Pages, so a page there is the cheapest route.
   - **Data safety** — see below.
   - Content rating questionnaire, target audience, ads declaration (none),
     government-apps and financial-features declarations (no).
   - **Foreground service permissions.** The app declares
     `FOREGROUND_SERVICE_LOCATION` for ride recording
     (`ble/RideLocationService.kt`, running only while the device reports it is
     recording). Play asks for a written justification and sometimes a short
     screen recording. **Expect this to be the slowest item.**

### What to tell the Data Safety form

Answer it from what the code does; these are the places it leaves the phone:

| Goes to | Where in the code | What is sent |
|---|---|---|
| CARTO basemap tiles | `map/MapStyle.kt` | tile coordinates being viewed |
| Nominatim (search) | `routing/Routing.kt` | the typed query, and current position as a bias |
| OSRM (directions) | `routing/Routing.kt` | start and destination coordinates |
| Overpass (map building) | `map/MapBuilder.kt`, `map/OsmData.kt` | the bounding box being downloaded |
| Open-Meteo (elevation) | `map/MapBuilder.kt` | tile corner coordinates |
| GitHub Releases (updates) | `data/FirmwareRelease.kt` | nothing but the request |

There is no account, no analytics and no crash reporting SDK. Location also goes
over BLE to the paired head unit, which is the user's own hardware rather than a
server. Precise location is genuinely used, so it has to be declared; whether
each of the above counts as "collected" or "shared" is a call to make against
Play's current definitions, not one to copy from here.

Permissions worth having an answer ready for: `BLUETOOTH_SCAN`/`CONNECT` (the
whole point of the app), `ACCESS_FINE_LOCATION` (BLE scanning below API 31, plus
sending the phone's position to the device), `CAMERA` (only to scan a mesh
channel's QR invite, only while that sheet is open, and optional — the invite
link can be pasted).

## 5. Upload to a closed track

**Testing → Closed testing → Create track** (or use the default "Alpha").

1. **Testers**: an email list or a Google Group. Testers must opt in through the
   link the Console gives you before they can see it.
2. **Create new release** → upload `app-release.aab`.
3. Release notes, then **Review release** → **Start rollout**.

First review typically takes a few hours to a couple of days.

Note for new personal developer accounts: Google requires 12 testers opted in
for 14 continuous days on a closed track before production access is granted.
That gate is on production, not on the closed test itself.

## 6. Optional: automate it

`fastlane` is already installed on this machine. To push from the command line
you need a Google Cloud service account with the Play Developer API enabled,
invited to the Play Console with release permissions, and its JSON key on disk
(keep it out of the repo). Then:

```sh
fastlane supply --aab app/build/outputs/bundle/release/app-release.aab \
  --track alpha --json_key ~/keys/play-service-account.json \
  --package_name com.raemond.opentrailpaper
```

Worth doing once the first manual upload has proved the listing is complete —
`supply` cannot create the app or fill in the store paperwork.

## Before you ship this particular build

- The **mesh feature has never run against real hardware**. It is protocol-
  complete against `src/ble_server.cpp` and the channel-invite codec is pinned
  by unit test to Meshtastic's own published bytes, but no packet has crossed a
  radio. A closed test is a reasonable place to learn that; it should be a
  decision, not a surprise.
- The branch `android-companion` is **not merged to `main`**.
