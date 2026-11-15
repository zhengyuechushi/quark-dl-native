/* Runs the page's own script against a stub DOM and drives the queue button.
 *
 * Why this exists: "I selected a file, pressed 加入下载队列, nothing happened"
 * was not a backend failure. Every answer the button produced - "pick something
 * first", "queued 3", the reason a link failed - was written into the *account*
 * card's status line, which sits above the QR image and the whole cookie block,
 * so it was always off-screen. A screenshot of the page cannot catch that; a
 * test that presses the button and looks at what the user would see, can.
 *
 * The page ships inside the ELF and cannot be unit-tested any other way, so the
 * script is extracted from index.html and run in a vm with just enough DOM. No
 * jsdom, no network: every request is answered from the table below.
 *
 *   node tools/test-page.mjs
 */

import fs from 'node:fs';
import vm from 'node:vm';

const HTML_URL = new URL('../assets/index.html', import.meta.url);
const html = fs.readFileSync(HTML_URL, 'utf8');
const inline = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)];
if (inline.length !== 1) {
  console.error(`expected exactly one inline script, found ${inline.length}`);
  process.exit(1);
}

/* ------------------------------------------------------------ DOM stub */

let failures = 0;
function check(name, cond, detail) {
  if (cond) {
    console.log(`  ok   ${name}`);
  } else {
    failures++;
    console.log(`  FAIL ${name}${detail ? ' — ' + detail : ''}`);
  }
}

function makeNode(tag) {
  const node = {
    tagName: String(tag).toUpperCase(),
    id: '',
    className: '',
    style: {},
    value: '',
    disabled: false,
    checked: false,
    type: '',
    textContent: '',
    children: [],
    onclick: null,
    onchange: null,
    appendChild(child) {
      this.children.push(child);
      return child;
    },
    getElementsByTagName(want) {
      const out = [];
      const walk = (n) => {
        for (const c of n.children) {
          if (c.tagName === String(want).toUpperCase()) out.push(c);
          walk(c);
        }
      };
      walk(this);
      return out;
    },
  };
  /* Real DOM drops the children when textContent or innerHTML is assigned, and
   * the page relies on that to clear the listing and the task list. */
  let text = '';
  Object.defineProperty(node, 'textContent', {
    get() {
      return text;
    },
    set(v) {
      text = v === null || v === undefined ? '' : String(v);
      if (!text) node.children.length = 0;
    },
  });
  Object.defineProperty(node, 'innerHTML', {
    get() {
      return text;
    },
    set(v) {
      text = v === null || v === undefined ? '' : String(v);
      if (!text) node.children.length = 0;
    },
  });
  return node;
}

const byId = new Map();
function nodeFor(id) {
  if (!byId.has(id)) byId.set(id, makeNode('div'));
  return byId.get(id);
}

const document = {
  title: '',
  getElementById: (id) => nodeFor(id),
  createElement: (tag) => makeNode(tag),
  createTextNode: (t) => {
    const n = makeNode('#text');
    n.textContent = t;
    return n;
  },
  /* The page's tab navigation and pane switching query by class. The stub has
   * no HTML parser, so these return the elements the script binds to rather
   * than a real selector match. */
  querySelectorAll: (sel) => {
    if (sel === '.nav-item') {
      const out = [];
      for (const name of ['status', 'drive', 'dl', 'log']) {
        const n = makeNode('button');
        n.setAttribute = (k, v) => { n[k] = v; };
        n.getAttribute = (k) => n[k];
        n.setAttribute('data-pane', name);
        n.className = 'nav-item';
        out.push(n);
      }
      return out;
    }
    if (sel === '.pane') {
      const out = [];
      for (const name of ['status', 'drive', 'dl', 'log']) {
        const n = makeNode('section');
        n.id = 'pane-' + name;
        n.className = 'pane';
        out.push(n);
      }
      return out;
    }
    return [];
  },
  querySelector: () => null,
};

