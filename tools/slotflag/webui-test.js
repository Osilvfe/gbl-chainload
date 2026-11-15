#!/usr/bin/env node
/*
 * Headless test for the module WebUI (tools/slotflag/module/webroot/index.html).
 *
 *   npm install jsdom
 *   node tools/slotflag/webui-test.js
 *
 * Loads the real page in jsdom, stubs `ksu.exec` with a fake `slotctl` that
 * keeps state in a temp file, then drives the UI: read, toggle, confirm,
 * write, both-unbootable guard, backup, restore list, log toggle. Exits
 * non-zero on any failure.
 */
// Loads the module WebUI in jsdom with a stubbed `ksu` that shells out to a
// fake slotctl over a temp state file. Exercises read, toggle, apply, backup.
const fs = require('fs');
const path = require('path');
const { JSDOM } = require('jsdom');

const HERE = __dirname;
const HTML = fs.readFileSync(path.join(HERE, 'module/webroot/index.html'), 'utf8');
const SCRIPT = (HTML.match(/<script>([\s\S]*?)<\/script>/g) || [])
  .map(s => s.replace(/^<script>/, '').replace(/<\/script>$/, ''))
  .pop();

const work = fs.mkdtempSync(path.join(require('os').tmpdir(), 'slotflag-webui-'));
fs.writeFileSync(path.join(work, 'state'), 'a=0x007f000000000000\nb=0x0000000000000000\n');
fs.writeFileSync(path.join(work, 'log'), '');

const CTL = path.join(work, 'slotctl');
fs.writeFileSync(CTL, `#!/bin/bash
set -u
S=${work}/state
GLOG=${work}/log
echo "CMD $*" >> "$GLOG"
get() { grep "^$1=" "$S" | head -1 | cut -d= -f2; }
setattr() { sed -i "s|^$1=.*|$1=$2|" "$S"; }
attr2hex() { printf '0x%016x' $1; }
fields() { # $1=hexattr
  python3 - "$1" <<'PY'
import sys
v=int(sys.argv[1],16)
f=lambda s,m:(v>>s)&m
print(f(48,3));print(f(50,1));print(f(51,7));print(f(54,1));print(f(55,1))
PY
}
case "\${1:-state}" in
state)
  echo "backup_dir=${work}/backup"
  [ -f "\${BACKUP_EXISTS:-}" ] || true
  mkdir -p ${work}/backup
  echo "disk=/dev/block/sde"; echo "lba=4096"; echo "entries=128"; echo "entry_size=128"; echo "crc=ok"
  for s in a b; do
    h=$(get $s); echo "$s.attr=$h"
    set -- $(fields "$h")
    echo "$s.priority=$1"; echo "$s.active=$2"; echo "$s.retry=$3"
    echo "$s.successful=$4"; echo "$s.unbootable=$5"
  done
  ;;
set)
  shift; slot=""; unb=""; dry=""; force=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --slot) slot=$2; shift 2;;
      --unbootable) unb=$2; shift 2;;
      --dry-run) dry=1; shift;;
      --force) force=1; shift;;
      *) shift;;
    esac
  done
  [ -n "$slot" ] || { echo "error=--slot a|b required"; exit 2; }
  other=$([ "$slot" = a ] && echo b || echo a)
  oldh=$(get $slot); newh=$oldh
  if [ -n "$unb" ]; then
    newh=$(python3 -c "v=int('$oldh',16); v=(v & ~(1<<55)) | ($unb<<55); print('0x%016x' % v)")
  else
    newh=$oldh
  fi
  if [ "$newh" = "$oldh" ]; then echo "result=unchanged"; echo "attr=0x$oldh"; exit 0; fi
  oub=$(python3 -c "print((int('$other',16)>>55)&1)")
  [ "$force" = 1 ] || [ "$oub" != 1 ] || [ "$(( (0x$newh >> 55) & 1 ))" != 1 ] || { echo "error=both slots would be unbootable; pass --force"; exit 3; }
  echo "old=$oldh"; echo "new=$newh"
  [ "$dry" = 1 ] && { echo "result=dry-run"; exit 0; }
  setattr $slot $newh
  echo "result=ok"
  ;;
backup)
  mkdir -p ${work}/backup
  cp ${work}/state ${work}/backup/gpt-test.bin
  echo "path=${work}/backup/gpt-test.bin"; echo "size=$(wc -c < ${work}/backup/gpt-test.bin)"
  ;;
backups) ls -1t ${work}/backup/*.bin 2>/dev/null | head -20 ;;
restore)
  echo "result=ok"; echo "path=${work}/backup/gpt-test.bin" ;;
*) echo "error=unknown"; exit 1 ;;
esac
`);
fs.chmodSync(CTL, 0o755);

const calls = [];
const dom = new JSDOM(HTML, { runScripts: 'outside-only', pretendToBeVisual: true });
const w = dom.window;

