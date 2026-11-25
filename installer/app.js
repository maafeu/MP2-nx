// renut_nx_ultimate_installer: the page. The player chooses their disc image (lib/xiso.js) or the folder the game was
// extracted to, and the DLC packages if the port takes them; worker.js makes the Switch package from them (the shaders,
// the LZ4-packed files: lib/install.js) and sw.js serves it as a download. Everything game-specific comes from
// game.json and release/manifest.json.
// From rr6-nx-installer's and kameo-nx-installer's app.js (bejeweled3-nx-installer's, ChanseyIsTheBest;
// nfsmw-nx-installer's download flow, StevensND; GPL-3.0).
import { listXiso } from './lib/xiso.js';

const $ = (id) => document.getElementById(id);
const folderInput = $('folder'), isoInput = $('iso'), chosen = $('chosen'), detected = $('detected');
const dlcInput = $('dlc'), dlcFolderInput = $('dlc-folder'), dlcChosen = $('dlc-chosen'), dlcDetected = $('dlc-detected');
const create = $('create'), compileBox = $('compile'), bar = $('bar'), logBox = $('log');
const nroInput = $('nro'), nroChosen = $('nro-chosen'), nroDetected = $('nro-detected');
const manifest = fetch('./release/manifest.json').then((r) => r.json());
const game = fetch('./game.json').then((r) => r.json());
let entries = null;
let dlc = [];  // [{ path (under dlc/), size, file }]
let busy = false;
let downloadFrame = null;
// mp2-nx (2026-11-24): a release without the port's NRO (no nro_sha256 in the manifest: the public page, since the NRO
// holds the game's recompiled code) takes the player's own build of it
let byo = false;
let userNro = null;

// the steps shown, numbered in order (some are hidden by what the release needs)
function numberSteps() {
  let n = 0;
  for (const s of document.querySelectorAll('main > section')) {
    const span = s.querySelector('h2 > .n');
    if (span) span.textContent = s.hidden ? '' : `${++n}. `;
  }
}

// the texts of game.json (trusted: the port's own file, as the page's HTML is)
game.then((g) => {
  document.title = `${g.package} web installer`;
  for (const el of document.querySelectorAll('[data-game]')) el.textContent = g[el.dataset.game];
  for (const el of document.querySelectorAll('[data-text]')) el.innerHTML = g.texts?.[el.dataset.text] || '';
  $('dlc-step').hidden = !g.dlc;
  $('compile-row').hidden = !g.shaders;
  create.textContent = `Create ${g.package}.zip`;
  manifest.then((m) => {
    $('version').textContent = `version ${m.version}`;
    byo = !m.builds.some((b) => b.nro_sha256);
    $('nro-step').hidden = !byo;
    numberSteps();
    updateCreate();
  });
  numberSteps();
});

function log(text, error = false) {
  const line = document.createElement('div');
  line.textContent = text;
  if (error) line.className = 'error';
  logBox.appendChild(line);
  logBox.scrollTop = logBox.scrollHeight;
}

function formatSize(bytes) {
  return bytes >= 1 << 30 ? `${(bytes / (1 << 30)).toFixed(2)} GB` : `${(bytes / (1 << 20)).toFixed(0)} MB`;
}

async function sha256(blob) {
  const d = new Uint8Array(await crypto.subtle.digest('SHA-256', await blob.arrayBuffer()));
  return Array.from(d, (b) => b.toString(16).padStart(2, '0')).join('');
}

function show(state, text, box = detected) {
  box.hidden = false;
  box.className = `detected ${state}`;
  box.textContent = text;
}

function updateCreate() {
  create.disabled = busy || !entries || (byo && !userNro);
}

