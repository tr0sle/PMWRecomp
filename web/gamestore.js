// Keeps the player's own copy of the game in the browser (IndexedDB). The site never hosts
// game files: the player picks a .zip of their installed game, which is unpacked here.
'use strict';
(function () {
  // PMR.exe this build was recompiled from (Pac-Man World Rally PC, 2006)
  const PMR_SHA256 = 'a017c5ec378b83474f57f496638f3a4d144add1f54f314e0d1e120fcac923f4c';
  const REQUIRED = ['pmr.exe', 'intrface.xpc', 'gshared.xpc', 'kart.xpc'];
  const DB = 'pmwr-game';

  let dbp = null;
  function db() {
    if (!dbp) dbp = new Promise((res, rej) => {
      const rq = indexedDB.open(DB, 1);
      rq.onupgradeneeded = () => { rq.result.createObjectStore('files'); rq.result.createObjectStore('meta'); };
      rq.onsuccess = () => res(rq.result);
      rq.onerror = () => rej(rq.error || new Error('IndexedDB is not available'));
    });
    return dbp;
  }
  const req = r => new Promise((res, rej) => { r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error); });
  const done = tx => new Promise((res, rej) => {
    tx.oncomplete = res;
    tx.onerror = tx.onabort = () => rej(storageError(tx.error));
  });
  function storageError(e) {
    const name = e && e.name || '';
    if (name === 'QuotaExceededError' || /quota/i.test(e && e.message || ''))
      return new Error('Not enough storage space in this browser for the game files (about 470 MB). Free some space on the device and try again.');
    return new Error('Could not save the game files in this browser' + (e ? ' (' + (e.name || '') + (e.message ? ': ' + e.message : '') + ')' : '') +
      '. Private/incognito tabs cannot store them: use a normal tab.');
  }

  async function info() {
    try {
      const d = await db();
      return await req(d.transaction('meta').objectStore('meta').get('info')) || null;
    } catch (e) { return null; }
  }

  async function read(name) {
    const d = await db();
    const blob = await req(d.transaction('files').objectStore('files').get(name.toLowerCase()));
    if (!blob) return null;
    return new Uint8Array(await blob.arrayBuffer());
  }

  // part of a stored file, without reading all of it
  async function readRange(name, start, end) {
    const d = await db();
    const blob = await req(d.transaction('files').objectStore('files').get(name.toLowerCase()));
    if (!blob) return null;
    return new Uint8Array(await blob.slice(start, end).arrayBuffer());
  }

  async function clear() {
    const d = await db();
    const tx = d.transaction(['files', 'meta'], 'readwrite');
    tx.objectStore('files').clear();
    tx.objectStore('meta').clear();
    await done(tx);
  }

  // ---------------------------------------------------------------- zip reading (central directory)
  async function bytes(file, start, end) { return new DataView(await file.slice(start, end).arrayBuffer()); }

  async function entries(file) {
    const tailLen = Math.min(file.size, 65536 + 22);
    const tail = await bytes(file, file.size - tailLen, file.size);
    let eocd = -1;
    for (let i = tail.byteLength - 22; i >= 0; i--) if (tail.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
    if (eocd < 0) throw new Error('This file is not a valid .zip.');
    let count = tail.getUint16(eocd + 10, true), cdSize = tail.getUint32(eocd + 12, true), cdOff = tail.getUint32(eocd + 16, true);
    if (cdOff === 0xffffffff || count === 0xffff) { /* zip64 */
      const loc = eocd - 20;
      if (loc >= 0 && tail.getUint32(loc, true) === 0x07064b50) {
        const off64 = Number(tail.getBigUint64(loc + 8, true));
        const z64 = await bytes(file, off64, off64 + 56);
        count = Number(z64.getBigUint64(32, true));
        cdSize = Number(z64.getBigUint64(40, true));
        cdOff = Number(z64.getBigUint64(48, true));
      }
    }
    const cd = await bytes(file, cdOff, cdOff + cdSize);
    const dec = new TextDecoder();
    const list = [];
    for (let p = 0, k = 0; k < count && p + 46 <= cd.byteLength; k++) {
      if (cd.getUint32(p, true) !== 0x02014b50) throw new Error('The .zip is damaged.');
      const flags = cd.getUint16(p + 8, true), method = cd.getUint16(p + 10, true);
      let csize = cd.getUint32(p + 20, true), size = cd.getUint32(p + 24, true);
      const nlen = cd.getUint16(p + 28, true), xlen = cd.getUint16(p + 30, true), clen = cd.getUint16(p + 32, true);
      let off = cd.getUint32(p + 42, true);
      const nameBytes = new Uint8Array(cd.buffer, cd.byteOffset + p + 46, nlen);
      const name = (flags & 0x800 ? dec : new TextDecoder('latin1')).decode(nameBytes).replace(/\\/g, '/');
      for (let x = p + 46 + nlen; x < p + 46 + nlen + xlen;) { /* zip64 extra field */
        const id = cd.getUint16(x, true), len = cd.getUint16(x + 2, true);
        if (id === 1) {
          let q = x + 4;
          if (size === 0xffffffff) { size = Number(cd.getBigUint64(q, true)); q += 8; }
          if (csize === 0xffffffff) { csize = Number(cd.getBigUint64(q, true)); q += 8; }
          if (off === 0xffffffff) { off = Number(cd.getBigUint64(q, true)); }
        }
        x += 4 + len;
      }
      if (!name.endsWith('/')) list.push({name, method, csize, size, off, encrypted: !!(flags & 1)});
      p += 46 + nlen + xlen + clen;
    }
    return list;
  }

  async function extract(file, e) {
    const lh = await bytes(file, e.off, e.off + 30);
    if (lh.getUint32(0, true) !== 0x04034b50) throw new Error('The .zip is damaged.');
    const start = e.off + 30 + lh.getUint16(26, true) + lh.getUint16(28, true);
    const raw = file.slice(start, start + e.csize);
    /* a slice of the picked file can stop being readable later (mobile file providers): copy it */
    if (e.method === 0) return new Blob([await raw.arrayBuffer()]);
    if (e.method !== 8) throw new Error('The .zip uses an unsupported compression method. Create it with Windows ("Send to > Compressed (zipped) folder").');
    const blob = await new Response(raw.stream().pipeThrough(new DecompressionStream('deflate-raw'))).blob();
    if (blob.size !== e.size) throw new Error('Erro ao descompactar ' + e.name + '.');
    return blob;
  }

  async function sha256(blob) {
    const h = await crypto.subtle.digest('SHA-256', await blob.arrayBuffer());
    return [...new Uint8Array(h)].map(b => b.toString(16).padStart(2, '0')).join('');
  }

  // imports a .zip of the installed game; onProgress(fraction, text)
  async function importZip(file, onProgress = () => {}) {
    onProgress(0, 'Reading the .zip…');
    const all = await entries(file);
    if (all.some(e => e.encrypted)) throw new Error('The .zip is password-protected. Create one without a password.');
    const exe = all.find(e => /(^|\/)pmr\.exe$/i.test(e.name));
    if (!exe) throw new Error('PMR.exe was not found in the .zip. Zip the folder where the game is installed.');
    const base = exe.name.slice(0, exe.name.length - 'PMR.exe'.length);
    /* only what the game reads: no launcher, DLLs, readme or extra folders */
    const unneeded = rel => /(^|\/)\./.test(rel) || (/\.(exe|dll|txt|url|ini|lnk)$/i.test(rel) && !/^pmr\.exe$/i.test(rel)) ||
                            (rel.includes('/') && !/^streams\//i.test(rel));
    const files = all.filter(e => e.name.startsWith(base) && !unneeded(e.name.slice(base.length)))
                     .map(e => ({...e, rel: e.name.slice(base.length)}));
    const names = new Set(files.map(f => f.rel.toLowerCase()));
    const missing = REQUIRED.filter(r => !names.has(r));
    if (missing.length) throw new Error('Game files missing from the .zip: ' + missing.join(', '));

    onProgress(0.01, 'Checking the game version…');
    const exeBlob = await extract(file, exe);
    if (await sha256(exeBlob) !== PMR_SHA256)
      throw new Error('This PMR.exe is from a different version of the game. This port works with Pac-Man World Rally for PC (2006, US release).');

    if (navigator.storage && navigator.storage.persist) { try { await navigator.storage.persist(); } catch (e) {} }
    await clear();
    const d = await db();
    const total = files.reduce((s, f) => s + f.size, 0);
    if (navigator.storage && navigator.storage.estimate) {
      try {
        const {quota, usage} = await navigator.storage.estimate();
        if (quota && quota - (usage || 0) < total * 1.05)
          throw new Error(`Not enough storage space in this browser: the game needs ${Math.ceil(total / 1e6)} MB and only ${Math.floor((quota - (usage || 0)) / 1e6)} MB are available. Free some space on the device and try again.`);
      } catch (e) { if (/storage space/.test(e.message)) throw e; }
    }
    let doneBytes = 0;
    const list = [];
    for (const f of files) {
      onProgress(doneBytes / total, 'Installing ' + f.rel + '…');
      const blob = f === exe ? exeBlob : await extract(file, f);
      const tx = d.transaction('files', 'readwrite');
      tx.objectStore('files').put(blob, f.rel.toLowerCase());
      await done(tx);
      list.push([f.rel, f.size]);
      doneBytes += f.size;
    }
    const meta = {files: list, sha256: PMR_SHA256, date: Date.now()};
    const tx = d.transaction('meta', 'readwrite');
    tx.objectStore('meta').put(meta, 'info');
    await done(tx);
    onProgress(1, 'Done!');
    return meta;
  }

  window.GameStore = {info, read, readRange, clear, importZip};
})();
