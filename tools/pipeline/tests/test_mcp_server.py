"""Unit tests for the MCP Server tools and human approval boundary."""

import json
import unittest
from unittest.mock import patch

from tools.pipeline.confirm_commit import confirm_pipeline_commit
from tools.pipeline.server import (
    cancel_pipeline_task,
    get_pipeline_result,
    get_pipeline_status,
    mcp,
    submit_pipeline_task,
)


class TestMCPServer(unittest.TestCase):
    def setUp(self):
        import os
        self._orig_mock = os.environ.get("CIPHER_PIPELINE_ALLOW_MOCK")
        self._orig_test = os.environ.get("CIPHER_TEST_MODE")
        os.environ["CIPHER_PIPELINE_ALLOW_MOCK"] = "1"
        os.environ["CIPHER_TEST_MODE"] = "1"

    def tearDown(self):
        import os
        if self._orig_mock is not None:
            os.environ["CIPHER_PIPELINE_ALLOW_MOCK"] = self._orig_mock
        else:
            os.environ.pop("CIPHER_PIPELINE_ALLOW_MOCK", None)
        if self._orig_test is not None:
            os.environ["CIPHER_TEST_MODE"] = self._orig_test
        else:
            os.environ.pop("CIPHER_TEST_MODE", None)

    def test_use_mock_disallowed_without_env_flag(self):
        import os
        os.environ.pop("CIPHER_PIPELINE_ALLOW_MOCK", None)
        res_json = submit_pipeline_task("test", use_mock=True)
        res = json.loads(res_json)
        self.assertFalse(res.get("success"))
        self.assertIn("use_mock is disallowed in production", res.get("error"))

    def test_mcp_server_does_not_expose_commit_approval(self):
        """Verify that Gemini cannot self-authorize commits via MCP tools."""
        import tools.pipeline.server as srv
        self.assertFalse(
            hasattr(srv, "approve_pipeline_commit"),
            "approve_pipeline_commit must NOT be exposed as an MCP tool to AI agents"
        )

    def test_submit_status_and_owner_confirmation(self):
        res_json = submit_pipeline_task(
            task_description="Harmless mock pipeline test",
            task_type="docs",
            use_mock=True,
            run_async=False
        )
        res = json.loads(res_json)
        self.assertTrue(res.get("success"))
        job_id = res.get("job_id")
        self.assertIsNotNone(job_id)

        # Check status
        status_json = get_pipeline_status(job_id)
        status_data = json.loads(status_json)
        self.assertEqual(status_data.get("job_id"), job_id)
        self.assertEqual(status_data.get("status"), "READY_FOR_APPROVAL")

        # Check result
        result_json = get_pipeline_result(job_id)
        result_data = json.loads(result_json)
        self.assertEqual(result_data.get("job_id"), job_id)
        self.assertIn("verification_results", result_data)

        # Test owner aborts confirmation
        with patch("builtins.input", return_value="n"):
            aborted = confirm_pipeline_commit(job_id, auto_confirm=False)
            self.assertFalse(aborted, "Owner answering 'n' must abort commit authorization")

        # Test owner approves confirmation
        with patch("builtins.input", return_value="y"):
            approved = confirm_pipeline_commit(job_id, commit_message="Owner verified commit", auto_confirm=False)
            self.assertTrue(approved, "Owner answering 'y' must approve local commit")

        # Check final status in job store
        status_after = json.loads(get_pipeline_status(job_id))
        self.assertEqual(status_after.get("status"), "COMMITTED")

    def test_cancel_nonexistent_and_existent_job(self):
        # Nonexistent job
        err_json = cancel_pipeline_task("nonexistent-uuid")
        self.assertIn("error", json.loads(err_json))


if __name__ == "__main__":
    unittest.main()