// The player's own NRO: a Switch homebrew executable ("NRO0" at 0x10 of its header), at least the header's size
nroInput.addEventListener('change', async (event) => {
  const [file] = event.target.files;
  event.target.value = '';
  userNro = null;
  updateCreate();
  if (!file) return;
  nroChosen.textContent = `${file.name}: ${formatSize(file.size)}`;
  const head = new Uint8Array(await file.slice(0, 0x80).arrayBuffer());
  const magic = head.length >= 0x14 ? String.fromCharCode(...head.slice(0x10, 0x14)) : '';
  if (magic !== 'NRO0') {
    show('bad', `${file.name} is not a Switch NRO: choose the mp2-nx.nro your build made.`, nroDetected);
    return;
  }
  const g = await game;
  show('good', `A Switch NRO: it goes in the zip as switch/${g.package}/${g.nro}.`, nroDetected);
  userNro = file;
  updateCreate();
});

// list: [{ path (in the game), size, file, offset (in a disc image) }]
async function choose(list, what) {
  entries = null;
  updateCreate();
  const g = await game;
  const total = list.reduce((n, e) => n + e.size, 0);
  chosen.textContent = `${what}: ${list.length} files, ${formatSize(total)}`;
  // mp2-nx: the executable game.json names (an original Xbox port: default.xbe)
  const exeName = g.exe || 'default.xex';
  const xex = list.find((e) => e.path.toLowerCase() === exeName.toLowerCase());
  if (!xex) {
    show('bad', `No ${exeName}: choose the folder the game disc was extracted to, or the disc image.`);
    return;
  }
  show('', `Checking ${exeName}...`);
  const blob = xex.offset === undefined ? xex.file : xex.file.slice(xex.offset, xex.offset + xex.size);
  const hash = await sha256(blob);
  const build = (await manifest).builds.find((b) => (b.exe_sha256 || b.xex_sha256) === hash);
  if (!build) {
    show('bad', `This ${exeName} is not one this release supports. Its SHA-256: ${hash}`);
    return;
  }
  const missing = (g.needs || []).filter((p) => !list.some((e) => new RegExp(`^(?:${p})$`, 'i').test(e.path)));
  if (missing.length) {
    show('bad', `Files are missing (${missing.map((p) => p.replace(/\\/g, '')).join(', ')}): choose the whole extracted game.`);
    return;
  }
  show('good', `${g.title} (${build.edition}), supported.`);
  entries = list;
  updateCreate();
}

// The DLC: Xbox 360 packages ("LIVE", "PIRS" or "CON "), copied whole to switch/<package>/dlc/; the game installs them.
async function chooseDlc(list) {
  dlc = [];
  const g = await game;
  if (!list.length) {
    dlcChosen.textContent = 'None.';
    dlcDetected.hidden = true;
    return;
  }
  const kept = [];
  let skipped = 0;
  for (const e of list) {
    const magic = String.fromCharCode(...new Uint8Array(await e.file.slice(0, 4).arrayBuffer()));
    if (e.size >= 0x1000 && (magic === 'LIVE' || magic === 'PIRS' || magic === 'CON ')) kept.push(e);
    else skipped++;
  }
  const total = kept.reduce((n, e) => n + e.size, 0);
  dlcChosen.textContent = `${kept.length} package${kept.length === 1 ? '' : 's'}, ${formatSize(total)}` +
    (skipped ? ` (${skipped} other file${skipped === 1 ? '' : 's'} left out)` : '');
  if (!kept.length) {
    show('bad', 'None of these is an Xbox 360 package.', dlcDetected);
    return;
  }
  show('good', `${kept.length} DLC package${kept.length === 1 ? '' : 's'} will go to switch/${g.package}/dlc/.`, dlcDetected);
  dlc = kept;
}

dlcInput.addEventListener('change', async (event) => {
  const files = [...event.target.files];
  event.target.value = '';
  await chooseDlc(files.map((file) => ({ path: file.name, size: file.size, file })));
});

dlcFolderInput.addEventListener('change', async (event) => {
  const files = [...event.target.files];
  event.target.value = '';
  // the paths inside the chosen folder (its own name dropped): the game walks into subfolders of dlc/
  await chooseDlc(files.map((file) => ({ path: file.webkitRelativePath.split('/').slice(1).join('/'), size: file.size, file })));
});

