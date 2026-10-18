"""Persistent job store for the CIPHER Agent Pipeline."""

import json
import os
import re
import threading
import uuid
from datetime import datetime, timezone
from enum import Enum
from pathlib import Path
from typing import Any, Dict, List, Optional
from dataclasses import dataclass, field, asdict

from .config import JOB_STORE_DIR


class JobStatus(str, Enum):
    PENDING = "PENDING"
    PREPARING_WORKTREE = "PREPARING_WORKTREE"
    IMPLEMENTING = "IMPLEMENTING"
    REVIEWING = "REVIEWING"
    REPAIRING = "REPAIRING"
    VERIFYING = "VERIFYING"
    READY_FOR_APPROVAL = "READY_FOR_APPROVAL"
    COMMITTED = "COMMITTED"
    COMPLETED = "COMPLETED"
    FAILED_REVIEW = "FAILED_REVIEW"
    FAILED_VERIFICATION = "FAILED_VERIFICATION"
    FAILED_ERROR = "FAILED_ERROR"
    CANCELLED = "CANCELLED"


@dataclass
class JobRecord:
    job_id: str
    task_description: str
    task_type: str = "general"
    status: JobStatus = JobStatus.PENDING
    created_at: str = field(default_factory=lambda: datetime.now(timezone.utc).isoformat())
    updated_at: str = field(default_factory=lambda: datetime.now(timezone.utc).isoformat())
    completed_at: Optional[str] = None
    worktree_path: Optional[str] = None
    branch_name: Optional[str] = None
    base_commit: Optional[str] = None
    repair_attempts: int = 0
    claude_executions: List[Dict[str, Any]] = field(default_factory=list)
    reviews: List[Dict[str, Any]] = field(default_factory=list)
    candidate_fingerprint: Optional[str] = None
    verification_results: Optional[Dict[str, Any]] = None
    git_diff: Optional[str] = None
    error_message: Optional[str] = None
    events: List[Dict[str, Any]] = field(default_factory=list)

    @property
    def codex_reviews(self) -> List[Dict[str, Any]]:
        """Deprecated legacy alias for reviews."""
        return self.reviews

    @codex_reviews.setter
    def codex_reviews(self, val: List[Dict[str, Any]]) -> None:
        self.reviews = val

    def to_dict(self) -> Dict[str, Any]:
        data = asdict(self)
        data["status"] = self.status.value if isinstance(self.status, JobStatus) else self.status
        return data

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> "JobRecord":
        d = dict(data)
        # Migrate legacy codex_reviews field to generic reviews field
        if "codex_reviews" in d:
            if "reviews" not in d or not d.get("reviews"):
                d["reviews"] = d["codex_reviews"]
            d.pop("codex_reviews", None)
        status_val = d.get("status", JobStatus.PENDING.value)
        try:
            status = JobStatus(status_val)
        except ValueError:
            status = JobStatus.FAILED_ERROR
        d["status"] = status
        return cls(**d)


class JobStore:
    def __init__(self, store_dir: Path = JOB_STORE_DIR):
        self.store_dir = Path(store_dir)
        self.store_dir.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()

    def _file_path(self, job_id: str) -> Path:
        if not re.match(r"^[a-zA-Z0-9_\-]+$", str(job_id)):
            raise ValueError(f"Invalid job ID '{job_id}': must be alphanumeric with '_' or '-'.")
        return self.store_dir / f"{job_id}.json"

    def create_job(self, task_description: str, task_type: str = "general") -> JobRecord:
        job_id = str(uuid.uuid4())
        record = JobRecord(
            job_id=job_id,
            task_description=task_description,
            task_type=task_type,
            status=JobStatus.PENDING
        )
        record.events.append({
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "event": "JOB_CREATED",
            "details": f"Task: {task_description[:80]}"
        })
        self.save_job(record)
        return record

    def get_job(self, job_id: str) -> Optional[JobRecord]:
        try:
            fp = self._file_path(job_id)
        except ValueError:
            return None
        if not fp.exists():
            return None
        try:
            with open(fp, "r", encoding="utf-8") as f:
                data = json.load(f)
            return JobRecord.from_dict(data)
        except Exception:
            return None

    def save_job(self, job: JobRecord) -> None:
        with self._lock:
            job.updated_at = datetime.now(timezone.utc).isoformat()
            fp = self._file_path(job.job_id)
            tmp_fp = fp.with_suffix(".tmp")
            with open(tmp_fp, "w", encoding="utf-8") as f:
                json.dump(job.to_dict(), f, indent=2)
            os.replace(tmp_fp, fp)

    def update_status(self, job_id: str, new_status: JobStatus, details: str = "") -> Optional[JobRecord]:
        with self._lock:
            job = self.get_job(job_id)
            if not job:
                return None
            terminal_states = {
                JobStatus.CANCELLED,
                JobStatus.COMMITTED,
                JobStatus.FAILED_ERROR,
                JobStatus.FAILED_REVIEW,
                JobStatus.FAILED_VERIFICATION
            }
            if job.status in terminal_states and new_status != job.status:
                return job
            job.status = new_status
            now_iso = datetime.now(timezone.utc).isoformat()
            job.updated_at = now_iso
            if new_status in (
                JobStatus.COMPLETED,
                JobStatus.READY_FOR_APPROVAL,
                JobStatus.FAILED_REVIEW,
                JobStatus.FAILED_VERIFICATION,
                JobStatus.FAILED_ERROR,
                JobStatus.CANCELLED
            ):
                job.completed_at = now_iso
                if new_status in (JobStatus.FAILED_REVIEW, JobStatus.FAILED_VERIFICATION, JobStatus.FAILED_ERROR):
                    job.error_message = details
            job.events.append({
                "timestamp": now_iso,
                "event": f"STATUS_{new_status.value}",
                "details": details
            })
            fp = self._file_path(job.job_id)
            tmp_fp = fp.with_suffix(".tmp")
            with open(tmp_fp, "w", encoding="utf-8") as f:
                json.dump(job.to_dict(), f, indent=2)
            os.replace(tmp_fp, fp)
            return job

    def list_jobs(self, limit: int = 20) -> List[JobRecord]:
        jobs = []
        for fp in sorted(self.store_dir.glob("*.json"), key=lambda p: p.stat().st_mtime, reverse=True):
            try:
                with open(fp, "r", encoding="utf-8") as f:
                    data = json.load(f)
                jobs.append(JobRecord.from_dict(data))
                if len(jobs) >= limit:
                    break
            except Exception:
                continue
        return jobs
