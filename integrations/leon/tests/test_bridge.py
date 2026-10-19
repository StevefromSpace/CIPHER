"""Unit and Security Tests for Leon Bridge Adapter.

Verifies:
1. Disabled state handling.
2. Tool/skill isolation fail-closed policy (live execution disabled by default).
3. Blank prompt rejection fail-closed.
4. Timeout range bounds (1 to 300s).
5. Cloud model rejection under strict_local.
6. Tool execution rejection (allow_tools=True).
7. Loopback binding enforcement and user-info rejection.
8. Successful response extraction (output.speech / output.text / direct text).
9. Connection error mapping.
10. Authentication error mapping (HTTP 401/403).
11. Request timeout handling.
12. Malformed JSON handling.
13. Sensitive API key and token masking.
14. Health probe availability check and redirect refusal.
15. Version compatibility (1.x stable vs 2.x developer preview rejection).
16. Strict redirect refusal fail-closed (safe_urlopen / NoRedirectHandler).
17. Provenance route defaults to UNKNOWN unless verified.
18. Response body size limit bounding (bounded reading aborts and terminates stream).
19. Error body size limit bounding (bounded reading of HTTPError.fp).
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
from integrations.leon.bridge import LeonBridge
from integrations.leon.config import LeonConfig


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


class TestLeonBridge(unittest.TestCase):
    """Test suite covering Leon integration constraints and security gates."""

    def setUp(self):
        self.config = LeonConfig(
            enabled=True,
            api_url="http://127.0.0.1:1337",
            api_key="mock-leon-api-key",
            default_model="local/default",
            timeout_seconds=10.0,
            strict_local=True,
            verified_local=True,
            enable_live_execution=True,  # Enabled specifically for mocked transport tests
        )

    def test_disabled_by_default(self):
        """Integration is disabled by default unless explicitly configured."""
        default_cfg = LeonConfig()
        bridge = LeonBridge(default_cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.DISABLED)
        self.assertFalse(bridge.is_available())

    def test_tool_isolation_unproven_disables_live_execution_fail_closed(self):
        """Verifies that default config without proven tool isolation disables execution."""
        safe_cfg = LeonConfig(
            enabled=True,
            api_url="http://127.0.0.1:1337",
            default_model="local/default",
            enable_live_execution=False,  # Default
        )
        bridge = LeonBridge(safe_cfg)
        self.assertFalse(bridge.is_available())
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.UNAVAILABLE)
        self.assertIn("tool/skill isolation cannot be proven", resp.error.lower())

    def test_blank_prompt_rejection(self):
        """Verifies empty or whitespace prompts fail closed."""
        bridge = LeonBridge(self.config)
        resp1 = bridge.submit_prompt(AssistantRequest(prompt=""))
        resp2 = bridge.submit_prompt(AssistantRequest(prompt="   \n\t  "))
        self.assertEqual(resp1.status, AssistantStatus.INVALID_REQUEST)
        self.assertEqual(resp2.status, AssistantStatus.INVALID_REQUEST)

    def test_timeout_bounds_rejection(self):
        """Verifies timeouts outside [1.0, 300.0] are rejected."""
        bridge = LeonBridge(self.config)
        resp_low = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=0.5))
        resp_high = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=301.0))
        self.assertEqual(resp_low.status, AssistantStatus.INVALID_REQUEST)
        self.assertEqual(resp_high.status, AssistantStatus.INVALID_REQUEST)

    def test_cloud_model_rejection(self):
        """Verifies cloud models are rejected under strict local mode."""
        bridge = LeonBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi", model_override="anthropic/claude"))
        self.assertEqual(resp.status, AssistantStatus.REJECTED_CLOUD_MODEL)
        self.assertIn("cloud pattern", resp.error.lower())

    def test_tool_request_rejection(self):
        """Verifies allow_tools=True is rejected fail-closed."""
        bridge = LeonBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi", allow_tools=True))
        self.assertEqual(resp.status, AssistantStatus.REJECTED_TOOLS_ENABLED)

    def test_loopback_binding_enforcement(self):
        """Verifies rejection of non-loopback URLs."""
        remote_cfg = LeonConfig(
            enabled=True,
            api_url="http://10.0.0.5:1337",
            default_model="local/default",
            verified_local=True,
            enable_live_execution=True,
        )
        bridge = LeonBridge(remote_cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("not bound to a loopback address", resp.error.lower())

    @patch("integrations.leon.bridge.safe_urlopen")
    def test_successful_mocked_completion_speech(self, mock_urlopen):
        """Verifies extraction of output.speech from Leon response."""
        data = json.dumps({
            "action": "greeting",
            "output": {
                "speech": "Hello! I am Leon running locally.",
                "text": "Hello! I am Leon running locally."
            }
        }).encode("utf-8")
        mock_resp = ChunkStream(data)
        mock_urlopen.return_value.__enter__.return_value = mock_resp

        bridge = LeonBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello Leon"))

        self.assertEqual(resp.status, AssistantStatus.OK)
        self.assertEqual(resp.text, "Hello! I am Leon running locally.")
        self.assertEqual(resp.route, ProcessingRoute.VERIFIED_LOCAL)

        called_req = mock_urlopen.call_args[0][0]
        self.assertTrue(called_req.full_url.endswith("/api/v1/query"))
        self.assertEqual(called_req.headers.get("X-api-key"), "mock-leon-api-key")

    @patch("integrations.leon.bridge.safe_urlopen")
    def test_successful_mocked_completion_direct_text(self, mock_urlopen):
        """Verifies extraction of direct text/reply fields."""
        data = json.dumps({
            "reply": "Leon direct text response."
        }).encode("utf-8")
        mock_resp = ChunkStream(data)
        mock_urlopen.return_value.__enter__.return_value = mock_resp

        bridge = LeonBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Status check"))

        self.assertEqual(resp.status, AssistantStatus.OK)
        self.assertEqual(resp.text, "Leon direct text response.")

    @patch("integrations.leon.bridge.safe_urlopen")
    def test_connection_failure(self, mock_urlopen):
        """Verifies URLError maps to CONNECTION_ERROR."""
        mock_urlopen.side_effect = urllib.error.URLError("Connection refused")

        bridge = LeonBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.CONNECTION_ERROR)
        self.assertIn("Connection refused", resp.error)

    @patch("integrations.leon.bridge.safe_urlopen")
    def test_auth_failure(self, mock_urlopen):
        """Verifies HTTP 401 maps to AUTH_ERROR."""
        err_stream = ChunkStream(b'{"message":"Unauthorized: Invalid API key"}')
        mock_err = urllib.error.HTTPError(
            url="http://127.0.0.1:1337/api/v1/query",
            code=401,
            msg="Unauthorized",
            hdrs={},
            fp=err_stream
        )
        mock_urlopen.side_effect = mock_err

        bridge = LeonBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.AUTH_ERROR)
        self.assertIn("401", resp.error)
        self.assertTrue(err_stream.closed_called)

    @patch("integrations.leon.bridge.safe_urlopen")
    def test_redirect_rejection(self, mock_urlopen):
        """Verifies HTTP 301/302 redirects are refused fail-closed."""
        mock_err = urllib.error.HTTPError(
            url="http://127.0.0.1:1337/redirect",
            code=301,
            msg="Automatic HTTP redirect (301) to 'http://attacker.com' is blocked for security.",
            hdrs={},
            fp=ChunkStream(b'')
        )
        mock_urlopen.side_effect = mock_err

        bridge = LeonBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("redirect", resp.error.lower())

    @patch("integrations.leon.bridge.safe_urlopen")
    def test_timeout_handling(self, mock_urlopen):
        """Verifies TimeoutError maps to TIMEOUT."""
        mock_urlopen.side_effect = TimeoutError()

        bridge = LeonBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=5.0))
        self.assertEqual(resp.status, AssistantStatus.TIMEOUT)
        self.assertIn("timed out", resp.error.lower())

    @patch("integrations.leon.bridge.safe_urlopen")
    def test_malformed_json_response(self, mock_urlopen):
        """Verifies invalid JSON is handled gracefully."""
        mock_resp = ChunkStream(b"<html>500 Internal Error</html>")
        mock_urlopen.return_value.__enter__.return_value = mock_resp

        bridge = LeonBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("invalid json", resp.error.lower())

    def test_credential_sanitization(self):
        """Verifies API keys and tokens are redacted."""
        bridge = LeonBridge(self.config)
        raw = "Error sending X-API-Key: leon_key_secret987654321 and Bearer token12345678"
        clean = bridge._sanitize_error(raw)
        self.assertNotIn("leon_key_secret987654321", clean)
        self.assertNotIn("token12345678", clean)
        self.assertIn("[REDACTED]", clean)

    @patch("integrations.leon.bridge.safe_urlopen")
    def test_is_available_probing(self, mock_urlopen):
        """Verifies is_available queries info endpoint without following redirects."""
        mock_resp = ChunkStream(json.dumps({"version": "1.0.0"}).encode("utf-8"))
        mock_resp.status = 200
        mock_urlopen.return_value.__enter__.return_value = mock_resp

        bridge = LeonBridge(self.config)
        self.assertTrue(bridge.is_available())

        called_req = mock_urlopen.call_args[0][0]
        self.assertTrue(called_req.full_url.endswith("/api/v1/info"))

        mock_urlopen.side_effect = Exception("offline")
        self.assertFalse(bridge.is_available())

    @patch("integrations.leon.bridge.safe_urlopen")
    def test_is_available_rejects_developer_preview_2x(self, mock_urlopen):
        """Verifies is_available rejects unstable Leon 2.0 developer preview."""
        mock_resp = ChunkStream(json.dumps({"version": "2.0.0-preview.1"}).encode("utf-8"))
        mock_resp.status = 200
        mock_urlopen.return_value.__enter__.return_value = mock_resp

        bridge = LeonBridge(self.config)
        self.assertFalse(bridge.is_available())

    def test_rejection_of_non_finite_timeout(self):
        """Verifies rejection of NaN and Inf timeout values."""
        bridge = LeonBridge(self.config)
        resp_nan = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=float("nan")))
        resp_inf = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=float("inf")))
        resp_ninf = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=float("-inf")))
        self.assertEqual(resp_nan.status, AssistantStatus.INVALID_REQUEST)
        self.assertEqual(resp_inf.status, AssistantStatus.INVALID_REQUEST)
        self.assertEqual(resp_ninf.status, AssistantStatus.INVALID_REQUEST)

    def test_spoofed_loopback_url_rejection(self):
        """Verifies that parsed hostname prevents spoofed loopback addresses."""
        spoofed_cfg = LeonConfig(
            enabled=True,
            api_url="http://127.0.0.1.attacker.com:1337",
            default_model="local/default",
            verified_local=True,
            enable_live_execution=True,
        )
        bridge = LeonBridge(spoofed_cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("not bound to a loopback address", resp.error.lower())

    def test_userinfo_credentials_in_url_rejected(self):
        """Verifies that URLs with embedded user credentials fail closed."""
        cred_cfg = LeonConfig(
            enabled=True,
            api_url="http://user:secret@127.0.0.1:1337",
            default_model="local/default",
            verified_local=True,
            enable_live_execution=True,
        )
        bridge = LeonBridge(cred_cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("user credentials", resp.error.lower())

    def test_route_provenance_unknown_by_default(self):
        """Verifies route provenance defaults to UNKNOWN and fails closed under require_local=True."""
        unverified_cfg = LeonConfig(
            enabled=True,
            api_url="http://127.0.0.1:1337",
            default_model="local/default",
            verified_local=False,
            enable_live_execution=True,
        )
        bridge = LeonBridge(unverified_cfg)
        self.assertEqual(bridge.capabilities.default_route, ProcessingRoute.UNKNOWN)

        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello", require_local=True))
        self.assertEqual(resp.status, AssistantStatus.REJECTED_CLOUD_MODEL)
        self.assertIn("unknown", resp.error.lower())

    @patch("integrations.leon.bridge.safe_urlopen")
    def test_bounded_response_size_terminates_promptly(self, mock_urlopen):
        """Verifies response exceeding 1MB terminates during stream reading and closes stream."""
        stream = ChunkStream(b"x" * (1048576 + 500000))
        mock_urlopen.return_value.__enter__.return_value = stream

        bridge = LeonBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("maximum permitted byte limit", resp.error.lower())
        self.assertTrue(stream.closed_called)
        self.assertLessEqual(stream.bytes_read, 1048576 + 1)

    @patch("integrations.leon.bridge.safe_urlopen")
    def test_bounded_error_body_terminates_promptly(self, mock_urlopen):
        """Verifies oversized HTTP error body terminates during stream reading and closes stream."""
        err_stream = ChunkStream(b"E" * 102400)
        mock_err = urllib.error.HTTPError(
            url="http://127.0.0.1:1337/api/v1/query",
            code=500,
            msg="Internal Error",
            hdrs={},
            fp=err_stream
        )
        mock_urlopen.side_effect = mock_err

        bridge = LeonBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("exceeded 4096 bytes", resp.error)
        self.assertTrue(err_stream.closed_called)
        self.assertLessEqual(err_stream.bytes_read, 4096 + 1)


if __name__ == "__main__":
    unittest.main()
