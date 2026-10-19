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
11. Tool policy invariants and fail-closed denial.
12. Isolated configuration and state environment isolation.
13. Subprocess bounded stream collection, termination on limit breach, and timeout kill.
"""

import json
import os
from pathlib import Path
import subprocess
import sys
import time
import unittest
from unittest.mock import MagicMock, patch

from integrations.contract import (
    AssistantRequest,
    AssistantResponse,
    AssistantStatus,
    BaseAssistantBackend,
    ProcessingRoute,
)
from integrations.openclaw.bridge import OpenClawBridge
from integrations.openclaw.config import OpenClawConfig


class TestOpenClawBridge(unittest.TestCase):
    """Test suite covering the required integration verification gates."""

    def setUp(self):
        self.mock_cli_path = Path("/mock/bin/openclaw")
        self.config = OpenClawConfig(
            enabled=True,
            cli_path=self.mock_cli_path,
            gateway_url="ws://127.0.0.1:18789",
            default_model="ollama/llama3.2",
            timeout_seconds=10.0,
            strict_local=True,
            verified_local=True,
            enable_live_execution=True,
        )

    # -------------------------------------------------------------------------
    # 1. Successful request and response
    # -------------------------------------------------------------------------
    @patch("integrations.openclaw.bridge.shutil.which")
    def test_successful_request_and_response(self, mock_which):
        """Validates normal text prompt submission and response extraction."""
        mock_which.return_value = str(self.mock_cli_path)
        bridge = OpenClawBridge(self.config)

        mock_payload = json.dumps({
            "status": "ok",
            "reply": "CIPHER is an independent contextual intelligence platform.",
            "model": "ollama/llama3.2",
            "usage": {"total_tokens": 42}
        }).encode("utf-8")

        with patch.object(Path, "exists", return_value=True), \
             patch.object(bridge, "_verify_empty_tool_inventory", return_value=(True, None)), \
             patch.object(bridge, "_execute_bounded_process", return_value=(0, mock_payload, b"", None)):
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
        config = OpenClawConfig(enabled=True, cli_path=None, verified_local=True, enable_live_execution=True)
        bridge = OpenClawBridge(config)

        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.UNAVAILABLE)
        self.assertFalse(resp.is_ok)
        self.assertIn("executable not found", resp.error.lower())

    # -------------------------------------------------------------------------
    # 3. Connection failure and authentication failure
    # -------------------------------------------------------------------------
    def test_connection_failure(self):
        """Verifies handling when the Gateway daemon is unreachable."""
        bridge = OpenClawBridge(self.config)
        stderr_text = "Error: connect ECONNREFUSED 127.0.0.1:18789\nGateway daemon is offline.".encode("utf-8")

        with patch.object(Path, "exists", return_value=True), \
             patch.object(bridge, "_verify_empty_tool_inventory", return_value=(True, None)), \
             patch.object(bridge, "_execute_bounded_process", return_value=(1, b"", stderr_text, None)):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Ping"))

        self.assertEqual(resp.status, AssistantStatus.CONNECTION_ERROR)
        self.assertIn("failed", resp.error.lower())
        self.assertIn("ECONNREFUSED", resp.error)

    def test_authentication_failure(self):
        """Verifies handling when the Gateway token or credentials are rejected."""
        bridge = OpenClawBridge(self.config)
        stderr_text = "Error: 401 Unauthorized: invalid or expired gateway token bearer secret_token_xyz123".encode("utf-8")

        with patch.object(Path, "exists", return_value=True), \
             patch.object(bridge, "_verify_empty_tool_inventory", return_value=(True, None)), \
             patch.object(bridge, "_execute_bounded_process", return_value=(1, b"", stderr_text, None)):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Ping"))

        self.assertEqual(resp.status, AssistantStatus.AUTH_ERROR)
        self.assertNotIn("secret_token_xyz123", resp.error)
        self.assertIn("[REDACTED]", resp.error)

    # -------------------------------------------------------------------------
    # 4. Request timeout
    # -------------------------------------------------------------------------
    def test_request_timeout(self):
        """Verifies subprocess timeout enforcement without process hang."""
        bridge = OpenClawBridge(self.config)
        with patch.object(Path, "exists", return_value=True), \
             patch.object(bridge, "_verify_empty_tool_inventory", return_value=(True, None)), \
             patch.object(bridge, "_execute_bounded_process", return_value=(-1, b"", b"", "OpenClaw execution timed out after 5.0s.")):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Compute something slow", timeout_seconds=5.0))

        self.assertEqual(resp.status, AssistantStatus.TIMEOUT)
        self.assertIn("timed out after 5.0s", resp.error)

    # -------------------------------------------------------------------------
    # 5. Invalid or malformed responses
    # -------------------------------------------------------------------------
    def test_malformed_invalid_json_response(self):
        """Verifies handling when OpenClaw returns non-JSON garbled output."""
        bridge = OpenClawBridge(self.config)
        with patch.object(Path, "exists", return_value=True), \
             patch.object(bridge, "_verify_empty_tool_inventory", return_value=(True, None)), \
             patch.object(bridge, "_execute_bounded_process", return_value=(0, b"<HTML><BODY>Internal Server Error</BODY></HTML>", b"", None)):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Test"))

        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("invalid json", resp.error.lower())

    def test_malformed_missing_reply_field(self):
        """Verifies handling when OpenClaw returns valid JSON missing the reply text."""
        bridge = OpenClawBridge(self.config)
        no_reply_payload = json.dumps({"status": "completed", "tokens": 100}).encode("utf-8")
        with patch.object(Path, "exists", return_value=True), \
             patch.object(bridge, "_verify_empty_tool_inventory", return_value=(True, None)), \
             patch.object(bridge, "_execute_bounded_process", return_value=(0, no_reply_payload, b"", None)):
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
            "custom-unknown-model",
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
        self.assertTrue(issubclass(OpenClawBridge, BaseAssistantBackend))
        default_config = OpenClawConfig()
        if "CIPHER_OPENCLAW_ENABLED" not in os.environ:
            self.assertFalse(default_config.enabled)

    # -------------------------------------------------------------------------
    # 9. No accidental invocation of a shell command or unrelated external tool
    # -------------------------------------------------------------------------
    def test_no_shell_expansion_and_safe_arguments(self):
        """Verifies argv array contains literal arguments without shell expansion."""
        bridge = OpenClawBridge(self.config)
        captured_cmd = []

        def mock_exec(cmd, env, timeout, max_bytes):
            captured_cmd.extend(cmd)
            return 0, json.dumps({"reply": "Response", "model": "ollama/llama3.2"}).encode("utf-8"), b"", None

        with patch.object(Path, "exists", return_value=True), \
             patch.object(bridge, "_verify_empty_tool_inventory", return_value=(True, None)), \
             patch.object(bridge, "_execute_bounded_process", side_effect=mock_exec):
            injection_prompt = "What is CIPHER? ; rm -rf / ; cat /etc/passwd | echo 'hacked'"
            resp = bridge.submit_prompt(AssistantRequest(prompt=injection_prompt, session_id="test-session"))

        self.assertEqual(resp.status, AssistantStatus.OK)
        self.assertEqual(captured_cmd[0], str(self.mock_cli_path))
        self.assertEqual(captured_cmd[1], "agent")
        self.assertIn("--message", captured_cmd)
        msg_idx = captured_cmd.index("--message")
        self.assertEqual(captured_cmd[msg_idx + 1], injection_prompt)
        self.assertIn("--session-id", captured_cmd)
        self.assertIn("--profile", captured_cmd)
        self.assertEqual(captured_cmd[captured_cmd.index("--profile") + 1], "minimal")

        # Crucial security checks: no tool enablement or delivery flags
        self.assertNotIn("--deliver", captured_cmd)
        self.assertNotIn("--tool", captured_cmd)
        self.assertNotIn("--enable-tools", captured_cmd)
        self.assertNotIn("--dangerously-skip-permissions", captured_cmd)
        self.assertNotIn("--deny-tools", captured_cmd)  # Verified not a real Commander CLI option

    def test_agent_exec_command_mode(self):
        """Verifies embedded execution mode builds 'openclaw agent exec' command."""
        exec_cfg = OpenClawConfig(
            enabled=True,
            cli_path=self.mock_cli_path,
            gateway_url="ws://127.0.0.1:18789",
            default_model="ollama/llama3.2",
            execution_mode="agent-exec",
            enable_live_execution=True,
            verified_local=True,
        )
        bridge = OpenClawBridge(exec_cfg)
        captured_cmd = []

        def mock_exec(cmd, env, timeout, max_bytes):
            captured_cmd.extend(cmd)
            return 0, json.dumps({"reply": "Exec ok", "model": "ollama/llama3.2"}).encode("utf-8"), b"", None

        with patch.object(Path, "exists", return_value=True), \
             patch.object(bridge, "_verify_empty_tool_inventory", return_value=(True, None)), \
             patch.object(bridge, "_execute_bounded_process", side_effect=mock_exec):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Test exec"))

        self.assertEqual(resp.status, AssistantStatus.OK)
        self.assertEqual(captured_cmd[0], str(self.mock_cli_path))
        self.assertEqual(captured_cmd[1], "agent")
        self.assertEqual(captured_cmd[2], "exec")


    # -------------------------------------------------------------------------
    # 10. Security: Loopback binding enforcement and credential masking
    # -------------------------------------------------------------------------
    def test_loopback_binding_enforcement(self):
        """Verifies rejection of non-loopback gateway URLs."""
        non_loopback_config = OpenClawConfig(
            enabled=True,
            cli_path=self.mock_cli_path,
            gateway_url="ws://192.168.1.50:18789",
            default_model="ollama/llama3.2",
            verified_local=True,
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
            tool_profile="full",
            verified_local=True,
        )
        bridge = OpenClawBridge(non_minimal_config)
        with patch.object(Path, "exists", return_value=True):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.REJECTED_TOOLS_ENABLED)
        self.assertIn("minimal", resp.error.lower())

    def test_rejection_of_non_finite_timeout(self):
        """Verifies rejection of NaN and Inf timeout values."""
        bridge = OpenClawBridge(self.config)
        with patch.object(Path, "exists", return_value=True):
            resp_nan = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=float("nan")))
            resp_inf = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=float("inf")))
            resp_ninf = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=float("-inf")))
        self.assertEqual(resp_nan.status, AssistantStatus.INVALID_REQUEST)
        self.assertEqual(resp_inf.status, AssistantStatus.INVALID_REQUEST)
        self.assertEqual(resp_ninf.status, AssistantStatus.INVALID_REQUEST)

    def test_spoofed_loopback_url_rejection(self):
        """Verifies that parsed hostname prevents spoofed loopback addresses."""
        spoofed_cfg = OpenClawConfig(
            enabled=True,
            cli_path=self.mock_cli_path,
            gateway_url="ws://127.0.0.1.attacker.com:18789",
            default_model="ollama/llama3.2",
            verified_local=True,
        )
        bridge = OpenClawBridge(spoofed_cfg)
        with patch.object(Path, "exists", return_value=True):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("not bound to a loopback address", resp.error.lower())

    # -------------------------------------------------------------------------
    # 12. Security Hardening: Tool denial, isolated config, and route provenance
    # -------------------------------------------------------------------------
    def test_unverified_local_route_rejected_under_require_local(self):
        """Verifies default unverified route fails closed when require_local is True."""
        unverified_cfg = OpenClawConfig(
            enabled=True,
            cli_path=self.mock_cli_path,
            gateway_url="ws://127.0.0.1:18789",
            default_model="ollama/llama3.2",
            verified_local=False,
        )
        bridge = OpenClawBridge(unverified_cfg)
        self.assertEqual(bridge.capabilities.default_route, ProcessingRoute.UNKNOWN)
        with patch.object(Path, "exists", return_value=True):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Hello", require_local=True))
        self.assertEqual(resp.status, AssistantStatus.REJECTED_CLOUD_MODEL)
        self.assertIn("unknown", resp.error.lower())

    def test_explicit_tool_denial_required(self):
        """Verifies config with explicit_tool_denial=False fails closed."""
        cfg = OpenClawConfig(
            enabled=True,
            cli_path=self.mock_cli_path,
            gateway_url="ws://127.0.0.1:18789",
            default_model="ollama/llama3.2",
            explicit_tool_denial=False,
            verified_local=True,
        )
        bridge = OpenClawBridge(cfg)
        with patch.object(Path, "exists", return_value=True):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.REJECTED_TOOLS_ENABLED)
        self.assertIn("explicit tool denial", resp.error.lower())

    def test_tool_deny_pattern_wildcard_required(self):
        """Verifies config without '*' in tool_deny_patterns fails closed."""
        cfg = OpenClawConfig(
            enabled=True,
            cli_path=self.mock_cli_path,
            gateway_url="ws://127.0.0.1:18789",
            default_model="ollama/llama3.2",
            tool_deny_patterns=("some_tool",),
            verified_local=True,
        )
        bridge = OpenClawBridge(cfg)
        with patch.object(Path, "exists", return_value=True):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.REJECTED_TOOLS_ENABLED)
        self.assertIn("explicit tool denial with '*' pattern is required", resp.error.lower())

    def test_enforce_isolated_config_required(self):
        """Verifies config with enforce_isolated_config=False fails closed."""
        cfg = OpenClawConfig(
            enabled=True,
            cli_path=self.mock_cli_path,
            gateway_url="ws://127.0.0.1:18789",
            default_model="ollama/llama3.2",
            enforce_isolated_config=False,
            verified_local=True,
        )
        bridge = OpenClawBridge(cfg)
        with patch.object(Path, "exists", return_value=True):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.REJECTED_TOOLS_ENABLED)
        self.assertIn("isolated configuration", resp.error.lower())

    def test_isolated_config_and_environment_variables(self):
        """Verifies that submit_prompt writes isolated config and binds environment variables."""
        bridge = OpenClawBridge(self.config)
        verified_conf = {}
        verified_env = {}

        def mock_exec(cmd, env, timeout, max_bytes):
            verified_env.update(env)
            conf_file = env.get("OPENCLAW_CONFIG_PATH")
            if conf_file and Path(conf_file).exists():
                with open(conf_file, "r", encoding="utf-8") as f:
                    verified_conf.update(json.load(f))
            return 0, json.dumps({"reply": "isolated ok"}).encode("utf-8"), b"", None

        with patch.object(Path, "exists", return_value=True), \
             patch.object(bridge, "_verify_empty_tool_inventory", return_value=(True, None)), \
             patch.object(bridge, "_execute_bounded_process", side_effect=mock_exec):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Check isolation"))

        self.assertEqual(resp.status, AssistantStatus.OK)
        # Verify environment variables were set
        self.assertIn("OPENCLAW_CONFIG_PATH", verified_env)
        self.assertIn("OPENCLAW_STATE_DIR", verified_env)
        self.assertEqual(verified_env["OPENCLAW_GATEWAY_URL"], "ws://127.0.0.1:18789")
        self.assertEqual(verified_env["OPENCLAW_PROFILE"], "minimal")

        # Verify isolated config contents strictly conform to upstream schema (tools only, no invalid gateway key)
        self.assertNotIn("gateway", verified_conf)
        self.assertEqual(verified_conf.get("tools", {}).get("profile"), "minimal")
        self.assertEqual(verified_conf.get("tools", {}).get("deny"), ["*"])

    def test_gateway_url_userinfo_rejected(self):
        """Verifies that gateway URLs containing credentials fail closed."""
        cred_cfg = OpenClawConfig(
            enabled=True,
            cli_path=self.mock_cli_path,
            gateway_url="ws://user:secret@127.0.0.1:18789",
            default_model="ollama/llama3.2",
            verified_local=True,
            enable_live_execution=True,
        )
        bridge = OpenClawBridge(cred_cfg)
        with patch.object(Path, "exists", return_value=True):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("user credentials", resp.error.lower())

    def test_live_execution_disabled_by_default(self):
        """Verifies that OpenClaw execution is disabled fail-closed by default under safety policy."""
        default_cfg = OpenClawConfig(enabled=True, cli_path=self.mock_cli_path, verified_local=True)
        self.assertFalse(default_cfg.enable_live_execution)
        bridge = OpenClawBridge(default_cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.UNAVAILABLE)
        self.assertIn("live execution is disabled under cipher safety policy", resp.error.lower())

    def test_unverified_tool_inventory_fails_closed(self):
        """Verifies that if tool inventory cannot be proven empty, execution fails closed."""
        bridge = OpenClawBridge(self.config)
        with patch.object(Path, "exists", return_value=True), \
             patch.object(bridge, "_verify_empty_tool_inventory", return_value=(False, "Tool inventory not empty")):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.UNAVAILABLE)
        self.assertIn("tool isolation blocker", resp.error.lower())

    # -------------------------------------------------------------------------
    # 13. Subprocess Bounded Stream Execution (Real Process Invariants)
    # -------------------------------------------------------------------------
    def test_bounded_process_execution_success(self):
        """Verifies _execute_bounded_process captures normal output successfully."""
        bridge = OpenClawBridge(self.config)
        code = "import sys; sys.stdout.write('CIPHER bounded ok'); sys.stderr.write('no error')"
        ret, out_b, err_b, err_msg = bridge._execute_bounded_process(
            cmd=[sys.executable, "-c", code],
            env={},
            timeout=5.0,
            max_bytes=1024,
        )
        self.assertEqual(ret, 0)
        self.assertEqual(out_b.decode("utf-8"), "CIPHER bounded ok")
        self.assertEqual(err_b.decode("utf-8"), "no error")
        self.assertIsNone(err_msg)

    def test_bounded_process_execution_oversized_terminates_promptly(self):
        """Verifies that stdout exceeding max_bytes terminates the subprocess and fails closed."""
        bridge = OpenClawBridge(self.config)
        # Process attempts to write 50,000 bytes; limit is set to 256 bytes
        code = "import sys; sys.stdout.write('A' * 50000); sys.stdout.flush()"
        ret, out_b, err_b, err_msg = bridge._execute_bounded_process(
            cmd=[sys.executable, "-c", code],
            env={},
            timeout=5.0,
            max_bytes=256,
        )
        self.assertEqual(ret, -1)
        self.assertIsNotNone(err_msg)
        self.assertIn("exceeded maximum permitted byte limit", err_msg)
        self.assertIn("256 bytes", err_msg)

    def test_bounded_process_execution_immediate_overflow_kill_stdout(self):
        """Verifies that a subprocess writing 50,000 bytes and then sleeping 100s is killed immediately."""
        bridge = OpenClawBridge(self.config)
        code = "import sys, time; sys.stdout.write('X' * 50000); sys.stdout.flush(); time.sleep(100)"
        start_t = time.perf_counter()
        ret, out_b, err_b, err_msg = bridge._execute_bounded_process(
            cmd=[sys.executable, "-c", code],
            env={},
            timeout=30.0,
            max_bytes=256,
        )
        elapsed = time.perf_counter() - start_t
        self.assertEqual(ret, -1)
        self.assertIsNotNone(err_msg)
        self.assertIn("exceeded maximum permitted byte limit", err_msg)
        self.assertLess(elapsed, 4.0, "Process should have been terminated immediately without waiting for timeout")

    def test_bounded_process_execution_immediate_overflow_kill_stderr(self):
        """Verifies that stderr overflow immediately terminates a sleeping subprocess."""
        bridge = OpenClawBridge(self.config)
        code = "import sys, time; sys.stderr.write('E' * 50000); sys.stderr.flush(); time.sleep(100)"
        start_t = time.perf_counter()
        ret, out_b, err_b, err_msg = bridge._execute_bounded_process(
            cmd=[sys.executable, "-c", code],
            env={},
            timeout=30.0,
            max_bytes=256,
        )
        elapsed = time.perf_counter() - start_t
        self.assertEqual(ret, -1)
        self.assertIsNotNone(err_msg)
        self.assertIn("exceeded maximum permitted byte limit", err_msg)
        self.assertLess(elapsed, 4.0, "Process should have been terminated immediately without waiting for timeout")

    def test_bounded_process_execution_broken_pipe_ignored_by_child(self):
        """Verifies process termination when child attempts to continue writing or sleep."""
        bridge = OpenClawBridge(self.config)
        code = "import sys, time; sys.stdout.write('A'*50000); sys.stdout.flush(); time.sleep(10)"
        start_t = time.perf_counter()
        ret, out_b, err_b, err_msg = bridge._execute_bounded_process(
            cmd=[sys.executable, "-c", code],
            env={},
            timeout=30.0,
            max_bytes=256,
        )
        elapsed = time.perf_counter() - start_t
        self.assertEqual(ret, -1)
        self.assertLess(elapsed, 4.0)

    def test_bounded_process_reader_thread_unexpected_exception_kills_proc(self):
        """Verifies child is terminated if a reader thread raises an unexpected error."""
        bridge = OpenClawBridge(self.config)
        with patch("integrations.openclaw.bridge.read_bounded", side_effect=RuntimeError("unexpected read crash")):
            code = "import time; time.sleep(10)"
            ret, out_b, err_b, err_msg = bridge._execute_bounded_process(
                cmd=[sys.executable, "-c", code],
                env={},
                timeout=30.0,
                max_bytes=1024,
            )
            self.assertEqual(ret, -1)
            self.assertIn("i/o error reading process streams", err_msg.lower())

    def test_bounded_process_execution_timeout_kills_process(self):
        """Verifies that a hanging subprocess is killed and returns timeout."""
        bridge = OpenClawBridge(self.config)
        code = "import time; time.sleep(10)"
        ret, out_b, err_b, err_msg = bridge._execute_bounded_process(
            cmd=[sys.executable, "-c", code],
            env={},
            timeout=0.5,
            max_bytes=1024,
        )
        self.assertEqual(ret, -1)
        self.assertIsNotNone(err_msg)
        self.assertIn("timed out after 0.5s", err_msg)


if __name__ == "__main__":
    unittest.main()
