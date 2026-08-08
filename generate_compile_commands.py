#!/usr/bin/env python3
import json
import os
import glob

def main():
    print("Scanning source files...")
    cwd = os.getcwd()
    db = []
    
    # Core app files
    files = ["src/main.c", "src/gui.c"]
    
    # Find all LVGL C source files recursively
    lvgl_c_files = glob.glob("lvgl/src/**/*.c", recursive=True)
    files.extend(lvgl_c_files)
    
    for f in files:
        db.append({
            "directory": cwd,
            "command": f"gcc -O3 -g -Wall -I. -Ilvgl -DLV_CONF_INCLUDE_SIMPLE=1 -DHOST_BUILD=1 -I/usr/include/SDL2 -c {f}",
            "file": f
        })
        
    with open("compile_commands.json", "w") as out:
        json.dump(db, out, indent=2)
    print(f"Generated compile_commands.json with {len(db)} entries successfully.")

if __name__ == "__main__":
    main()
