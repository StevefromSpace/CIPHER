"""Claude Code CLI independent reviewer adapter (Agent B) for the CIPHER pipeline."""

import json
import re
import subprocess
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from ..config import (
    CLAUDE_CLI,
    CLAUDE_REVIEWER_ALLOWED_TOOLS,
    CLAUDE_REVIEWER_DISALLOWED_TOOLS,
    CLAUDE_REVIEWER_PERMISSION_MODE,
    AGENT_TIMEOUT_SECONDS,
    MAX_REVIEW_DIFF_CHARS
)
from .base import BaseReviewerAgent, FindingSeverity, ReviewFinding, ReviewResult


class ClaudeReviewAdapter(BaseReviewerAgent):
    """Independent reviewer agent (Agent B) powered by Claude Code CLI.

    Operates in an isolated, strictly read-only subprocess with tool access limited
    to inspection tools (Read, Glob, Grep) and forbidden from writing, editing,
    or executing shell commands.
    """

    def __init__(
        self,
        cli_path: Path = CLAUDE_CLI,
        allowed_tools: str = CLAUDE_REVIEWER_ALLOWED_TOOLS,
        disallowed_tools: str = CLAUDE_REVIEWER_DISALLOWED_TOOLS,
        permission_mode: str = CLAUDE_REVIEWER_PERMISSION_MODE,
        timeout: int = AGENT_TIMEOUT_SECONDS,
        max_diff_chars: int = MAX_REVIEW_DIFF_CHARS
    ):
        self.cli_path = Path(cli_path)
        self.allowed_tools = allowed_tools
        self.disallowed_tools = disallowed_tools
        self.permission_mode = permission_mode
        self.timeout = timeout
        self.max_diff_chars = max_diff_chars

    def _build_review_prompt(
        self,
        task_description: str,
        git_diff: str,
        modified_files: Optional[List[str]] = None
    ) -> str:
        prompt_lines = [
            "You are acting as CLAUDE AGENT B, the INDEPENDENT CODE REVIEWER for CIPHER.",
            "Your role is to independently inspect all uncommitted candidate changes in this repository",
            "against the task specification and project invariants.",
            "",
            "Review Rubric & Constraints:",
            "1. Correctness: The changes must faithfully implement the requested task without regressions or incomplete stubs.",
            "2. Security: ZERO hardcoded secrets, API keys, passwords, bearer tokens, or untrusted shell injection vectors.",
            "3. Decoupling & Architecture: cipher_core must NOT depend on or include config or logger subsystems.",
            "   Primary language is C++17. No unnecessary third-party libraries.",
            "4. Canonical Documentation: Invariants in docs/PROJECT_STATE.md, docs/DECISIONS.md, and docs/ARCHITECTURE.md must be respected.",
            "   No fabricated capabilities or misleading claims.",
            "5. Test & Build Integrity: Test suites must compile and pass cleanly without broken references.",
            "6. Scope & Path Safety: Prohibited files (.git/, .github/, cipher.conf) must never be modified.",
            ""
        ]

        if modified_files:
            prompt_lines.append(f"CANDIDATE MODIFIED FILES ({len(modified_files)} total):")
            for mf in sorted(modified_files):
                prompt_lines.append(f"- {mf}")
            prompt_lines.append("")

        prompt_lines.extend([
            f"TASK SPECIFICATION:\n{task_description}\n",
            f"COMPLETE CANDIDATE GIT DIFF:\n```diff\n{git_diff}\n```\n",
            "INSTRUCTIONS FOR YOUR RESPONSE:",
            "You MUST return a JSON code block with a list of structured findings:",
            "```json",
            "{",
            '  "summary": "<1-2 sentence overall review assessment>",',
            '  "findings": [',
            "    {",
            '      "severity": "BLOCKER" | "WARNING" | "INFO",',
            '      "file": "<relative file path>",',
            '      "loc": "<line number or range>",',
            '      "evidence": "<exact snippet or concise explanation of violation>",',
            '      "recommended_fix": "<actionable fix recommendation>"',
            "    }",
            "  ]",
            "}",
            "```",
            "If no issues are found, return an empty findings list: {\"summary\": \"Review passed\", \"findings\": []}.",
            "Mark any invariant violation, security risk, or broken requirement as BLOCKER.",
            "DO NOT return unstructured markdown text or omit the JSON schema. Unstructured prose will fail the review."
        ])
        return "\n".join(prompt_lines)

    def _parse_review_output(self, raw_output: str) -> Tuple[List[ReviewFinding], str, bool]:
        """Strictly validates and parses reviewer output according to the review schema.

        Returns:
            Tuple of (findings, summary, is_schema_valid).
            If schema is violated, returns BLOCKER findings and is_schema_valid=False.
        """
        findings: List[ReviewFinding] = []
        summary = ""

        # Extract JSON block: look for code fence ```json ... ``` or first '{' to last '}'
        json_match = re.search(r"```(?:json)?\s*(\{.*?\})\s*```", raw_output, re.DOTALL)
        candidate_json = json_match.group(1) if json_match else None

        if not candidate_json:
            start_idx = raw_output.find("{")
            end_idx = raw_output.rfind("}")
            if start_idx != -1 and end_idx != -1 and end_idx > start_idx:
                candidate_json = raw_output[start_idx:end_idx + 1]

        if not candidate_json:
            # Arbitrary prose without JSON structure
            findings.append(ReviewFinding(
                severity=FindingSeverity.BLOCKER,
                file="claude_reviewer",
                evidence="Reviewer returned unstructured prose without required JSON schema.",
                recommended_fix="Format response with strict JSON block conforming to review schema."
            ))
            return findings, "Review FAILED: Malformed output (no JSON structure found).", False

        try:
            data = json.loads(candidate_json)
        except Exception as e:
            findings.append(ReviewFinding(
                severity=FindingSeverity.BLOCKER,
                file="claude_reviewer",
                evidence=f"Reviewer output contained invalid JSON syntax: {str(e)}",
                recommended_fix="Ensure reviewer output is valid parseable JSON."
            ))
            return findings, f"Review FAILED: Invalid JSON syntax ({str(e)}).", False

        if not isinstance(data, dict):
            findings.append(ReviewFinding(
                severity=FindingSeverity.BLOCKER,
                file="claude_reviewer",
                evidence="Top-level review JSON must be an object/dict.",
                recommended_fix="Format top-level JSON as an object with 'summary' and 'findings' fields."
            ))
            return findings, "Review FAILED: Schema invalid (top-level is not an object).", False

        # Validate 'summary'
        if "summary" not in data or not isinstance(data["summary"], str) or not data["summary"].strip():
            findings.append(ReviewFinding(
                severity=FindingSeverity.BLOCKER,
                file="claude_reviewer",
                evidence="Review JSON missing required non-empty 'summary' string field.",
                recommended_fix="Include a non-empty 'summary' field describing the review assessment."
            ))
            return findings, "Review FAILED: Schema invalid (missing or empty 'summary').", False

        summary = data["summary"].strip()

        # Validate 'findings'
        if "findings" not in data or not isinstance(data["findings"], list):
            findings.append(ReviewFinding(
                severity=FindingSeverity.BLOCKER,
                file="claude_reviewer",
                evidence="Review JSON missing required 'findings' list field.",
                recommended_fix="Include a 'findings' list field (use [] if no issues)."
            ))
            return findings, "Review FAILED: Schema invalid ('findings' must be a list).", False

        raw_findings = data["findings"]
        valid_severities = {"BLOCKER", "WARNING", "INFO"}

        has_schema_error = False

        for idx, rf in enumerate(raw_findings):
            if not isinstance(rf, dict):
                has_schema_error = True
                findings.append(ReviewFinding(
                    severity=FindingSeverity.BLOCKER,
                    file="claude_reviewer",
                    evidence=f"Finding at index {idx} must be an object/dict.",
                    recommended_fix="Format each finding as an object with severity, file, evidence, and recommended_fix."
                ))
                continue

            # Check required fields
            required_fields = ["severity", "file", "evidence", "recommended_fix"]
            missing = [f for f in required_fields if f not in rf or not str(rf.get(f, "")).strip()]
            if missing:
                has_schema_error = True
                findings.append(ReviewFinding(
                    severity=FindingSeverity.BLOCKER,
                    file="claude_reviewer",
                    evidence=f"Finding at index {idx} is missing required non-empty fields: {missing}.",
                    recommended_fix=f"Provide all required fields ({', '.join(required_fields)})."
                ))
                continue

            sev_str = str(rf["severity"]).strip().upper()
            if sev_str not in valid_severities:
                has_schema_error = True
                findings.append(ReviewFinding(
                    severity=FindingSeverity.BLOCKER,
                    file="claude_reviewer",
                    evidence=f"Finding at index {idx} has invalid severity '{rf['severity']}'. Allowed: {sorted(list(valid_severities))}.",
                    recommended_fix="Set severity strictly to 'BLOCKER', 'WARNING', or 'INFO'."
                ))
                continue

            sev = (
                FindingSeverity.BLOCKER if sev_str == "BLOCKER"
                else (FindingSeverity.INFO if sev_str == "INFO" else FindingSeverity.WARNING)
            )

            findings.append(ReviewFinding(
                severity=sev,
                file=str(rf["file"]).strip(),
                evidence=str(rf["evidence"]).strip()[:300],
                recommended_fix=str(rf["recommended_fix"]).strip()
            ))

        is_valid = not has_schema_error
        return findings, summary, is_valid


    def review(
        self,
        worktree_path: Path,
        git_diff: str,
        task_description: str,
        candidate_fingerprint: Optional[str] = None,
        modified_files: Optional[List[str]] = None
    ) -> ReviewResult:
        findings: List[ReviewFinding] = []
        raw_output = ""
        model_reviewed = False

        # Guard 1: Enforce diff input limit (Fail closed instead of silent truncation)
        if len(git_diff) > self.max_diff_chars:
            findings.append(ReviewFinding(
                severity=FindingSeverity.BLOCKER,
                file="claude_reviewer",
                evidence=(
                    f"Candidate diff size ({len(git_diff)} chars) exceeds maximum review limit "
                    f"({self.max_diff_chars} chars). Input cannot be truncated without losing review coverage."
                ),
                recommended_fix="Decompose task into smaller atomic changes within review input limits."
            ))
            return ReviewResult(
                passed=False,
                blocking=True,
                summary=f"Review FAILED: Candidate diff exceeds limit ({len(git_diff)} > {self.max_diff_chars}).",
                findings=findings,
                raw_output=f"Diff too large: {len(git_diff)} chars",
                model_reviewed=False,
                candidate_fingerprint=candidate_fingerprint
            )

        # Guard 2: Check CLI presence
        if not self.cli_path.exists():
            findings.append(ReviewFinding(
                severity=FindingSeverity.BLOCKER,
                file="claude_reviewer",
                evidence=f"Claude CLI executable not found at '{self.cli_path}'.",
                recommended_fix="Ensure Claude Code CLI is installed at the configured location."
            ))
            return ReviewResult(
                passed=False,
                blocking=True,
                summary=f"Review FAILED: Reviewer CLI missing at {self.cli_path}.",
                findings=findings,
                raw_output="CLI not found.",
                model_reviewed=False,
                candidate_fingerprint=candidate_fingerprint
            )

        prompt = self._build_review_prompt(task_description, git_diff, modified_files=modified_files)

        # Strictly restricted CLI invocation: read-only tools, no shell, no edit, no MCP
        # Pass prompt on stdin to prevent Win32 command line length limits
        cmd = [
            str(self.cli_path),
            "-p",
            "--output-format", "json",
            "--tools", self.allowed_tools,
            "--disallowed-tools", self.disallowed_tools,
            "--strict-mcp-config",
            "--permission-mode", self.permission_mode
        ]

        try:
            proc = subprocess.run(
                cmd,
                cwd=str(worktree_path),
                input=prompt,
                capture_output=True,
                text=True,
                timeout=self.timeout
            )
            raw_output = proc.stdout + ("\n" + proc.stderr if proc.stderr else "")

            # Guard: Non-zero exit code is an immediate blocking failure
            if proc.returncode != 0:
                err_snippet = proc.stderr.strip()[:300] or proc.stdout.strip()[:300]
                findings.append(ReviewFinding(
                    severity=FindingSeverity.BLOCKER,
                    file="claude_reviewer",
                    evidence=f"Reviewer process exited with error code {proc.returncode}: {err_snippet}",
                    recommended_fix="Inspect Claude Code CLI logs and environment."
                ))
                return ReviewResult(
                    passed=False,
                    blocking=True,
                    summary=f"Review FAILED: Reviewer process crashed with code {proc.returncode}.",
                    findings=findings,
                    raw_output=raw_output,
                    model_reviewed=False,
                    candidate_fingerprint=candidate_fingerprint
                )

            # In output-format=json, extract result text
            stdout_clean = proc.stdout.strip()
            json_start = stdout_clean.find("{")
            result_text = stdout_clean
            if json_start != -1:
                try:
                    data = json.loads(stdout_clean[json_start:])
                    if data.get("is_error"):
                        findings.append(ReviewFinding(
                            severity=FindingSeverity.BLOCKER,
                            file="claude_reviewer",
                            evidence=f"Claude API returned error: {data.get('result', '')[:200]}",
                            recommended_fix="Retry review after checking API connectivity."
                        ))
                        return ReviewResult(
                            passed=False,
                            blocking=True,
                            summary="Review FAILED: Claude API error reported.",
                            findings=findings,
                            raw_output=raw_output,
                            model_reviewed=False,
                            candidate_fingerprint=candidate_fingerprint
                        )
                    result_text = data.get("result", stdout_clean)
                except Exception:
                    pass

            # Guard: Empty or whitespace-only response is an immediate blocking failure
            if not result_text or not result_text.strip():
                findings.append(ReviewFinding(
                    severity=FindingSeverity.BLOCKER,
                    file="claude_reviewer",
                    evidence="Reviewer returned an empty response.",
                    recommended_fix="Check reviewer output formatting instructions."
                ))
                return ReviewResult(
                    passed=False,
                    blocking=True,
                    summary="Review FAILED: Empty response from reviewer.",
                    findings=findings,
                    raw_output=raw_output,
                    model_reviewed=False,
                    candidate_fingerprint=candidate_fingerprint
                )

            parsed_findings, summary, is_schema_valid = self._parse_review_output(result_text)
            findings.extend(parsed_findings)
            model_reviewed = is_schema_valid

        except subprocess.TimeoutExpired:
            findings.append(ReviewFinding(
                severity=FindingSeverity.BLOCKER,
                file="claude_reviewer",
                evidence=f"Review timed out after {self.timeout}s.",
                recommended_fix="Increase review timeout or optimize worktree diff size."
            ))
            return ReviewResult(
                passed=False,
                blocking=True,
                summary=f"Review FAILED: Reviewer timed out after {self.timeout}s.",
                findings=findings,
                raw_output=f"Timeout after {self.timeout}s",
                model_reviewed=False,
                candidate_fingerprint=candidate_fingerprint
            )
        except Exception as e:
            findings.append(ReviewFinding(
                severity=FindingSeverity.BLOCKER,
                file="claude_reviewer",
                evidence=f"Unexpected review failure: {str(e)}",
                recommended_fix="Check reviewer execution environment."
            ))
            return ReviewResult(
                passed=False,
                blocking=True,
                summary=f"Review FAILED: {str(e)}",
                findings=findings,
                raw_output=str(e),
                model_reviewed=False,
                candidate_fingerprint=candidate_fingerprint
            )

        blocking_count = sum(1 for f in findings if f.severity == FindingSeverity.BLOCKER)
        has_blocker = (blocking_count > 0) or (not model_reviewed)
        passed = (not has_blocker) and model_reviewed

        final_summary = (
            f"Review PASSED (model_reviewed=True) with {len(findings)} non-blocking findings. {summary}"
            if passed
            else f"Review FAILED: {blocking_count} blocking findings require resolution. {summary}"
        )

        return ReviewResult(
            passed=passed,
            blocking=has_blocker,
            summary=final_summary.strip(),
            findings=findings,
            raw_output=raw_output,
            model_reviewed=model_reviewed,
            candidate_fingerprint=candidate_fingerprint
        )
