#!/usr/bin/env python3
"""Replace the kernel in stock PPA-LX2 12_BOOT.img (boot v2 + AVB footer).

usage: repack_boot.py <stock_boot.img> <Image.gz> <out.img>
"""
import struct
import sys

AVB_FOOTER = 64


def pad(n, ps):
    return (n + ps - 1) // ps * ps


def main(stock, kernel, out):
    d = open(stock, 'rb').read()
    k = open(kernel, 'rb').read()
    assert d[:8] == b'ANDROID!'
    ks, ka, rs, ra, ss, sa, ta, ps, hv, osv = struct.unpack('<10I', d[8:48])
    assert hv == 2, hv
    o = 1632
    rds, rdo, hs, ds, da = struct.unpack('<IQIIQ', d[o:o + 28])

    pos = ps
    parts = {}
    for name, size in (('kernel', ks), ('ramdisk', rs), ('second', ss),
                       ('rdtbo', rds), ('dtb', ds)):
        parts[name] = d[pos:pos + size]
        pos += pad(size, ps)

    hdr = bytearray(d[:ps])
    struct.pack_into('<I', hdr, 8, len(k))
    cmd = bytes(hdr[64:576])
    cmd = cmd.replace(b'androidboot.selinux=enforcing', b'androidboot.selinux=permissive')
    assert cmd[512:].strip(b'\0') == b''
    cmd = cmd[:512]
    hdr[64:576] = cmd
    body = bytes(hdr)
    for name, blob in (('kernel', k), ('ramdisk', parts['ramdisk']),
                       ('second', parts['second']), ('rdtbo', parts['rdtbo']),
                       ('dtb', parts['dtb'])):
        body += blob + b'\0' * (pad(len(blob), ps) - len(blob))

    f = d[-AVB_FOOTER:]
    assert f[:4] == b'AVBf'
    orig, vb_off, vb_size = struct.unpack('>QQQ', f[12:36])
    vbmeta = d[vb_off:vb_off + vb_size]
    vb_new = pad(len(body), 4096)
    total = len(d)
    if vb_new + vb_size > total - 4096:
        sys.exit('kernel too large for partition')

    img = bytearray(total)
    img[:len(body)] = body
    img[vb_new:vb_new + vb_size] = vbmeta
    nf = bytearray(f)
    struct.pack_into('>QQQ', nf, 12, len(body), vb_new, vb_size)
    img[-AVB_FOOTER:] = nf
    open(out, 'wb').write(img)
    print(f'kernel {ks} -> {len(k)}, payload {len(body)}, vbmeta @{vb_new:#x}')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