/* --------------------------------------------------------- XHR + routes */

const requests = [];
let route = () => ({ status: 200, text: '{}' });

class FakeXHR {
  constructor() {
    this.headers = {};
    this.status = 0;
    this.responseText = '';
    this.timeout = 0;
  }
  open(method, url) {
    this.method = method;
    this.url = url;
  }
  setRequestHeader(k, v) {
    this.headers[k] = v;
  }
  send(body) {
    this.body = body === undefined ? null : body;
    requests.push(this);
  }
  /* Not called by send(): the test drains the queue itself, so it can decide
   * what each endpoint answers and in what order.
   *
   * onload fires for *any* complete response, including 4xx/5xx - that is what
   * XHR does, and the page branches on xhr.status inside onload. Routing a 500
   * to onerror here would test a branch the browser never takes. */
  deliver() {
    const r = route(this.method, this.url, this.body);
    this.status = r.status;
    this.responseText = r.text;
    if (this.onload) this.onload();
  }
}

/* Ticking a row toggles it, so a failure that leaves the selection alone (on
 * purpose: the user may want to retry) has to be accounted for before the next
 * press. */
function ensureChecked(row) {
  const cb = row.children.find((c) => c.tagName === 'INPUT');
  if (!cb.checked) row.onclick({ target: row.children[1] });
  return cb;
}

function drain(limit = 50) {
  for (let i = 0; i < limit && requests.length; i++) {
    const next = requests.shift();
    next.deliver();
  }
}

const timers = [];
const context = {
  document,
  window: {
    event: null,
    prompt: () => null,
    qrCode: { encode: () => ({ size: 1, modules: [[0]] }), darkRuns: () => [] },
  },
  XMLHttpRequest: FakeXHR,
  setInterval: (fn, ms) => {
    const t = { fn, ms, cleared: false };
    timers.push(t);
    return t;
  },
  clearInterval: (t) => {
    if (t) t.cleared = true;
  },
  setTimeout: (fn) => {
    const t = { fn, cleared: false };
    timers.push(t);
    return t;
  },
  clearTimeout: (t) => {
    if (t) t.cleared = true;
  },
  Date,
  Math,
  JSON,
  console,
  parseInt,
  parseFloat,
  isNaN,
  encodeURIComponent,
  decodeURIComponent,
  Error,
};
context.window.document = document;
context.globalThis = context;

/* ------------------------------------------------------------- the page */

const LIST_ENTRIES = [
  { fid: 'f1', name: '第一集.mkv', size: 734003200, dir: false },
  { fid: 'd1', name: '第二季', size: 0, dir: true },
  { fid: 'f2', name: '花絮.mp4', size: 10485760, dir: false },
];

const DEFAULTS = {
  '/api/quark/status': () => ({ ok: true, logged_in: true, nickname: '测试' }),
  '/api/quark/list': () => ({ ok: true, entries: LIST_ENTRIES }),
  '/api/dl/list': () => ({ ok: true, tasks: [] }),
  '/api/diag': () => ({ version: '0.4.6', build: 'test', pid: 1, dl: {} }),
};

let addAnswer = { ok: true, queued: 1, failed: 0 };
route = (method, url) => {
  const path = url.split('?')[0];
  if (path === '/api/quark/add') return { status: 200, text: JSON.stringify(addAnswer) };
  const d = DEFAULTS[path];
  return { status: 200, text: JSON.stringify(d ? d() : {}) };
};

vm.createContext(context);
vm.runInContext(inline[0][1], context, { filename: 'index.html#script' });
drain();

/* --------------------------------------------------------------- checks */

console.log('列表渲染');
const list = nodeFor('qList');
check('三条记录都渲染出来', list.children.length === LIST_ENTRIES.length,
      `got ${list.children.length}`);

const row0 = list.children[0];
const cb0 = row0.children.find((c) => c.tagName === 'INPUT');
const nm0 = row0.children.find((c) => c.tagName === 'SPAN');
check('第一行有复选框和文件名', !!cb0 && !!nm0);
check('未选中时行没有高亮', !/\bon\b/.test(row0.className), row0.className);

