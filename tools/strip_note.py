#!/usr/bin/env python3
"""
Remove PT_NOTE segment from an ELF file using elftools.
This is needed because objcopy/strip can't remove PT_NOTE segments reliably.
"""
import sys
from elftools.elf.elffile import ELFFile
from elftools.elf.segments import NoteSegment
import struct

def remove_pt_note(input_path, output_path):
    with open(input_path, 'rb') as f:
        data = bytearray(f.read())
    
    # Read ELF header
    ident = data[:16]
    if ident[:4] != b'\x7fELF':
        raise ValueError("Not an ELF file")
    ei_class = ident[4]
    
    if ei_class == 1:  # 32-bit
        ph_fmt = '<IIIIIIII'
        phentsize = 32
        phoff_offset = 28  # e_phoff in 32-bit ELF
        phnum_offset = 44  # e_phnum in 32-bit ELF
        ehdr_size = 52
        phoff = struct.unpack('<I', data[28:32])[0]
        phnum = struct.unpack('<H', data[44:46])[0]
        phentsize = struct.unpack('<H', data[42:44])[0]
    elif ident[4] == 2:  # 64-bit
        phoff = struct.unpack('<Q', data[32:40])[0]
        phnum = struct.unpack('<H', data[56:58])[0]
        phentsize = struct.unpack('<H', data[54:56])[0]
    else:
        raise ValueError("Unknown ELF class")
    
    PT_NOTE = 4
    new_phdrs = []
    removed = 0
    
    for i in range(phnum):
        offset = phoff + i * phentsize
        phdr_data = bytes(data[offset:offset + phentsize])
        p_type = struct.unpack('<I', phdr_data[:4])[0]
        
        if p_type == 4:  # PT_NOTE
            print(f"Removing PT_NOTE segment at index {i}")
            removed += 1
            continue
        # Keep the program header
        phdr_bytes = data[phoff + i * phentsize : phoff + (i + 1) * phentsize]
        new_phdrs.append(phdr_data)
    
    if removed == 0:
        print("No PT_NOTE segment found")
        return False
    
    new_phnum = phnum - removed
    
    # Update e_phnum in ELF header
    phnum_offset = 44 if ident[4] == 1 else 56
    struct.pack_into('<H', data, phnum_offset, phnum - removed)
    
    # Write new program headers
    for i, phdr_bytes in enumerate(new_phdrs):
        offset = phoff + i * 56 if ident[4] == 2 else phoff + i * 32
        data[offset:offset + 56 if ident[4] == 2 else offset + 32] = phdr_bytes
    
    # Write output
    with open(output_path, 'wb') as f:
        f.write(data)
    
    print(f"Removed {removed} PT_NOTE segment(s), new e_phnum = {new_phnum}")
    return True

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input> <output>")
        sys.exit(1)
    
    if remove_pt_note(sys.argv[1], sys.argv[2]):
        print("Success")
    else:
        print("No changes made")
        sys.exit(1)