#!/usr/bin/env python3
import sys, struct

with open(sys.argv[1], "rb") as f:
    data = bytearray(f.read())

ident = data[:16]
assert ident[:4] == b"\x7fELF"
ei_class = ident[4]
assert ei_class == 1

e_phoff = int.from_bytes(bytes(data[28:32]), "little")
e_phnum = int.from_bytes(bytes(data[44:46]), "little")
e_phentsize = int.from_bytes(bytes(data[42:44]), "little")
e_shoff = int.from_bytes(bytes(data[32:36]), "little")
e_shnum = int.from_bytes(bytes(data[46:48]), "little")
e_shentsize = int.from_bytes(bytes(data[48:50]), "little")
e_shstrndx = int.from_bytes(bytes(data[50:52]), "little")

print(f"phoff=0x{e_phoff:x}, phnum={e_phnum}, phentsize={e_phentsize}")
print(f"shoff=0x{e_shoff:x}, shnum={e_shnum}, shentsize={e_shentsize}")

e_shstrndx = int.from_bytes(bytes(data[50:52]), "little")
shstr_offset = int.from_bytes(bytes(data[e_shoff + e_shstrndx * 40 + 16:e_shoff + e_shstrndx * 40 + 20]), "little")

for i in range(int.from_bytes(bytes(data[46:48]), "little")):
    sh_offset = e_shoff + i * 40
    sh_name_offset = int.from_bytes(bytes(data[sh_offset:sh_offset+4]), "little")
    sh_name = bytes(data[shstr_offset + sh_name_offset:]).split(b"\x00")[0].decode("ascii", errors="ignore")
    sh_addr = int.from_bytes(bytes(data[sh_offset+12:sh_offset+16]), "little")
    sh_offset_val = int.from_bytes(bytes(data[sh_offset+16:sh_offset+20]), "little")
    sh_size = int.from_bytes(bytes(data[sh_offset+20:sh_offset+24]), "little")

    if sh_name == ".ap_tramp":
        print(f"Found .ap_tramp at section {i}: addr=0x{sh_addr:x}, offset=0x{sh_offset_val:x}, size=0x{sh_size:x}")
    elif sh_name == ".boot32":
        print(f"Found .boot32 at section {i}: addr=0x{sh_addr:x}, offset=0x{sh_offset_val:x}, size=0x{sh_size:x}")

struct.pack_into("<I", data, 24, 0x80c0)
print("Fixed entry point to 0x80c0")

phoff = int.from_bytes(bytes(data[28:32]), "little")
phnum = int.from_bytes(bytes(data[44:46]), "little")
phentsize = int.from_bytes(bytes(data[42:44]), "little")

new_phdrs = []
for i in range(int.from_bytes(bytes(data[44:46]), "little")):
    offset = phoff + i * 32
    phdr_data = bytes(data[phoff + i * 32:phoff + (i+1)*32])
    p_type = int.from_bytes(bytes(phdr_data[:4]), "little")
    if p_type == 4:
        print(f"Removing PT_NOTE segment at index {i}")
        continue
    new_phdrs.append(bytes(data[phoff + i * 32:phoff + (i+1)*32]))

if len(new_phdrs) < 8:
    new_phnum = 7
    import struct
    struct.pack_into("<H", data, 44, new_phnum)
    print(f"Removed PT_NOTE segment, new phnum = {new_phnum}")

with open(sys.argv[2], "wb") as f:
    f.write(data)
print("Done!")
