"""Base interfaces and data structures for pipeline agent adapters."""

from abc import ABC, abstractmethod
from dataclasses import dataclass, field, asdict
from enum import Enum
from pathlib import Path
from typing import Any, Dict, List, Optional


class FindingSeverity(str, Enum):
    BLOCKER = "BLOCKER"
    WARNING = "WARNING"
    INFO = "INFO"


@dataclass
class ReviewFinding:
    severity: FindingSeverity
    file: str
    evidence: str
    recommended_fix: str

    def to_dict(self) -> Dict[str, Any]:
        return {
            "severity": self.severity.value if isinstance(self.severity, FindingSeverity) else self.severity,
            "file": self.file,
            "evidence": self.evidence,
            "recommended_fix": self.recommended_fix
        }


@dataclass
class ReviewResult:
    passed: bool
    blocking: bool
    summary: str
    findings: List[ReviewFinding] = field(default_factory=list)
    raw_output: Optional[str] = None
    model_reviewed: bool = False
    candidate_fingerprint: Optional[str] = None

    def to_dict(self) -> Dict[str, Any]:
        return {
            "passed": self.passed,
            "blocking": self.blocking,
            "summary": self.summary,
            "findings": [f.to_dict() if hasattr(f, "to_dict") else f for f in self.findings],
            "raw_output": self.raw_output,
            "model_reviewed": self.model_reviewed,
            "candidate_fingerprint": self.candidate_fingerprint
        }


@dataclass
class ImplementationResult:
    success: bool
    summary: str
    modified_files: List[str] = field(default_factory=list)
    raw_output: Optional[str] = None
    duration_seconds: float = 0.0

    def to_dict(self) -> Dict[str, Any]:
        return asdict(self)


class BaseImplementationAgent(ABC):
    @abstractmethod
    def implement(self, task_description: str, worktree_path: Path, repair_feedback: Optional[str] = None) -> ImplementationResult:
        """Executes the implementation task inside the specified worktree."""
        pass


class BaseReviewerAgent(ABC):
    @abstractmethod
    def review(
        self,
        worktree_path: Path,
        git_diff: str,
        task_description: str,
        candidate_fingerprint: Optional[str] = None,
        modified_files: Optional[List[str]] = None
    ) -> ReviewResult:
        """Executes an independent review of the changes introduced in the worktree."""
        pass
