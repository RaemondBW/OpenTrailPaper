// OpenTrailPaper sync-auth: the confidential half of the Strava and
// RideWithGPS OAuth flows, as a Cloud Run service.
//
// Why a service at all: both providers hand out a client secret that must
// never ship inside a phone app, and RideWithGPS additionally wants its API
// key on every request. So the phones never see either. The service:
//
//   POST /v1/auth/:provider/begin {state:S}    [App Check] -> {url}: the consent
//        URL, carrying a ticket only an attested app can obtain
//   GET  /v1/auth/:provider/start?ticket=T      -> 302 to the provider's consent page
//   GET  /v1/auth/:provider/callback            <- the provider sends the code here
//        exchanges it (secret stays here), wraps the tokens in a short-lived
//        AES-GCM "handoff" and 302s to  https://<host>/app/sync/:provider?handoff=..&state=S
//        - a Universal Link / App Link only the signed apps can claim
//   GET  /app/sync/:provider                    fallback page when no app claimed it
//   POST /v1/auth/handoff        {handoff}      [App Check] -> {provider, tokens..., athlete}
//   POST /v1/auth/strava/refresh {refresh_token} [App Check]
//   POST /v1/auth/strava/revoke  {access_token}  [App Check]
//   POST /v1/rwgps/trips         (multipart, Bearer user token) [App Check] -> proxied
//                                upload, the API key added here
//   GET  /v1/rwgps/me            (Bearer user token) [App Check] -> the account
//   GET  /.well-known/apple-app-site-association, /.well-known/assetlinks.json
//
// "Only my apps": two layers. [App Check] routes require a Firebase App Check
// token (App Attest on iOS, Play Integrity on Android) minted for one of this
// project's apps, verified here against Firebase's public keys; without a
// valid one there is no ticket to start a flow and no way to redeem a handoff.
// And the handoff travels back on an https link on this host, which iOS and
// Android only hand to apps whose signing identity the well-known files list.
// App Check is on whenever APP_CHECK_PROJECT_NUMBER is set; unset it only for
// a local run against the tests' fakes.
//
// Stateless on purpose: nothing is stored server-side, so there is no user
// database to protect. What the app keeps is its own user's tokens; what the
// service keeps is the secrets, read from the environment (Cloud Run mounts
// them from Secret Manager - see deploy.sh). Uploads to Strava go straight
// from the phone with the user's bearer token; only RWGPS is proxied, because
// of the API key.
//
// Zero dependencies: node:http, node:crypto and the global fetch of Node 20.

'use strict';

const http = require('node:http');
const crypto = require('node:crypto');

const PORT = Number(process.env.PORT || 8080);
const APP_SCHEME = process.env.APP_SCHEME || 'opentrailpaper';
const HANDOFF_TTL_S = 120;
const STATE_TTL_S = 600;

// App identity, for the well-known files and App Check.
const APP = {
    appleTeamId: process.env.APPLE_TEAM_ID || 'G5JFC849XY',
    iosBundleId: process.env.IOS_BUNDLE_ID || 'com.raemond.opentrailpaper',
    androidPackage: process.env.ANDROID_PACKAGE || 'com.raemond.opentrailpaper',
    // SHA-256 fingerprints of every certificate the Android app may be signed
    // with: the Play app-signing key (from Play Console > App integrity) plus
    // the sideload key CI signs with. Comma separated, colon-hex.
    androidCertSha256: (process.env.ANDROID_CERT_SHA256 || '').split(',').map((s) => s.trim()).filter(Boolean),
    appCheckProjectNumber: process.env.APP_CHECK_PROJECT_NUMBER || '',
    // Optional: Firebase app ids (1:123:ios:abc...) allowed as the token's
    // subject. Empty = any app of the project.
    appCheckAppIds: (process.env.APP_CHECK_APP_IDS || '').split(',').map((s) => s.trim()).filter(Boolean),
    appCheckJwks: process.env.APP_CHECK_JWKS_URL || 'https://firebaseappcheck.googleapis.com/v1/jwks',
};