console.log('\n未勾选就点「加入下载队列」');
const btn = nodeFor('qAdd');
/* The stub has no HTML parser, so the button's own idle label is seeded here:
 * it is the text the page restores after a run. */
btn.textContent = '加入下载队列';
const labelBefore = btn.textContent;
const msg = nodeFor('qAddMsg');
check('提示元素在网盘卡里（不是账号卡的 qMsg）', !!msg);
btn.onclick();
check('立刻给出可见提示', /先在上面勾选/.test(msg.textContent), msg.textContent);
check('提示是显示状态', msg.style.display === 'block', msg.style.display);
check('提示用错误色', msg.style.color === '#f87171', msg.style.color);
check('没有发请求', requests.length === 0, `${requests.length} 个请求`);

console.log('\n勾选一行（点整行，不是点小方框）');
row0.onclick({ target: nm0 });
check('复选框被勾上', cb0.checked === true);
check('行有高亮', /\bon\b/.test(row0.className), row0.className);
const selLabel = nodeFor('qSel');
check('已选 1 项', /已选 1 项/.test(selLabel.textContent), selLabel.textContent);
check('已选数量变色', selLabel.style.color === '#7aa2ff', selLabel.style.color);

console.log('\n目录名点击仍然是进目录，不勾选');
const row1 = list.children[1];
const cb1 = row1.children.find((c) => c.tagName === 'INPUT');
const nm1 = row1.children.find((c) => c.tagName === 'SPAN');
row1.onclick({ target: nm1 });
check('点目录名没有勾选它', cb1.checked === false);
drain(); /* 进目录会重新列一次 */

console.log('\n勾选后点「加入下载队列」');
requests.length = 0;
btn.onclick();
check('按钮被禁用', btn.disabled === true);
check('按钮显示进度而不是静止', /^处理中 \d+ 秒$/.test(btn.textContent), btn.textContent);
const pending = requests.find((r) => r.url.split('?')[0] === '/api/quark/add');
check('请求发到了 /api/quark/add', !!pending);
check('items 带上了勾选的文件 id', !!pending && /items=f1%09/.test(pending.body),
      pending && pending.body);
check('items 带上了 d/f 标记', !!pending && /%09f(&|$)/.test(pending.body),
      pending && pending.body);
/* 0.12.2: the default is its own subdirectory, not /data/homebrew itself. */
check('请求里带了保存目录', !!pending && /dir=%2Fdata%2Fhomebrew%2FQuarkdownload(&|$)/.test(pending.body),
      pending && pending.body);
check('等待时也告诉用户在干什么', /正在加入队列/.test(msg.textContent), msg.textContent);

addAnswer = { ok: true, queued: 1, failed: 0 };
pending.deliver();
check('按钮恢复原样', btn.disabled === false && btn.textContent === labelBefore,
      btn.textContent);
check('结果写在按钮旁边', /已加入 1 个任务/.test(msg.textContent), msg.textContent);
check('成功用绿色', msg.style.color === '#4ade80', msg.style.color);
check('勾选被清空', pending && /已选 0 项/.test(selLabel.textContent), selLabel.textContent);

console.log('\n后端一个都没换到直链');
route = (method, url) => {
  const path = url.split('?')[0];
  if (path === '/api/quark/add') {
    return {
      status: 200,
      text: JSON.stringify({ ok: true, queued: 0, failed: 1, error: '一个直链都没换到' }),
    };
  }
  const d = DEFAULTS[path];
  return { status: 200, text: JSON.stringify(d ? d() : {}) };
};
const list2 = nodeFor('qList');
drain();
const row0b = list2.children[0];
ensureChecked(row0b);
requests.length = 0;
btn.onclick();
const second = requests.find((r) => r.url.split('?')[0] === '/api/quark/add');
check('第二次请求已发出', !!second);
second.deliver();
check('失败原因显示出来', /一个直链都没换到/.test(msg.textContent), msg.textContent);
check('失败用错误色', msg.style.color === '#f87171', msg.style.color);
check('失败后按钮可用', btn.disabled === false);

