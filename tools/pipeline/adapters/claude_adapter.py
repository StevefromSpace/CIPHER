"""Claude Code CLI implementation adapter for the CIPHER pipeline."""

import json
import subprocess
import time
from pathlib import Path
from typing import List, Optional

from ..config import (
    CLAUDE_CLI,
    MCP_CONFIG_PATH,
    AGENT_TIMEOUT_SECONDS,
    CLAUDE_PERMISSION_MODE,
    CLAUDE_ALLOWED_TOOLS
)
from .base import BaseImplementationAgent, ImplementationResult


class ClaudeCodeAdapter(BaseImplementationAgent):
    def __init__(self, cli_path: Path = CLAUDE_CLI, mcp_config: Path = MCP_CONFIG_PATH, timeout: int = AGENT_TIMEOUT_SECONDS):
        self.cli_path = Path(cli_path)
        self.mcp_config = Path(mcp_config)
        self.timeout = timeout

    def _build_prompt(self, task_description: str, repair_feedback: Optional[str] = None) -> str:
        prompt_lines = [
            "You are acting as the IMPLEMENTATION AGENT for the CIPHER project.",
            "Operate strictly inside this isolated worktree repository.",
            "",
            "Core Guidelines:",
            "- CIPHER is local-first, C++17, with zero external cloud dependencies.",
            "- Canonical docs in docs/ (PROJECT_STATE.md, DECISIONS.md, ARCHITECTURE.md) govern design.",
            "- Decoupling rule: cipher_core has no dependency on config or logger.",
            "- Use the installed Graphify MCP tool for code navigation if needed.",
            "- Do NOT touch .git/, .github/, or cipher.conf.",
            "",
            f"TASK: {task_description}"
        ]

        if repair_feedback:
            prompt_lines.extend([
                "",
                "PREVIOUS REVIEW FINDINGS / REPAIR REQUIRED:",
                repair_feedback,
                "Please fix the issues identified above while strictly adhering to project guidelines."
            ])

        return "\n".join(prompt_lines)

    def implement(self, task_description: str, worktree_path: Path, repair_feedback: Optional[str] = None) -> ImplementationResult:
        start_time = time.time()
        prompt = self._build_prompt(task_description, repair_feedback)

        if not self.cli_path.exists():
            return ImplementationResult(
                success=False,
                summary=f"Claude CLI not found at: {self.cli_path}",
                duration_seconds=time.time() - start_time
            )

        cmd = [
            str(self.cli_path),
            "-p", prompt,
            "--output-format", "json",
            "--permission-mode", CLAUDE_PERMISSION_MODE,
            "--allowed-tools", CLAUDE_ALLOWED_TOOLS
        ]

        if self.mcp_config.exists():
            cmd.extend(["--mcp-config", str(self.mcp_config)])

        try:
            proc = subprocess.run(
                cmd,
                cwd=str(worktree_path),
                stdin=subprocess.DEVNULL,
                capture_output=True,
                text=True,
                timeout=self.timeout
            )
            duration = time.time() - start_time

            if proc.returncode != 0:
                return ImplementationResult(
                    success=False,
                    summary=f"Claude CLI exited with code {proc.returncode}: {proc.stderr[:300]}",
                    raw_output=proc.stdout,
                    duration_seconds=duration
                )

            # Parse JSON output from Claude Code
            try:
                # In output-format=json, stdout might have a leading warning line before JSON
                stdout_text = proc.stdout.strip()
                json_start = stdout_text.find("{")
                if json_start != -1:
                    data = json.loads(stdout_text[json_start:])
                    res_text = data.get("result", "")
                    subtype = data.get("subtype", "")
                    is_err = data.get("is_error", False)
                    success = (subtype == "success" and not is_err)
                    return ImplementationResult(
                        success=success,
                        summary=res_text[:500] if res_text else "Implementation finished",
                        raw_output=stdout_text,
                        duration_seconds=duration
                    )
            except Exception:
                pass

            return ImplementationResult(
                success=True,
                summary=proc.stdout[:500],
                raw_output=proc.stdout,
                duration_seconds=duration
            )

        except subprocess.TimeoutExpired:
            return ImplementationResult(
                success=False,
                summary=f"Claude CLI timed out after {self.timeout}s",
                duration_seconds=time.time() - start_time
            )
        except Exception as e:
            return ImplementationResult(
                success=False,
                summary=f"Claude execution failed: {str(e)}",
                duration_seconds=time.time() - start_time
            )
