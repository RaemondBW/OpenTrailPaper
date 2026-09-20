'use strict';
// Runs the service against fake providers: no network, no real secrets.
const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http');
const crypto = require('node:crypto');

process.env.HANDOFF_KEY = '11'.repeat(32);
process.env.STRAVA_CLIENT_ID = 'sid';
process.env.STRAVA_CLIENT_SECRET = 'ssecret';
process.env.RWGPS_CLIENT_ID = 'rid';
process.env.RWGPS_CLIENT_SECRET = 'rsecret';
process.env.RWGPS_API_KEY = 'rkey';
process.env.APP_CHECK_PROJECT_NUMBER = '424242';
process.env.ANDROID_CERT_SHA256 = 'AA:BB, CC:DD';

const srv = require('../server.js');

// A fake Firebase: one RSA key pair, its JWKS served by the fake provider
// server, tokens minted here. The service only ever sees the public half.
const { publicKey, privateKey } = crypto.generateKeyPairSync('rsa', { modulusLength: 2048 });
const jwk = { ...publicKey.export({ format: 'jwk' }), kid: 'k1', alg: 'RS256', use: 'sig' };
function appCheckToken(over = {}) {
    const b64 = (o) => Buffer.from(JSON.stringify(o)).toString('base64url');
    const h = b64({ alg: 'RS256', typ: 'JWT', kid: 'k1' });
    const c = b64({
        iss: 'https://firebaseappcheck.googleapis.com/424242', aud: ['projects/424242', 'projects/otp'],
        sub: '1:424242:ios:abc', exp: Math.floor(Date.now() / 1000) + 600, ...over,
    });
    const sig = crypto.sign('RSA-SHA256', Buffer.from(`${h}.${c}`), privateKey).toString('base64url');
    return `${h}.${c}.${sig}`;
}
const APPCHECK = { 'x-firebase-appcheck': appCheckToken() };

let fake, fakeUrl, app, appUrl;
const seen = [];   // requests the fake providers received

before(async () => {
    fake = http.createServer(async (req, res) => {
        let body = '';
        for await (const c of req) body += c;
        seen.push({ path: req.url, headers: req.headers, body });
        res.setHeader('content-type', 'application/json');
        if (req.url === '/strava/oauth/token') {
            const p = new URLSearchParams(body);
            if (p.get('grant_type') === 'refresh_token') {
                return res.end(JSON.stringify({ access_token: 'sa2', refresh_token: 'sr2', expires_at: 999 }));
            }
            assert.equal(p.get('client_secret'), 'ssecret');
            return res.end(JSON.stringify({ access_token: 'sa1', refresh_token: 'sr1', expires_at: 123,
                athlete: { id: 7, firstname: 'Ada', lastname: 'L' } }));
        }
        if (req.url === '/jwks') return res.end(JSON.stringify({ keys: [jwk] }));
        if (req.url === '/rwgps/oauth/token') return res.end(JSON.stringify({ access_token: 'ra1' }));
        if (req.url === '/rwgps/api/v1/users/current.json') {
            assert.equal(req.headers['x-rwgps-api-key'], 'rkey');
            return res.end(JSON.stringify({ user: { id: 42, name: 'Ada' } }));
        }
        if (req.url === '/rwgps/api/v1/trips.json') {
            assert.equal(req.headers['x-rwgps-api-key'], 'rkey');
            assert.equal(req.headers.authorization, 'Bearer ra1');
            assert.match(req.headers['content-type'], /^multipart\/form-data/);
            return res.end(JSON.stringify({ trip: { id: 5 } }));
        }
        res.statusCode = 404; res.end('{}');
    });
    await new Promise((r) => fake.listen(0, r));
    fakeUrl = `http://127.0.0.1:${fake.address().port}`;
    srv.PROVIDERS.strava.token = `${fakeUrl}/strava/oauth/token`;
    srv.PROVIDERS.ridewithgps.token = `${fakeUrl}/rwgps/oauth/token`;
    srv.PROVIDERS.ridewithgps.api = `${fakeUrl}/rwgps/api/v1`;
    srv.APP.appCheckJwks = `${fakeUrl}/jwks`;

    app = srv.createServer();
    await new Promise((r) => app.listen(0, r));
    appUrl = `http://127.0.0.1:${app.address().port}`;
});
after(() => { fake.close(); app.close(); });

