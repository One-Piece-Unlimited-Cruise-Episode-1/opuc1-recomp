// Builds the product icon of One Piece: Unlimited Recompilation Episode 1 from the user's own disc dump.
//   node tools/make-icon.mjs [--dvd <DATA root>] [--out <dir>] [--frame 1|2]
// Source: memcard/banner_ep1.en.tpl, the game's Wii save banner. TPL image 0 is the 192x64 banner, images 1 and 2
// are the 48x48 RGB5A3 save icon frames (the save shows image 1 for six of its eight frames; vault entities/save-data).
// Output (default build/icon/): opuc1_recomp.ico (16-256 px, PNG entries), opuc1_recomp.png (256 px),
// opuc1_recomp_48.png (the frame as stored) and opuc1_recomp.rc for the resource compiler.
// The icon is game art, so it is generated into build/ like the translated code and never committed.
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..');
const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : def; };
const dvd = path.resolve(opt('--dvd', path.join(root, 'data/extract/DATA')));
const out = path.resolve(opt('--out', path.join(root, 'build/icon')));
const frame = Number(opt('--frame', '1'));

// ---- TPL (big-endian): magic 0x0020AF30, image count, image table offset; image header {h u16, w u16, fmt u32, data u32}
function readTpl(buf, index) {
    if (buf.readUInt32BE(0) !== 0x0020af30) throw new Error('not a TPL file');
    const count = buf.readUInt32BE(4), table = buf.readUInt32BE(8);
    if (index >= count) throw new Error(`TPL has ${count} images, no image ${index}`);
    const ih = buf.readUInt32BE(table + index * 8);
    const h = buf.readUInt16BE(ih), w = buf.readUInt16BE(ih + 2), fmt = buf.readUInt32BE(ih + 4), data = buf.readUInt32BE(ih + 8);
    if (fmt !== 5) throw new Error(`image ${index} is GX format ${fmt}, expected 5 (RGB5A3)`);
    return { w, h, rgba: decodeRgb5a3(buf, data, w, h) };
}

// RGB5A3: 4x4 texel tiles, 16-bit BE texels. Top bit set: RGB555 opaque; clear: A3 RGB444.
function decodeRgb5a3(buf, off, w, h) {
    const px = new Uint8Array(w * h * 4);
    const e = (v, bits) => (v << (8 - bits)) | (v >> (2 * bits - 8));
    for (let ty = 0; ty < h; ty += 4)
        for (let tx = 0; tx < w; tx += 4)
            for (let y = 0; y < 4; y++)
                for (let x = 0; x < 4; x++, off += 2) {
                    const X = tx + x, Y = ty + y;
                    if (X >= w || Y >= h) continue;
                    const v = buf.readUInt16BE(off), o = (Y * w + X) * 4;
                    if (v & 0x8000) {
                        px[o] = e((v >> 10) & 31, 5); px[o + 1] = e((v >> 5) & 31, 5); px[o + 2] = e(v & 31, 5); px[o + 3] = 255;
                    } else {
                        const n4 = n => (n << 4) | n;
                        px[o + 3] = Math.round(((v >> 12) & 7) * 255 / 7);
                        px[o] = n4((v >> 8) & 15); px[o + 1] = n4((v >> 4) & 15); px[o + 2] = n4(v & 15);
                    }
                }
    return px;
}

