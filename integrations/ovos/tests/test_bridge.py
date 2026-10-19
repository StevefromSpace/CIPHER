"""Unit and Security Tests for OpenVoiceOS Bridge Adapter.

Verifies:
1. Disabled state handling.
2. Blank prompt rejection fail-closed.
3. Timeout range bounds (1 to 300s).
4. Tool execution rejection (allow_tools=True).
5. System control / privileged command rejection (shutdown, reboot).
6. Loopback binding enforcement.
7. Message envelope construction.
8. Response correlation via session_id.
9. Successful mocked speech event resolution.
10. Timeout waiting for bus response.
11. Intent failure handling.
12. Unavailable status when bus is offline.
13. Socket availability probe.
14. Broadcast notice present in health status.
"""

import json
import socket
import unittest
from unittest.mock import MagicMock, patch

from integrations.contract import (
    AssistantCapabilities,
    AssistantRequest,
    AssistantResponse,
    AssistantStatus,
    ProcessingRoute,
)
from integrations.ovos.bridge import OVOSBridge
from integrations.ovos.config import OVOSConfig
from integrations.ovos.message import OVOSMessage


class MockBusClient:
    """Mock messagebus client simulating OVOS message broadcast and response."""

    def __init__(self, response_msg: Optional[OVOSMessage] = None, raise_exc: bool = False):
        self.response_msg = response_msg
        self.raise_exc = raise_exc
        self.sent_messages = []

    def send_and_wait(self, msg: OVOSMessage, timeout: float = 5.0) -> Optional[OVOSMessage]:
        self.sent_messages.append(msg)
        if self.raise_exc:
            raise ConnectionError("Mock bus connection dropped")
        return self.response_msg