const get = (path) => fetch(`${appUrl}${path}`, { redirect: 'manual' });
const post = (path, json, headers = {}) => fetch(`${appUrl}${path}`, {
    method: 'POST', headers: { 'content-type': 'application/json', ...headers }, body: JSON.stringify(json),
});

async function begin(provider, state, headers = APPCHECK) {
    const r = await post(`/v1/auth/${provider}/begin`, { state }, headers);
    return r;
}

test('begin needs an app check token and hands back a ticketed start url', async () => {
    assert.equal((await begin('strava', 'abcdefgh', {})).status, 401);
    assert.equal((await begin('strava', 'abcdefgh', { 'x-firebase-appcheck': 'nope' })).status, 401);
    const r = await begin('strava', 'abcdefgh');
    assert.equal(r.status, 200);
    const { url } = await r.json();
    const u = new URL(url);
    assert.equal(u.pathname, '/v1/auth/strava/start');
    assert.equal(srv.verifyState(u.searchParams.get('ticket'), 'strava'), 'abcdefgh');
});

test('app check: wrong issuer, audience, expiry, app id, signature are all refused', async () => {
    const cases = [
        appCheckToken({ iss: 'https://firebaseappcheck.googleapis.com/999' }),
        appCheckToken({ aud: ['projects/999'] }),
        appCheckToken({ exp: Math.floor(Date.now() / 1000) - 5 }),
        appCheckToken() + 'x',
    ];
    for (const t of cases) assert.equal((await begin('strava', 'abcdefgh', { 'x-firebase-appcheck': t })).status, 401, t.slice(-8));
    srv.APP.appCheckAppIds = ['1:424242:android:zzz'];
    assert.equal((await begin('strava', 'abcdefgh')).status, 401);
    srv.APP.appCheckAppIds = [];
    assert.equal((await begin('strava', 'abcdefgh')).status, 200);
});

test('start needs a ticket; a bare state is refused', async () => {
    assert.equal((await get('/v1/auth/strava/start?state=abcdefgh')).status, 400);
    assert.equal((await get('/v1/auth/strava/start')).status, 400);
});

test('start redirects to the provider with a signed state and our callback', async () => {
    const { url } = await (await begin('strava', 'abcdefgh')).json();
    const r = await fetch(url, { redirect: 'manual' });
    assert.equal(r.status, 302);
    const loc = new URL(r.headers.get('location'));
    assert.equal(loc.origin + loc.pathname, 'https://www.strava.com/oauth/authorize');
    assert.equal(loc.searchParams.get('client_id'), 'sid');
    assert.equal(loc.searchParams.get('scope'), 'read,activity:write');
    assert.equal(loc.searchParams.get('redirect_uri'), `${appUrl}/v1/auth/strava/callback`);
    assert.equal(srv.verifyState(loc.searchParams.get('state'), 'strava'), 'abcdefgh');
    // The secret never appears in anything the browser sees.
    assert.ok(!r.headers.get('location').includes('ssecret'));
});

test('begin rejects a weak state', async () => {
    assert.equal((await begin('strava', 'x')).status, 400);
});

test('the well-known files name the signed apps', async () => {
    const aasa = await (await get('/.well-known/apple-app-site-association')).json();
    assert.deepEqual(aasa.applinks.details[0].appIDs, ['G5JFC849XY.com.raemond.opentrailpaper']);
    assert.equal(aasa.applinks.details[0].components[0]['/'], '/app/sync/*');
    const al = await (await get('/.well-known/assetlinks.json')).json();
    assert.equal(al[0].target.package_name, 'com.raemond.opentrailpaper');
    assert.deepEqual(al[0].target.sha256_cert_fingerprints, ['AA:BB', 'CC:DD']);
});

test('the return page offers the custom-scheme fallback with the same query', async () => {
    const r = await get('/app/sync/strava?handoff=abc&state=s1');
    assert.equal(r.status, 200);
    const html = await r.text();
    assert.ok(html.includes('href="opentrailpaper://sync/strava?handoff=abc&amp;state=s1"'));
    assert.ok(html.includes('location.replace("opentrailpaper://sync/strava?handoff=abc&state=s1")'));
    assert.equal((await get('/app/sync/nope')).status, 404);
});