console.log('\n网络层直接失败');
route = (method, url) => {
  if (url.split('?')[0] === '/api/quark/add') return { status: 500, text: 'boom' };
  const d = DEFAULTS[url.split('?')[0]];
  return { status: 200, text: JSON.stringify(d ? d() : {}) };
};
drain();
const list3 = nodeFor('qList');
const row0c = list3.children[0];
ensureChecked(row0c);
requests.length = 0;
btn.onclick();
const third = requests.find((r) => r.url.split('?')[0] === '/api/quark/add');
third.deliver();
check('HTTP 错误也说得出来', /加入失败/.test(msg.textContent) && /500/.test(msg.textContent),
      msg.textContent);
check('并且带上了耗时', /用时 \d+ 秒/.test(msg.textContent), msg.textContent);

console.log('\n队列按顶层文件夹分组');
/* Feed the queue a mix of folder tasks (relative-path names) and a bare file,
 * then run the queue poll and inspect the DOM the page built. */
DEFAULTS['/api/dl/list'] = () => ({
  ok: true,
  tasks: [
    { id: 1, state: 'queued', done: 0, total: 100, attempts: 0, name: '剧集/第01话/第01集.mkv', dir: '/data/homebrew', exists: null },
    { id: 2, state: 'done', done: 100, total: 100, attempts: 0, name: '剧集/第02话/第01集.mkv', dir: '/data/homebrew', exists: true },
    { id: 3, state: 'queued', done: 0, total: 50, attempts: 0, name: '音乐/专辑/单曲.mp3', dir: '/data/homebrew', exists: null },
    { id: 4, state: 'queued', done: 0, total: 200, attempts: 0, name: '单文件.bin', dir: '/data/homebrew', exists: null },
  ],
});
route = (method, url) => {
  const d = DEFAULTS[url.split('?')[0]];
  return { status: 200, text: JSON.stringify(d ? d() : {}) };
};
drain(); /* pollQueue was scheduled at start-up; run it against the new list */
const tasksHost = nodeFor('tasks');
const grps = tasksHost.children.filter((c) => c.className.indexOf('grp') === 0);
check('两个文件夹分组', grps.length === 2, `got ${grps.length}`);
const head0 = grps[0].children.find((c) => c.className === 'grp-head');
check('第一个分组标题是「剧集」',
      !!head0 && head0.children.some((c) => c.className === 'grp-title' && c.textContent === '剧集'));
check('分组计数是 done/总数',
      !!head0 && head0.children.some((c) => c.className === 'grp-count' && c.textContent === '1 / 2'));
const flat = tasksHost.children.filter((c) => c.className === 'task');
check('裸文件仍平铺', flat.length === 1);
check('分组默认折叠', !/\bopen\b/.test(grps[0].className), grps[0].className);
/* Clicking the header toggles the open class. */
const head = grps[0].children.find((c) => c.className === 'grp-head');
head.onclick();
check('点击后展开', /\bopen\b/.test(grps[0].className), grps[0].className);
/* The 2s pollQueue rebuilds the whole list; the open choice must survive it
 * instead of snapping shut. Re-run the poll and re-grab the freshly-built node. */
drain();
const grps2 = nodeFor('tasks').children.filter((c) => c.className.indexOf('grp') === 0);
check('刷新后展开态保持', /\bopen\b/.test(grps2[0].className), grps2[0].className);
const head2 = grps2[0].children.find((c) => c.className === 'grp-head');
head2.onclick();
check('刷新后再点收起', !/\bopen\b/.test(grps2[0].className), grps2[0].className);

