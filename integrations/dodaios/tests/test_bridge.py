"""Unit and Security Tests for Dodaios Bridge Adapter.

Verifies:
1. Disabled state handling.
2. Blank prompt rejection fail-closed.
3. Timeout range bounds (1 to 300s).
4. Cloud model rejection under strict_local.
5. Tool execution rejection (allow_tools=True).
6. Privileged action rejection (computer use, mission dispatch, shell, install, config, tools/functions).
7. Loopback binding enforcement and user-info rejection.
8. Verified chat endpoint (/api/runtime-gateway/v1/chat/completions) parsing.
9. Connection error mapping.
10. Authentication error mapping (HTTP 401/403).
11. Request timeout handling.
12. Malformed JSON handling.
13. Sensitive credential masking.
14. Health probe availability check on verified /api/health endpoint.
15. Strict redirect refusal fail-closed (NoRedirectHandler / safe_urlopen).
16. Provenance route defaults to UNKNOWN unless verified.
17. Response body size limit bounding (bounded reading terminates and aborts stream).
18. Error body size limit bounding (HTTPError.fp bounded reading).
19. Unverified live compatibility reporting when unverified.
"""

import io
import json
import unittest
from unittest.mock import MagicMock, patch
import urllib.error

from integrations.contract import (
    AssistantCapabilities,
    AssistantRequest,
    AssistantResponse,
    AssistantStatus,
    ProcessingRoute,
)
from integrations.dodaios.bridge import DodaiosBridge
from integrations.dodaios.config import DodaiosConfig


class ChunkStream(io.BytesIO):
    """Test stream that records bytes read and close calls to prove bounded reads."""

    def __init__(self, data: bytes):
        super().__init__(data)
        self.bytes_read = 0
        self.closed_called = False

    def read(self, size=-1):
        chunk = super().read(size)
        self.bytes_read += len(chunk)
        return chunk

    def close(self):
        self.closed_called = True
        super().close()


