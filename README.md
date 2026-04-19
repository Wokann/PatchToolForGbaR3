# PatchToolForGbaR3
Custom GBA ROM Patch Tool fot gbarunner3

# Compile
gcc PatchToolForGbaR3.c -o PatchToolForGbaR3.exe -O2 -std=c99 -Wall

# Usage
Usage: PatchToolForGbaR3 <mode> <input1> [input2] [output]</br>
Modes:</br>
  -c/create <orig_rom> <new_rom> [out_patch]     - Create patch file (if don't give name, file will be named like "BPEE00.patch")</br>
  -a/apply <patch_file> <orig_rom> [out_rom]     - Apply patch to rom (if don't give name, file will be named like "***_patched.gba")</br>
  -p2j/patch2json <patch_file>     - Convert patch to json (the same name as input file, ***.patch -> ***.patch.json)</br>
  -i2p/ips2patch <ips_file>     - Convert ips to patch (the same name as input file, ***.ips -> ***.patch)</br>
  -p2i/patch2ips <patch_file>     - Convert patch to ips (the same name as input file, ***.patch -> ***.ips)</br>
  automode     - drag .patch/.patch.json/.ips to this exe to auto use -p2j/-p2i/i2p
