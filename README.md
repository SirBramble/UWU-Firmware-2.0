### FS Notes
Find the drive with `lsblk` and format the drive with `sudo mkfs.fat -v -F 12 -S 512 -n LABEL /dev/YOURDRIVE`. This will format the drive as FAT12 with a sector size of 512 bytes.

Optionally use `sudo dosfslabel /dev/YOURDRIVE NAME` to set a name for the drive.

If everything fails: use the `flash_nuke.uf2` and use it to reset the flash to factory settings (may take 1-2 minutes). Reflash the firmware and try formatting the disk again.

### CLANG & IDE SETUP

**Important**: PlatformIO manages the `c_cpp_properties.json` file automatically when you switch environments via the PlatformIO VS Code extension. However, clangd uses its own `compile_commands.json` file which must be regenerated for each environment.

**Workflow**:

1. **Switch environment in VS Code**: Use the PlatformIO environment selector in the status bar to switch between `macruwu3` and `typeuwu2`.

2. **Regenerate compile_commands.json**: After switching environments, run one of the following:
   ```bash
   # Using the helper script (recommended)
   ./regenerate_compile_commands.sh
   
   # Or manually using PlatformIO
   pio run -t compiledb -e macruwu3
   pio run -t compiledb -e typeuwu2
   ```

3. **Clangd will automatically detect** the new `compile_commands.json` file and update IntelliSense.

**Why this is needed**:
- PlatformIO VS Code extension updates `c_cpp_properties.json` when you switch environments
- Clangd uses `compile_commands.json` which is not automatically updated
- The `compile_commands.json` must match the current environment's build directory

**Verification**:
After regeneration, verify the correct environment is used:
```bash
grep -oE '"output": ".pio/build/[a-zA-Z0-9_]+' compile_commands.json | head -1
# Should show: "output": ".pio/build/macruwu3" (or typeuwu2)
```

**Optional: Enable automatic regeneration**:
The Python hook at `.platformio/hooks/auto_regenerate_compile_commands.py` can be used to automate this process. It detects the current environment from `c_cpp_properties.json` and regenerates `compile_commands.json` accordingly.