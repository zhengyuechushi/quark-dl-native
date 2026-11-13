// QR 码编码（byte mode，纠错级别 M），零依赖，跑在 QuickJS 上。
// PS5 上没有任何二维码库可用，也不该为一个二维码引入打包体积，所以自己实现。
// 输出 modules：普通数组（PS5 的 QuickJS 没有 TypedArray），1 = 深色模块，索引 y * size + x。

// ---- GF(256)：本原多项式 x^8 + x^4 + x^3 + x^2 + 1 ----
const EXP = new Array(512).fill(0);
const LOG = new Array(256).fill(0);
(function buildLogTables() {
  let x = 1;
  for (let i = 0; i < 255; i++) {
    EXP[i] = x;
    LOG[x] = i;
    x <<= 1;
    if (x & 0x100) x ^= 0x11d;
  }
  // 表尾再铺一轮，省掉 mul 里的取模。
  for (let i = 255; i < 512; i++) EXP[i] = EXP[i - 255];
})();

function mul(a, b) {
  if (a === 0 || b === 0) return 0;
  return EXP[LOG[a] + LOG[b]];
}

/** 生成多项式 g(x) = ∏(x + α^i)，i 取 0..degree-1。返回系数数组，下标 0 为最高次。 */
function generatorPoly(degree) {
  let poly = [1];
  for (let i = 0; i < degree; i++) {
    const next = new Array(poly.length + 1).fill(0);
    for (let j = 0; j < poly.length; j++) {
      next[j] ^= poly[j];
      next[j + 1] ^= mul(poly[j], EXP[i]);
    }
    poly = next;
  }
  return poly;
}

/** Reed-Solomon 余数，即纠错码字。 */
function rsRemainder(data, ecLength) {
  const gen = generatorPoly(ecLength);
  const rem = new Array(ecLength).fill(0);
  for (let i = 0; i < data.length; i++) {
    const factor = data[i] ^ rem[0];
    rem.copyWithin(0, 1);
    rem[ecLength - 1] = 0;
    for (let j = 0; j < ecLength; j++) rem[j] ^= mul(gen[j + 1], factor);
  }
  return rem;
}

// ---- 版本参数表（纠错级别 M）----
// [每块纠错码字, 第一组块数, 第一组数据码字, 第二组块数, 第二组数据码字]
const VERSIONS_M = [
  [10, 1, 16, 0, 0],   // 1
  [16, 1, 28, 0, 0],   // 2
  [26, 1, 44, 0, 0],   // 3
  [18, 2, 32, 0, 0],   // 4
  [24, 2, 43, 0, 0],   // 5
  [16, 4, 27, 0, 0],   // 6
  [18, 4, 31, 0, 0],   // 7
  [22, 2, 38, 2, 39],  // 8
  [22, 3, 36, 2, 37],  // 9
  [26, 4, 43, 1, 44],  // 10
  [30, 1, 50, 4, 51],  // 11
  [22, 6, 33, 2, 34],  // 12
  [22, 8, 39, 1, 40],  // 13
  [24, 4, 40, 5, 41],  // 14
  [24, 5, 41, 5, 42],  // 15
];

/** 校准图形中心坐标，按版本号索引。 */
const ALIGNMENT = [
  [], [6, 18], [6, 22], [6, 26], [6, 30], [6, 34],
  [6, 22, 38], [6, 24, 42], [6, 26, 46], [6, 28, 50],
  [6, 30, 54], [6, 32, 58], [6, 34, 62], [6, 26, 46, 66], [6, 26, 48, 70],
];

function versionCapacity(version) {
  const [ec, g1Blocks, g1Data, g2Blocks, g2Data] = VERSIONS_M[version - 1];
  return g1Blocks * g1Data + g2Blocks * g2Data;
}

/** 挑能装下这段字节的最小版本；装不下返回 0。 */
function chooseVersion(byteLength) {
  for (let v = 1; v <= VERSIONS_M.length; v++) {
    const lengthBits = v <= 9 ? 8 : 16;
    const needed = 4 + lengthBits + byteLength * 8;
    if (needed <= versionCapacity(v) * 8) return v;
  }
  return 0;
}

// ---- 位流 ----
function bitStream(version, bytes) {
  const bits = [];
  const push = (value, width) => {
    for (let i = width - 1; i >= 0; i--) bits.push((value >>> i) & 1);
  };
  push(0b0100, 4);                                  // byte mode
  push(bytes.length, version <= 9 ? 8 : 16);        // 字符计数指示符
  for (let i = 0; i < bytes.length; i++) push(bytes[i], 8);

  const dataCodewords = versionCapacity(version);
  const totalBits = dataCodewords * 8;
  // 结束符 0000，随后补齐到码字边界，再以 0xEC / 0x11 交替填充。
  push(0, Math.min(4, totalBits - bits.length));
  while (bits.length % 8 !== 0) bits.push(0);
  const pad = [0xec, 0x11];
  for (let i = 0; bits.length < totalBits; i++) push(pad[i % 2], 8);

  const codewords = new Array(dataCodewords).fill(0);
  for (let i = 0; i < dataCodewords; i++) {
    let byte = 0;
    for (let b = 0; b < 8; b++) byte = (byte << 1) | bits[i * 8 + b];
    codewords[i] = byte;
  }
  return codewords;
}

