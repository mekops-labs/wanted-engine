"""Minimal RFB client for latency measurement. Prints one JSON summary.
usage: rfbclient.py <port> <seconds> <rgb565|gray4> [hextile|raw]"""
import socket, struct, sys, time, json
from cryptography.hazmat.primitives.ciphers import Cipher, modes
try:
    from cryptography.hazmat.decrepit.ciphers.algorithms import TripleDES
except Exception:
    from cryptography.hazmat.primitives.ciphers.algorithms import TripleDES

port, secs, fmt = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
enc_name = sys.argv[4] if len(sys.argv) > 4 else 'hextile'

def rev(b): return int('{:08b}'.format(b)[::-1], 2)
def des(key, data):
    k = bytes(rev(b) for b in (key + b'\0' * 8)[:8])
    c = Cipher(TripleDES(k * 3), modes.ECB()).encryptor()
    return c.update(data) + c.finalize()

s = socket.create_connection(('127.0.0.1', port), timeout=30)
buf = bytearray()
def need(n):
    while len(buf) < n:
        d = s.recv(1 << 20)
        if not d: raise EOFError
        buf.extend(d)
def take(n):
    need(n); r = bytes(buf[:n]); del buf[:n]; return r

take(12); s.sendall(b'RFB 003.008\n')
n = take(1)[0]; types = take(n)
assert 2 in types, types
s.sendall(bytes([2])); chal = take(16); s.sendall(des(b'wanted', chal))
assert struct.unpack('>I', take(4))[0] == 0
s.sendall(b'\x01')
w, h = struct.unpack('>HH', take(4)); take(16); ln = struct.unpack('>I', take(4))[0]; take(ln)
# 32 bpp, depth 24, little endian, true colour, 8 bits each, shifts 16/8/0
s.sendall(struct.pack('>BxxxBBBBHHHBBBxxx', 0, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
encs = [5, 0] if enc_name == 'hextile' else [0]
s.sendall(struct.pack('>BxH', 2, len(encs)) + b''.join(struct.pack('>i', e) for e in encs))

stamp_px = {}
stamp_t = [0.0]
def px32(b):  # little endian 32 bpp: b,g,r,x
    return b[2], b[1], b[0]

def decode_stamp():
    if fmt == 'rgb565':
        v = 0
        for i in range(4):
            r, g, b = stamp_px[i]
            p = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            v = (v << 16) | p
        return v
    v = 0
    for i in range(16):
        r = stamp_px[i][0]
        v = (v << 4) | int(round(r / 17.0))
    return v

def read_rect_raw(x, y, rw, rh):
    if y == 0 and x == 0: need(12)
    data = take(rw * rh * 4)
    if y == 0 and x == 0:
        stamp_t[0] = time.time() * 1e6
        for i in range(min(16, rw)): stamp_px[i] = px32(data[i*4:i*4+4])

def read_rect_hextile(x, y, rw, rh):
    bg = fg = (0, 0, 0)
    for ty in range(0, rh, 16):
        for tx in range(0, rw, 16):
            tw, th = min(16, rw - tx), min(16, rh - ty)
            sub = take(1)[0]
            want = (y + ty == 0 and x + tx == 0)
            if want: stamp_t[0] = time.time() * 1e6
            if sub & 1:
                data = take(tw * th * 4)
                if want:
                    for i in range(min(16, tw)): stamp_px[i] = px32(data[i*4:i*4+4])
                continue
            if sub & 2: bg = px32(take(4))
            if sub & 4: fg = px32(take(4))
            if want:
                for i in range(min(16, tw)): stamp_px[i] = bg
            if sub & 8:
                nsub = take(1)[0]
                for _ in range(nsub):
                    c = fg
                    if sub & 16: c = px32(take(4))
                    xy, wh = take(2)
                    if want:
                        sx, sy, sw, sh = xy >> 4, xy & 15, (wh >> 4) + 1, (wh & 15) + 1
                        if sy == 0:
                            for i in range(sx, min(sx + sw, 16)): stamp_px[i] = c

def request(incremental):
    s.sendall(struct.pack('>BBHHHH', 3, incremental, 0, 0, w, h))

request(0)
lat = []; frames = 0; bytes0 = 0
deadline = time.time() + secs
first = None
s.settimeout(1)
while time.time() < deadline:
    try:
        mt = take(1)[0]
    except (socket.timeout, TimeoutError):
        continue
    if mt != 0:
        raise SystemExit(f'unexpected message {mt}')
    take(1); nr = struct.unpack('>H', take(2))[0]
    got_stamp = False
    for _ in range(nr):
        x, y, rw, rh, e = struct.unpack('>HHHHi', take(12))
        if e == 0: read_rect_raw(x, y, rw, rh)
        elif e == 5: read_rect_hextile(x, y, rw, rh)
        else: raise SystemExit(f'encoding {e}')
        if x == 0 and y == 0: got_stamp = True
    t = stamp_t[0]
    frames += 1
    if got_stamp and len(stamp_px) >= (4 if fmt == 'rgb565' else 16):
        v = decode_stamp()
        d = t - v
        if first is None:
            first = time.time()   # the first update shows a stale stamp
        elif 0 < d < 5e6:
            lat.append(d)
    request(1)
lat.sort()
def pct(p): return lat[min(len(lat) - 1, int(len(lat) * p))] / 1000 if lat else None
print(json.dumps({'updates': frames, 'stamped': len(lat), 'p50_ms': pct(.5), 'p95_ms': pct(.95), 'max_ms': pct(.9999), 'size': [w, h]}))
