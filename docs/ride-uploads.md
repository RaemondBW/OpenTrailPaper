# Ride uploads

On iOS and Android, open **Settings → Upload services**, enter your personal
Intervals.icu API key (Intervals.icu Settings → Developer Settings), and save.
Download a ride from the head unit, open its details, then tap **Share →
Intervals.icu**. Share lists only connected services; Manage services lets you
connect or disconnect them. Cached rides can be uploaded without the head unit connected;
the phone needs internet access. The **Export** button opens the system share sheet for the original FIT file.
Disconnect removes the saved key. It does not delete rides on either service.

Uploads are manual. An in-flight request survives closing the detail sheet and
Android rotation. No background auto-sync or automatic POST retries are enabled.
The original FIT bytes are sent, with a stable SHA-256 external ID. Intervals.icu
also deduplicates by file content. A dropped connection has an uncertain outcome;
the UI says so and manual retry is safe. Confirmed receipts are cached per
provider, API-key fingerprint and file hash (never a filename alone). Changing
accounts cannot reuse the previous account's receipt. Changing the API key may
resubmit the file, which Intervals deduplicates on its side.

API keys are stored in iOS Keychain (unlocked, this device only) or in an atomic,
AES-GCM encrypted file under Android's no-backup directory, with the encryption
key held in Android Keystore. They are never stored on the head unit or logged.
The FIT file contains the recorded GPS track and sensor readings and is sent only
to the service selected in Share; opening the picker does not upload anything. Tests use synthetic credentials
and do not post rides to live accounts.

## Adding providers

`RideUploadService` is the boundary in each app: stable provider ID, display name,
credential-help URL, and an asynchronous `upload` returning a receipt. Add a
provider to `RideUploads.services` and supply its authentication/settings UI.
The detail screen, in-flight tracking, FIT transfer and recorder do not need a
service-specific code path. Strava is not advertised until API access and its
OAuth flow exist. The current Intervals integration supports personal API keys;
a distribution using registered OAuth credentials should add its authorization
flow rather than embed a client secret in either app.

## Contract and checks

- [Official API reference](https://intervals.icu/api-docs.html): multipart `file`
  POST to `/api/v1/athlete/0/activities`; response contains `activities[].id`;
  HTTP 201 means created, 200 means duplicate.
- [Authentication](https://forum.intervals.icu/t/api-access-to-intervals-icu/609):
  Basic username `API_KEY`, password the user's key; athlete `0` is its owner.
- `sh tools/upload_test/run.sh` tests the actual Swift request/response code.
- Android `:app:testDebugUnitTest` includes `RideUploadTest`: binary multipart,
  authentication, stable IDs, creation, duplicates, malformed responses and
  actionable errors. Both full app builds are also required.
- Live verification still needs a rider's API key: upload a downloaded FIT,
  check the resulting activity, retry it, replace/remove the key, and try offline.