// Resample with a premultiplied-alpha bilinear (upscale) or box filter (downscale).
function resize(src, sw, sh, dw, dh) {
    const pre = new Float32Array(sw * sh * 4);
    for (let i = 0; i < sw * sh; i++) {
        const a = src[i * 4 + 3] / 255;
        pre[i * 4] = src[i * 4] * a; pre[i * 4 + 1] = src[i * 4 + 1] * a; pre[i * 4 + 2] = src[i * 4 + 2] * a; pre[i * 4 + 3] = a;
    }
    const dst = new Uint8Array(dw * dh * 4);
    const sx = sw / dw, sy = sh / dh;
    for (let y = 0; y < dh; y++)
        for (let x = 0; x < dw; x++) {
            const acc = [0, 0, 0, 0];
            if (dw >= sw) {   // bilinear
                const fx = Math.min(sw - 1, Math.max(0, (x + 0.5) * sx - 0.5)), fy = Math.min(sh - 1, Math.max(0, (y + 0.5) * sy - 0.5));
                const x0 = Math.floor(fx), y0 = Math.floor(fy), x1 = Math.min(sw - 1, x0 + 1), y1 = Math.min(sh - 1, y0 + 1);
                const ax = fx - x0, ay = fy - y0;
                for (const [xx, yy, wt] of [[x0, y0, (1 - ax) * (1 - ay)], [x1, y0, ax * (1 - ay)], [x0, y1, (1 - ax) * ay], [x1, y1, ax * ay]])
                    for (let c = 0; c < 4; c++) acc[c] += pre[(yy * sw + xx) * 4 + c] * wt;
            } else {          // box
                let n = 0;
                for (let yy = Math.floor(y * sy); yy < Math.ceil((y + 1) * sy); yy++)
                    for (let xx = Math.floor(x * sx); xx < Math.ceil((x + 1) * sx); xx++, n++)
                        for (let c = 0; c < 4; c++) acc[c] += pre[(yy * sw + xx) * 4 + c];
                for (let c = 0; c < 4; c++) acc[c] /= n;
            }
            const o = (y * dw + x) * 4, a = acc[3];
            dst[o + 3] = Math.round(a * 255);
            for (let c = 0; c < 3; c++) dst[o + c] = a > 0 ? Math.min(255, Math.round(acc[c] / a)) : 0;
        }
    return dst;
}

// ---- PNG / ICO writers
const crcTable = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
const crc32 = b => { let c = 0xffffffff; for (const x of b) c = crcTable[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
function png(rgba, w, h) {
    const chunk = (type, data) => {
        const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
        const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
        const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
        return Buffer.concat([len, td, crc]);
    };
    const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
    const raw = Buffer.alloc((w * 4 + 1) * h);
    for (let y = 0; y < h; y++) { raw[y * (w * 4 + 1)] = 0; Buffer.from(rgba.buffer, rgba.byteOffset + y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1); }
    return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]);
}
function ico(images) {   // images: [{size, png}]
    const head = Buffer.alloc(6 + 16 * images.length);
    head.writeUInt16LE(0, 0); head.writeUInt16LE(1, 2); head.writeUInt16LE(images.length, 4);
    let off = head.length;
    images.forEach((im, i) => {
        const e = 6 + i * 16;
        head[e] = im.size >= 256 ? 0 : im.size; head[e + 1] = im.size >= 256 ? 0 : im.size;
        head.writeUInt16LE(1, e + 4); head.writeUInt16LE(32, e + 6);
        head.writeUInt32LE(im.png.length, e + 8); head.writeUInt32LE(off, e + 12);
        off += im.png.length;
    });
    return Buffer.concat([head, ...images.map(im => im.png)]);
}

const tplPath = path.join(dvd, 'files/memcard/banner_ep1.en.tpl');
if (!fs.existsSync(tplPath)) {
    console.error(`[make-icon] ${tplPath} not found; pass --dvd <extracted DATA root>`);
    process.exit(2);
}
const icon = readTpl(fs.readFileSync(tplPath), frame);
fs.mkdirSync(out, { recursive: true });
const sizes = [16, 24, 32, 48, 64, 128, 256];
const entries = sizes.map(s => ({ size: s, png: png(s === icon.w ? icon.rgba : resize(icon.rgba, icon.w, icon.h, s, s), s, s) }));
fs.writeFileSync(path.join(out, 'opuc1_recomp.ico'), ico(entries));
fs.writeFileSync(path.join(out, 'opuc1_recomp.png'), entries.at(-1).png);
fs.writeFileSync(path.join(out, 'opuc1_recomp_48.png'), png(icon.rgba, icon.w, icon.h));
fs.writeFileSync(path.join(out, 'opuc1_recomp.rc'), '1 ICON "opuc1_recomp.ico"\n');
console.log(`[make-icon] ${path.relative(root, tplPath)} image ${frame} (${icon.w}x${icon.h} RGB5A3) -> ${path.join(out, 'opuc1_recomp.ico')} (${sizes.join('/')} px)`);