console.log('\n勾选文件夹时，文件夹名作为 d 标记一起发出');
/* The folder row's checkbox is the way in (its name span navigates). The name
 * must travel as the tab field so the backend can use it as the root prefix of
 * the recreated tree - without it the folder vanishes and its files land loose. */
route = (method, url) => {
  const path = url.split('?')[0];
  if (path === '/api/quark/add') return { status: 200, text: JSON.stringify(addAnswer) };
  const d = DEFAULTS[path];
  return { status: 200, text: JSON.stringify(d ? d() : {}) };
};
requests.length = 0;
drain(); /* settle any queued poll before the selection below */
const listD = nodeFor('qList');
/* The "network layer failed" test above left f1 ticked (a failure keeps the
 * selection so the user can retry). Untick it so the folder is the only pick. */
const f1Row = listD.children[0];
const f1Cb = f1Row.children.find((c) => c.tagName === 'INPUT');
if (f1Cb.checked) { f1Cb.checked = false; if (f1Cb.onchange) f1Cb.onchange(); }
const folderRow = listD.children[1]; /* 第二季 (d1) */
const folderCb = folderRow.children.find((c) => c.tagName === 'INPUT');
folderCb.checked = true;
if (folderCb.onchange) folderCb.onchange();
check('只勾选了文件夹一行', /已选 1 项/.test(selLabel.textContent), selLabel.textContent);
requests.length = 0;
btn.onclick();
const dirReq = requests.find((r) => r.url.split('?')[0] === '/api/quark/add');
check('文件夹请求发到了 /api/quark/add', !!dirReq);
check('items 把文件夹 id 和 d 标记一起发出',
      !!dirReq && /items=d1%09/.test(dirReq.body) && /%09d/.test(dirReq.body),
      dirReq && dirReq.body);
check('items 把文件夹名当第二段发出',
      !!dirReq && /items=d1%09%E7%AC%AC%E4%BA%8C%E5%AD%A3%09d/.test(dirReq.body),
      dirReq && dirReq.body);

console.log('\n暂停/继续/取消 与 速度显示');
/* A running task shows 暂停 + 取消, and its meta line carries a speed. */
DEFAULTS['/api/dl/list'] = () => ({
  ok: true, limit_drive: 2, limit_link: 2,
  tasks: [
    { id: 10, state: 'running', done: 5000000, total: 10000000, attempts: 1, speed: 2400000, name: '剧集/第01话.mkv', dir: '/data/homebrew', exists: null },
    { id: 11, state: 'queued', done: 0, total: 50, attempts: 0, speed: 0, name: '单文件.bin', dir: '/data/homebrew', exists: null },
  ],
});
route = (method, url) => {
  const path = url.split('?')[0];
  const d = DEFAULTS[path];
  return { status: 200, text: JSON.stringify(d ? d() : {}) };
};
drain();
const tHost = nodeFor('tasks');
const grpNodes = tHost.children.filter((c) => c.className.indexOf('grp') === 0);
const runGrp = grpNodes[0];
const runTask = runGrp.children.find((c) => c.className === 'grp-body').children[0];
const runBtns = runTask.getElementsByTagName('BUTTON');
check('下载中有暂停和取消两个按钮', runBtns.length === 2, `got ${runBtns.length}`);
check('第一个按钮是「暂停」', runBtns.some((b) => b.textContent === '暂停'));
check('有「取消」按钮', runBtns.some((b) => b.textContent === '取消'));
const runMeta = runTask.children.find((c) => c.className === 'meta');
check('下载中显示速度', !!runMeta && /\/s/.test(runMeta.textContent), runMeta && runMeta.textContent);
const grpHead0 = runGrp.children.find((c) => c.className === 'grp-head');
check('文件夹头显示聚合速度', !!grpHead0 && grpHead0.children.some((c) => c.className === 'grp-speed'),
      grpHead0 && grpHead0.children.map((c) => c.className + ':' + c.textContent).join('|'));

