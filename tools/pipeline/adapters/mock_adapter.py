"""Mock adapters for fast, deterministic unit testing without model calls."""

from pathlib import Path
from typing import List, Optional

from .base import (
    BaseImplementationAgent,
    BaseReviewerAgent,
    FindingSeverity,
    ImplementationResult,
    ReviewFinding,
    ReviewResult,
)


class MockImplementationAgent(BaseImplementationAgent):
    def __init__(self, should_succeed: bool = True, files_to_create: Optional[dict] = None):
        self.should_succeed = should_succeed
        self.files_to_create = files_to_create or {}
        self.call_count = 0

    def implement(self, task_description: str, worktree_path: Path, repair_feedback: Optional[str] = None) -> ImplementationResult:
        self.call_count += 1
        if not self.should_succeed:
            return ImplementationResult(
                success=False,
                summary="Mock implementation error: agent failed",
                duration_seconds=0.1
            )

        # Create simulated files if specified
        created = []
        for rel_path, content in self.files_to_create.items():
            dest = worktree_path / rel_path
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(content, encoding="utf-8")
            created.append(rel_path)

        return ImplementationResult(
            success=True,
            summary=f"Mock implementation successful. Created/modified: {created}",
            modified_files=created,
            duration_seconds=0.1
        )


class MockReviewerAgent(BaseReviewerAgent):
    def __init__(self, pass_on_round: int = 1, mock_findings: Optional[List[ReviewFinding]] = None):
        self.pass_on_round = pass_on_round
        self.mock_findings = mock_findings or []
        self.call_count = 0

    def review(
        self,
        worktree_path: Path,
        git_diff: str,
        task_description: str,
        candidate_fingerprint: Optional[str] = None,
        modified_files: Optional[List[str]] = None
    ) -> ReviewResult:
        self.call_count += 1
        should_pass = (self.call_count >= self.pass_on_round)

        if should_pass:
            return ReviewResult(
                passed=True,
                blocking=False,
                summary="Mock review: all checks passed cleanly",
                findings=[],
                model_reviewed=True,
                candidate_fingerprint=candidate_fingerprint
            )

        findings = self.mock_findings or [
            ReviewFinding(
                severity=FindingSeverity.BLOCKER,
                file="mock_file.txt",
                evidence="Simulated architectural issue",
                recommended_fix="Resolve mock issue"
            )
        ]
        return ReviewResult(
            passed=False,
            blocking=True,
            summary="Mock review: blocking issues found",
            findings=findings,
            model_reviewed=True,
            candidate_fingerprint=candidate_fingerprint
        )
