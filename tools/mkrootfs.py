#!/usr/bin/env python3
"""
FelinOS Rootfs Generator
========================
Generates a bootable rootfs image from the ports sysroot.
"""

import os
import sys
import subprocess
import shutil
import tempfile

# Configuration
PORTS_DIR = os.path.join(os.path.dirname(__file__), '..', 'felinos-ports')
SYSROOT = os.path.join(os.path.dirname(__file__), '..', 'userspace')
ROOTFS_IMG = os.path.join(os.path.dirname(__file__), '..', 'rootfs.img')
ROOTFS_SIZE = '256M'

def run_cmd(cmd, cwd=None):
    """Run command and return (success, output)"""
    try:
        result = subprocess.run(cmd, shell=True, cwd=cwd, 
                               capture_output=True, text=True, check=True)
        return True, result.stdout
    except subprocess.CalledProcessError as e:
        return False, e.stderr

def main():
    print("FelinOS Rootfs Generator")
    print("========================")
    
    # Check if sysroot exists
    if not os.path.exists(SYSROOT):
        print(f"Error: Sysroot not found at {SYSROOT}")
        print("Run 'make ports-base' first to build the userspace")
        return 1
    
    # Check if sysroot has content
    if not os.listdir(SYSROOT):
        print(f"Error: Sysroot is empty at {SYSROOT}")
        return 1
    
    print(f"Sysroot: {SYSROOT}")
    print(f"Output:  {ROOTFS_IMG}")
    print(f"Size:    {ROOTFS_SIZE}")
    
    # Create disk image
    print("\nCreating disk image...")
    success, output = run_cmd(f'dd if=/dev/zero of={ROOTFS_IMG} bs=1 count=0 seek={ROOTFS_SIZE}')
    if not success:
        print(f"Error creating image: {output}")
        return 1
    
    # Format as ext4
    print("Formatting as ext4...")
    success, output = run_cmd(f'mkfs.ext4 -F -L FELINOS_ROOT {ROOTFS_IMG}')
    if not success:
        print(f"Error formatting: {output}")
        return 1
    
    # Mount and copy
    with tempfile.TemporaryDirectory() as mount_dir:
        print(f"Mounting at {mount_dir}...")
        success, output = run_cmd(f'sudo mount -o loop {ROOTFS_IMG} {mount_dir}')
        if not success:
            print(f"Error mounting (need sudo): {output}")
            return 1
        
        try:
            print("Copying files...")
            # Copy sysroot contents
            for item in os.listdir(SYSROOT):
                src = os.path.join(SYSROOT, item)
                dst = os.path.join(mount_dir, item)
                if os.path.isdir(src):
                    shutil.copytree(src, dst, dirs_exist_ok=True)
                else:
                    shutil.copy2(src, dst)
            
            print("Syncing...")
            run_cmd('sync')
            
        finally:
            print("Unmounting...")
            run_cmd(f'sudo umount {mount_dir}')
    
    print(f"\nRootfs created: {ROOTFS_IMG}")
    print(f"Size: {os.path.getsize(ROOTFS_IMG) / (1024*1024):.1f} MB")
    
    return 0

if __name__ == '__main__':
    sys.exit(main())