/* 点「暂停」发 /api/dl/pause，点「继续」发 /api/dl/resume，取消发 /api/dl/cancel。 */
requests.length = 0;
const pauseBtn = runBtns.find((b) => b.textContent === '暂停');
pauseBtn.onclick();
let pauseReq = requests.find((r) => r.url.split('?')[0] === '/api/dl/pause');
check('点暂停发到 /api/dl/pause', !!pauseReq);
requests.length = 0;
const cancelBtn = runBtns.find((b) => b.textContent === '取消');
cancelBtn.onclick();
const cancelReq = requests.find((r) => r.url.split('?')[0] === '/api/dl/cancel');
check('点取消发到 /api/dl/cancel', !!cancelReq);
requests.length = 0;

/* 暂停态的任务显示「继续」。触发一次 pollQueue 渲染（借 clear 按钮的回调）。 */
DEFAULTS['/api/dl/list'] = () => ({
  ok: true, limit_drive: 2, limit_link: 2,
  tasks: [
    { id: 10, state: 'paused', done: 5000000, total: 10000000, attempts: 1, speed: 0, name: '剧集/第01话.mkv', dir: '/data/homebrew', exists: null },
  ],
});
requests.length = 0;
nodeFor('clear').onclick(); /* posts /api/dl/clear then re-polls the queue */
drain();
const pGrp = nodeFor('tasks').children.find((c) => c.className.indexOf('grp') === 0);
const pTask = pGrp.children.find((c) => c.className === 'grp-body').children[0];
const pBtns = pTask.getElementsByTagName('BUTTON');
check('暂停态显示「继续」', pBtns.some((b) => b.textContent === '继续'));
check('暂停态仍有「取消」', pBtns.some((b) => b.textContent === '取消'));
requests.length = 0;
const resumeBtn = pBtns.find((b) => b.textContent === '继续');
resumeBtn.onclick();
const resumeReq = requests.find((r) => r.url.split('?')[0] === '/api/dl/resume');
check('点继续发到 /api/dl/resume', !!resumeReq);

console.log('\n并发数选择器');
/* Each page has its own budget, so the selector must say which one it is
 * changing and must not smuggle the directory along with it. */
requests.length = 0;
const limitSel = nodeFor('limitSel');
limitSel.value = '3';
if (limitSel.onchange) limitSel.onchange();
const setReq = requests.find((r) => r.url.split('?')[0] === '/api/settings' && /limit=3/.test(r.body));
check('改网盘并发发 limit=3 到 /api/settings', !!setReq, setReq && setReq.body);
check('网盘并发带 src=drive', !!setReq && /(^|&)src=drive(&|$)/.test(setReq.body),
      setReq && setReq.body);
check('改并发不再连带提交目录', !!setReq && !/dir=/.test(setReq.body),
      setReq && setReq.body);
drain();

/* The settings reply carries both budgets, so each selector echoes its own. */
DEFAULTS['/api/settings'] = () => ({
  ok: true, dir: '/data/homebrew/Quarkdownload', limit_drive: 2, limit_link: 4,
  tasks: 0, running: 0,
});

requests.length = 0;
const limitSel2 = nodeFor('limitSel2');
limitSel2.value = '4';
if (limitSel2.onchange) limitSel2.onchange();
const setReq2 = requests.find((r) => r.url.split('?')[0] === '/api/settings' && /limit=4/.test(r.body));
check('直链页也有并发选择器，改它发 limit=4', !!setReq2, setReq2 && setReq2.body);
check('直链并发带 src=link', !!setReq2 && /(^|&)src=link(&|$)/.test(setReq2.body),
      setReq2 && setReq2.body);
drain();
check('直链并发在原处回显本页那份', /并发数已设为 4/.test(nodeFor('limitState2').textContent),
      nodeFor('limitState2').textContent);

console.log('\n直链下载页');
/* 0.12.4: this pane is now usable on its own - the queue button sits under the
 * paste box, and the directory line carries 记住这个目录 / 清掉已完成 just like
 * the queue pane, instead of making the user switch tabs for them. */