// ---- provider definitions ---------------------------------------------------
// URLs per the Strava API v3 docs and the RideWithGPS API v1 docs
// (https://github.com/ridewithgps/developers). Kept in one place so a doc
// change is a one-line edit.
const PROVIDERS = {
    strava: {
        authorize: 'https://www.strava.com/oauth/authorize',
        token: 'https://www.strava.com/oauth/token',
        revoke: 'https://www.strava.com/oauth/deauthorize',
        scope: 'read,activity:write',
        clientId: () => env('STRAVA_CLIENT_ID'),
        clientSecret: () => env('STRAVA_CLIENT_SECRET'),
    },
    ridewithgps: {
        authorize: 'https://ridewithgps.com/oauth/authorize',
        token: 'https://ridewithgps.com/oauth/token',
        api: 'https://ridewithgps.com/api/v1',
        clientId: () => env('RWGPS_CLIENT_ID'),
        clientSecret: () => env('RWGPS_CLIENT_SECRET'),
        apiKey: () => env('RWGPS_API_KEY'),
    },
};

function env(name) {
    const v = process.env[name];
    if (!v || v === 'TODO') throw new HttpError(503, `${name} is not configured`);   // TODO = deploy.sh placeholder
    return v;
}

class HttpError extends Error {
    constructor(status, message) { super(message); this.status = status; }
}

// ---- crypto: signed state, encrypted handoff --------------------------------
// One 32-byte key (HANDOFF_KEY, hex) does both: HMAC for the OAuth `state`
// (so the callback can trust it came from our /start) and AES-256-GCM for the
// handoff (so the tokens cross the browser -> app hop opaque and expiring).

function key() {
    const hex = env('HANDOFF_KEY');
    if (!/^[0-9a-f]{64}$/i.test(hex)) throw new HttpError(503, 'HANDOFF_KEY must be 32 bytes hex');
    return Buffer.from(hex, 'hex');
}

const b64u = {
    enc: (buf) => Buffer.from(buf).toString('base64url'),
    dec: (s) => Buffer.from(String(s), 'base64url'),
};

function signState(appState, provider) {
    const body = b64u.enc(JSON.stringify({ s: appState, p: provider, exp: now() + STATE_TTL_S }));
    const mac = crypto.createHmac('sha256', key()).update(body).digest();
    return `${body}.${b64u.enc(mac)}`;
}

function verifyState(state, provider) {
    const [body, mac] = String(state || '').split('.');
    if (!body || !mac) throw new HttpError(400, 'bad state');
    const want = crypto.createHmac('sha256', key()).update(body).digest();
    const got = b64u.dec(mac);
    if (got.length !== want.length || !crypto.timingSafeEqual(got, want)) throw new HttpError(400, 'bad state');
    const obj = JSON.parse(b64u.dec(body).toString('utf8'));
    if (obj.p !== provider) throw new HttpError(400, 'state is for another provider');
    if (obj.exp < now()) throw new HttpError(400, 'state expired');
    return obj.s;
}

function sealHandoff(payload) {
    const iv = crypto.randomBytes(12);
    const c = crypto.createCipheriv('aes-256-gcm', key(), iv);
    const pt = Buffer.from(JSON.stringify({ ...payload, exp: now() + HANDOFF_TTL_S }));
    const ct = Buffer.concat([c.update(pt), c.final()]);
    return b64u.enc(Buffer.concat([iv, c.getAuthTag(), ct]));
}

function openHandoff(token) {
    const buf = b64u.dec(token);
    if (buf.length < 12 + 16 + 2) throw new HttpError(400, 'bad handoff');
    const d = crypto.createDecipheriv('aes-256-gcm', key(), buf.subarray(0, 12));
    d.setAuthTag(buf.subarray(12, 28));
    let pt;
    try {
        pt = Buffer.concat([d.update(buf.subarray(28)), d.final()]);
    } catch {
        throw new HttpError(400, 'bad handoff');
    }
    const obj = JSON.parse(pt.toString('utf8'));
    if (obj.exp < now()) throw new HttpError(410, 'handoff expired');
    delete obj.exp;
    return obj;
}

const now = () => Math.floor(Date.now() / 1000);