/** 按 QR 规范交织数据码字与纠错码字。 */
function interleave(version, dataCodewords) {
  const [ec, g1Blocks, g1Data, g2Blocks, g2Data] = VERSIONS_M[version - 1];
  const blocks = [];
  let offset = 0;
  for (let i = 0; i < g1Blocks; i++) {
    blocks.push(dataCodewords.slice(offset, offset + g1Data));
    offset += g1Data;
  }
  for (let i = 0; i < g2Blocks; i++) {
    blocks.push(dataCodewords.slice(offset, offset + g2Data));
    offset += g2Data;
  }

  const ecBlocks = blocks.map(block => rsRemainder(block, ec));
  const out = [];
  const maxData = Math.max(g1Data, g2Data);
  for (let col = 0; col < maxData; col++) {
    for (const block of blocks) if (col < block.length) out.push(block[col]);
  }
  for (let col = 0; col < ec; col++) {
    for (const block of ecBlocks) out.push(block[col]);
  }
  return out;
}

// ---- 矩阵 ----
/**
 * 定位图形 7x7 加一圈分隔符，占据角落 8x8。
 * ox/oy 是 8x8 区域的原点；fx/fy 是图形在区域内的偏移——分隔符贴在朝向中心的一侧，
 * 所以左上角图形偏 (0,0)，右上角偏 (1,0)，左下角偏 (0,1)。
 */
function placeFinder(reserved, modules, size, ox, oy, fx, fy) {
  for (let dy = 0; dy < 8; dy++) {
    for (let dx = 0; dx < 8; dx++) {
      const x = ox + dx, y = oy + dy;
      if (x >= size || y >= size) continue;
      let dark = 0;
      const lx = dx - fx, ly = dy - fy;
      if (lx >= 0 && lx < 7 && ly >= 0 && ly < 7) {
        // 外环与内环深色，次外环浅色，中心 3x3 实心。
        const d = Math.max(Math.abs(lx - 3), Math.abs(ly - 3));
        dark = d === 2 ? 0 : 1;
      }
      modules[y * size + x] = dark;
      reserved[y * size + x] = 1;
    }
  }
}

function placeAlignment(reserved, modules, size, cx, cy) {
  for (let dy = -2; dy <= 2; dy++) {
    for (let dx = -2; dx <= 2; dx++) {
      const x = cx + dx, y = cy + dy;
      if (reserved[y * size + x]) continue;
      const d = Math.max(Math.abs(dx), Math.abs(dy));
      modules[y * size + x] = d === 1 ? 0 : 1;
      reserved[y * size + x] = 1;
    }
  }
}

const MASKS = [
  (i, j) => (i + j) % 2 === 0,
  (i, j) => i % 2 === 0,
  (i, j) => j % 3 === 0,
  (i, j) => (i + j) % 3 === 0,
  (i, j) => (Math.floor(i / 2) + Math.floor(j / 3)) % 2 === 0,
  (i, j) => ((i * j) % 2) + ((i * j) % 3) === 0,
  (i, j) => (((i * j) % 2) + ((i * j) % 3)) % 2 === 0,
  (i, j) => (((i + j) % 2) + ((i * j) % 3)) % 2 === 0,
];