nodeFor('dir2').value = '/data/homebrew/Quarkdownload';
requests.length = 0;
nodeFor('saveDir2').onclick();
/* Found before drain(): drain() shifts the queue, so a request is only
 * inspectable while it is still pending. */
const save2Req = requests.find((r) => r.url.split('?')[0] === '/api/settings');
drain();
check('直链页的「记住这个目录」发到 /api/settings',
      !!save2Req && /dir=%2Fdata%2Fhomebrew%2FQuarkdownload(&|$)/.test(save2Req.body),
      save2Req && save2Req.body);
check('保存成功后在原处告诉用户存到哪',
      /默认目录已保存/.test(nodeFor('dirState2').textContent),
      nodeFor('dirState2').textContent);

requests.length = 0;
nodeFor('clear2').onclick();
const clear2Req = requests.find((r) => r.url.split('?')[0] === '/api/dl/clear');
drain();
check('直链页的「清掉已完成」发到 /api/dl/clear', !!clear2Req);
check('清空后在原处回显', /已清掉本页已完成的任务/.test(nodeFor('dirState2').textContent),
      nodeFor('dirState2').textContent);

console.log('\n两个页面各看各的队列');
/* One queue on the server, two lists on screen: a drive task carries src
 * "drive", a pasted link carries "link". The split is presentation only, so the
 * page must also say out loud that the concurrency limit is shared - otherwise
 * "0 个下载中，5 个等待中" on this page reads like a stuck queue. */
function allText(node) {
  let out = node.textContent || '';
  for (const c of node.children || []) out += ' ' + allText(c);
  return out;
}

DEFAULTS['/api/dl/list'] = () => ({
  ok: true, limit_drive: 4, limit_link: 3,
  tasks: [
    { id: 20, state: 'running', src: 'drive', done: 1, total: 10, attempts: 0, speed: 0, name: '网盘文件.mkv', dir: '/data/homebrew/Quarkdownload', exists: null },
    { id: 21, state: 'queued', src: 'link', done: 0, total: 10, attempts: 0, speed: 0, name: '直链文件.bin', dir: '/data/homebrew/Quarkdownload', exists: null },
  ],
});

requests.length = 0;
nodeFor('clear2').onclick(); /* clears this page, then re-polls */
const clearLinkReq = requests.find((r) => r.url === '/api/dl/clear?src=link');
drain();

check('直链页清空只清直链那半（?src=link）', !!clearLinkReq,
      clearLinkReq && clearLinkReq.url);
const driveText = allText(nodeFor('tasks'));
const linkText = allText(nodeFor('tasks2'));
check('网盘页只列网盘任务', /网盘文件/.test(driveText) && !/直链文件/.test(driveText), driveText);
check('直链页只列直链任务', /直链文件/.test(linkText) && !/网盘文件/.test(linkText), linkText);
check('直链页写的是本页自己的并发上限', /并发上限 3 个（本页单独设置）/.test(nodeFor('q2Info').textContent),
      nodeFor('q2Info').textContent);
check('两个选择器各显示各的上限',
      nodeFor('limitSel').value === '4' && nodeFor('limitSel2').value === '3',
      nodeFor('limitSel').value + '/' + nodeFor('limitSel2').value);
check('直链页数出本页有几个任务/几个在跑',
      /本页 1 个任务，0 个下载中/.test(nodeFor('q2Info').textContent),
      nodeFor('q2Info').textContent);

requests.length = 0;
nodeFor('clear').onclick();
const clearDriveReq = requests.find((r) => r.url === '/api/dl/clear?src=drive');
drain();
check('网盘页清空只清网盘那半（?src=drive）', !!clearDriveReq,
      clearDriveReq && clearDriveReq.url);

console.log('');
if (failures) {
  console.log(`${failures} 项失败`);
  process.exit(1);
}
console.log('全部通过');
