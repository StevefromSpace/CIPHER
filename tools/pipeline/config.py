"""Configuration constants for the CIPHER Agent Pipeline."""

import os
import shutil
import sys
from pathlib import Path

# Repository root (CIPHER project root)
REPO_ROOT = Path(__file__).resolve().parent.parent.parent

# Persistent directories
PIPELINE_DATA_DIR = REPO_ROOT / ".agents" / "pipeline"
JOB_STORE_DIR = PIPELINE_DATA_DIR / "jobs"
WORKTREE_BASE_DIR = PIPELINE_DATA_DIR / "worktrees"

# Executable locations
USER_PROFILE = Path(os.environ.get("USERPROFILE") or Path.home())

_which_claude = shutil.which("claude") or shutil.which("claude.exe")
CLAUDE_CLI = Path(_which_claude) if _which_claude else USER_PROFILE / ".local" / "bin" / "claude.exe"

_which_python = shutil.which("python") or shutil.which("python.exe")
PYTHON_EXECUTABLE = Path(_which_python) if _which_python else Path(sys.executable)

# MCP Configuration for spawned agents
MCP_CONFIG_PATH = REPO_ROOT / ".agents" / "mcp_config.json"
GRAPHIFY_OUT_DIR = REPO_ROOT / "graphify-out"
MEMORY_FILE_PATH = REPO_ROOT / ".agents" / "memory" / "cipher_memory.json"

# Operational Bounds & Safety Limits
MAX_REPAIR_ROUNDS = 2
AGENT_TIMEOUT_SECONDS = 180
TEST_TIMEOUT_SECONDS = 30
BUILD_TIMEOUT_SECONDS = 120
MAX_REVIEW_DIFF_CHARS = 100_000

# Build tools for isolated worktree compilation
_prog_files_x86 = os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")
VS_BUILD_TOOLS = Path(_prog_files_x86) / "Microsoft Visual Studio/2022/BuildTools"
CMAKE_BIN = VS_BUILD_TOOLS / "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
NINJA_BIN = VS_BUILD_TOOLS / "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
VCVARS64_BAT = VS_BUILD_TOOLS / "VC/Auxiliary/Build/vcvars64.bat"


# Claude Implementer (Agent A) Permission Settings
CLAUDE_PERMISSION_MODE = "acceptEdits"  # Restricts automatic edits to worktree directory only
CLAUDE_ALLOWED_TOOLS = "Edit,Write,Read,Glob,Grep"

# Claude Reviewer (Agent B) Permission Settings (Strict Read-Only)
CLAUDE_REVIEWER_ALLOWED_TOOLS = "Read,Glob,Grep"
CLAUDE_REVIEWER_DISALLOWED_TOOLS = "Bash,PowerShell,Write,Edit"
CLAUDE_REVIEWER_PERMISSION_MODE = "dontAsk"

# Test binaries relative to workspace/worktree
TEST_TARGETS = [
    Path("build") / "cipher_tests.exe",
    Path("build") / "cipher_config_tests.exe",
    Path("build") / "cipher_logger_tests.exe",
    Path("build") / "cipher_memory_tests.exe"
]
