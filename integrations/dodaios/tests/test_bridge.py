"""Unit and Security Tests for Dodaios Bridge Adapter.

Verifies:
1. Disabled state handling.
2. Blank prompt rejection fail-closed.
3. Timeout range bounds (1 to 300s).
4. Cloud model rejection under strict_local.
5. Tool execution rejection (allow_tools=True).
6. Privileged action rejection (computer use, mission dispatch).
7. Loopback binding enforcement.
8. Successful chat completion parsing.
9. Connection error mapping.
10. Authentication error mapping (HTTP 401/403).
11. Request timeout handling.
12. Malformed JSON handling.
13. Sensitive credential masking.
14. Health probe availability check.
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


class TestDodaiosBridge(unittest.TestCase):
    """Test suite covering Dodaios integration constraints and security gates."""

    def setUp(self):
        self.config = DodaiosConfig(
            enabled=True,
            api_url="http://127.0.0.1:8000",
            api_key="mock-dodaios-token",
            default_model="local/default",
            timeout_seconds=10.0,
            strict_local=True
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
        """Verifies computer use and mission dispatch are rejected."""
        bridge = DodaiosBridge(self.config)
        resp_cu = bridge.submit_prompt(AssistantRequest(prompt="Hi", metadata={"computer_use": True}))
        resp_md = bridge.submit_prompt(AssistantRequest(prompt="Hi", metadata={"mission": "install"}))
        self.assertEqual(resp_cu.status, AssistantStatus.REJECTED_TOOLS_ENABLED)
        self.assertEqual(resp_md.status, AssistantStatus.REJECTED_TOOLS_ENABLED)

    def test_loopback_binding_enforcement(self):
        """Verifies rejection of non-loopback URLs."""
        remote_cfg = DodaiosConfig(
            enabled=True,
            api_url="http://192.168.1.100:8000",
            default_model="local/default"
        )
        bridge = DodaiosBridge(remote_cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("not bound to a loopback address", resp.error.lower())

    @patch("integrations.dodaios.bridge.urllib.request.urlopen")
    def test_successful_mocked_completion(self, mock_urlopen):
        """Verifies successful text response parsing from OpenAI-compatible structure."""
        mock_resp = MagicMock()
        mock_resp.read.return_value = json.dumps({
            "choices": [
                {"message": {"role": "assistant", "content": "Dodaios local response."}}
            ]
        }).encode("utf-8")
        mock_urlopen.return_value.__enter__.return_value = mock_resp

        bridge = DodaiosBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="What is CIPHER?"))

        self.assertEqual(resp.status, AssistantStatus.OK)
        self.assertEqual(resp.text, "Dodaios local response.")
        self.assertEqual(resp.route, ProcessingRoute.VERIFIED_LOCAL)

    @patch("integrations.dodaios.bridge.urllib.request.urlopen")
    def test_connection_failure(self, mock_urlopen):
        """Verifies URLError maps to CONNECTION_ERROR."""
        mock_urlopen.side_effect = urllib.error.URLError("Connection refused")

        bridge = DodaiosBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.CONNECTION_ERROR)
        self.assertIn("Connection refused", resp.error)

    @patch("integrations.dodaios.bridge.urllib.request.urlopen")
    def test_auth_failure(self, mock_urlopen):
        """Verifies HTTP 401 maps to AUTH_ERROR."""
        mock_err = urllib.error.HTTPError(
            url="http://127.0.0.1:8000",
            code=401,
            msg="Unauthorized",
            hdrs={},
            fp=io.BytesIO(b'{"detail":"Invalid API Key"}')
        )
        mock_urlopen.side_effect = mock_err

        bridge = DodaiosBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.AUTH_ERROR)
        self.assertIn("401", resp.error)

    @patch("integrations.dodaios.bridge.urllib.request.urlopen")
    def test_timeout_handling(self, mock_urlopen):
        """Verifies TimeoutError maps to TIMEOUT."""
        mock_urlopen.side_effect = TimeoutError()

        bridge = DodaiosBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=5.0))
        self.assertEqual(resp.status, AssistantStatus.TIMEOUT)
        self.assertIn("timed out", resp.error.lower())

    @patch("integrations.dodaios.bridge.urllib.request.urlopen")
    def test_malformed_json_response(self, mock_urlopen):
        """Verifies invalid JSON is handled gracefully."""
        mock_resp = MagicMock()
        mock_resp.read.return_value = b"<html>502 Bad Gateway</html>"
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

    @patch("integrations.dodaios.bridge.urllib.request.urlopen")
    def test_is_available_probing(self, mock_urlopen):
        """Verifies is_available correctly queries health endpoint."""
        mock_resp = MagicMock()
        mock_resp.status = 200
        mock_urlopen.return_value.__enter__.return_value = mock_resp

        bridge = DodaiosBridge(self.config)
        self.assertTrue(bridge.is_available())

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
            default_model="local/default"
        )
        bridge = DodaiosBridge(spoofed_cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("not bound to a loopback address", resp.error.lower())


if __name__ == "__main__":
    unittest.main()
