// node test.mjs: the Worker against a fake Discord.
import worker from './src/index.js';

let posted = null;
globalThis.fetch = async (url, init) => {
  posted = { url, form: init.body };
  return new Response('{}', { status: 200 });
};
const env = { DISCORD_WEBHOOK_URL: 'https://discord.example/api/webhooks/1/abc' };
const ctx = { waitUntil() {} };
const report = 'PS5SX2 session report\nBuild: vk-285-112\n';
function req(headers, body = report, path = '/v1/logs', method = 'POST') {
  const h = { 'content-type': 'text/plain; charset=utf-8', 'x-ps5sx2-console': '0123456789abcdef', ...headers };
  if (body !== null) h['content-length'] = String(new TextEncoder().encode(body).length);
  return new Request('https://relay.example' + path, { method, headers: h, body: method === 'POST' ? body : undefined });
}
const results = [];
async function expect(name, r, status) {
  const res = await worker.fetch(r, env, ctx);
  results.push([name, res.status === status ? 'PASS' : 'FAIL ' + res.status + ' ' + (await res.text())]);
}
await expect('good report', req({ 'x-ps5sx2-build': 'vk-285-112', 'x-ps5sx2-end': 'crash+note',
  'x-ps5sx2-game': 'Black (USA).iso', 'x-ps5sx2-tester': '@everyone *bold*' }), 201);
const fields = Object.fromEntries([...posted.form.entries()].map(([k, v]) => [k, typeof v === 'string' ? v : v.name]));
const payload = JSON.parse(fields.payload_json);
results.push(['summary line', payload.content.includes('**crash+note**') && payload.content.includes('`Black (USA).iso`')
  && payload.content.includes('`@everyone *bold*`') && payload.allowed_mentions.parse.length === 0 ? 'PASS' : 'FAIL ' + payload.content]);
results.push(['file name', /^PS5SX2-vk-285-112-012345-\d{4}-\d\d-\d\d_\d{4}-crash\+note\.txt$/.test(fields['files[0]']) ? 'PASS' : 'FAIL ' + fields['files[0]']]);
await expect('url in game name stays text', req({ 'x-ps5sx2-game': 'https://phish.example/login`x' }), 201);
{
  const f2 = Object.fromEntries([...posted.form.entries()].map(([k, v]) => [k, typeof v === 'string' ? v : v.name]));
  const c2 = JSON.parse(f2.payload_json).content;
  results.push(['url in a code span', c2.includes("`https://phish.example/login'x`") ? 'PASS' : 'FAIL ' + c2]);
}
let hits = 0;
env.IP_LIMITER = { limit: async () => ({ success: ++hits <= 2 }) };
await expect('ip limit 1', req({ 'x-ps5sx2-console': '1111111111111111' }), 201);
await expect('ip limit 2', req({ 'x-ps5sx2-console': '2222222222222222' }), 201);
await expect('ip limit 3 (new id, same ip)', req({ 'x-ps5sx2-console': '3333333333333333' }), 429);
delete env.IP_LIMITER;
await expect('bad console id', req({ 'x-ps5sx2-console': 'nope' }), 400);
await expect('not a report', req({}, 'hello'), 400);
await expect('wrong type', req({ 'content-type': 'application/zip' }), 415);
await expect('GET', req({}, null, '/v1/logs', 'GET'), 405);
await expect('other path', req({}, report, '/x'), 404);
globalThis.fetch = async () => new Response('slow', { status: 429 });
await expect('discord busy', req({}), 429);
for (const [n, r] of results) console.log(r.padEnd(6), n);
process.exit(results.every(([, r]) => r === 'PASS') ? 0 : 1);