test('strava callback exchanges the code and hands the app an opaque handoff', async () => {
    const state = srv.signState('appstate1', 'strava');
    const r = await get(`/v1/auth/strava/callback?code=c0de&state=${encodeURIComponent(state)}`);
    assert.equal(r.status, 302);
    const back = new URL(r.headers.get('location'));
    assert.equal(back.origin + back.pathname, `${appUrl}/app/sync/strava`);   // an app link, not a scheme
    assert.equal(back.searchParams.get('state'), 'appstate1');
    const handoff = back.searchParams.get('handoff');
    assert.ok(handoff && !handoff.includes('sa1'));           // opaque
    assert.equal((await post('/v1/auth/handoff', { handoff })).status, 401);   // no app check
    const tok = await (await post('/v1/auth/handoff', { handoff }, APPCHECK)).json();
    assert.deepEqual(tok, { provider: 'strava', access_token: 'sa1', refresh_token: 'sr1', expires_at: 123,
        athlete: { id: 7, name: 'Ada L' } });
    const ex = seen.find((s) => s.path === '/strava/oauth/token');
    assert.equal(new URLSearchParams(ex.body).get('redirect_uri'), `${appUrl}/v1/auth/strava/callback`);
});

test('callback with a state for another provider or a forged one is refused', async () => {
    const state = srv.signState('s', 'ridewithgps');
    assert.equal((await get(`/v1/auth/strava/callback?code=c&state=${encodeURIComponent(state)}`)).status, 400);
    assert.equal((await get('/v1/auth/strava/callback?code=c&state=abc.def')).status, 400);
});

test('user denial comes back to the app as an error, no exchange attempted', async () => {
    const n = seen.length;
    const state = srv.signState('appstate2', 'strava');
    const r = await get(`/v1/auth/strava/callback?error=access_denied&state=${encodeURIComponent(state)}`);
    const back = new URL(r.headers.get('location'));
    assert.equal(back.searchParams.get('error'), 'access_denied');
    assert.equal(seen.length, n);
});

test('ridewithgps callback also looks up the account name', async () => {
    const state = srv.signState('appstate3', 'ridewithgps');
    const r = await get(`/v1/auth/ridewithgps/callback?code=c0de&state=${encodeURIComponent(state)}`);
    const back = new URL(r.headers.get('location'));
    const tok = await (await post('/v1/auth/handoff', { handoff: back.searchParams.get('handoff') }, APPCHECK)).json();
    assert.deepEqual(tok, { provider: 'ridewithgps', access_token: 'ra1', athlete: { id: 42, name: 'Ada' } });
});

test('a handoff expires and a tampered one is rejected', async () => {
    const h = srv.sealHandoff({ provider: 'strava', access_token: 'x' });
    assert.equal((await post('/v1/auth/handoff', { handoff: h.slice(0, -2) + 'AA' }, APPCHECK)).status, 400);
    const past = Math.floor(Date.now() / 1000) - 1000;
    const realNow = Date.now;
    Date.now = () => (past - 200) * 1000;               // seal in the past
    const old = srv.sealHandoff({ provider: 'strava', access_token: 'x' });
    Date.now = realNow;
    assert.equal((await post('/v1/auth/handoff', { handoff: old }, APPCHECK)).status, 410);
});

test('strava refresh proxies with the secret', async () => {
    assert.equal((await post('/v1/auth/strava/refresh', { refresh_token: 'sr1' })).status, 401);
    const r = await post('/v1/auth/strava/refresh', { refresh_token: 'sr1' }, APPCHECK);
    assert.deepEqual(await r.json(), { access_token: 'sa2', refresh_token: 'sr2', expires_at: 999 });
});

test('rwgps upload is proxied with the API key and the user token', async () => {
    const boundary = 'xyz';
    const body = `--${boundary}\r\nContent-Disposition: form-data; name="file"; filename="r.fit"\r\n\r\nFITDATA\r\n--${boundary}--\r\n`;
    const r = await fetch(`${appUrl}/v1/rwgps/trips`, {
        method: 'POST',
        headers: { 'content-type': `multipart/form-data; boundary=${boundary}`, authorization: 'Bearer ra1', ...APPCHECK },
        body,
    });
    assert.equal(r.status, 200);
    assert.deepEqual(await r.json(), { trip: { id: 5 } });
    const up = seen.find((s) => s.path === '/rwgps/api/v1/trips.json');
    assert.ok(up.body.includes('FITDATA'));
});

test('rwgps routes need app check and then a bearer token', async () => {
    assert.equal((await get('/v1/rwgps/me')).status, 401);
    const r = await fetch(`${appUrl}/v1/rwgps/me`, { headers: APPCHECK });
    assert.equal(r.status, 401);
    assert.equal((await r.json()).error, 'missing bearer token');
});
