#!/usr/bin/env python3
"""
Auto-regenerate compile_commands.json for the current PlatformIO environment.
Detects the active environment from .vscode/c_cpp_properties.json which is
updated by the PlatformIO VS Code extension when you switch environments.
"""

import json
import subprocess
import sys
import re
from pathlib import Path


def get_current_env_from_c_cpp_properties():
    """
    Detect the current PlatformIO environment from c_cpp_properties.json.
    The PlatformIO VS Code extension updates this file when you switch environments.
    Handles files with comment lines at the beginning.
    """
    c_cpp_props = Path.cwd() / ".vscode" / "c_cpp_properties.json"
    if not c_cpp_props.exists():
        return None
    
    try:
        with open(c_cpp_props) as f:
            # Read file and remove comment lines
            content = f.read()
            # Remove lines that are just comments (start with //)
            lines = content.split('\n')
            json_lines = [line for line in lines if not line.strip().startswith('//')]
            content = '\n'.join(json_lines)
            
            props = json.loads(content)
            
            for config in props.get("configurations", []):
                # Check includePath for environment name in libdeps paths
                for include_path in config.get("includePath", []):
                    # Match paths like ".pio/libdeps/macruwu3/UWU-Lib/include"
                    match = re.search(r'\.pio/libdeps/([a-zA-Z0-9_\-]+)', include_path)
                    if match:
                        env_name = match.group(1)
                        return env_name
                        
    except json.JSONDecodeError as e:
        print(f"Warning: Could not parse c_cpp_properties.json: {e}", file=sys.stderr)
    except Exception as e:
        print(f"Warning: Could not parse c_cpp_properties.json: {e}", file=sys.stderr)
    
    return None


def regenerate_compile_commands(env):
    """
    Regenerate compile_commands.json for the specified environment.
    """
    if not env:
        print("Error: Could not determine current environment", file=sys.stderr)
        return False
    
    print(f"Regenerating compile_commands.json for environment: {env}")
    
    try:
        # Run PlatformIO compiledb target
        result = subprocess.run(
            ["pio", "run", "-t", "compiledb", "-e", env],
            cwd=Path.cwd(),
            capture_output=True,
            text=True,
            timeout=120
        )
        
        if result.returncode != 0:
            print(f"Error: {result.stderr}", file=sys.stderr)
            return False
        
        # Check if compile_commands.json was generated
        compile_commands = Path.cwd() / "compile_commands.json"
        if compile_commands.exists():
            print(f"Successfully generated compile_commands.json")
            return True
        else:
            print("Error: compile_commands.json was not generated", file=sys.stderr)
            return False
            
    except subprocess.TimeoutExpired:
        print("Error: Command timed out", file=sys.stderr)
        return False
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        return False


def main():
    env = get_current_env_from_c_cpp_properties()
    if not env:
        print("Error: Could not determine current environment", file=sys.stderr)
        print("Hint: Make sure you've selected an environment in PlatformIO VS Code extension", file=sys.stderr)
        sys.exit(1)
    
    success = regenerate_compile_commands(env)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()