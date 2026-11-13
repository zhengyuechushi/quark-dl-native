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
check('请求里带了保存目录', !!pending && /dir=%2Fdata%2Fhomebrew/.test(pending.body),
      pending && pending.body);
check('等待时也告诉用户在干什么', /正在换直链/.test(msg.textContent), msg.textContent);

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

console.log('');
if (failures) {
  console.log(`${failures} 项失败`);
  process.exit(1);
}
console.log('全部通过');
