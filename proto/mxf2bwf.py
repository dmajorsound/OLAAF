#!/usr/bin/env python3
"""Prototype: unwrap PCM audio from an MXF (OP-Atom or OP1a) file into a Broadcast WAV.

Lossless: the PCM samples are copied unchanged. The BWF TimeReference is set from
the MXF source timecode, so the file keeps its timeline position.
This is the reference for the C++ version inside olAAF.
"""
import struct, sys, os

KEY_PREFIX = b'\x06\x0e\x2b\x34'
PRIMER = bytes.fromhex('060e2b34020501010d01020101050100')
# essence element keys: 06 0e 2b 34 01 02 01 01 0d 01 03 01 16 xx xx xx
# 0x16 = sound item (system/item types used by SMPTE 382 wave and AES3)
ESS_PREFIX = bytes.fromhex('060e2b34010201010d010301')

# metadata ULs we care about (matched on bytes 8.. to ignore version byte 7)
UL = {
    'AudioSamplingRate': '060e2b340101010504020301010100',
    'QuantizationBits':  '060e2b340101010404020303040000',
    'ChannelCount':      '060e2b340101010504020101040000',
    'BlockAlign':        '060e2b340101010504020302010000',
    'StartTimecode':     '060e2b340101010207020103010500',
    'RoundedTCBase':     '060e2b340101010104040101020600',
    'DropFrame':         '060e2b340101010104040101050000',
    'SoundEssenceCoding':'060e2b340101010204020402000000',
}

STATIC = {0x1501: 'StartTimecode', 0x1502: 'RoundedTCBase', 0x1503: 'DropFrame',
          0x3D03: 'AudioSamplingRate', 0x3D01: 'QuantizationBits', 0x3D07: 'ChannelCount',
          0x3D0A: 'BlockAlign', 0x3D06: 'SoundEssenceCoding'}


def ber(d, i):
    b = d[i]
    if b & 0x80:
        n = b & 0x7f
        return int.from_bytes(d[i + 1:i + 1 + n], 'big'), 1 + n
    return b, 1


def walk(d):
    i = 0
    while i < len(d) - 17:
        if d[i:i + 4] != KEY_PREFIX:
            j = d.find(KEY_PREFIX, i + 1)
            if j < 0:
                return
            i = j
            continue
        k = d[i:i + 16]
        L, n = ber(d, i + 16)
        v0 = i + 16 + n
        yield i, k, v0, L
        i = v0 + L


def ul_match(ul_bytes, name):
    ref = bytes.fromhex(UL[name])
    # ignore registry version byte (index 7)
    return ul_bytes[:7] == ref[:7] and ul_bytes[8:15] == ref[8:15]


def parse(path):
    d = open(path, 'rb').read()
    primer = {}
    info = {}
    tcs = []
    starts = []
    essence = []
    for i, k, v0, L in walk(d):
        if k == PRIMER:
            cnt, sz = struct.unpack('>II', d[v0:v0 + 8])
            p = v0 + 8
            for _ in range(cnt):
                tag = struct.unpack('>H', d[p:p + 2])[0]
                primer[tag] = d[p + 2:p + 18]
                p += sz
        elif k[4] == 0x02 and k[5] == 0x53:  # local set
            p, end = v0, v0 + L
            if k[14] == 0x11:  # SourceClip: StartPosition points into the source (tape) package
                q = v0
                while q + 4 <= end:
                    tg, ln = struct.unpack('>HH', d[q:q + 4])
                    if tg == 0x1201 and ln == 8:
                        starts.append(struct.unpack('>q', d[q + 4:q + 12])[0])
                    q += 4 + ln
            while p + 4 <= end:
                tag, ln = struct.unpack('>HH', d[p:p + 4])
                val = d[p + 4:p + 4 + ln]
                p += 4 + ln
                ul = primer.get(tag)
                name = None
                if ul is not None:
                    for nm in UL:
                        if ul_match(ul, nm):
                            name = nm
                            break
                if name is None:
                    # static local tags are not always listed in the primer
                    name = STATIC.get(tag)
                if name is None:
                    continue
                if name == 'StartTimecode':
                    tcs.append(struct.unpack('>q', val)[0])
                elif name not in info:
                    info[name] = val
        elif k[:12] == ESS_PREFIX and k[12] == 0x16:
            essence.append((v0, L, k))
    if not essence:
        raise SystemExit('no sound essence found in ' + path)
    sr = struct.unpack('>ii', info['AudioSamplingRate']) if 'AudioSamplingRate' in info else (48000, 1)
    bits = struct.unpack('>I', info['QuantizationBits'])[0] if 'QuantizationBits' in info else 24
    ch = struct.unpack('>I', info['ChannelCount'])[0] if 'ChannelCount' in info else 1
    # material package and file package each carry a timecode track; the file
    # (source) package holds the real recording timecode, the other is usually 0
    tc = max(tcs) if tcs else 0
    tcb = struct.unpack('>H', info['RoundedTCBase'])[0] if 'RoundedTCBase' in info else 25
    coding = info.get('SoundEssenceCoding', b'').hex()
    pcm = b''.join(d[v0:v0 + L] for v0, L, k in essence)
    return dict(rate=sr[0] / sr[1], bits=bits, ch=ch, tc_frames=tc, tc_base=tcb,
                coding=coding, pcm=pcm, src_start=max(starts) if starts else 0, ess_key=essence[0][2].hex(), parts=len(essence))


def write_bwf(out, m, description, originator='olAAF'):
    rate = int(round(m['rate']))
    bits, ch = m['bits'], m['ch']
    ba = ch * bits // 8
    pcm = m['pcm']
    pcm = pcm[:len(pcm) - len(pcm) % ba]
    # samples since midnight: tape timecode + where this file starts on the tape
    # (assumes the SourceClip edit rate equals the audio rate, true for PCM audio tracks)
    time_ref = m['tc_frames'] * rate // m['tc_base'] + m['src_start']
    bext = struct.pack('<256s32s32s10s8sQH64s190s',
                       description.encode('utf-8')[:256], originator.encode()[:32],
                       b'', b'2026-11-22', b'00:00:00', time_ref, 1, b'\0' * 64, b'\0' * 190)
    fmt = struct.pack('<HHIIHH', 1, ch, rate, rate * ba, ba, bits)
    chunks = b'bext' + struct.pack('<I', len(bext)) + bext
    chunks += b'fmt ' + struct.pack('<I', len(fmt)) + fmt
    chunks += b'data' + struct.pack('<I', len(pcm)) + pcm + (b'\0' if len(pcm) % 2 else b'')
    with open(out, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 4 + len(chunks)) + b'WAVE' + chunks)
    return time_ref, len(pcm) // ba


if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    m = parse(src)
    tr, n = write_bwf(dst, m, os.path.basename(src))
    b = m['tc_base']; tcf = tr * b // int(round(m['rate']))
    tc = '%02d:%02d:%02d:%02d' % (tcf // (b * 3600), tcf // (b * 60) % 60, tcf // b % 60, tcf % b)
    print('%s  %d Hz %d bit %d ch  %d samples (%.2f s)  TC %s  parts %d  key %s' % (
        os.path.basename(dst), m['rate'], m['bits'], m['ch'], n, n / m['rate'], tc, m['parts'], m['ess_key']))