/** 罚分规则，用来挑最优掩码；不是最优也能扫，只是对比度差些。 */
function penalty(modules, size) {
  let score = 0;

  // 规则 1：同色连续 5 个以上
  const runScore = (get, length) => {
    let total = 0;
    for (let i = 0; i < length; i++) {
      let run = 1;
      for (let j = 1; j < length; j++) {
        if (get(i, j) === get(i, j - 1)) {
          run++;
        } else {
          if (run >= 5) total += 3 + (run - 5);
          run = 1;
        }
      }
      if (run >= 5) total += 3 + (run - 5);
    }
    return total;
  };
  score += runScore((i, j) => modules[i * size + j], size);
  score += runScore((i, j) => modules[j * size + i], size);

  // 规则 2：2x2 同色块
  for (let y = 0; y < size - 1; y++) {
    for (let x = 0; x < size - 1; x++) {
      const v = modules[y * size + x];
      if (v === modules[y * size + x + 1] && v === modules[(y + 1) * size + x] &&
          v === modules[(y + 1) * size + x + 1]) score += 3;
    }
  }

  // 规则 3：1:1:3:1:1 图案（前后各留 4 个浅色）
  const PATTERN_A = [1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0];
  const PATTERN_B = [0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1];
  const matches = (line, start, pattern) => {
    for (let k = 0; k < 11; k++) if (line[start + k] !== pattern[k]) return false;
    return true;
  };
  const scanRow = y => {
    const line = [];
    for (let x = 0; x < size; x++) line.push(modules[y * size + x]);
    let hits = 0;
    for (let i = 0; i + 11 <= size; i++) {
      if (matches(line, i, PATTERN_A) || matches(line, i, PATTERN_B)) hits++;
    }
    return hits;
  };
  const scanCol = x => {
    const line = [];
    for (let y = 0; y < size; y++) line.push(modules[y * size + x]);
    let hits = 0;
    for (let i = 0; i + 11 <= size; i++) {
      if (matches(line, i, PATTERN_A) || matches(line, i, PATTERN_B)) hits++;
    }
    return hits;
  };
  for (let i = 0; i < size; i++) score += 40 * (scanRow(i) + scanCol(i));

  // 规则 4：深浅比例偏离 50%
  let dark = 0;
  for (let i = 0; i < modules.length; i++) dark += modules[i];
  const percent = (dark * 100) / modules.length;
  score += 10 * Math.floor(Math.abs(percent - 50) / 5);
  return score;
}

function formatBits(ecBits, mask) {
  const data = (ecBits << 3) | mask;
  let rem = data;
  for (let i = 0; i < 10; i++) rem = (rem << 1) ^ (((rem >>> 9) & 1) * 0x537);
  return ((data << 10) | (rem & 0x3ff)) ^ 0x5412;
}

function versionBits(version) {
  let rem = version;
  for (let i = 0; i < 12; i++) rem = (rem << 1) ^ (((rem >>> 11) & 1) * 0x1f25);
  return (version << 12) | (rem & 0xfff);
}

/**
 * 把文本编成二维码矩阵。
 * @param {string} text 要编码的内容（UTF-8）
 * @param {{mask?: number}} [options] 指定掩码可跳过自动选择，用于与参考实现逐位对比
 * @returns {{size: number, modules: number[], mask: number} | null} 太长或编码失败返回 null
 */
