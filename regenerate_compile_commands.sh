#!/bin/bash
# Auto-regenerate compile_commands.json for the current PlatformIO environment
# This script detects the active environment from .vscode/c_cpp_properties.json
# and regenerates the compile_commands.json file accordingly.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"

# Python script to detect environment and regenerate
PYTHON_SCRIPT="${PROJECT_DIR}/.platformio/hooks/auto_regenerate_compile_commands.py"

echo "Detecting current PlatformIO environment..."

if [ -f "$PYTHON_SCRIPT" ]; then
    echo "Running auto-regeneration script..."
    python3 "$PYTHON_SCRIPT"
    EXIT_CODE=$?
else
    echo "Error: Auto-regeneration script not found at $PYTHON_SCRIPT"
    echo ""
    echo "Available environments:"
    pio run --help 2>&1 | grep -E '^\s+-e' | head -5
    echo ""
    echo "Usage: pio run -e <env_name>"
    exit 1
fi

if [ $EXIT_CODE -eq 0 ]; then
    echo ""
    echo "compile_commands.json has been regenerated for the current environment."
    echo "Clangd will automatically pick up the changes."
    exit 0
else
    echo ""
    echo "Error: Failed to regenerate compile_commands.json"
    exit 1
fi