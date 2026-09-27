#!/usr/bin/env python3
"""
Convert 64-bit ELF to 32-bit ELF while preserving section layout and removing PT_NOTE.
"""
import sys
import struct
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import Section

def convert_elf64_to_32(input_path, output_path):
    with open(input_path, 'rb') as f:
        data = bytearray(f.read())
    
    # Parse 64-bit ELF
    with open(input_path, 'rb') as f:
        elf = ELFFile(f)
        
        # Get section info
        sections = []
        for section in elf.iter_sections():
            if section['sh_size'] == 0:
                continue
            sec_data = section.data()
            sections.append({
                'name': section.name,
                'sh_type': section['sh_type'],
                'sh_flags': section['sh_flags'],
                'sh_addr': section['sh_addr'],
                'sh_offset': section['sh_offset'],
                'sh_size': section['sh_size'],
                'sh_link': section['sh_link'],
                'sh_info': section['sh_info'],
                'sh_addralign': section['sh_addralign'],
                'sh_entsize': section['sh_entsize'],
                'data': sec_data,
            })
        
        # Get program headers
        phdrs = []
        for segment in elf.iter_segments():
            if segment['p_type'] == 'PT_NOTE':
                continue  # Skip PT_NOTE
            phdrs.append({
                'p_type': segment['p_type'],
                'p_flags': segment['p_flags'],
                'p_offset': segment['p_offset'],
                'p_vaddr': segment['p_vaddr'],
                'p_paddr': segment['p_paddr'],
                'p_filesz': segment['p_filesz'],
                'p_memsz': segment['p_memsz'],
                'p_align': segment['p_align'],
            })
        
        # Get entry point
        entry = elf.header['e_entry']
        
        # Get section header string table index
        shstrndx = elf.header['e_shstrndx']
        
    # Build new 32-bit ELF
    # This is complex, let's use a different approach: modify the existing 32-bit ELF
    # to fix the section layout
    
    print("Use a different approach: patch the existing 32-bit ELF")
    return False

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input> <output>")
        sys.exit(1)
    convert_elf64_to_32(sys.argv[1], sys.argv[2])