class TestDodaiosBridge(unittest.TestCase):
    """Test suite covering Dodaios integration constraints and security gates."""

    def setUp(self):
        self.config = DodaiosConfig(
            enabled=True,
            api_url="http://127.0.0.1:8000",
            api_key="mock-dodaios-token",
            default_model="local/default",
            timeout_seconds=10.0,
            strict_local=True,
            verified_local=True,
            enable_live_execution=True,
            verified_tool_isolation=True,
        )


    def test_disabled_by_default(self):
        """Integration is disabled by default unless explicitly configured."""
        default_cfg = DodaiosConfig()
        bridge = DodaiosBridge(default_cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.DISABLED)
        self.assertFalse(bridge.is_available())

    def test_blank_prompt_rejection(self):
        """Verifies empty or whitespace prompts fail closed."""
        bridge = DodaiosBridge(self.config)
        resp1 = bridge.submit_prompt(AssistantRequest(prompt=""))
        resp2 = bridge.submit_prompt(AssistantRequest(prompt="   \n\t  "))
        self.assertEqual(resp1.status, AssistantStatus.INVALID_REQUEST)
        self.assertEqual(resp2.status, AssistantStatus.INVALID_REQUEST)

    def test_timeout_bounds_rejection(self):
        """Verifies timeouts outside [1.0, 300.0] are rejected."""
        bridge = DodaiosBridge(self.config)
        resp_low = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=0.2))
        resp_high = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=500.0))
        self.assertEqual(resp_low.status, AssistantStatus.INVALID_REQUEST)
        self.assertEqual(resp_high.status, AssistantStatus.INVALID_REQUEST)

    def test_cloud_model_rejection(self):
        """Verifies cloud models are rejected under strict local mode."""
        bridge = DodaiosBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi", model_override="gpt-4o"))
        self.assertEqual(resp.status, AssistantStatus.REJECTED_CLOUD_MODEL)
        self.assertIn("cloud pattern", resp.error.lower())

    def test_tool_request_rejection(self):
        """Verifies allow_tools=True is rejected fail-closed."""
        bridge = DodaiosBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi", allow_tools=True))
        self.assertEqual(resp.status, AssistantStatus.REJECTED_TOOLS_ENABLED)

    def test_privileged_actions_rejection(self):
        """Verifies computer use, mission dispatch, shell execution, install, config mutation, tools are rejected."""
        bridge = DodaiosBridge(self.config)
        for act in (
            "computer_use", "mission", "execute", "shell", "config", "install",
            "mission_dispatch", "shell_execution", "tools", "functions", "tool_choice"
        ):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Hi", metadata={act: True}))
            self.assertEqual(
                resp.status,
                AssistantStatus.REJECTED_TOOLS_ENABLED,
                f"Action '{act}' should have been rejected as a privileged tool."
            )

    def test_loopback_binding_enforcement(self):
        """Verifies rejection of non-loopback URLs."""
        remote_cfg = DodaiosConfig(
            enabled=True,
            api_url="http://192.168.1.100:8000",
            default_model="local/default",
            verified_local=True,
        )
        bridge = DodaiosBridge(remote_cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("not bound to a loopback address", resp.error.lower())

    @patch("integrations.dodaios.bridge.safe_urlopen")
    def test_successful_mocked_completion(self, mock_urlopen):
        """Verifies successful text response parsing from runtime gateway endpoint."""
        data = json.dumps({
            "choices": [
                {"message": {"role": "assistant", "content": "Dodaios local response."}}
            ]
        }).encode("utf-8")
        mock_resp = ChunkStream(data)
        mock_urlopen.return_value.__enter__.return_value = mock_resp

        bridge = DodaiosBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="What is CIPHER?"))

        self.assertEqual(resp.status, AssistantStatus.OK)
        self.assertEqual(resp.text, "Dodaios local response.")
        self.assertEqual(resp.route, ProcessingRoute.VERIFIED_LOCAL)

        called_req = mock_urlopen.call_args[0][0]
        self.assertTrue(called_req.full_url.endswith("/api/runtime-gateway/v1/chat/completions"))

    @patch("integrations.dodaios.bridge.safe_urlopen")
    def test_connection_failure(self, mock_urlopen):
        """Verifies URLError maps to CONNECTION_ERROR."""
        mock_urlopen.side_effect = urllib.error.URLError("Connection refused")

        bridge = DodaiosBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.CONNECTION_ERROR)
        self.assertIn("Connection refused", resp.error)

    @patch("integrations.dodaios.bridge.safe_urlopen")
    def test_auth_failure(self, mock_urlopen):
        """Verifies HTTP 401 maps to AUTH_ERROR."""
        err_stream = ChunkStream(b'{"detail":"Invalid API Key"}')
        mock_err = urllib.error.HTTPError(
            url="http://127.0.0.1:8000/api/runtime-gateway/v1/chat/completions",
            code=401,
            msg="Unauthorized",
            hdrs={},
            fp=err_stream
        )
        mock_urlopen.side_effect = mock_err

        bridge = DodaiosBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.AUTH_ERROR)
        self.assertIn("401", resp.error)
        self.assertTrue(err_stream.closed_called)

    @patch("integrations.dodaios.bridge.safe_urlopen")
    def test_redirect_rejection(self, mock_urlopen):
        """Verifies HTTP 301/302 redirects are refused fail-closed."""
        mock_err = urllib.error.HTTPError(
            url="http://127.0.0.1:8000/redirect",
            code=302,
            msg="Automatic HTTP redirect (302) to 'http://attacker.com' is blocked for security.",
            hdrs={},
            fp=ChunkStream(b'')
        )
        mock_urlopen.side_effect = mock_err

        bridge = DodaiosBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("redirect", resp.error.lower())

    @patch("integrations.dodaios.bridge.safe_urlopen")
    def test_timeout_handling(self, mock_urlopen):
        """Verifies TimeoutError maps to TIMEOUT."""
        mock_urlopen.side_effect = TimeoutError()

        bridge = DodaiosBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=5.0))
        self.assertEqual(resp.status, AssistantStatus.TIMEOUT)
        self.assertIn("timed out", resp.error.lower())

    @patch("integrations.dodaios.bridge.safe_urlopen")
    def test_malformed_json_response(self, mock_urlopen):
        """Verifies invalid JSON is handled gracefully."""
        mock_resp = ChunkStream(b"<html>502 Bad Gateway</html>")
        mock_urlopen.return_value.__enter__.return_value = mock_resp

        bridge = DodaiosBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("invalid json", resp.error.lower())

    def test_credential_sanitization(self):
        """Verifies sensitive bearer tokens and secrets are redacted."""
        bridge = DodaiosBridge(self.config)
        raw = "Error with Bearer abcdef1234567890 and key=secret123456"
        clean = bridge._sanitize_error(raw)
        self.assertNotIn("abcdef1234567890", clean)
        self.assertNotIn("secret123456", clean)
        self.assertIn("[REDACTED]", clean)

    @patch("integrations.dodaios.bridge.safe_urlopen")
    def test_is_available_probing(self, mock_urlopen):
        """Verifies is_available queries the verified /api/health endpoint without following redirects."""
        mock_resp = ChunkStream(b'{"status":"ok"}')
        mock_resp.status = 200
        mock_urlopen.return_value.__enter__.return_value = mock_resp

        bridge = DodaiosBridge(self.config)
        self.assertTrue(bridge.is_available())

        called_req = mock_urlopen.call_args[0][0]
        self.assertTrue(called_req.full_url.endswith("/api/health"))

        mock_urlopen.side_effect = Exception("offline")
        self.assertFalse(bridge.is_available())

    def test_rejection_of_non_finite_timeout(self):
        """Verifies rejection of NaN and Inf timeout values."""
        bridge = DodaiosBridge(self.config)
        resp_nan = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=float("nan")))
        resp_inf = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=float("inf")))
        resp_ninf = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=float("-inf")))
        self.assertEqual(resp_nan.status, AssistantStatus.INVALID_REQUEST)
        self.assertEqual(resp_inf.status, AssistantStatus.INVALID_REQUEST)
        self.assertEqual(resp_ninf.status, AssistantStatus.INVALID_REQUEST)

    def test_spoofed_loopback_url_rejection(self):
        """Verifies that parsed hostname prevents spoofed loopback addresses."""
        spoofed_cfg = DodaiosConfig(
            enabled=True,
            api_url="http://127.0.0.1.attacker.com:8000",
            default_model="local/default",
            verified_local=True,
        )
        bridge = DodaiosBridge(spoofed_cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("not bound to a loopback address", resp.error.lower())

    def test_userinfo_credentials_in_url_rejected(self):
        """Verifies that API URLs with embedded credentials fail closed."""
        cred_cfg = DodaiosConfig(
            enabled=True,
            api_url="http://user:pass@127.0.0.1:8000",
            default_model="local/default",
            verified_local=True,
        )
        bridge = DodaiosBridge(cred_cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("user credentials", resp.error.lower())

    def test_route_provenance_unknown_by_default(self):
        """Verifies route provenance defaults to UNKNOWN and fails closed under require_local=True."""
        unverified_cfg = DodaiosConfig(
            enabled=True,
            api_url="http://127.0.0.1:8000",
            default_model="local/default",
            verified_local=False,
        )
        bridge = DodaiosBridge(unverified_cfg)
        self.assertEqual(bridge.capabilities.default_route, ProcessingRoute.UNKNOWN)

        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello", require_local=True))
        self.assertEqual(resp.status, AssistantStatus.REJECTED_CLOUD_MODEL)
        self.assertIn("unknown", resp.error.lower())

        status = bridge.get_status()
        self.assertEqual(status["live_compatibility"], "unverified")

    @patch("integrations.dodaios.bridge.safe_urlopen")
    def test_bounded_response_size_terminates_promptly(self, mock_urlopen):
        """Verifies oversized response terminates during stream reading and closes stream."""
        # 2MB stream; limit is 1MB (1048576)
        stream = ChunkStream(b"A" * (1048576 + 500000))
        mock_urlopen.return_value.__enter__.return_value = stream

        bridge = DodaiosBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))

        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("maximum permitted byte limit", resp.error.lower())
        # Stream MUST be closed when limit exceeded
        self.assertTrue(stream.closed_called)
        # Bytes read must NOT consume the whole 1.5MB; must stop right around max_bytes + 1
        self.assertLessEqual(stream.bytes_read, 1048576 + 1)

    @patch("integrations.dodaios.bridge.safe_urlopen")
    def test_bounded_error_body_terminates_promptly(self, mock_urlopen):
        """Verifies oversized HTTP error body terminates during reading and closes stream."""
        # 100KB error stream; error limit is 4KB (4096)
        err_stream = ChunkStream(b"E" * 102400)
        mock_err = urllib.error.HTTPError(
            url="http://127.0.0.1:8000/api/runtime-gateway/v1/chat/completions",
            code=500,
            msg="Internal Error",
            hdrs={},
            fp=err_stream
        )
        mock_urlopen.side_effect = mock_err

        bridge = DodaiosBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))

        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("exceeded 4096 bytes", resp.error)
        self.assertTrue(err_stream.closed_called)
        # Bytes read must not read 100KB; must stop right at 4097
        self.assertLessEqual(err_stream.bytes_read, 4096 + 1)

    def test_live_execution_disabled_by_default(self):
        """Verifies Dodaios live execution fails closed by default."""
        cfg = DodaiosConfig(enabled=True, api_url="http://127.0.0.1:8000", verified_local=True)
        self.assertFalse(cfg.enable_live_execution)
        bridge = DodaiosBridge(cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.UNAVAILABLE)
        self.assertIn("execution unavailable", resp.error.lower())
        self.assertIn("tool isolation cannot be proven", resp.error.lower())

    def test_boolean_opt_in_without_verified_tool_isolation_fails_closed(self):
        """Verifies a Boolean opt-in alone does NOT permit Dodaios execution."""
        cfg = DodaiosConfig(
            enabled=True,
            api_url="http://127.0.0.1:8000",
            verified_local=True,
            enable_live_execution=True,
            verified_tool_isolation=False,
        )
        bridge = DodaiosBridge(cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.UNAVAILABLE)
        self.assertIn("execution unavailable", resp.error.lower())
        self.assertIn("tool isolation cannot be proven", resp.error.lower())

    def test_status_reports_operator_assertion(self):
        """Verifies get_status explicitly declares verified_local as operator assertion."""
        bridge = DodaiosBridge(self.config)
        status = bridge.get_status()
        self.assertIn("verified_local_status", status)
        self.assertIn("operator_assertion", status["verified_local_status"])


if __name__ == "__main__":
    unittest.main()