// stub ksu: exec(CTL + " ...") -> run the fake script
/* Mirror the native bridge: ksu.exec(cmd, optionsJson, callbackName) and the
 * manager invokes window[callbackName](errno, stdout, stderr) afterwards. */
w.ksu = {
  exec(cmd, optsJson, cbName) {
    calls.push(cmd);
    const real = cmd.replace('/data/adb/modules/gbl-slotflag/bin/slotctl', CTL);
    let res;
    try {
      const out = require('child_process').execSync(real, { encoding: 'utf8' });
      res = [0, out, ''];
    } catch (e) {
      res = [e.status || 1, e.stdout || '', e.stderr || ''];
    }
    setTimeout(() => {
      const cb = w[cbName];
      if (typeof cb !== 'function') throw new Error('missing callback ' + cbName);
      cb(res[0], res[1], res[2]);
    }, 0);
  }
};

w.eval(SCRIPT);

const sleep = ms => new Promise(r => setTimeout(r, ms));
const $ = id => w.document.getElementById(id);
const text = id => $(id).textContent;

(async () => {
  const results = [];
  const ok = (name, cond, extra) => results.push([cond ? 'PASS' : 'FAIL', name, extra || '']);

  await sleep(250);
  ok('reads both slots', /boot_a/.test($('card-a').textContent) && /boot_b/.test($('card-b').textContent));
  ok('shows gpt path', /sde/.test(text('disk')), text('disk'));
  ok('a chip ACTIVE', /ACTIVE/.test($('card-a').textContent));
  const aChips = $('card-a').querySelector('.chips').textContent;
  ok('a chips rendered', /PRIO 3/.test(aChips) && /SUCCESSFUL/.test(aChips), aChips);
  ok('a chip says BOOTABLE', /BOOTABLE/.test(aChips) && !/UNBOOTABLE/.test(aChips), aChips);
  ok('b chips show 0 retry', /RETRY 0\/7/.test($('card-b').textContent));

  // toggle b unbootable on
  const bUnb = $('card-b').querySelectorAll('.sw input')[0];
  ok('b unbootable switch exists', !!bUnb);
  bUnb.checked = true;
  bUnb.dispatchEvent(new w.Event('change'));
  ok('apply enabled after toggle', $('apply-b').disabled === false);
  ok('chip turns unbootable', /UNBOOTABLE/.test($('card-b').querySelector('.chips').textContent));

  $('apply-b').click();
  await sleep(50);
  ok('confirm sheet opens', $('sheet').classList.contains('open'));
  $('sh-ok').click();
  await sleep(400);
  ok('write happened', calls.some(c => /slotctl set --slot b --unbootable 1/.test(c)), calls.join(' | '));
  const st = fs.readFileSync(path.join(work,'state'),'utf8');
  const bval = parseInt((st.match(/^b=(0x[0-9a-f]+)/m)||[])[1] || '0', 16);
  ok('state persisted', ((bval / Math.pow(2,55)) | 0) === 1, 'b=' + bval.toString(16));
  ok('success banner', /Written/.test(text('banners')), text('banners'));

  // now b unbootable=1, toggling a should be refused
  const aUnb = $('card-a').querySelectorAll('.sw input')[0];
  aUnb.checked = true;
  aUnb.dispatchEvent(new w.Event('change'));
  await sleep(10);
  ok('both-unbootable banner', /Both slots/.test(text('banners')), text('banners'));

  // Reset must drop the draft, disable Apply, and snap back to persisted state.
  // (b was just written as unbootable, so the persisted switch is checked.)
  const swB = $('card-b').querySelectorAll('.sw input')[0];
  swB.checked = false;
  swB.dispatchEvent(new w.Event('change'));
  ok('draft makes apply enabled', $('apply-b').disabled === false);
  const resetB = $('card-b').querySelector('.bar button.ghost');
  ok('reset button enabled', resetB.disabled === false);
  resetB.click();
  await sleep(20);
  ok('reset disables apply', $('apply-b').disabled === true);
  ok('reset restores persisted switch',
     $('card-b').querySelectorAll('.sw input')[0].checked === true);

  // backup
  $('btn-backup').click();
  await sleep(400);
  ok('backup ok banner', /Saved/.test(text('banners')), text('banners'));

  // restore sheet lists the backup
  $('btn-restore').click();
  await sleep(300);
  ok('restore sheet open', $('sheet-restore').classList.contains('open'));
  ok('backup listed', /gpt-test\.bin/.test($('rs-list').textContent), $('rs-list').textContent);
  ok('restore preselected', $('rs-ok').disabled === false);

  // log toggle
  $('btn-log').click();
  ok('log visible', !$('log').classList.contains('hidden'));

  let fails = 0;
  for (const [st, name, extra] of results) {
    if (st === 'FAIL') fails++;
    console.log(`${st}  ${name}${extra ? '   ['+extra.slice(0,110)+']' : ''}`);
  }
  console.log(fails ? `\n${fails} FAILED` : '\nALL PASS');
  process.exit(fails ? 1 : 0);
})();
