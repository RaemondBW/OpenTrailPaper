'use strict';
// Runs the service against fake providers: no network, no real secrets.
const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http');

process.env.HANDOFF_KEY = '11'.repeat(32);
process.env.STRAVA_CLIENT_ID = 'sid';
process.env.STRAVA_CLIENT_SECRET = 'ssecret';
process.env.RWGPS_CLIENT_ID = 'rid';
process.env.RWGPS_CLIENT_SECRET = 'rsecret';
process.env.RWGPS_API_KEY = 'rkey';

const srv = require('../server.js');

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

    app = srv.createServer();
    await new Promise((r) => app.listen(0, r));
    appUrl = `http://127.0.0.1:${app.address().port}`;
});
after(() => { fake.close(); app.close(); });

const get = (path) => fetch(`${appUrl}${path}`, { redirect: 'manual' });
const post = (path, json, headers = {}) => fetch(`${appUrl}${path}`, {
    method: 'POST', headers: { 'content-type': 'application/json', ...headers }, body: JSON.stringify(json),
});

test('start redirects to the provider with a signed state and our callback', async () => {
    const r = await get('/v1/auth/strava/start?state=abcdefgh');
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

test('start rejects a weak state', async () => {
    assert.equal((await get('/v1/auth/strava/start?state=x')).status, 400);
});

test('strava callback exchanges the code and hands the app an opaque handoff', async () => {
    const state = srv.signState('appstate1', 'strava');
    const r = await get(`/v1/auth/strava/callback?code=c0de&state=${encodeURIComponent(state)}`);
    assert.equal(r.status, 302);
    const back = new URL(r.headers.get('location'));
    assert.equal(back.protocol, 'opentrailpaper:');
    assert.equal(back.host, 'sync');
    assert.equal(back.pathname, '/strava');
    assert.equal(back.searchParams.get('state'), 'appstate1');
    const handoff = back.searchParams.get('handoff');
    assert.ok(handoff && !handoff.includes('sa1'));           // opaque
    const tok = await (await post('/v1/auth/handoff', { handoff })).json();
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
    const tok = await (await post('/v1/auth/handoff', { handoff: back.searchParams.get('handoff') })).json();
    assert.deepEqual(tok, { provider: 'ridewithgps', access_token: 'ra1', athlete: { id: 42, name: 'Ada' } });
});

test('a handoff expires and a tampered one is rejected', async () => {
    const h = srv.sealHandoff({ provider: 'strava', access_token: 'x' });
    assert.equal((await post('/v1/auth/handoff', { handoff: h.slice(0, -2) + 'AA' })).status, 400);
    const past = Math.floor(Date.now() / 1000) - 1000;
    const realNow = Date.now;
    Date.now = () => (past - 200) * 1000;               // seal in the past
    const old = srv.sealHandoff({ provider: 'strava', access_token: 'x' });
    Date.now = realNow;
    assert.equal((await post('/v1/auth/handoff', { handoff: old })).status, 410);
});

test('strava refresh proxies with the secret', async () => {
    const r = await post('/v1/auth/strava/refresh', { refresh_token: 'sr1' });
    assert.deepEqual(await r.json(), { access_token: 'sa2', refresh_token: 'sr2', expires_at: 999 });
});

test('rwgps upload is proxied with the API key and the user token', async () => {
    const boundary = 'xyz';
    const body = `--${boundary}\r\nContent-Disposition: form-data; name="file"; filename="r.fit"\r\n\r\nFITDATA\r\n--${boundary}--\r\n`;
    const r = await fetch(`${appUrl}/v1/rwgps/trips`, {
        method: 'POST',
        headers: { 'content-type': `multipart/form-data; boundary=${boundary}`, authorization: 'Bearer ra1' },
        body,
    });
    assert.equal(r.status, 200);
    assert.deepEqual(await r.json(), { trip: { id: 5 } });
    const up = seen.find((s) => s.path === '/rwgps/api/v1/trips.json');
    assert.ok(up.body.includes('FITDATA'));
});

test('rwgps routes need a bearer token', async () => {
    assert.equal((await get('/v1/rwgps/me')).status, 401);
});
