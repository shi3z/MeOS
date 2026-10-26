"""Keep engine initializers in a single resettable section, without modifying upstream C.
ESP32-P4's default linker template expands dram0_data twice; SURROUND there would redefine
the snapshot boundaries around the second, empty expansion. Rename only writable initializers.
"""
import re
import subprocess
import sys

objdump, objcopy, archive = sys.argv[1:]
headers = subprocess.check_output([objdump, '-h', archive], text=True)
sections = sorted(set(re.findall(r'^\s*\d+\s+(\.s?data(?:\.[^\s]+)?)\s', headers, re.M)))
args = [objcopy]
for name in sections:
    args += ['--rename-section', f'{name}=.doomengine{name}']
if sections:
    subprocess.run(args + [archive], check=True)
