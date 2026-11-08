#!/usr/bin/env python3
"""
Pre-build script to manage UWU-Lib submodule
- Initializes submodule if missing
- Updates submodule if out of sync with .gitmodules
- Ensures shared library is available for all environments
"""
import os
import subprocess
from pathlib import Path

Import("env")

# Use current working directory as project root (where platformio.ini is located)
PROJECT_ROOT = Path.cwd()
SUBMODULE_PATH = PROJECT_ROOT / "lib" / "UWU-Lib"
GIT_MODULES_PATH = PROJECT_ROOT / ".gitmodules"


def get_submodule_commit():
    """Get current submodule HEAD commit"""
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--quiet", "--verify", "+:lib/UWU-Lib"],
            capture_output=True,
            text=True,
            cwd=str(PROJECT_ROOT)
        )
        if result.returncode == 0:
            return result.stdout.strip()
    except:
        pass
    return None


def get_gitmodules_commit():
    """Get commit recorded in .gitmodules"""
    if not GIT_MODULES_PATH.exists():
        return None

    content = GIT_MODULES_PATH.read_text()
    url = None
    for line in content.split('\n'):
        if line.startswith('url = '):
            url = line.split('url = ')[1].strip()
            break
        elif line.startswith('url\t'):
            url = line.split('url\t')[1].strip()
            break
        elif line.startswith('url '):
            url = line.split('url ')[1].strip()
            break
    
    if not url:
        return None

    # Get the SHA1 from git config
    try:
        result = subprocess.run(
            ["git", "config", "--get", f"submodule.lib/UWU-Lib.sha1"],
            capture_output=True,
            text=True,
            cwd=str(PROJECT_ROOT)
        )
        if result.returncode == 0:
            return result.stdout.strip()
    except:
        pass

    return None


def setup_uwu_lib(source, target, env):
    """
    Ensure UWU-Lib submodule is properly initialized and synced
    """
    print("\n" + "=" * 60)
    print("[UWU-Lib Setup]")
    print("=" * 60)

    # Step 1: Check if submodule exists and has content
    if not SUBMODULE_PATH.exists():
        print("[1/3] Submodule not found, initializing...")
        try:
            subprocess.run(
                ["git", "submodule", "update", "--init", "--recursive",
                 "lib/UWU-Lib"],
                check=True,
                cwd=str(PROJECT_ROOT),
                capture_output=False
            )
            print(f"[✓] Initialized at {SUBMODULE_PATH}")
        except subprocess.CalledProcessError as e:
            print(f"[✗] Error initializing submodule: {e}")
            raise

    # Step 2: Check if submodule is out of sync
    current_commit = get_submodule_commit()
    recorded_commit = get_gitmodules_commit()

    if current_commit and recorded_commit and current_commit != recorded_commit:
        print(f"[2/3] Submodule out of sync!")
        print(f"    Current:  {current_commit[:8]}")
        print(f"    Recorded: {recorded_commit[:8]}")
        print("[2/3] Updating submodule to match .gitmodules...")
        try:
            subprocess.run(
                ["git", "submodule", "update", "--remote", "--recursive",
                 "lib/UWU-Lib"],
                check=True,
                cwd=str(PROJECT_ROOT),
                capture_output=False
            )
            new_commit = get_submodule_commit()
            print(f"[✓] Updated to {new_commit[:8]}")
        except subprocess.CalledProcessError as e:
            print(f"[✗] Error updating submodule: {e}")
            raise

    # Step 3: Verify library.json exists
    lib_json = SUBMODULE_PATH / "library.json"
    if not lib_json.exists():
        print("[3/3] Warning: library.json not found in UWU-Lib")
        print("       PlatformIO may have issues finding the library")
    else:
        print(f"[3/3] Library.json found at {lib_json}")

    print("=" * 60 + "\n")


# Register the pre-build action
env.AddPreAction("buildprog", setup_uwu_lib)