class TestOVOSBridge(unittest.TestCase):
    """Test suite covering OpenVoiceOS integration constraints and event handling."""

    def setUp(self):
        self.config = OVOSConfig(
            enabled=True,
            bus_url="ws://127.0.0.1:8181/core",
            language="en-us",
            timeout_seconds=10.0,
            strict_local=True
        )

    def test_disabled_by_default(self):
        """Integration is disabled by default unless explicitly configured."""
        default_cfg = OVOSConfig()
        bridge = OVOSBridge(default_cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.DISABLED)
        self.assertFalse(bridge.is_available())

    def test_blank_prompt_rejection(self):
        """Verifies empty or whitespace prompts fail closed."""
        bridge = OVOSBridge(self.config)
        resp1 = bridge.submit_prompt(AssistantRequest(prompt=""))
        resp2 = bridge.submit_prompt(AssistantRequest(prompt="   \n\t  "))
        self.assertEqual(resp1.status, AssistantStatus.INVALID_REQUEST)
        self.assertEqual(resp2.status, AssistantStatus.INVALID_REQUEST)

    def test_timeout_bounds_rejection(self):
        """Verifies timeouts outside [1.0, 300.0] are rejected."""
        bridge = OVOSBridge(self.config)
        resp_low = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=0.1))
        resp_high = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=400.0))
        self.assertEqual(resp_low.status, AssistantStatus.INVALID_REQUEST)
        self.assertEqual(resp_high.status, AssistantStatus.INVALID_REQUEST)

    def test_tool_request_rejection(self):
        """Verifies allow_tools=True is rejected fail-closed."""
        bridge = OVOSBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi", allow_tools=True))
        self.assertEqual(resp.status, AssistantStatus.REJECTED_TOOLS_ENABLED)

    def test_system_control_command_rejection(self):
        """Verifies privileged system control commands are blocked."""
        bridge = OVOSBridge(self.config)
        resp1 = bridge.submit_prompt(AssistantRequest(prompt="Shutdown the computer now"))
        resp2 = bridge.submit_prompt(AssistantRequest(prompt="Reboot system"))
        self.assertEqual(resp1.status, AssistantStatus.REJECTED_TOOLS_ENABLED)
        self.assertEqual(resp2.status, AssistantStatus.REJECTED_TOOLS_ENABLED)
        self.assertIn("blocked", resp1.error.lower())

    def test_loopback_binding_enforcement(self):
        """Verifies rejection of non-loopback URLs."""
        remote_cfg = OVOSConfig(
            enabled=True,
            bus_url="ws://10.200.1.1:8181/core"
        )
        bridge = OVOSBridge(remote_cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("not bound to a loopback address", resp.error.lower())

    def test_message_envelope_creation(self):
        """Verifies OVOSMessage envelope structure and context propagation."""
        msg = OVOSMessage.create_utterance(
            text="Turn on living room light",
            session_id="session-xyz-123",
            lang="en-us"
        )
        self.assertEqual(msg.type, "recognizer_loop:utterance")
        self.assertEqual(msg.data["utterances"], ["Turn on living room light"])
        self.assertEqual(msg.data["lang"], "en-us")
        self.assertEqual(msg.context["session_id"], "session-xyz-123")
        self.assertEqual(msg.context["source"], "cipher_bridge")

        # Verify JSON round-trip
        json_str = msg.to_json()
        parsed = OVOSMessage.from_json(json_str)
        self.assertEqual(parsed.type, msg.type)
        self.assertEqual(parsed.context["session_id"], "session-xyz-123")

    def test_response_correlation(self):
        """Verifies correlation detection based on session_id and event type."""
        req = OVOSMessage.create_utterance("Test", session_id="session-42")

        # Correlated speak event
        matching_resp = OVOSMessage(
            type="speak",
            data={"utterance": "Intent handled."},
            context={"session_id": "session-42"}
        )
        self.assertTrue(matching_resp.is_correlated_response_to(req))

        # Different session event (broadcast noise)
        unrelated_resp = OVOSMessage(
            type="speak",
            data={"utterance": "Noise from other client."},
            context={"session_id": "other-session-99"}
        )
        self.assertFalse(unrelated_resp.is_correlated_response_to(req))

    def test_successful_mocked_interaction(self):
        """Verifies end-to-end prompt submission with correlated speak event."""
        speak_msg = OVOSMessage(
            type="speak",
            data={"utterance": "The weather is currently 72 degrees and sunny."},
            context={"session_id": "test-sid-1"}
        )
        mock_client = MockBusClient(response_msg=speak_msg)
        bridge = OVOSBridge(self.config, bus_client_factory=lambda: mock_client)

        resp = bridge.submit_prompt(AssistantRequest(prompt="What's the weather?", session_id="test-sid-1"))
        self.assertEqual(resp.status, AssistantStatus.OK)
        self.assertEqual(resp.text, "The weather is currently 72 degrees and sunny.")
        self.assertEqual(resp.route, ProcessingRoute.VERIFIED_LOCAL)
        self.assertEqual(len(mock_client.sent_messages), 1)

    def test_timeout_waiting_for_bus_response(self):
        """Verifies TIMEOUT status when no response is received."""
        mock_client = MockBusClient(response_msg=None)
        bridge = OVOSBridge(self.config, bus_client_factory=lambda: mock_client)

        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello", timeout_seconds=2.0))
        self.assertEqual(resp.status, AssistantStatus.TIMEOUT)
        self.assertIn("timed out", resp.error.lower())

    def test_complete_intent_failure(self):
        """Verifies error reporting when complete_intent_failure event occurs."""
        fail_msg = OVOSMessage(
            type="complete_intent_failure",
            data={},
            context={"session_id": "test-sid-2"}
        )
        mock_client = MockBusClient(response_msg=fail_msg)
        bridge = OVOSBridge(self.config, bus_client_factory=lambda: mock_client)

        resp = bridge.submit_prompt(AssistantRequest(prompt="Unknown phrase", session_id="test-sid-2"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("intent failure", resp.error.lower())

    def test_unavailable_when_offline_and_no_client(self):
        """Verifies UNAVAILABLE status when bus is not running and no client supplied."""
        bridge = OVOSBridge(self.config)
        with patch.object(bridge, "is_available", return_value=False):
            resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
            self.assertEqual(resp.status, AssistantStatus.UNAVAILABLE)
            self.assertIn("not reachable", resp.error.lower())

    @patch("integrations.ovos.bridge.socket.create_connection")
    def test_socket_availability_probe(self, mock_create_conn):
        """Verifies is_available correctly probes socket connect."""
        mock_conn = MagicMock()
        mock_create_conn.return_value.__enter__.return_value = mock_conn

        bridge = OVOSBridge(self.config)
        self.assertTrue(bridge.is_available())

        mock_create_conn.side_effect = ConnectionRefusedError()
        self.assertFalse(bridge.is_available())

    def test_broadcast_notice_in_status(self):
        """Verifies that health status includes broadcast confidentiality warning."""
        bridge = OVOSBridge(self.config)
        status = bridge.get_status()
        self.assertIn("broadcast_notice", status)
        self.assertIn("broadcasts frames", status["broadcast_notice"].lower())

    def test_rejection_of_non_finite_timeout(self):
        """Verifies rejection of NaN and Inf timeout values."""
        bridge = OVOSBridge(self.config)
        resp_nan = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=float("nan")))
        resp_inf = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=float("inf")))
        resp_ninf = bridge.submit_prompt(AssistantRequest(prompt="Hi", timeout_seconds=float("-inf")))
        self.assertEqual(resp_nan.status, AssistantStatus.INVALID_REQUEST)
        self.assertEqual(resp_inf.status, AssistantStatus.INVALID_REQUEST)
        self.assertEqual(resp_ninf.status, AssistantStatus.INVALID_REQUEST)

    def test_spoofed_loopback_url_rejection(self):
        """Verifies that parsed hostname prevents spoofed loopback addresses."""
        spoofed_cfg = OVOSConfig(
            enabled=True,
            bus_url="ws://127.0.0.1.attacker.com:8181/core"
        )
        bridge = OVOSBridge(spoofed_cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("not bound to a loopback address", resp.error.lower())


if __name__ == "__main__":
    unittest.main()
