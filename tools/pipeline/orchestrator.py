"""Pipeline orchestrator coordinating Claude Agent A, Claude Agent B, Git worktrees, and verification."""

import threading
from pathlib import Path
from typing import Optional

from .adapters.base import BaseImplementationAgent, BaseReviewerAgent
from .adapters.claude_adapter import ClaudeCodeAdapter
from .adapters.claude_reviewer_adapter import ClaudeReviewAdapter
from .adapters.mock_adapter import MockImplementationAgent, MockReviewerAgent
from .config import MAX_REPAIR_ROUNDS
from .git_manager import GitManager, GitSafetyViolation
from .job_store import JobRecord, JobStatus, JobStore
from .verifier import Verifier


class PipelineOrchestrator:
    def __init__(self, job_store: Optional[JobStore] = None, git_mgr: Optional[GitManager] = None):
        self.job_store = job_store or JobStore()
        self.git_mgr = git_mgr or GitManager()
        self._running_threads = {}

    def submit_task(
        self,
        task_description: str,
        task_type: str = "general",
        use_mock: bool = False,
        run_async: bool = False,
        custom_impl: Optional[BaseImplementationAgent] = None,
        custom_reviewer: Optional[BaseReviewerAgent] = None,
        allow_waiver: bool = False
    ) -> JobRecord:
        """Creates and submits a new pipeline job."""
        job = self.job_store.create_job(task_description, task_type)

        if run_async:
            thread = threading.Thread(
                target=self.execute_job,
                args=(job.job_id,),
                kwargs={
                    "use_mock": use_mock,
                    "custom_impl": custom_impl,
                    "custom_reviewer": custom_reviewer,
                    "allow_waiver": allow_waiver
                },
                daemon=True
            )
            self._running_threads[job.job_id] = thread
            thread.start()
            return job
        else:
            return self.execute_job(
                job.job_id,
                use_mock=use_mock,
                custom_impl=custom_impl,
                custom_reviewer=custom_reviewer,
                allow_waiver=allow_waiver
            )

    def _is_job_cancelled(self, job_id: str) -> bool:
        """Helper to check if a job was asynchronously cancelled."""
        job = self.job_store.get_job(job_id)
        return job is not None and job.status == JobStatus.CANCELLED

    def execute_job(
        self,
        job_id: str,
        use_mock: bool = False,
        custom_impl: Optional[BaseImplementationAgent] = None,
        custom_reviewer: Optional[BaseReviewerAgent] = None,
        allow_waiver: bool = False
    ) -> JobRecord:
        """Executes the full pipeline workflow for a job."""
        job = self.job_store.get_job(job_id)
        if not job:
            raise ValueError(f"Job {job_id} not found")

        # Resolve adapters (Agent A: Implementer, Agent B: Reviewer)
        impl_agent: BaseImplementationAgent = (
            custom_impl or (MockImplementationAgent() if use_mock else ClaudeCodeAdapter())
        )
        reviewer_agent: BaseReviewerAgent = (
            custom_reviewer or (MockReviewerAgent() if use_mock else ClaudeReviewAdapter())
        )

        try:
            if self._is_job_cancelled(job_id):
                return self.job_store.get_job(job_id)

            # Record baseline commit before branching
            base_commit = self.git_mgr.get_current_head()

            # Step 1: Create isolated worktree
            self.job_store.update_status(job_id, JobStatus.PREPARING_WORKTREE, "Creating Git worktree")
            worktree_path, branch_name = self.git_mgr.create_worktree(job_id)
            job = self.job_store.get_job(job_id)
            job.worktree_path = str(worktree_path)
            job.branch_name = branch_name
            job.base_commit = base_commit
            self.job_store.save_job(job)

            if self._is_job_cancelled(job_id):
                return self.job_store.get_job(job_id)

            # Step 2: Implementation via Claude Agent A
            self.job_store.update_status(job_id, JobStatus.IMPLEMENTING, "Running implementation agent (Claude Agent A)")
            impl_res = impl_agent.implement(job.task_description, worktree_path)
            job = self.job_store.get_job(job_id)
            job.claude_executions.append(impl_res.to_dict())
            self.job_store.save_job(job)

            if self._is_job_cancelled(job_id):
                return self.job_store.get_job(job_id)

            if not impl_res.success:
                self.job_store.update_status(
                    job_id,
                    JobStatus.FAILED_ERROR,
                    f"Implementation failed: {impl_res.summary}"
                )
                return self.job_store.get_job(job_id)

            # Step 3: Combined Review, Verification, and Bounded Repair Loop (max 2 rounds)
            candidate_fingerprint = None
            diff_text = ""

            while True:
                if self._is_job_cancelled(job_id):
                    return self.job_store.get_job(job_id)

                # Compute and record candidate fingerprint before review
                candidate_fingerprint = self.git_mgr.compute_candidate_fingerprint(worktree_path, base_commit=base_commit)
                job = self.job_store.get_job(job_id)
                job.candidate_fingerprint = candidate_fingerprint
                self.job_store.save_job(job)

                diff_text, modified_files = self.git_mgr.get_worktree_diff(worktree_path, base_commit=base_commit)
                job = self.job_store.get_job(job_id)
                job.git_diff = diff_text
                self.job_store.save_job(job)

                # Validate Git Safety rules
                try:
                    self.git_mgr.validate_safety(worktree_path, modified_files)
                except GitSafetyViolation as gsv:
                    self.job_store.update_status(job_id, JobStatus.FAILED_ERROR, str(gsv))
                    return self.job_store.get_job(job_id)

                # Step 3a: Independent Review (Claude Agent B)
                self.job_store.update_status(job_id, JobStatus.REVIEWING, "Running independent review (Claude Agent B)")
                try:
                    review_res = reviewer_agent.review(
                        worktree_path,
                        diff_text,
                        job.task_description,
                        candidate_fingerprint=candidate_fingerprint,
                        modified_files=modified_files
                    )
                except TypeError:
                    review_res = reviewer_agent.review(
                        worktree_path,
                        diff_text,
                        job.task_description,
                        candidate_fingerprint=candidate_fingerprint
                    )
                job = self.job_store.get_job(job_id)
                job.reviews.append(review_res.to_dict())
                self.job_store.save_job(job)

                if self._is_job_cancelled(job_id):
                    return self.job_store.get_job(job_id)

                # Invariant: Verify candidate state did not mutate during review
                post_review_fingerprint = self.git_mgr.compute_candidate_fingerprint(worktree_path, base_commit=base_commit)
                if post_review_fingerprint != candidate_fingerprint:
                    self.job_store.update_status(
                        job_id,
                        JobStatus.FAILED_ERROR,
                        "Candidate state mutated during review. Pipeline aborted to maintain invariant."
                    )
                    return self.job_store.get_job(job_id)

                # If review failed with blocking issues or schema violations
                if not review_res.passed:
                    if not review_res.model_reviewed:
                        # Infrastructure error (CLI missing, timeout, crash) — do not waste repair rounds
                        self.job_store.update_status(
                            job_id,
                            JobStatus.FAILED_REVIEW,
                            f"Reviewer infrastructure failure: {review_res.summary}"
                        )
                        return self.job_store.get_job(job_id)

                    if job.repair_attempts >= MAX_REPAIR_ROUNDS:
                        self.job_store.update_status(
                            job_id,
                            JobStatus.FAILED_REVIEW,
                            f"Review failed after {job.repair_attempts} repair attempts: {review_res.summary}"
                        )
                        return self.job_store.get_job(job_id)

                    # Execute repair round via Claude Agent A
                    job.repair_attempts += 1
                    self.job_store.save_job(job)
                    self.job_store.update_status(
                        job_id,
                        JobStatus.REPAIRING,
                        f"Repair round {job.repair_attempts}/{MAX_REPAIR_ROUNDS} (Review feedback)"
                    )
                    repair_prompt = "\n".join(
                        f"- [{f.get('severity')}] {f.get('file')}: {f.get('evidence')} -> Fix: {f.get('recommended_fix')}"
                        for f in review_res.to_dict().get("findings", [])
                    )
                    repair_res = impl_agent.implement(job.task_description, worktree_path, repair_feedback=repair_prompt)
                    job = self.job_store.get_job(job_id)
                    job.claude_executions.append(repair_res.to_dict())
                    self.job_store.save_job(job)

                    if not repair_res.success:
                        self.job_store.update_status(
                            job_id,
                            JobStatus.FAILED_ERROR,
                            f"Repair implementation failed: {repair_res.summary}"
                        )
                        return self.job_store.get_job(job_id)

                    continue  # Repaired candidate triggers fresh review and fresh verification

                # Step 3b: Independent Verification (Run CIPHER test suites)
                self.job_store.update_status(job_id, JobStatus.VERIFYING, "Executing independent test verification")
                verifier = Verifier(worktree_path)
                test_results = verifier.run_tests(git_diff=diff_text)
                job = self.job_store.get_job(job_id)
                job.verification_results = test_results
                self.job_store.save_job(job)

                if self._is_job_cancelled(job_id):
                    return self.job_store.get_job(job_id)

                # Invariant: Verify candidate state did not mutate during verification
                post_verify_fingerprint = self.git_mgr.compute_candidate_fingerprint(worktree_path, base_commit=base_commit)
                if post_verify_fingerprint != candidate_fingerprint:
                    self.job_store.update_status(
                        job_id,
                        JobStatus.FAILED_ERROR,
                        "Candidate state mutated during verification. Pipeline aborted."
                    )
                    return self.job_store.get_job(job_id)

                # If test verification failed
                if not test_results.get("all_passed", False):
                    if job.repair_attempts >= MAX_REPAIR_ROUNDS:
                        self.job_store.update_status(
                            job_id,
                            JobStatus.FAILED_VERIFICATION,
                            f"Tests failed after {job.repair_attempts} repair attempts: {test_results.get('summary', '')}"
                        )
                        return self.job_store.get_job(job_id)

                    # Execute repair round via Claude Agent A
                    job.repair_attempts += 1
                    self.job_store.save_job(job)
                    self.job_store.update_status(
                        job_id,
                        JobStatus.REPAIRING,
                        f"Repair round {job.repair_attempts}/{MAX_REPAIR_ROUNDS} (Verification failure)"
                    )
                    repair_prompt = (
                        f"Test Verification Failed:\n{test_results.get('summary', '')}\nErrors:\n"
                        + "\n".join(test_results.get("error_details", []))
                    )
                    repair_res = impl_agent.implement(job.task_description, worktree_path, repair_feedback=repair_prompt)
                    job = self.job_store.get_job(job_id)
                    job.claude_executions.append(repair_res.to_dict())
                    self.job_store.save_job(job)
                    continue  # Repaired candidate triggers fresh review and fresh verification

                # Both review and verification passed cleanly!
                break

            # Step 4: Final Git Diff & Ready for Local Commit Approval
            diff_text, _ = self.git_mgr.get_worktree_diff(worktree_path, base_commit=base_commit)
            job = self.job_store.get_job(job_id)
            job.git_diff = diff_text
            self.job_store.save_job(job)

            if self._is_job_cancelled(job_id):
                return self.job_store.get_job(job_id)

            self.job_store.update_status(
                job_id,
                JobStatus.READY_FOR_APPROVAL,
                "Pipeline completed successfully. Candidate verified and ready for owner local commit approval."
            )
            return self.job_store.get_job(job_id)

        except Exception as e:
            current = self.job_store.get_job(job_id)
            if current and current.status == JobStatus.CANCELLED:
                return current
            self.job_store.update_status(job_id, JobStatus.FAILED_ERROR, f"Unexpected error: {str(e)}")
            return self.job_store.get_job(job_id)

    def approve_commit(self, job_id: str, commit_message: Optional[str] = None) -> JobRecord:
        """Validates candidate integrity and records owner approval for a local Git commit.

        Guarantees that:
        1. Job is currently in READY_FOR_APPROVAL status.
        2. The worktree's candidate fingerprint matches the verified candidate fingerprint.
           If any candidate file was modified, added, or deleted after approval was generated,
           the stale approval is invalidated and the job is transitioned to FAILED_ERROR.
        """
        job = self.job_store.get_job(job_id)
        if not job:
            raise ValueError(f"Job '{job_id}' not found.")

        if job.status != JobStatus.READY_FOR_APPROVAL:
            raise RuntimeError(
                f"Job '{job_id}' is in status {job.status.value}, not READY_FOR_APPROVAL."
            )

        if not job.worktree_path:
            raise RuntimeError(f"Job '{job_id}' has no associated worktree path.")

        # Invariant checks: Verify review and verification succeeded
        if not job.reviews or not job.reviews[-1].get("passed") or not job.reviews[-1].get("model_reviewed"):
            raise RuntimeError(f"Cannot approve commit: latest review for job '{job_id}' did not pass or was not validated by model.")

        if not job.verification_results or not job.verification_results.get("all_passed"):
            raise RuntimeError(f"Cannot approve commit: verification checks for job '{job_id}' did not all pass.")

        worktree_path = Path(job.worktree_path)
        if not worktree_path.exists():
            raise RuntimeError(f"Worktree path '{worktree_path}' no longer exists.")

        # Invariant check: Verify worktree candidate state has not mutated since approval was generated
        base_commit = job.base_commit or self.git_mgr.get_current_head()
        current_fingerprint = self.git_mgr.compute_candidate_fingerprint(worktree_path, base_commit=base_commit)

        if current_fingerprint != job.candidate_fingerprint:
            job = self.job_store.update_status(
                job_id,
                JobStatus.FAILED_ERROR,
                f"Stale approval invalidated: candidate worktree mutated after approval was generated "
                f"(expected {job.candidate_fingerprint}, got {current_fingerprint}). Re-review and re-verification required."
            )
            raise RuntimeError(
                f"Candidate integrity check failed: worktree mutated after review/verification. "
                f"Stale approval invalidated for job '{job_id}'."
            )

        # Record local commit approval
        msg = commit_message or f"CIPHER Pipeline: {job.task_description}"
        job = self.job_store.update_status(
            job_id,
            JobStatus.COMMITTED,
            f"Local commit approved by owner. Ready for separate remote push approval. Commit msg: {msg}"
        )
        return job

    def cancel_job(self, job_id: str) -> Optional[JobRecord]:
        """Cancels a job and cleans up its worktree."""
        job = self.job_store.get_job(job_id)
        if not job:
            return None

        # Immediately mark as CANCELLED to prevent concurrent execution advancement
        job = self.job_store.update_status(job_id, JobStatus.CANCELLED, "Job cancelled by user request")

        if job.worktree_path:
            self.git_mgr.remove_worktree(Path(job.worktree_path), job.branch_name)

        return job
