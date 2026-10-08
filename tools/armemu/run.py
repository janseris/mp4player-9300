#!/usr/bin/env python3
"""Runs the decoder core compiled by the phone's GCC 2.9 (build.sh) in the Unicorn ARM emulator.

  run.py decode FILE.mp4 [N]      decode N samples (default 150): checksum + instructions per frame
  run.py colour FILE.mp4 [FRAME] [W H] convert one frame to RGB565: instructions per pixel, output hash
  run.py h264 FILE.mp4 [N]        H.264: like decode
  run.py h264prof FILE.mp4 [N]    H.264: instructions per function

Needs Unicorn (built from source; set UNICORN_PY to the bindings dir and LIBUNICORN_PATH)."""
import os, re, struct, sys
sys.path.insert(0, os.environ.get('UNICORN_PY', os.path.expanduser('~/pyuc')))
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_BLOCK
from unicorn.arm_const import *

OUT = os.environ.get('OUT', '/tmp/armemu')
CODE, STOP = 0x10000, 0x000F0000
HEAP, HEAPSZ = 0x01000000, 0x02000000
STACK, STACKSZ = 0x04000000, 0x00100000
DATA, DATASZ = 0x05000000, 0x01000000

def load_symbols():
    syms = {}
    for line in open(OUT + '/core.sym'):
        p = line.split()
        if len(p) == 3: syms[p[2]] = int(p[0], 16)
    return syms

class Emu:
    def __init__(self):
        self.syms = load_symbols()
        self.mu = mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        exe = open(OUT + '/core.exe', 'rb').read()
        # PE: copy every section to its virtual address
        pe = struct.unpack_from('<I', exe, 0x3c)[0]
        nsec = struct.unpack_from('<H', exe, pe + 6)[0]
        opt = struct.unpack_from('<H', exe, pe + 20)[0]
        base = struct.unpack_from('<I', exe, pe + 24 + 28)[0]
        mu.mem_map(CODE, 0x40000)
        for i in range(nsec):
            o = pe + 24 + opt + 40 * i
            vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', exe, o + 8)
            if rawsize: mu.mem_write(base + va, exe[rawptr:rawptr + min(rawsize, vsize or rawsize)])
        mu.mem_map(STOP, 0x1000)
        mu.mem_map(HEAP, HEAPSZ); mu.mem_map(STACK, STACKSZ); mu.mem_map(DATA, DATASZ)
        self.count = 0
        self.prof = None
        fn = sorted((v, k) for k, v in self.syms.items() if CODE <= v < CODE + 0x40000 and not k.startswith('.') and not re.match(r'L[0-9]+(\x02[0-9]+)?$', k))
        self.addrs = [a for a, _ in fn]; self.names = [n for _, n in fn]
        mu.hook_add(UC_HOOK_BLOCK, self._blk)
        self.dp = DATA
        self.call('emu_heap_init', HEAP, HEAPSZ)
        self.alloc = self.put(struct.pack('<III', self.sym('emu_alloc'), self.sym('emu_free'), 0))

    def _blk(self, mu, addr, size, ud):
        self.count += size // 4
        if self.prof is not None:
            import bisect
            i = bisect.bisect_right(self.addrs, addr) - 1
            name = self.names[i] if i >= 0 else '?'
            self.prof[name] = self.prof.get(name, 0) + size // 4

    def sym(self, prefix):
        for k, v in self.syms.items():
            if k == prefix or k.startswith(prefix + '__'): return v
        raise KeyError(prefix)

    def put(self, data, align=8):
        self.dp = (self.dp + align - 1) & ~(align - 1)
        a = self.dp; self.mu.mem_write(a, bytes(data)); self.dp += len(data); return a

    def call(self, name, *args):
        mu = self.mu
        regs = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
        sp = STACK + STACKSZ - 0x1000
        extra = list(args[4:])
        sp -= 4 * len(extra)
        for i, v in enumerate(extra): mu.mem_write(sp + 4 * i, struct.pack('<I', v & 0xffffffff))
        for r, v in zip(regs, args[:4]): mu.reg_write(r, v & 0xffffffff)
        mu.reg_write(UC_ARM_REG_SP, sp); mu.reg_write(UC_ARM_REG_LR, STOP)
        self.count = 0
        mu.emu_start(self.sym(name), STOP)
        return mu.reg_read(UC_ARM_REG_R0)

    def u32(self, a): return struct.unpack('<I', self.mu.mem_read(a, 4))[0]

def mp4_samples(path, n):
    """sample tables via the host build of the demuxer is overkill: parse with ffprobe-free code"""
    import subprocess, json
    out = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_packets',
                          '-show_entries', 'packet=pos,size', '-of', 'json', path], capture_output=True, text=True).stdout
    pk = json.loads(out)['packets'][:n]
    data = open(path, 'rb').read()
    return [data[int(p['pos']):int(p['pos']) + int(p['size'])] for p in pk]

def dsi(path):
    import subprocess
    out = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=extradata',
                          '-show_data', '-of', 'default=nw=1', path], capture_output=True, text=True).stdout
    hexs = ''.join(re.findall(r'^[0-9a-f]{8}: ((?:[0-9a-f]{2,4} ?)+)', out, re.M)).replace(' ', '')
    return bytes.fromhex(hexs)

