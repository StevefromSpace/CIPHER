"""Unit tests for the PipelineOrchestrator state machine and repair bounds."""

import shutil
import tempfile
import unittest
from pathlib import Path

from tools.pipeline.adapters.base import FindingSeverity, ReviewFinding
from tools.pipeline.adapters.mock_adapter import MockImplementationAgent, MockReviewerAgent
from tools.pipeline.config import MAX_REPAIR_ROUNDS
from tools.pipeline.git_manager import GitManager
from tools.pipeline.job_store import JobStatus, JobStore
from tools.pipeline.orchestrator import PipelineOrchestrator


class TestPipelineOrchestrator(unittest.TestCase):
    def setUp(self):
        self.temp_dir = Path(tempfile.mkdtemp())
        self.job_store = JobStore(store_dir=self.temp_dir / "jobs")
        self.git_mgr = GitManager(worktree_base=self.temp_dir / "worktrees")
        self.orchestrator = PipelineOrchestrator(job_store=self.job_store, git_mgr=self.git_mgr)

    def tearDown(self):
        # Clean up any leftover worktrees
        for job in self.job_store.list_jobs():
            if job.worktree_path:
                self.git_mgr.remove_worktree(Path(job.worktree_path), job.branch_name)
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def test_successful_pipeline_flow(self):
        impl = MockImplementationAgent(
            should_succeed=True,
            files_to_create={"docs/scratch_test_note.txt": "Pipeline harmless test note"}
        )
        reviewer = MockReviewerAgent(pass_on_round=1)

        job = self.orchestrator.submit_task(
            "Test harmless documentation task",
            task_type="docs",
            custom_impl=impl,
            custom_reviewer=reviewer
        )

        self.assertEqual(job.status, JobStatus.READY_FOR_APPROVAL)
        self.assertEqual(job.repair_attempts, 0)
        self.assertEqual(len(job.claude_executions), 1)
        self.assertEqual(len(job.reviews), 1)
        self.assertIsNotNone(job.candidate_fingerprint)
        self.assertIsNotNone(job.verification_results)
        self.assertTrue(job.verification_results.get("all_passed"))

    def test_repair_loop_success_on_round_two(self):
        # Reviewer fails on round 1, passes on round 2
        impl = MockImplementationAgent(should_succeed=True)
        reviewer = MockReviewerAgent(pass_on_round=2)

        job = self.orchestrator.submit_task(
            "Test task with 1 repair round",
            custom_impl=impl,
            custom_reviewer=reviewer
        )

        self.assertEqual(job.status, JobStatus.READY_FOR_APPROVAL)
        self.assertEqual(job.repair_attempts, 1)
        self.assertEqual(len(job.claude_executions), 2)  # initial + 1 repair
        self.assertEqual(len(job.reviews), 2)

    def test_repair_budget_exceeded(self):
        # Reviewer never passes
        impl = MockImplementationAgent(should_succeed=True)
        reviewer = MockReviewerAgent(
            pass_on_round=999,
            mock_findings=[
                ReviewFinding(
                    severity=FindingSeverity.BLOCKER,
                    file="src/cipher_core.cpp",
                    evidence="Unresolved issue",
                    recommended_fix="Refactor"
                )
            ]
        )

        job = self.orchestrator.submit_task(
            "Task that fails review repeatedly",
            custom_impl=impl,
            custom_reviewer=reviewer
        )

        self.assertEqual(job.status, JobStatus.FAILED_REVIEW)
        self.assertEqual(job.repair_attempts, MAX_REPAIR_ROUNDS)
        self.assertEqual(len(job.claude_executions), MAX_REPAIR_ROUNDS + 1)

    def test_implementation_failure(self):
        impl = MockImplementationAgent(should_succeed=False)
        reviewer = MockReviewerAgent(pass_on_round=1)

        job = self.orchestrator.submit_task(
            "Task where implementation errors out",
            custom_impl=impl,
            custom_reviewer=reviewer
        )

        self.assertEqual(job.status, JobStatus.FAILED_ERROR)
        self.assertEqual(len(job.reviews), 0)

    def test_git_safety_violation_in_pipeline(self):
        # Implementation attempts to create forbidden cipher.conf
        impl = MockImplementationAgent(
            should_succeed=True,
            files_to_create={"cipher.conf": "log_level=debug"}
        )
        reviewer = MockReviewerAgent(pass_on_round=1)

        job = self.orchestrator.submit_task(
            "Task touching forbidden file",
            custom_impl=impl,
            custom_reviewer=reviewer
        )

        self.assertEqual(job.status, JobStatus.FAILED_ERROR)
        self.assertIn("prohibited", job.error_message or "")

    def test_cancel_job(self):
        job = self.job_store.create_job("Job to cancel")
        cancelled = self.orchestrator.cancel_job(job.job_id)
        self.assertEqual(cancelled.status, JobStatus.CANCELLED)

    def test_missing_reviewer_cli_fails(self):
        from tools.pipeline.adapters.claude_reviewer_adapter import ClaudeReviewAdapter
        reviewer = ClaudeReviewAdapter(cli_path=Path("nonexistent_claude_cli.exe"))
        impl = MockImplementationAgent(should_succeed=True)

        job = self.orchestrator.submit_task(
            "Task with missing reviewer CLI",
            custom_impl=impl,
            custom_reviewer=reviewer
        )

        self.assertEqual(job.status, JobStatus.FAILED_REVIEW)
        self.assertGreater(len(job.reviews), 0)
        findings = job.reviews[0].get("findings", [])
        self.assertTrue(any(f.get("file") == "claude_reviewer" for f in findings))
        self.assertFalse(job.reviews[0].get("model_reviewed", True))

    def test_reviewer_crash_fails(self):
        from tools.pipeline.adapters.base import BaseReviewerAgent, ReviewResult
        class CrashingReviewer(BaseReviewerAgent):
            def review(self, worktree_path, git_diff, task_description, candidate_fingerprint=None):
                return ReviewResult(
                    passed=False,
                    blocking=True,
                    summary="Reviewer process crashed",
                    findings=[ReviewFinding(severity=FindingSeverity.BLOCKER, file="reviewer", evidence="exit code 1", recommended_fix="fix")],
                    model_reviewed=False,
                    candidate_fingerprint=candidate_fingerprint
                )

        impl = MockImplementationAgent(should_succeed=True)
        job = self.orchestrator.submit_task(
            "Task with crashing reviewer",
            custom_impl=impl,
            custom_reviewer=CrashingReviewer()
        )
        self.assertEqual(job.status, JobStatus.FAILED_REVIEW)

    def test_codex_is_never_invoked(self):
        import tools.pipeline
        import tools.pipeline.config as cfg
        self.assertFalse(hasattr(cfg, "CODEX_CLI"))
        self.assertFalse(hasattr(tools.pipeline, "CODEX_CLI"))

    def test_cancelled_job_does_not_resume(self):
        job = self.job_store.create_job("Job to cancel mid-flight")
        # Cancel the job first
        self.orchestrator.cancel_job(job.job_id)
        # Attempt to execute the job
        res = self.orchestrator.execute_job(job.job_id, use_mock=True)
        # Verify it halted immediately and stayed CANCELLED
        self.assertEqual(res.status, JobStatus.CANCELLED)
        self.assertEqual(len(res.claude_executions), 0)

    def test_agent_commit_diff_preservation(self):
        # Implementation agent creates a file AND runs git commit
        class CommittingAgent(MockImplementationAgent):
            def implement(self, task_description, worktree_path, repair_feedback=None):
                p = worktree_path / "docs" / "committed_file.md"
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text("Committed content", encoding="utf-8")
                # Run git add & commit inside worktree
                import subprocess
                subprocess.run(["git", "add", "."], cwd=str(worktree_path), capture_output=True)
                subprocess.run(
                    ["git", "-c", "user.name=Test", "-c", "user.email=test@test.com", "commit", "-m", "Agent commit"],
                    cwd=str(worktree_path),
                    capture_output=True
                )
                return super().implement(task_description, worktree_path, repair_feedback)

        impl = CommittingAgent(should_succeed=True)
        reviewer = MockReviewerAgent(pass_on_round=1)

        job = self.orchestrator.submit_task(
            "Task where agent creates a commit",
            custom_impl=impl,
            custom_reviewer=reviewer
        )

        self.assertEqual(job.status, JobStatus.READY_FOR_APPROVAL)
        # Verify that git_diff is NOT empty despite HEAD having advanced!
        self.assertIn("committed_file.md", job.git_diff)
        self.assertIn("Committed content", job.git_diff)

    def test_post_review_mutation_aborts_pipeline(self):
        """Mutating worktree during review triggers post-review candidate invariant violation."""
        from tools.pipeline.adapters.base import BaseReviewerAgent, ReviewResult
        class MutatingReviewer(BaseReviewerAgent):
            def review(self, worktree_path, git_diff, task_description, candidate_fingerprint=None, modified_files=None):
                # Illegally mutate a file in candidate worktree during review
                tampered = worktree_path / "docs" / "tampered.txt"
                tampered.parent.mkdir(parents=True, exist_ok=True)
                tampered.write_text("tampered content during review", encoding="utf-8")
                return ReviewResult(
                    passed=True,
                    blocking=False,
                    summary="Review claimed pass but secretly mutated worktree",
                    findings=[],
                    model_reviewed=True,
                    candidate_fingerprint=candidate_fingerprint
                )

        impl = MockImplementationAgent(should_succeed=True)
        job = self.orchestrator.submit_task(
            "Task with mutating reviewer",
            custom_impl=impl,
            custom_reviewer=MutatingReviewer()
        )

        self.assertEqual(job.status, JobStatus.FAILED_ERROR)
        self.assertIn("Candidate state mutated during review", job.error_message or "")

    def test_post_verification_mutation_aborts_pipeline(self):
        """Mutating worktree during verification triggers post-verification candidate invariant violation."""
        from unittest.mock import patch
        from tools.pipeline.verifier import Verifier

        orig_run_tests = Verifier.run_tests

        def mutating_run_tests(verifier_self, git_diff=""):
            # Illegally mutate worktree during test execution
            tampered = verifier_self.target_dir / "docs" / "tampered_verify.txt"
            tampered.parent.mkdir(parents=True, exist_ok=True)
            tampered.write_text("tampered during verify", encoding="utf-8")
            return orig_run_tests(verifier_self, git_diff=git_diff)

        impl = MockImplementationAgent(should_succeed=True)
        reviewer = MockReviewerAgent(pass_on_round=1)

        with patch.object(Verifier, "run_tests", side_effect=mutating_run_tests, autospec=True):
            job = self.orchestrator.submit_task(
                "Task with mutating verification",
                custom_impl=impl,
                custom_reviewer=reviewer
            )

        self.assertEqual(job.status, JobStatus.FAILED_ERROR)
        self.assertIn("Candidate state mutated during verification", job.error_message or "")

    def test_stale_approval_invalidation(self):
        """Worktree modification after reaching READY_FOR_APPROVAL invalidates approval and sets FAILED_ERROR."""
        impl = MockImplementationAgent(
            should_succeed=True,
            files_to_create={"docs/initial_doc.txt": "Clean content"}
        )
        reviewer = MockReviewerAgent(pass_on_round=1)

        job = self.orchestrator.submit_task(
            "Task to test approval invalidation",
            task_type="docs",
            custom_impl=impl,
            custom_reviewer=reviewer
        )

        self.assertEqual(job.status, JobStatus.READY_FOR_APPROVAL)
        verified_fp = job.candidate_fingerprint

        # Tamper worktree after approval was generated
        wt_path = Path(job.worktree_path)
        tamper_file = wt_path / "docs" / "initial_doc.txt"
        tamper_file.write_text("Tampered content after approval", encoding="utf-8")

        # Attempt to approve commit on stale candidate
        with self.assertRaises(RuntimeError) as ctx:
            self.orchestrator.approve_commit(job.job_id)

        self.assertIn("Candidate integrity check failed", str(ctx.exception))
        updated_job = self.job_store.get_job(job.job_id)
        self.assertEqual(updated_job.status, JobStatus.FAILED_ERROR)
        self.assertIn("Stale approval invalidated", updated_job.error_message or "")

    def test_repairs_trigger_fresh_review_and_fresh_verification(self):
        """A repair triggers both a fresh independent review and a fresh verification run."""
        impl = MockImplementationAgent(should_succeed=True)
        # Fails round 1, passes round 2
        reviewer = MockReviewerAgent(pass_on_round=2)

        job = self.orchestrator.submit_task(
            "Task testing repair fresh review and verification",
            custom_impl=impl,
            custom_reviewer=reviewer
        )

        self.assertEqual(job.status, JobStatus.READY_FOR_APPROVAL)
        self.assertEqual(job.repair_attempts, 1)

        # 1. Fresh review verified
        self.assertEqual(len(job.reviews), 2)
        self.assertFalse(job.reviews[0]["passed"])
        self.assertTrue(job.reviews[1]["passed"])
        self.assertEqual(job.reviews[1]["candidate_fingerprint"], job.candidate_fingerprint)

        # 2. Fresh verification verified
        self.assertIsNotNone(job.verification_results)
        self.assertTrue(job.verification_results["all_passed"])
        self.assertGreater(job.verification_results["total_assertions_checked"], 0)


if __name__ == "__main__":
    unittest.main()
