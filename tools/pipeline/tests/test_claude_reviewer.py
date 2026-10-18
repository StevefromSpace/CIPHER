"""Tests for Claude Code CLI independent reviewer adapter (Agent B)."""

import json
import subprocess
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch

from tools.pipeline.adapters.base import FindingSeverity, ReviewFinding, ReviewResult
from tools.pipeline.adapters.claude_reviewer_adapter import ClaudeReviewAdapter


class TestClaudeReviewAdapter(unittest.TestCase):
    """Unit tests for ClaudeReviewAdapter."""

    def setUp(self):
        self.dummy_cli = Path("/bin/dummy-claude")
        self.adapter = ClaudeReviewAdapter(
            cli_path=self.dummy_cli,
            allowed_tools="Read,Glob,Grep",
            disallowed_tools="Bash,PowerShell,Write,Edit",
            permission_mode="dontAsk",
            timeout=30,
            max_diff_chars=50_000
        )

    def test_missing_cli_returns_blocking_failure(self):
        """Missing CLI executable immediately fails with BLOCKER and model_reviewed=False."""
        res = self.adapter.review(
            worktree_path=Path("/tmp/worktree"),
            git_diff="dummy diff",
            task_description="test task",
            candidate_fingerprint="abc123hash"
        )
        self.assertFalse(res.passed)
        self.assertTrue(res.blocking)
        self.assertFalse(res.model_reviewed)
        self.assertEqual(res.candidate_fingerprint, "abc123hash")
        self.assertTrue(any(f.severity == FindingSeverity.BLOCKER for f in res.findings))

    @patch("pathlib.Path.exists", return_value=True)
    @patch("subprocess.run")
    def test_successful_clean_review(self, mock_run, mock_exists):
        """A review returning valid schema with empty findings passes cleanly."""
        mock_output = {
            "result": json.dumps({
                "summary": "All checks passed cleanly.",
                "findings": []
            })
        }
        mock_run.return_value = MagicMock(
            returncode=0,
            stdout=json.dumps(mock_output),
            stderr=""
        )

        res = self.adapter.review(
            worktree_path=Path("/tmp/worktree"),
            git_diff="dummy diff",
            task_description="implement something",
            candidate_fingerprint="fingerprint_test"
        )

        self.assertTrue(res.passed)
        self.assertFalse(res.blocking)
        self.assertTrue(res.model_reviewed)
        self.assertEqual(len(res.findings), 0)
        self.assertEqual(res.candidate_fingerprint, "fingerprint_test")

        # Verify command-line arguments enforce read-only tool confinement
        call_args = mock_run.call_args[0][0]
        self.assertIn("--tools", call_args)
        tools_idx = call_args.index("--tools")
        self.assertEqual(call_args[tools_idx + 1], "Read,Glob,Grep")

        self.assertIn("--disallowed-tools", call_args)
        disallowed_idx = call_args.index("--disallowed-tools")
        self.assertEqual(call_args[disallowed_idx + 1], "Bash,PowerShell,Write,Edit")

        self.assertIn("--strict-mcp-config", call_args)
        self.assertIn("--permission-mode", call_args)

    @patch("pathlib.Path.exists", return_value=True)
    @patch("subprocess.run")
    def test_review_with_blocker_findings(self, mock_run, mock_exists):
        """A review returning BLOCKER findings fails and blocks."""
        review_data = {
            "summary": "Found invariant violation.",
            "findings": [
                {
                    "severity": "BLOCKER",
                    "file": "cipher_core/include/core.h",
                    "loc": "line 12",
                    "evidence": "Illegal include of cipher_config.h violating decoupling rules.",
                    "recommended_fix": "Remove include and use interface abstraction."
                },
                {
                    "severity": "WARNING",
                    "file": "CMakeLists.txt",
                    "loc": "line 40",
                    "evidence": "Commented out debug flag.",
                    "recommended_fix": "Clean up dead comment."
                }
            ]
        }
        mock_run.return_value = MagicMock(
            returncode=0,
            stdout=json.dumps({"result": f"```json\n{json.dumps(review_data)}\n```"}),
            stderr=""
        )

        res = self.adapter.review(
            worktree_path=Path("/tmp/worktree"),
            git_diff="bad diff",
            task_description="bad task",
            candidate_fingerprint="fp1"
        )

        self.assertFalse(res.passed)
        self.assertTrue(res.blocking)
        self.assertTrue(res.model_reviewed)
        self.assertEqual(len(res.findings), 2)
        self.assertEqual(res.findings[0].severity, FindingSeverity.BLOCKER)
        self.assertEqual(res.findings[1].severity, FindingSeverity.WARNING)

    @patch("pathlib.Path.exists", return_value=True)
    @patch("subprocess.run")
    def test_reviewer_crash_non_zero_exit(self, mock_run, mock_exists):
        """Non-zero exit code results in a blocking failure without model_reviewed=True."""
        mock_run.return_value = MagicMock(
            returncode=1,
            stdout="",
            stderr="Fatal error: API rate limit or auth failure"
        )

        res = self.adapter.review(
            worktree_path=Path("/tmp/worktree"),
            git_diff="diff",
            task_description="task",
            candidate_fingerprint="fp2"
        )

        self.assertFalse(res.passed)
        self.assertTrue(res.blocking)
        self.assertFalse(res.model_reviewed)
        self.assertTrue(any(f.severity == FindingSeverity.BLOCKER for f in res.findings))
        self.assertIn("Review FAILED: Reviewer process crashed with code 1", res.summary)

    @patch("pathlib.Path.exists", return_value=True)
    @patch("subprocess.run")
    def test_empty_reviewer_response_fails(self, mock_run, mock_exists):
        """Empty or blank response from Claude Code fails with BLOCKER."""
        mock_run.return_value = MagicMock(
            returncode=0,
            stdout="   \n  ",
            stderr=""
        )

        res = self.adapter.review(
            worktree_path=Path("/tmp/worktree"),
            git_diff="diff",
            task_description="task"
        )

        self.assertFalse(res.passed)
        self.assertTrue(res.blocking)
        self.assertFalse(res.model_reviewed)
        self.assertIn("Empty response", res.summary)

    @patch("pathlib.Path.exists", return_value=True)
    @patch("subprocess.run")
    def test_timeout_fails_with_blocker(self, mock_run, mock_exists):
        """TimeoutExpired results in a blocking failure."""
        mock_run.side_effect = subprocess.TimeoutExpired(cmd=["claude"], timeout=30)

        res = self.adapter.review(
            worktree_path=Path("/tmp/worktree"),
            git_diff="diff",
            task_description="task"
        )

        self.assertFalse(res.passed)
        self.assertTrue(res.blocking)
        self.assertFalse(res.model_reviewed)
        self.assertIn("timed out after 30s", res.summary)

    @patch("pathlib.Path.exists", return_value=True)
    @patch("subprocess.run")
    def test_arbitrary_prose_blocks_job(self, mock_run, mock_exists):
        """Arbitrary prose without required JSON schema blocks the job."""
        prose_output = "I have reviewed the code. Everything looks great to me! No problems found."
        mock_run.return_value = MagicMock(
            returncode=0,
            stdout=json.dumps({"result": prose_output}),
            stderr=""
        )

        res = self.adapter.review(
            worktree_path=Path("/tmp/worktree"),
            git_diff="diff",
            task_description="task"
        )

        self.assertFalse(res.passed)
        self.assertTrue(res.blocking)
        self.assertFalse(res.model_reviewed)
        self.assertTrue(any(f.severity == FindingSeverity.BLOCKER for f in res.findings))
        self.assertIn("Malformed output", res.summary)

    @patch("pathlib.Path.exists", return_value=True)
    @patch("subprocess.run")
    def test_invalid_json_syntax_blocks_job(self, mock_run, mock_exists):
        """Broken JSON syntax blocks the job with a schema violation BLOCKER."""
        bad_json = "```json\n{ summary: 'unquoted key', findings: [ } \n```"
        mock_run.return_value = MagicMock(
            returncode=0,
            stdout=json.dumps({"result": bad_json}),
            stderr=""
        )

        res = self.adapter.review(
            worktree_path=Path("/tmp/worktree"),
            git_diff="diff",
            task_description="task"
        )

        self.assertFalse(res.passed)
        self.assertTrue(res.blocking)
        self.assertFalse(res.model_reviewed)
        self.assertTrue(any(f.severity == FindingSeverity.BLOCKER for f in res.findings))
        self.assertIn("Invalid JSON syntax", res.summary)

    @patch("pathlib.Path.exists", return_value=True)
    @patch("subprocess.run")
    def test_missing_summary_blocks_job(self, mock_run, mock_exists):
        """Missing summary field blocks the job."""
        missing_summary = json.dumps({"findings": []})
        mock_run.return_value = MagicMock(
            returncode=0,
            stdout=json.dumps({"result": missing_summary}),
            stderr=""
        )

        res = self.adapter.review(
            worktree_path=Path("/tmp/worktree"),
            git_diff="diff",
            task_description="task"
        )

        self.assertFalse(res.passed)
        self.assertTrue(res.blocking)
        self.assertFalse(res.model_reviewed)
        self.assertTrue(any(f.severity == FindingSeverity.BLOCKER for f in res.findings))

    @patch("pathlib.Path.exists", return_value=True)
    @patch("subprocess.run")
    def test_missing_finding_fields_blocks_job(self, mock_run, mock_exists):
        """Finding item missing required fields blocks the job."""
        incomplete_finding = {
            "summary": "Review assessment",
            "findings": [
                {
                    "severity": "BLOCKER",
                    "file": "src/main.cpp"
                    # missing evidence and recommended_fix!
                }
            ]
        }
        mock_run.return_value = MagicMock(
            returncode=0,
            stdout=json.dumps({"result": json.dumps(incomplete_finding)}),
            stderr=""
        )

        res = self.adapter.review(
            worktree_path=Path("/tmp/worktree"),
            git_diff="diff",
            task_description="task"
        )

        self.assertFalse(res.passed)
        self.assertTrue(res.blocking)
        self.assertFalse(res.model_reviewed)
        self.assertTrue(any(f.severity == FindingSeverity.BLOCKER for f in res.findings))

    @patch("pathlib.Path.exists", return_value=True)
    @patch("subprocess.run")
    def test_invalid_severity_blocks_job(self, mock_run, mock_exists):
        """Unrecognized severity string blocks the job."""
        bad_severity = {
            "summary": "Review assessment",
            "findings": [
                {
                    "severity": "CRITICAL",  # Not in {"BLOCKER", "WARNING", "INFO"}
                    "file": "src/main.cpp",
                    "evidence": "Some issue",
                    "recommended_fix": "Fix it"
                }
            ]
        }
        mock_run.return_value = MagicMock(
            returncode=0,
            stdout=json.dumps({"result": json.dumps(bad_severity)}),
            stderr=""
        )

        res = self.adapter.review(
            worktree_path=Path("/tmp/worktree"),
            git_diff="diff",
            task_description="task"
        )

        self.assertFalse(res.passed)
        self.assertTrue(res.blocking)
        self.assertFalse(res.model_reviewed)
        self.assertTrue(any("invalid severity" in f.evidence for f in res.findings))

    @patch("pathlib.Path.exists", return_value=True)
    @patch("subprocess.run")
    def test_incorrect_empty_findings_blocks_job(self, mock_run, mock_exists):
        """Non-list findings representations (null, string, dict) block the job."""
        for invalid_findings in [None, "none", {}, "[]"]:
            mock_run.return_value = MagicMock(
                returncode=0,
                stdout=json.dumps({"result": json.dumps({"summary": "Valid summary", "findings": invalid_findings})}),
                stderr=""
            )

            res = self.adapter.review(
                worktree_path=Path("/tmp/worktree"),
                git_diff="diff",
                task_description="task"
            )

            self.assertFalse(res.passed, f"Failed to reject invalid findings type: {type(invalid_findings)}")
            self.assertTrue(res.blocking)
            self.assertFalse(res.model_reviewed)

    def test_diff_exceeding_limit_fails_closed(self):
        """Diff exceeding max_diff_chars immediately fails closed without silent truncation."""
        adapter = ClaudeReviewAdapter(cli_path=Path("dummy"), max_diff_chars=100)
        huge_diff = "diff --git a/foo.cpp b/foo.cpp\n" + ("+" + "x" * 80 + "\n") * 5

        res = adapter.review(
            worktree_path=Path("/tmp/worktree"),
            git_diff=huge_diff,
            task_description="huge diff task"
        )

        self.assertFalse(res.passed)
        self.assertTrue(res.blocking)
        self.assertFalse(res.model_reviewed)
        self.assertIn("exceeds limit", res.summary)
        self.assertTrue(any("exceeds maximum review limit" in f.evidence for f in res.findings))

    @patch("pathlib.Path.exists", return_value=True)
    @patch("subprocess.run")
    def test_diff_not_truncated_within_limit(self, mock_run, mock_exists):
        """Diffs within limit are not sliced (preserves full diff past 12000 chars)."""
        adapter = ClaudeReviewAdapter(cli_path=Path("dummy"), max_diff_chars=50_000)
        diff_15k = "diff --git a/foo.cpp b/foo.cpp\n" + ("+ line of code\n" * 900)
        self.assertGreater(len(diff_15k), 12000)

        mock_output = {
            "result": json.dumps({
                "summary": "Full diff reviewed cleanly.",
                "findings": []
            })
        }
        mock_run.return_value = MagicMock(
            returncode=0,
            stdout=json.dumps(mock_output),
            stderr=""
        )

        res = adapter.review(
            worktree_path=Path("/tmp/worktree"),
            git_diff=diff_15k,
            task_description="large complete diff task",
            modified_files=["foo.cpp"]
        )

        self.assertTrue(res.passed)
        # Verify the prompt sent to subprocess contains the complete diff without truncation
        sent_prompt = mock_run.call_args[1].get("input", "")
        if not sent_prompt:
            call_args = mock_run.call_args[0][0]
            prompt_idx = call_args.index("-p")
            if prompt_idx + 1 < len(call_args):
                sent_prompt = call_args[prompt_idx + 1]
        self.assertIn(diff_15k, sent_prompt)
        self.assertIn("foo.cpp", sent_prompt)


if __name__ == "__main__":
    unittest.main()
