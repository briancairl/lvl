// Browser smoke test of the web build: serves the build directory, opens the editor in headless
// Chromium (software WebGL2) with the demo level, waits for it to be created and rendered, and fails
// on any page error. Optionally saves a screenshot.
//
//   node platform/web/smoke_test.mjs <build-dir> [screenshot.png]
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join } from 'node:path';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');

const [dir, screenshot] = process.argv.slice(2);
if (!dir) {
  console.error('usage: smoke_test.mjs <build-dir> [screenshot.png]');
  process.exit(2);
}

const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' };
const server = createServer(async (req, res) => {
  try {
    const path = join(dir, decodeURIComponent(new URL(req.url, 'http://x').pathname));
    const data = await readFile(path);
    res.writeHead(200, { 'Content-Type': types[extname(path)] || 'application/octet-stream' });
    res.end(data);
  } catch {
    res.writeHead(404);
    res.end();
  }
});
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const url = `http://127.0.0.1:${server.address().port}/lvl.html?arg=--demo`;

const browser = await chromium.launch({ args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
const page = await browser.newPage({ viewport: { width: 1600, height: 900 } });
const errors = [];
let created = false;
page.on('pageerror', (e) => errors.push(String(e)));
page.on('console', (msg) => {
  const text = msg.text();
  if (msg.type() === 'error' && !text.includes('favicon')) {
    errors.push(text);
  }
  if (text.includes('Created demo level')) {
    created = true;
  }
  console.log(`[page] ${text}`);
});

let ok = false;
try {
  await page.goto(url);
  const deadline = Date.now() + 120000;
  while (!created && errors.length === 0 && Date.now() < deadline) {
    await page.waitForTimeout(250);
  }
  // Let a few frames render (the atlas reload job finishes on the next frame)
  await page.waitForTimeout(2000);
  if (screenshot) {
    await page.screenshot({ path: screenshot });
  }
  ok = created && errors.length === 0;
} finally {
  await browser.close();
  server.close();
}
if (!ok) {
  console.error(created ? `FAIL: page errors:\n${errors.join('\n')}` : 'FAIL: demo level was never created');
  process.exit(1);
}
console.log('PASS');