// ---- provider calls ---------------------------------------------------------

async function exchangeCode(provider, code, redirectUri) {
    const p = PROVIDERS[provider];
    const form = new URLSearchParams({
        client_id: p.clientId(),
        client_secret: p.clientSecret(),
        code,
        grant_type: 'authorization_code',
        redirect_uri: redirectUri,
    });
    const r = await fetch(p.token, {
        method: 'POST',
        headers: { 'content-type': 'application/x-www-form-urlencoded', accept: 'application/json' },
        body: form,
    });
    const json = await r.json().catch(() => ({}));
    if (!r.ok) throw new HttpError(502, `${provider} token exchange failed: ${r.status} ${JSON.stringify(json).slice(0, 200)}`);
    return json;
}

async function stravaRefresh(refreshToken) {
    const p = PROVIDERS.strava;
    const form = new URLSearchParams({
        client_id: p.clientId(),
        client_secret: p.clientSecret(),
        grant_type: 'refresh_token',
        refresh_token: refreshToken,
    });
    const r = await fetch(p.token, {
        method: 'POST',
        headers: { 'content-type': 'application/x-www-form-urlencoded', accept: 'application/json' },
        body: form,
    });
    const json = await r.json().catch(() => ({}));
    if (!r.ok) throw new HttpError(r.status === 400 || r.status === 401 ? 401 : 502, `strava refresh failed: ${r.status}`);
    return {
        access_token: json.access_token,
        refresh_token: json.refresh_token,
        expires_at: json.expires_at,
    };
}

