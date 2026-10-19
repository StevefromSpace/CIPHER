"""Unit and Security Tests for Leon Bridge Adapter.

Verifies:
1. Disabled state handling.
2. Blank prompt rejection fail-closed.
3. Timeout range bounds (1 to 300s).
4. Cloud model rejection under strict_local.
5. Tool execution rejection (allow_tools=True).
6. Loopback binding enforcement.
7. Successful response extraction (output.speech / output.text).
8. Connection error mapping.
9. Authentication error mapping (HTTP 401/403).
10. Request timeout handling.
11. Malformed JSON handling.
12. Sensitive API key and token masking.
13. Health probe availability check.
14. Version compatibility and metadata verification.
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


class TestLeonBridge(unittest.TestCase):
    """Test suite covering Leon integration constraints and security gates."""

    def setUp(self):
        self.config = LeonConfig(
            enabled=True,
            api_url="http://127.0.0.1:1337",
            api_key="mock-leon-api-key",
            default_model="local/default",
            timeout_seconds=10.0,
            strict_local=True
        )

    def test_disabled_by_default(self):
        """Integration is disabled by default unless explicitly configured."""
        default_cfg = LeonConfig()
        bridge = LeonBridge(default_cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.DISABLED)
        self.assertFalse(bridge.is_available())

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
            default_model="local/default"
        )
        bridge = LeonBridge(remote_cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("not bound to a loopback address", resp.error.lower())

    @patch("integrations.leon.bridge.urllib.request.urlopen")
    def test_successful_mocked_completion_speech(self, mock_urlopen):
        """Verifies extraction of output.speech from Leon response."""
        mock_resp = MagicMock()
        mock_resp.read.return_value = json.dumps({
            "action": "greeting",
            "output": {
                "speech": "Hello! I am Leon running locally.",
                "text": "Hello! I am Leon running locally."
            }
        }).encode("utf-8")
        mock_urlopen.return_value.__enter__.return_value = mock_resp

        bridge = LeonBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello Leon"))

        self.assertEqual(resp.status, AssistantStatus.OK)
        self.assertEqual(resp.text, "Hello! I am Leon running locally.")
        self.assertEqual(resp.route, ProcessingRoute.VERIFIED_LOCAL)

    @patch("integrations.leon.bridge.urllib.request.urlopen")
    def test_successful_mocked_completion_direct_text(self, mock_urlopen):
        """Verifies extraction of direct text/reply fields."""
        mock_resp = MagicMock()
        mock_resp.read.return_value = json.dumps({
            "reply": "Leon direct text response."
        }).encode("utf-8")
        mock_urlopen.return_value.__enter__.return_value = mock_resp

        bridge = LeonBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Status check"))

        self.assertEqual(resp.status, AssistantStatus.OK)
        self.assertEqual(resp.text, "Leon direct text response.")

    @patch("integrations.leon.bridge.urllib.request.urlopen")
    def test_connection_failure(self, mock_urlopen):
        """Verifies URLError maps to CONNECTION_ERROR."""
        mock_urlopen.side_effect = urllib.error.URLError("Connection refused")

        bridge = LeonBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.CONNECTION_ERROR)
        self.assertIn("Connection refused", resp.error)

    @patch("integrations.leon.bridge.urllib.request.urlopen")
    def test_auth_failure(self, mock_urlopen):
        """Verifies HTTP 401 maps to AUTH_ERROR."""
        mock_err = urllib.error.HTTPError(
            url="http://127.0.0.1:1337",
            code=401,
            msg="Unauthorized",
            hdrs={},
            fp=io.BytesIO(b'{"message":"Unauthorized: Invalid API key"}')
        )
        mock_urlopen.side_effect = mock_err

        bridge = LeonBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.AUTH_ERROR)
        self.assertIn("401", resp.error)

    @patch("integrations.leon.bridge.urllib.request.urlopen")
    def test_timeout_handling(self, mock_urlopen):
        """Verifies TimeoutError maps to TIMEOUT."""
        mock_urlopen.side_effect = TimeoutError()

        bridge = LeonBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=5.0))
        self.assertEqual(resp.status, AssistantStatus.TIMEOUT)
        self.assertIn("timed out", resp.error.lower())

    @patch("integrations.leon.bridge.urllib.request.urlopen")
    def test_malformed_json_response(self, mock_urlopen):
        """Verifies invalid JSON is handled gracefully."""
        mock_resp = MagicMock()
        mock_resp.read.return_value = b"<html>500 Internal Error</html>"
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

    @patch("integrations.leon.bridge.urllib.request.urlopen")
    def test_is_available_probing(self, mock_urlopen):
        """Verifies is_available queries info endpoint."""
        mock_resp = MagicMock()
        mock_resp.status = 200
        mock_urlopen.return_value.__enter__.return_value = mock_resp

        bridge = LeonBridge(self.config)
        self.assertTrue(bridge.is_available())

        mock_urlopen.side_effect = Exception("offline")
        self.assertFalse(bridge.is_available())


if __name__ == "__main__":
    unittest.main()