def open_decoder(e, path):
    slot = e.put(b'\0' * 4)
    e.call('m4v_open', slot, e.alloc)
    dec = e.u32(slot)
    d = dsi(path)
    if d:
        buf = e.put(d + b'\0' * 16)
        e.call('m4v_config', dec, buf, len(d))
    return dec

def decode(e, dec, samples, want_hash=True):
    buf = e.put(b'\0' * (max(len(s) for s in samples) + 16))
    pic = e.put(b'\0' * 28)
    h, counts, last = 2166136261, [], None
    for s in samples:
        e.mu.mem_write(buf, s + b'\0' * 16)
        r = e.call('m4v_decode', dec, buf, len(s), pic)
        counts.append(e.count)
        if r in (0, 1, 3):
            last = pic
            if want_hash:
                h = e.call('m4v_picture_hash', pic, h)
    return h, counts, last

def main():
    mode, path = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else '')
    e = Emu()
    if mode == 'decode':
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 150
        samples = mp4_samples(path, n)
        dec = open_decoder(e, path)
        h, counts, _ = decode(e, dec, samples)
        tot = sum(counts)
        print('samples %d  check %08X  instructions/frame: avg %d  min %d  max %d' %
              (len(counts), h, tot // len(counts), min(counts), max(counts)))
    elif mode in ('h264', 'h264prof'):
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 150
        samples = mp4_samples(path, n)
        slot = e.put(b'\0' * 4)
        e.call('h264d_open', slot); dec = e.u32(slot)
        cfg = dsi(path)
        cbuf = e.put(cfg + b'\0' * 16)
        r = e.call('h264d_config', dec, cbuf, len(cfg))
        if os.environ.get('FAST'): e.call('h264d_set_fast', dec, int(os.environ['FAST']))
        if r >= 0x80000000: print('config failed', r - (1 << 32)); return
        buf = e.put(b'\0' * (max(len(s_) for s_ in samples) + 16))
        pic = e.put(b'\0' * 28)
        if mode == 'h264prof': e.prof = {}
        h, counts = 2166136261, []
        for s_ in samples:
            e.mu.mem_write(buf, s_ + b'\0' * 16)
            r = e.call('h264d_decode', dec, buf, len(s_), pic)
            counts.append(e.count)
            if r in (0, 1) and mode == 'h264':
                save = e.count
                h = e.call('m4v_picture_hash', pic, h)
        if mode == 'h264':
            tot = sum(counts)
            print('samples %d  check %08X  instructions/frame: avg %d  min %d  max %d' %
                  (len(counts), h, tot // len(counts), min(counts), max(counts)))
        else:
            tot = sum(e.prof.values())
            for name, c in sorted(e.prof.items(), key=lambda x: -x[1])[:int(os.environ.get('TOP','22'))]:
                print('%5.1f%%  %8d/frame  %s' % (100.0 * c / tot, c // len(samples), name))
    elif mode == 'ktest':
        # kernel tests: assembler routines against C references on random data
        iters = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
        a = e.put(b'\0' * 16384); b = e.put(b'\0' * 16384)
        for name in [n for n in sorted(e.syms) if n.startswith('ktest_')]:
            bad = e.call(name, a, b, 12345, iters)
            print('%-24s %s (%d instructions)' % (name, 'ok' if bad == 0 else '%d BAD' % bad, e.count))
    elif mode == 'profile':
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 60
        samples = mp4_samples(path, n)
        dec = open_decoder(e, path)
        e.prof = {}
        h, counts, _ = decode(e, dec, samples, want_hash=False)
        tot = sum(e.prof.values())
        for name, c in sorted(e.prof.items(), key=lambda x: -x[1])[:16]:
            print('%5.1f%%  %8d/frame  %s' % (100.0 * c / tot, c // len(samples), name))
    elif mode == 'colour':
        fr = int(sys.argv[3]) if len(sys.argv) > 3 else 40
        samples = mp4_samples(path, fr + 1)
        dec = open_decoder(e, path)
        _, _, pic = decode(e, dec, samples, want_hash=False)
        w, h_ = struct.unpack('<II', e.mu.mem_read(pic + 20, 8))
        dw = int(sys.argv[4]) if len(sys.argv) > 5 else w
        dh = int(sys.argv[5]) if len(sys.argv) > 5 else h_
        slot = e.put(b'\0' * 4)
        e.call('yuvconv_open', slot, e.alloc); conv = e.u32(slot)
        if len(sys.argv) > 9:
            cx, cy, cw, ch = [int(v) for v in sys.argv[6:10]]
            e.call('yuvconv_setup_crop', conv, cx, cy, cw, ch, dw, dh, 0)
        else:
            e.call('yuvconv_setup', conv, w, h_, dw, dh, 0)
        stride = ((dw * 2 + 3) // 4) * 4
        dst = e.put(b'\0' * (stride * dh))
        e.call('yuvconv_run', conv, pic, dst, stride)
        n = e.count
        img = bytes(e.mu.mem_read(dst, stride * dh))
        hh = 2166136261
        for b in img: hh = ((hh ^ b) * 16777619) & 0xffffffff
        print('colour %dx%d -> %dx%d: %d instructions, %.1f per pixel, output hash %08X' %
              (w, h_, dw, dh, n, n / (dw * dh), hh))
        open(OUT + '/emu_rgb565.raw', 'wb').write(img)

if __name__ == '__main__':
    main()