folderInput.addEventListener('change', async (event) => {
  const files = [...event.target.files];
  event.target.value = '';
  if (!files.length) return;
  // the paths inside the chosen folder (its own name dropped)
  await choose(files.map((file) => ({ path: file.webkitRelativePath.split('/').slice(1).join('/'), size: file.size, file })),
               files[0].webkitRelativePath.split('/')[0]);
});

isoInput.addEventListener('change', async (event) => {
  const [file] = event.target.files;
  event.target.value = '';
  if (!file) return;
  const read = async (offset, length) => new Uint8Array(await file.slice(offset, offset + length).arrayBuffer());
  try {
    const files = await listXiso(read, file.size);
    if (!files) {
      entries = null;
      updateCreate();
      show('bad', `${file.name} is not an Xbox disc image.`);
      return;
    }
    await choose(files.map((f) => ({ path: f.path, size: f.size, file, offset: f.offset })), file.name);
  } catch (error) {
    show('bad', error.message);
  }
});

// The zip is served by sw.js as a normal browser download, streamed while it is being made.
async function downloadHelper() {
  if (!('serviceWorker' in navigator)) throw new Error('this browser cannot stream a download (no service workers)');
  await navigator.serviceWorker.register('./sw.js', { updateViaCache: 'none' });
  const registration = await navigator.serviceWorker.ready;
  if (!navigator.serviceWorker.controller) {
    const claimed = new Promise((resolve) => navigator.serviceWorker.addEventListener('controllerchange', resolve, { once: true }));
    registration.active.postMessage({ type: 'claim' });
    await claimed;
  }
  return navigator.serviceWorker.controller;
}
const helper = downloadHelper();
helper.catch(() => {});

create.addEventListener('click', async () => {
  if (!entries || busy) return;
  let controller;
  try {
    controller = await helper;
  } catch (error) {
    log(`Error: ${error.message}`, true);
    return;
  }
  const zipName = `${(await game).package}.zip`;
  busy = true;
  create.disabled = true;
  bar.hidden = false;
  bar.value = 0;
  logBox.textContent = '';
  const channel = new MessageChannel();
  const keepAlive = setInterval(() => controller.postMessage({ type: 'ping' }), 10000);
  const worker = new Worker('./worker.js', { type: 'module' });
  const finish = () => {
    clearInterval(keepAlive);
    worker.terminate();
    busy = false;
    updateCreate();
  };
  worker.onmessage = async ({ data }) => {
    if (data.type === 'log') {
      log(data.text);
    } else if (data.type === 'progress') {
      bar.value = data.fraction;
    } else if (data.type === 'start') {
      const id = `${Date.now()}-${Math.random().toString(36).slice(2)}`;
      const ack = new MessageChannel();
      const registered = new Promise((resolve) => (ack.port1.onmessage = resolve));
      controller.postMessage({ type: 'download', id, filename: zipName, size: null }, [channel.port2, ack.port2]);
      await registered;
      // a hidden frame opens the download: its request is a navigation, which always reaches the service worker
      downloadFrame?.remove();
      downloadFrame = document.createElement('iframe');
      downloadFrame.hidden = true;
      downloadFrame.src = `./download/${id}/${zipName}`;
      document.body.appendChild(downloadFrame);
      log(`Download started: ${zipName}, about ${formatSize(data.estimate)}. Keep this page open until it finishes.`);
    } else if (data.type === 'done') {
      bar.value = 1;
      log(`Finished in ${data.seconds} s: ${zipName} is ${formatSize(data.size)}.`);
      finish();
    } else if (data.type === 'error') {
      log(`Error: ${data.message}`, true);
      finish();
    }
  };
  worker.onerror = (event) => {
    log(`Error: ${event.message}`, true);
    finish();
  };
  worker.postMessage({ entries, dlc, compile: compileBox.checked, nro: byo ? userNro : null, port: channel.port1 },
                     [channel.port1]);
});