async function stravaRevoke(accessToken) {
    const r = await fetch(PROVIDERS.strava.revoke, {
        method: 'POST',
        headers: { 'content-type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({ access_token: accessToken }),
    });
    // 401 = already gone; either way the app forgets the token.
    if (!r.ok && r.status !== 401) throw new HttpError(502, `strava deauthorize failed: ${r.status}`);
}

function rwgpsHeaders(userToken, extra = {}) {
    return {
        'x-rwgps-api-key': PROVIDERS.ridewithgps.apiKey(),
        authorization: `Bearer ${userToken}`,
        accept: 'application/json',
        ...extra,
    };
}

async function rwgpsCurrentUser(userToken) {
    const r = await fetch(`${PROVIDERS.ridewithgps.api}/users/current.json`, { headers: rwgpsHeaders(userToken) });
    const json = await r.json().catch(() => ({}));
    if (!r.ok) throw new HttpError(r.status === 401 ? 401 : 502, `ridewithgps user lookup failed: ${r.status}`);
    // v1 wraps it as {user: {...}}; be lenient.
    const u = json.user || json;
    return { id: u.id, name: u.name || u.display_name || null };
}

// ---- Firebase App Check -----------------------------------------------------
// A token is an RS256 JWT from Firebase: iss https://firebaseappcheck.googleapis.com/<n>,
// aud includes projects/<n>, sub = the app id. Keys come from the JWKS
// endpoint (cached; refetched on an unknown kid). No SDK needed.

let jwksCache = { keys: [], at: 0 };

async function jwks(forceRefresh) {
    if (!forceRefresh && jwksCache.keys.length && now() - jwksCache.at < 6 * 3600) return jwksCache.keys;
    const r = await fetch(APP.appCheckJwks, { headers: { accept: 'application/json' } });
    if (!r.ok) throw new HttpError(503, 'app check keys unavailable');
    const json = await r.json();
    jwksCache = { keys: json.keys || [], at: now() };
    return jwksCache.keys;
}

async function verifyAppCheck(token) {
    const parts = String(token || '').split('.');
    if (parts.length !== 3) throw new HttpError(401, 'app check token malformed');
    let header, claims;
    try {
        header = JSON.parse(b64u.dec(parts[0]).toString('utf8'));
        claims = JSON.parse(b64u.dec(parts[1]).toString('utf8'));
    } catch { throw new HttpError(401, 'app check token malformed'); }
    if (header.alg !== 'RS256' || header.typ !== 'JWT') throw new HttpError(401, 'app check token unsupported');
    let key = (await jwks(false)).find((k) => k.kid === header.kid);
    if (!key) key = (await jwks(true)).find((k) => k.kid === header.kid);
    if (!key) throw new HttpError(401, 'app check key unknown');
    const pub = crypto.createPublicKey({ key, format: 'jwk' });
    const ok = crypto.verify('RSA-SHA256', Buffer.from(`${parts[0]}.${parts[1]}`), pub, b64u.dec(parts[2]));
    if (!ok) throw new HttpError(401, 'app check signature invalid');
    const n = APP.appCheckProjectNumber;
    if (claims.iss !== `https://firebaseappcheck.googleapis.com/${n}`) throw new HttpError(401, 'app check issuer');
    const aud = Array.isArray(claims.aud) ? claims.aud : [claims.aud];
    if (!aud.includes(`projects/${n}`)) throw new HttpError(401, 'app check audience');
    if (!claims.exp || claims.exp < now()) throw new HttpError(401, 'app check token expired');
    if (APP.appCheckAppIds.length && !APP.appCheckAppIds.includes(claims.sub)) throw new HttpError(401, 'app check app');
    return claims.sub;
}

/* Gate: throws unless the request carries a valid App Check token (or App
 * Check is not configured, which only a local run should ever be). */
async function requireApp(req) {
    if (!APP.appCheckProjectNumber) {
        if (!requireApp.warned) { console.warn('APP_CHECK_PROJECT_NUMBER unset: accepting unattested callers'); requireApp.warned = true; }
        return 'unverified';
    }
    return verifyAppCheck(req.headers['x-firebase-appcheck']);
}

// ---- Universal Links / App Links ---------------------------------------------

function appleAppSiteAssociation() {
    const appID = `${APP.appleTeamId}.${APP.iosBundleId}`;
    return {
        applinks: {
            details: [{ appIDs: [appID], components: [{ '/': '/app/sync/*', comment: 'Strava / RideWithGPS sign-in return' }] }],
        },
        webcredentials: { apps: [appID] },
    };
}

function assetLinks() {
    return [{
        relation: ['delegate_permission/common.handle_all_urls'],
        target: {
            namespace: 'android_app',
            package_name: APP.androidPackage,
            sha256_cert_fingerprints: APP.androidCertSha256,
        },
    }];
}

/* Where the callback sends the user. Only the signed apps can claim this
 * https link (see the well-known files); it also works with no app installed,
 * as a page (below). */
function appReturnUrl(req, provider) {
    return new URL(`${baseUrl(req)}/app/sync/${provider}`);
}

function returnPage(provider, query) {
    // Reached when the OS did not hand the link to the app directly (the
    // in-app auth browser on iOS, a developer build not associated with the
    // domain, or no app installed). It immediately continues on the custom
    // scheme with the same parameters - inside ASWebAuthenticationSession or a
    // Custom Tab that is exactly the callback the app is waiting for - and
    // leaves a button for a browser that blocks the automatic hop. A hijacking
    // app gets nothing from the scheme: redeeming the handoff needs App Check.
    const deep = `${APP_SCHEME}://sync/${provider}${query ? `?${query}` : ''}`;
    const esc = (s) => s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
    return `<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>OpenTrailPaper</title>
<script>location.replace(${JSON.stringify(deep)});</script>
<style>body{font-family:-apple-system,system-ui,sans-serif;background:#EDEAE1;color:#1A1A1A;margin:0;padding:48px 24px;text-align:center}
a.b{display:inline-block;margin-top:24px;padding:14px 22px;background:#F4501E;color:#fff;border-radius:12px;text-decoration:none;font-weight:600}p{color:#5C564B}</style>
<h1>Almost there</h1><p>Finish connecting ${esc(provider === 'strava' ? 'Strava' : 'RideWithGPS')} in the OpenTrailPaper app.</p>
<a class="b" href="${esc(deep)}">Open OpenTrailPaper</a>
<p>No app? This link only does something on a phone with OpenTrailPaper installed.</p>`;
}

// ---- request plumbing -------------------------------------------------------

function baseUrl(req) {
    if (process.env.BASE_URL) return process.env.BASE_URL.replace(/\/$/, '');
    // Cloud Run terminates TLS and forwards the original host/proto.
    const proto = req.headers['x-forwarded-proto'] || 'http';
    return `${proto}://${req.headers.host}`;
}

function readBody(req, limit = 1 << 20) {
    return new Promise((resolve, reject) => {
        const chunks = [];
        let n = 0;
        req.on('data', (c) => {
            n += c.length;
            if (n > limit) { reject(new HttpError(413, 'body too large')); req.destroy(); return; }
            chunks.push(c);
        });
        req.on('end', () => resolve(Buffer.concat(chunks)));
        req.on('error', reject);
    });
}

async function readJson(req) {
    const buf = await readBody(req);
    if (!buf.length) return {};
    try { return JSON.parse(buf.toString('utf8')); } catch { throw new HttpError(400, 'invalid JSON'); }
}

function send(res, status, body, headers = {}) {
    const isJson = body !== undefined && typeof body !== 'string';
    const data = isJson ? JSON.stringify(body) : (body || '');
    res.writeHead(status, {
        'content-type': isJson ? 'application/json' : 'text/plain; charset=utf-8',
        'cache-control': 'no-store',
        ...headers,
    });
    res.end(data);
}

function redirect(res, location) {
    res.writeHead(302, { location, 'cache-control': 'no-store' });
    res.end();
}

function bearer(req) {
    const m = /^Bearer\s+(.+)$/i.exec(req.headers.authorization || '');
    if (!m) throw new HttpError(401, 'missing bearer token');
    return m[1];
}

function providerOf(name) {
    if (!PROVIDERS[name]) throw new HttpError(404, 'unknown provider');
    return name;
}

// ---- routes -----------------------------------------------------------------

async function handle(req, res) {
    const url = new URL(req.url, 'http://x');
    const path = url.pathname.replace(/\/+$/, '') || '/';
    const m = (method, re) => req.method === method && re.exec(path);
    let r;

    if (m('GET', /^\/health$/)) return send(res, 200, 'ok');

    if (m('GET', /^\/\.well-known\/apple-app-site-association$/)) {
        return send(res, 200, appleAppSiteAssociation(), { 'content-type': 'application/json' });
    }
    if (m('GET', /^\/\.well-known\/assetlinks\.json$/)) {
        return send(res, 200, assetLinks());
    }
    if ((r = m('GET', /^\/app\/sync\/([a-z]+)$/))) {
        providerOf(r[1]);
        res.writeHead(200, { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store' });
        return res.end(returnPage(r[1], url.searchParams.toString()));
    }

    if ((r = m('POST', /^\/v1\/auth\/([a-z]+)\/begin$/))) {
        // The only way to get a ticket, and a ticket is the only way to start.
        const provider = providerOf(r[1]);
        await requireApp(req);
        const { state } = await readJson(req);
        if (!/^[A-Za-z0-9_-]{8,128}$/.test(state || '')) throw new HttpError(400, 'state must be 8-128 url-safe chars');
        const u = new URL(`${baseUrl(req)}/v1/auth/${provider}/start`);
        u.searchParams.set('ticket', signState(state, provider));
        return send(res, 200, { url: u.toString() });
    }

    if ((r = m('GET', /^\/v1\/auth\/([a-z]+)\/start$/))) {
        const provider = providerOf(r[1]);
        // The browser cannot carry an App Check header, so the proof is the
        // ticket /begin issued to an attested app; verifying it re-signs the
        // same state for the provider round trip.
        const appState = verifyState(url.searchParams.get('ticket'), provider);
        const p = PROVIDERS[provider];
        const q = new URLSearchParams({
            client_id: p.clientId(),
            response_type: 'code',
            redirect_uri: `${baseUrl(req)}/v1/auth/${provider}/callback`,
            state: signState(appState, provider),
        });
        if (p.scope) q.set('scope', p.scope);
        if (provider === 'strava') q.set('approval_prompt', 'auto');
        return redirect(res, `${p.authorize}?${q}`);
    }

    if ((r = m('GET', /^\/v1\/auth\/([a-z]+)\/callback$/))) {
        const provider = providerOf(r[1]);
        const appState = verifyState(url.searchParams.get('state'), provider);
        const back = appReturnUrl(req, provider);
        back.searchParams.set('state', appState);
        const denied = url.searchParams.get('error');
        if (denied) {
            back.searchParams.set('error', denied);
            return redirect(res, back.toString());
        }
        const code = url.searchParams.get('code');
        if (!code) throw new HttpError(400, 'missing code');
        try {
            const tok = await exchangeCode(provider, code, `${baseUrl(req)}/v1/auth/${provider}/callback`);
            const payload = { provider, access_token: tok.access_token };
            if (provider === 'strava') {
                payload.refresh_token = tok.refresh_token;
                payload.expires_at = tok.expires_at;
                if (tok.athlete) payload.athlete = { id: tok.athlete.id, name: [tok.athlete.firstname, tok.athlete.lastname].filter(Boolean).join(' ') };
            } else {
                if (tok.refresh_token) payload.refresh_token = tok.refresh_token;
                if (tok.expires_in) payload.expires_at = now() + Number(tok.expires_in);
                payload.athlete = await rwgpsCurrentUser(tok.access_token).catch(() => null);
            }
            back.searchParams.set('handoff', sealHandoff(payload));
        } catch (e) {
            console.error(`[${provider}] callback: ${e.message}`);
            back.searchParams.set('error', 'exchange_failed');
        }
        return redirect(res, back.toString());
    }

    if (m('POST', /^\/v1\/auth\/handoff$/)) {
        await requireApp(req);
        const { handoff } = await readJson(req);
        if (!handoff) throw new HttpError(400, 'missing handoff');
        return send(res, 200, openHandoff(handoff));
    }

    if (m('POST', /^\/v1\/auth\/strava\/refresh$/)) {
        await requireApp(req);
        const { refresh_token } = await readJson(req);
        if (!refresh_token) throw new HttpError(400, 'missing refresh_token');
        return send(res, 200, await stravaRefresh(refresh_token));
    }

    if (m('POST', /^\/v1\/auth\/strava\/revoke$/)) {
        await requireApp(req);
        const { access_token } = await readJson(req);
        if (!access_token) throw new HttpError(400, 'missing access_token');
        await stravaRevoke(access_token);
        return send(res, 204);
    }

    if (m('GET', /^\/v1\/rwgps\/me$/)) {
        await requireApp(req);
        return send(res, 200, await rwgpsCurrentUser(bearer(req)));
    }

    if (m('POST', /^\/v1\/rwgps\/trips$/)) {
        // Pass the multipart body through untouched: the phone builds the
        // form (file + trip[name]) exactly as RWGPS wants it; we add the key.
        await requireApp(req);
        const token = bearer(req);
        const ct = req.headers['content-type'] || '';
        if (!ct.startsWith('multipart/form-data')) throw new HttpError(415, 'multipart/form-data expected');
        const body = await readBody(req, 64 << 20);   // a long ride's FIT is a few MB
        const up = await fetch(`${PROVIDERS.ridewithgps.api}/trips.json`, {
            method: 'POST',
            headers: rwgpsHeaders(token, { 'content-type': ct }),
            body,
        });
        const text = await up.text();
        res.writeHead(up.ok ? 200 : (up.status === 401 ? 401 : 502), {
            'content-type': up.headers.get('content-type') || 'application/json',
            'cache-control': 'no-store',
        });
        return res.end(text);
    }

    throw new HttpError(404, 'not found');
}

function createServer() {
    return http.createServer((req, res) => {
        handle(req, res).catch((e) => {
            const status = e instanceof HttpError ? e.status : 500;
            if (status >= 500) console.error(`${req.method} ${req.url}: ${e.stack || e}`);
            if (!res.headersSent) send(res, status, { error: e.message });
            else res.end();
        });
    });
}

if (require.main === module) {
    createServer().listen(PORT, () => console.log(`sync-auth listening on :${PORT}`));
}

module.exports = { createServer, PROVIDERS, APP, signState, verifyState, sealHandoff, openHandoff, verifyAppCheck };