function encode(text, options = {}) {
  const bytes = [];
  for (let i = 0; i < text.length; i++) {
    const code = text.codePointAt(i);
    if (code < 0x80) {
      bytes.push(code);
    } else if (code < 0x800) {
      bytes.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f));
    } else if (code < 0x10000) {
      bytes.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f));
    } else {
      bytes.push(0xf0 | (code >> 18), 0x80 | ((code >> 12) & 0x3f),
        0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f));
      i++; // 代理对
    }
  }

  const version = chooseVersion(bytes.length);
  if (version === 0) return null;

  const size = 17 + version * 4;
  const codewords = interleave(version, bitStream(version, bytes));

  const modules = new Array(size * size).fill(0);
  const reserved = new Array(size * size).fill(0);

  // 定位图形（含分隔符），分居左上、右上、左下三角
  placeFinder(reserved, modules, size, 0, 0, 0, 0);
  placeFinder(reserved, modules, size, size - 8, 0, 1, 0);
  placeFinder(reserved, modules, size, 0, size - 8, 0, 1);

  // 定时图形
  for (let i = 8; i < size - 8; i++) {
    const on = i % 2 === 0 ? 1 : 0;
    modules[6 * size + i] = on;
    modules[i * size + 6] = on;
    reserved[6 * size + i] = 1;
    reserved[i * size + 6] = 1;
  }

  // 校准图形，跳过与定位图形重叠的三个角
  const coords = ALIGNMENT[version - 1] || [];
  for (const cy of coords) {
    for (const cx of coords) {
      const nearTopLeft = cx === 6 && cy === 6;
      const nearTopRight = cx === coords[coords.length - 1] && cy === 6;
      const nearBottomLeft = cx === 6 && cy === coords[coords.length - 1];
      if (nearTopLeft || nearTopRight || nearBottomLeft) continue;
      placeAlignment(reserved, modules, size, cx, cy);
    }
  }

  // 预留格式信息区。范围必须精确到实际占用的那些格：多留一格，数据填充就会整体错位一格。
  for (let i = 0; i <= 8; i++) {
    reserved[8 * size + i] = 1;   // 第 8 行前 9 格
    reserved[i * size + 8] = 1;   // 第 8 列前 9 格
  }
  // 第二份格式信息：第 8 列下侧 7 格 + 第 8 行右侧 8 格，另有独立一格的固定深色模块。
  // 分界是 7 不是 8——按 8/7 分会漏掉 (row8, col size-8) 这一格，数据填充会整体错位。
  for (let i = 0; i <= 6; i++) reserved[(size - 1 - i) * size + 8] = 1;
  for (let i = 0; i <= 7; i++) reserved[8 * size + (size - 8 + i)] = 1;
  reserved[(size - 8) * size + 8] = 1;
  if (version >= 7) {
    for (let i = 0; i < 6; i++) {
      for (let j = size - 11; j < size - 8; j++) reserved[j * size + i] = 1;
      for (let j = size - 11; j < size - 8; j++) reserved[i * size + j] = 1;
    }
  }

  // 数据以之字形填入，自右下角起，两列一组，跳过第 6 列
  let bitIndex = 0;
  const totalBits = codewords.length * 8;
  for (let right = size - 1; right >= 1; right -= 2) {
    // 必须直接改 right：跳过第 6 列后，后面的列对要接着新值往下走。
    // 若只改副本，第 4 列会被填两次，而第 0 列永远轮不到。
    if (right === 6) right = 5;
    for (let vert = 0; vert < size; vert++) {
      for (let k = 0; k < 2; k++) {
        const x = right - k;
        const upward = ((right + 1) & 2) === 0;
        const y = upward ? size - 1 - vert : vert;
        if (reserved[y * size + x]) continue;
        const bit = bitIndex < totalBits ? (codewords[bitIndex >> 3] >> (7 - (bitIndex & 7))) & 1 : 0;
        modules[y * size + x] = bit;
        bitIndex++;
      }
    }
  }

  // 八种掩码各算一遍罚分，取最低；指定掩码时跳过自动选择
  const forcedMask = options.mask;
  const candidates = forcedMask == null ? [0, 1, 2, 3, 4, 5, 6, 7] : [forcedMask];
  let best = null;
  for (const mask of candidates) {
    const candidate = modules.slice();
    for (let y = 0; y < size; y++) {
      for (let x = 0; x < size; x++) {
        if (reserved[y * size + x]) continue;
        if (MASKS[mask](y, x)) candidate[y * size + x] ^= 1;
      }
    }
    const score = penalty(candidate, size);
    if (best === null || score < best.score) best = {score, modules: candidate, mask};
  }

  // 写入格式信息（两份，互为备份）。注意 modules 下标是 row * size + col。
  const fmt = formatBits(0b00, best.mask);
  for (let i = 0; i < 15; i++) {
    const bit = (fmt >> i) & 1;
    // 第一份绕左上角：低 6 位走第 8 列自上而下，其余走第 8 行自右而左
    if (i < 6) best.modules[i * size + 8] = bit;
    else if (i === 6) best.modules[7 * size + 8] = bit;
    else if (i === 7) best.modules[8 * size + 8] = bit;
    else if (i === 8) best.modules[8 * size + 7] = bit;
    else best.modules[8 * size + (14 - i)] = bit;
    // 第二份：低 8 位沿第 8 行自右而左，高 7 位沿第 8 列自下而上。
    // 方向与第一份相反，且分界是 8/7 而非 7/8——这两点都曾用参考矩阵反解确认。
    if (i < 8) best.modules[8 * size + (size - 1 - i)] = bit;
    else best.modules[(size - 15 + i) * size + 8] = bit;
  }

  // 固定深色模块必须最后写：它压在格式信息第二份的末位上，先写会被覆盖。
  best.modules[(size - 8) * size + 8] = 1;

  if (version >= 7) {
    const ver = versionBits(version);
    for (let i = 0; i < 18; i++) {
      const bit = (ver >> i) & 1;
      const a = Math.floor(i / 3);
      const b = i % 3;
      best.modules[(size - 11 + b) * size + a] = bit;
      best.modules[a * size + (size - 11 + b)] = bit;
    }
  }

  return {size, modules: best.modules, mask: best.mask};
}

/**
 * 把矩阵压成每行的连续色块，供 UI 渲染。
 * 61x61 的码直接铺 3721 个方块在 PS5 上会掉帧，合并成 run 后通常只剩几百个。
 * @returns {{y: number, x: number, length: number}[][]} 按行组织的深色段
 */
function darkRuns(matrix, quiet = 0) {
  const {size, modules} = matrix;
  const rows = [];
  for (let y = 0; y < size; y++) {
    const runs = [];
    let start = -1;
    for (let x = 0; x <= size; x++) {
      const dark = x < size && modules[y * size + x] === 1;
      if (dark && start < 0) start = x;
      else if (!dark && start >= 0) {
        runs.push({x: start + quiet, length: x - start});
        start = -1;
      }
    }
    rows.push({y: y + quiet, runs});
  }
  return rows;
}

// Served as a plain <script>, so the two entry points are published on window
// instead of through module exports.
window.qrCode = {encode: encode, darkRuns: darkRuns};
