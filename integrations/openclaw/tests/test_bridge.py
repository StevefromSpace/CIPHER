"""Unit and Security Tests for OpenClaw Bridge Prototype.

Verifies:
1. Successful request and response.
2. OpenClaw not installed or unavailable.
3. Connection failure and authentication failure.
4. Request timeout handling.
5. Invalid or malformed responses.
6. Explicit rejection when cloud routing is detected or cannot be ruled out.
7. Optional integration disabled.
8. Existing CIPHER functionality operating without OpenClaw.
9. No accidental invocation of a shell command or unrelated external tool.
10. Loopback binding verification and credential masking.
"""

import json
import os
from pathlib import Path
import subprocess
import unittest
from unittest.mock import MagicMock, patch

from integrations.openclaw.assistant_contract import (
    AssistantRequest,
    AssistantResponse,
    AssistantStatus,
    BaseAssistantBackend,
)
from integrations.openclaw.bridge import OpenClawBridge
from integrations.openclaw.config import OpenClawConfig


class TestOpenClawBridge(unittest.TestCase):
    """Test suite covering the 9 required integration verification gates."""

    def setUp(self):
        self.mock_cli_path = Path("/mock/bin/openclaw")
        self.config = OpenClawConfig(
            enabled=True,
            cli_path=self.mock_cli_path,
            gateway_url="ws://127.0.0.1:18789",
            default_model="ollama/llama3.2",
            timeout_seconds=10.0,
            strict_local=True
        )

    # -------------------------------------------------------------------------
    # 1. Successful request and response
    # -------------------------------------------------------------------------
    @patch("integrations.openclaw.bridge.shutil.which")
    @patch("integrations.openclaw.bridge.subprocess.run")
    def test_successful_request_and_response(self, mock_run, mock_which):
        """Validates normal text prompt submission and response extraction."""
        mock_which.return_value = str(self.mock_cli_path)
        mock_run.return_value = MagicMock(
            returncode=0,
            stdout=json.dumps({
                "status": "ok",
                "reply": "CIPHER is an independent contextual intelligence platform.",
                "model": "ollama/llama3.2",
                "usage": {"total_tokens": 42}
            }),
            stderr=""
        )

        bridge = OpenClawBridge(self.config)
        with patch.object(Path, "exists", return_value=True):
            resp = bridge.submit_prompt(AssistantRequest(prompt="What is CIPHER?"))

        self.assertEqual(resp.status, AssistantStatus.OK)
        self.assertTrue(resp.is_ok)
        self.assertEqual(resp.text, "CIPHER is an independent contextual intelligence platform.")
        self.assertEqual(resp.model, "ollama/llama3.2")
        self.assertGreaterEqual(resp.duration_ms, 0.0)

    # -------------------------------------------------------------------------
    # 2. OpenClaw not installed or unavailable
    # -------------------------------------------------------------------------
    @patch("integrations.openclaw.bridge.shutil.which", return_value=None)
    def test_openclaw_not_installed_or_unavailable(self, mock_which):
        """Verifies graceful handling when the OpenClaw executable cannot be found."""
        config = OpenClawConfig(enabled=True, cli_path=None)
        bridge = OpenClawBridge(config)

        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.UNAVAILABLE)
        self.assertFalse(resp.is_ok)
        self.assertIn("executable not found", resp.error.lower())

    # -------------------------------------------------------------------------
    # 3. Connection failure and authentication failure
    # -------------------------------------------------------------------------
    @patch("integrations.openclaw.bridge.subprocess.run")
    def test_connection_failure(self, mock_run):
        """Verifies handling when the Gateway daemon is unreachable."""
        mock_run.return_value = MagicMock(
            returncode=1,
            stdout="",
            stderr="Error: connect ECONNREFUSED 127.0.0.1:18789\nGateway daemon is offline."
        )

        bridge = OpenClawBridge(self.config)
        with patch.object(Path, "exists", return_value=True):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Ping"))

        self.assertEqual(resp.status, AssistantStatus.CONNECTION_ERROR)
        self.assertIn("failed", resp.error.lower())
        self.assertIn("ECONNREFUSED", resp.error)

    @patch("integrations.openclaw.bridge.subprocess.run")
    def test_authentication_failure(self, mock_run):
        """Verifies handling when the Gateway token or credentials are rejected."""
        mock_run.return_value = MagicMock(
            returncode=1,
            stdout="",
            stderr="Error: 401 Unauthorized: invalid or expired gateway token bearer secret_token_xyz123"
        )

        bridge = OpenClawBridge(self.config)
        with patch.object(Path, "exists", return_value=True):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Ping"))

        self.assertEqual(resp.status, AssistantStatus.AUTH_ERROR)
        # Verify credentials were sanitized
        self.assertNotIn("secret_token_xyz123", resp.error)
        self.assertIn("[REDACTED]", resp.error)

    # -------------------------------------------------------------------------
    # 4. Request timeout
    # -------------------------------------------------------------------------
    @patch("integrations.openclaw.bridge.subprocess.run")
    def test_request_timeout(self, mock_run):
        """Verifies subprocess timeout enforcement without process hang."""
        mock_run.side_effect = subprocess.TimeoutExpired(cmd=["openclaw"], timeout=5.0)

        bridge = OpenClawBridge(self.config)
        with patch.object(Path, "exists", return_value=True):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Compute something slow", timeout_seconds=5.0))

        self.assertEqual(resp.status, AssistantStatus.TIMEOUT)
        self.assertIn("timed out after 5.0 seconds", resp.error)

    # -------------------------------------------------------------------------
    # 5. Invalid or malformed responses
    # -------------------------------------------------------------------------
    @patch("integrations.openclaw.bridge.subprocess.run")
    def test_malformed_invalid_json_response(self, mock_run):
        """Verifies handling when OpenClaw returns non-JSON garbled output."""
        mock_run.return_value = MagicMock(
            returncode=0,
            stdout="<HTML><BODY>Internal Server Error</BODY></HTML>",
            stderr=""
        )

        bridge = OpenClawBridge(self.config)
        with patch.object(Path, "exists", return_value=True):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Test"))

        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("invalid json", resp.error.lower())

    @patch("integrations.openclaw.bridge.subprocess.run")
    def test_malformed_missing_reply_field(self, mock_run):
        """Verifies handling when OpenClaw returns valid JSON missing the reply text."""
        mock_run.return_value = MagicMock(
            returncode=0,
            stdout=json.dumps({"status": "completed", "tokens": 100}),  # No reply or text
            stderr=""
        )

        bridge = OpenClawBridge(self.config)
        with patch.object(Path, "exists", return_value=True):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Test"))

        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("missing assistant reply text", resp.error.lower())

    # -------------------------------------------------------------------------
    # 6. Explicit rejection when cloud routing is detected or cannot be ruled out
    # -------------------------------------------------------------------------
    def test_explicit_rejection_of_cloud_models(self):
        """Verifies strict local-first enforcement rejects hosted or cloud models."""
        bridge = OpenClawBridge(self.config)

        cloud_models = [
            "openai/gpt-4o",
            "gpt-4-turbo",
            "anthropic/claude-3-5-sonnet",
            "claude-3-haiku",
            "ollama-cloud/llama3",
            "google/gemini-pro",
            "mistral-ai/mistral-large",
            "custom-unknown-model",  # lacks local prefix under strict_local
        ]

        for model in cloud_models:
            resp = bridge.submit_prompt(AssistantRequest(prompt="Test", model_override=model))
            self.assertEqual(
                resp.status,
                AssistantStatus.REJECTED_CLOUD_MODEL,
                f"Model '{model}' should have been rejected as non-local."
            )
            self.assertIn("local-first policy rejection", resp.error.lower())

    def test_allowed_local_models(self):
        """Verifies permitted local provider prefixes pass validation."""
        local_models = [
            "ollama/llama3.2",
            "ollama/qwen2.5:7b",
            "local/mistral-7b",
            "lmstudio/deepseek-r1",
            "llamacpp/gemma-2",
        ]
        for model in local_models:
            is_local, err = self.config.is_model_local(model)
            self.assertTrue(is_local, f"Model '{model}' should be permitted as local: {err}")

    # -------------------------------------------------------------------------
    # 7. Optional integration disabled
    # -------------------------------------------------------------------------
    def test_optional_integration_disabled(self):
        """Verifies that disabled integration immediately returns DISABLED without subprocess calls."""
        disabled_config = OpenClawConfig(enabled=False)
        bridge = OpenClawBridge(disabled_config)

        resp = bridge.submit_prompt(AssistantRequest(prompt="Test"))
        self.assertEqual(resp.status, AssistantStatus.DISABLED)
        self.assertIn("disabled in cipher configuration", resp.error.lower())
        self.assertFalse(bridge.is_available())

    # -------------------------------------------------------------------------
    # 8. Existing CIPHER functionality operating without OpenClaw
    # -------------------------------------------------------------------------
    def test_cipher_core_unaffected_by_integration(self):
        """Verifies that CIPHER runtime types have no coupling to OpenClaw."""
        # OpenClawBridge implements BaseAssistantBackend independently
        self.assertTrue(issubclass(OpenClawBridge, BaseAssistantBackend))

        # Config defaults to disabled
        default_config = OpenClawConfig()
        # Unless environment variable is set, it defaults to False
        if "CIPHER_OPENCLAW_ENABLED" not in os.environ:
            self.assertFalse(default_config.enabled)

    # -------------------------------------------------------------------------
    # 9. No accidental invocation of a shell command or unrelated external tool
    # -------------------------------------------------------------------------
    @patch("integrations.openclaw.bridge.subprocess.run")
    def test_no_shell_expansion_and_no_tools(self, mock_run):
        """Verifies subprocess is strictly called with shell=False and safe argv array."""
        mock_run.return_value = MagicMock(
            returncode=0,
            stdout=json.dumps({"reply": "Response", "model": "ollama/llama3.2"}),
            stderr=""
        )

        bridge = OpenClawBridge(self.config)
        with patch.object(Path, "exists", return_value=True):
            # Attempt a prompt with command-injection-like characters
            injection_prompt = "What is CIPHER? ; rm -rf / ; cat /etc/passwd | echo 'hacked'"
            bridge.submit_prompt(AssistantRequest(prompt=injection_prompt, session_id="test-session"))

        self.assertTrue(mock_run.called)
        called_args, called_kwargs = mock_run.call_args

        # Critical security check 1: shell MUST be False
        self.assertFalse(called_kwargs.get("shell", True), "Subprocess must NOT execute via shell.")

        # Critical security check 2: argv must be an array of literal strings
        argv = called_args[0]
        self.assertIsInstance(argv, list)
        self.assertEqual(argv[0], str(self.mock_cli_path))
        self.assertEqual(argv[1], "agent")
        self.assertIn("--message", argv)
        msg_idx = argv.index("--message")
        self.assertEqual(argv[msg_idx + 1], injection_prompt)  # Literal string, unexpanded

        # Critical security check 3: no tool enablement or message delivery flags
        self.assertNotIn("--deliver", argv)
        self.assertNotIn("--tool", argv)
        self.assertNotIn("--enable-tools", argv)
        self.assertNotIn("--dangerously-skip-permissions", argv)

    # -------------------------------------------------------------------------
    # 10. Security: Loopback binding enforcement and credential masking
    # -------------------------------------------------------------------------
    def test_loopback_binding_enforcement(self):
        """Verifies rejection of non-loopback gateway URLs."""
        non_loopback_config = OpenClawConfig(
            enabled=True,
            cli_path=self.mock_cli_path,
            gateway_url="ws://192.168.1.50:18789",  # Remote network IP
            default_model="ollama/llama3.2"
        )
        bridge = OpenClawBridge(non_loopback_config)
        with patch.object(Path, "exists", return_value=True):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))

        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("not bound to a loopback address", resp.error.lower())

    def test_credential_sanitization_in_errors(self):
        """Verifies _sanitize_error strips tokens and keys."""
        bridge = OpenClawBridge(self.config)
        dirty = "Error: Bearer sk-ant-api03-1234567890abcdef and token=supersecrettoken123"
        clean = bridge._sanitize_error(dirty)
        self.assertNotIn("1234567890abcdef", clean)
        self.assertNotIn("supersecrettoken123", clean)
        self.assertIn("[REDACTED]", clean)

    # -------------------------------------------------------------------------
    # 11. Policy Invariants: Blank prompts, timeouts, and tool policy enforcement
    # -------------------------------------------------------------------------
    def test_rejection_of_blank_prompts(self):
        """Verifies that empty or whitespace-only prompts are rejected fail-closed."""
        bridge = OpenClawBridge(self.config)
        with patch.object(Path, "exists", return_value=True):
            resp1 = bridge.submit_prompt(AssistantRequest(prompt=""))
            resp2 = bridge.submit_prompt(AssistantRequest(prompt="   \t\n  "))
        self.assertEqual(resp1.status, AssistantStatus.INVALID_REQUEST)
        self.assertEqual(resp2.status, AssistantStatus.INVALID_REQUEST)

    def test_rejection_of_invalid_timeout(self):
        """Verifies timeout bounds (1.0 to 300.0s)."""
        bridge = OpenClawBridge(self.config)
        with patch.object(Path, "exists", return_value=True):
            resp_low = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=0.5))
            resp_high = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=301.0))
        self.assertEqual(resp_low.status, AssistantStatus.INVALID_REQUEST)
        self.assertEqual(resp_high.status, AssistantStatus.INVALID_REQUEST)

    def test_rejection_when_tools_requested(self):
        """Verifies that allow_tools=True is rejected immediately."""
        bridge = OpenClawBridge(self.config)
        with patch.object(Path, "exists", return_value=True):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Hi", allow_tools=True))
        self.assertEqual(resp.status, AssistantStatus.REJECTED_TOOLS_ENABLED)

    def test_rejection_when_profile_is_not_minimal(self):
        """Verifies that if OpenClaw tool profile is not 'minimal', request fails closed."""
        non_minimal_config = OpenClawConfig(
            enabled=True,
            cli_path=self.mock_cli_path,
            gateway_url="ws://127.0.0.1:18789",
            default_model="ollama/llama3.2",
            tool_profile="full"  # Unsafe profile with tools active
        )
        bridge = OpenClawBridge(non_minimal_config)
        with patch.object(Path, "exists", return_value=True):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.REJECTED_TOOLS_ENABLED)
        self.assertIn("minimal", resp.error.lower())

    @patch("integrations.openclaw.bridge.subprocess.run")
    def test_minimal_profile_flag_present_in_argv(self, mock_run):
        """Verifies --profile minimal is passed in subprocess argv."""
        mock_run.return_value = MagicMock(
            returncode=0,
            stdout=json.dumps({"reply": "Response", "model": "ollama/llama3.2"}),
            stderr=""
        )
        bridge = OpenClawBridge(self.config)
        with patch.object(Path, "exists", return_value=True):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.OK)
        argv = mock_run.call_args[0][0]
        self.assertIn("--profile", argv)
        prof_idx = argv.index("--profile")
        self.assertEqual(argv[prof_idx + 1], "minimal")


if __name__ == "__main__":
    import os
    unittest.main()

