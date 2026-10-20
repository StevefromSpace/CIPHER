"""Unit and Security Tests for OpenVoiceOS Bridge Adapter.

Verifies:
1. Disabled state handling.
2. Blank prompt rejection fail-closed.
3. Timeout range bounds (1 to 300s).
4. Tool execution rejection (allow_tools=True, metadata flags).
5. Safety boundary: live skill dispatch is disabled and unavailable (mock-only assistant submission).
6. Loopback binding enforcement and user-info credential rejection.
7. Message envelope construction.
8. Explicit response correlation via session_id and response event types.
9. Rejection of uncorrelated responses (session mismatch, missing session, wrong event type).
10. Successful mocked speech event resolution.
11. Timeout waiting for bus response.
12. Intent failure handling.
13. Socket availability probe for voice/event bus.
14. Broadcast confidentiality guard: best-effort pattern detection rejects sensitive prompts.
15. Provenance route defaults to UNKNOWN unless verified.
16. is_mock_only capability is True for assistant text prompts.
"""

import json
import socket
import unittest
from typing import Optional
from unittest.mock import MagicMock, patch

from integrations.contract import (
    AssistantCapabilities,
    AssistantRequest,
    AssistantResponse,
    AssistantStatus,
    ProcessingRoute,
)
from integrations.ovos.bridge import MockBusClient, OVOSBridge
from integrations.ovos.config import OVOSConfig
from integrations.ovos.message import OVOSMessage


class TestOVOSBridge(unittest.TestCase):
    """Test suite covering OpenVoiceOS integration constraints and event handling."""

    def setUp(self):
        self.config = OVOSConfig(
            enabled=True,
            bus_url="ws://127.0.0.1:8181/core",
            language="en-us",
            timeout_seconds=10.0,
            strict_local=True,
            verified_local=True,
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
        """Verifies allow_tools=True and metadata tool actions are rejected fail-closed."""
        bridge = OVOSBridge(self.config)
        resp1 = bridge.submit_prompt(AssistantRequest(prompt="Hi", allow_tools=True))
        resp2 = bridge.submit_prompt(AssistantRequest(prompt="Hi", metadata={"shell": True}))
        resp3 = bridge.submit_prompt(AssistantRequest(prompt="Hi", metadata={"computer_use": True}))
        self.assertEqual(resp1.status, AssistantStatus.REJECTED_TOOLS_ENABLED)
        self.assertEqual(resp2.status, AssistantStatus.REJECTED_TOOLS_ENABLED)
        self.assertEqual(resp3.status, AssistantStatus.REJECTED_TOOLS_ENABLED)

    def test_safety_boundary_live_skill_dispatch_unavailable(self):
        """Verifies that live skill execution without a mock client is UNAVAILABLE fail-closed."""
        bridge = OVOSBridge(self.config)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Tell me a joke"))
        self.assertEqual(resp.status, AssistantStatus.UNAVAILABLE)
        self.assertIn("destination: ['skills']", resp.error)
        self.assertIn("mock-only", resp.error)

    def test_broadcast_confidentiality_sensitive_prompt_rejection(self):
        """Verifies best-effort detection heuristic rejects prompts containing sensitive credentials."""
        bridge = OVOSBridge(self.config)
        sensitive_prompt = "My api_key: sk-ant-api03-1234567890abcdef please save it"
        resp = bridge.submit_prompt(AssistantRequest(prompt=sensitive_prompt))
        self.assertEqual(resp.status, AssistantStatus.INVALID_REQUEST)
        self.assertIn("best-effort detection", resp.error.lower())

    def test_loopback_binding_enforcement(self):
        """Verifies rejection of non-loopback URLs."""
        remote_cfg = OVOSConfig(
            enabled=True,
            bus_url="ws://10.200.1.1:8181/core",
            verified_local=True,
        )
        bridge = OVOSBridge(remote_cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("not bound to a loopback address", resp.error.lower())

    def test_userinfo_credentials_in_url_rejected(self):
        """Verifies rejection of URLs containing credentials."""
        cred_cfg = OVOSConfig(
            enabled=True,
            bus_url="ws://user:secret@127.0.0.1:8181/core",
            verified_local=True,
        )
        bridge = OVOSBridge(cred_cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hi"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("user credentials", resp.error.lower())

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
        self.assertEqual(msg.context["destination"], ["skills"])

        json_str = msg.to_json()
        parsed = OVOSMessage.from_json(json_str)
        self.assertEqual(parsed.type, msg.type)
        self.assertEqual(parsed.context["session_id"], "session-xyz-123")

    def test_explicit_response_correlation_logic(self):
        """Verifies strict explicit correlation rules in OVOSMessage."""
        req = OVOSMessage.create_utterance("Test", session_id="session-42")

        # 1. Matching session and valid event type -> correlated
        for valid_type in ("speak", "ovos.utterance.handled", "complete_intent_failure"):
            matching_resp = OVOSMessage(
                type=valid_type,
                data={"utterance": "Intent handled."},
                context={"session_id": "session-42"}
            )
            self.assertTrue(matching_resp.is_correlated_response_to(req))

        # 2. Mismatched session_id -> uncorrelated
        diff_session_resp = OVOSMessage(
            type="speak",
            data={"utterance": "Noise."},
            context={"session_id": "other-session-99"}
        )
        self.assertFalse(diff_session_resp.is_correlated_response_to(req))

        # 3. Missing session_id -> uncorrelated
        missing_session_resp = OVOSMessage(
            type="speak",
            data={"utterance": "No session."},
            context={}
        )
        self.assertFalse(missing_session_resp.is_correlated_response_to(req))

        # 4. Wrong event type -> uncorrelated
        wrong_type_resp = OVOSMessage(
            type="skill.internal.event",
            data={},
            context={"session_id": "session-42"}
        )
        self.assertFalse(wrong_type_resp.is_correlated_response_to(req))

        # 5. Invalid request object -> uncorrelated
        self.assertFalse(matching_resp.is_correlated_response_to(None))

    def test_uncorrelated_response_rejected_in_bridge(self):
        """Verifies bridge rejects responses with mismatched session_id fail-closed."""
        # Response has mismatched session_id
        wrong_resp = OVOSMessage(
            type="speak",
            data={"utterance": "Unrelated reply."},
            context={"session_id": "mismatched-session"}
        )
        mock_client = MockBusClient(response_msg=wrong_resp)
        bridge = OVOSBridge(self.config, bus_client_factory=lambda: mock_client)

        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello", session_id="expected-session"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("uncorrelated ovos response", resp.error.lower())

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

    @patch("integrations.ovos.bridge.socket.create_connection")
    def test_socket_availability_probe(self, mock_create_conn):
        """Verifies is_available correctly probes socket connect for event bus."""
        mock_conn = MagicMock()
        mock_create_conn.return_value.__enter__.return_value = mock_conn

        bridge = OVOSBridge(self.config)
        self.assertTrue(bridge.is_available())

        mock_create_conn.side_effect = ConnectionRefusedError()
        self.assertFalse(bridge.is_available())

    def test_confidentiality_notice_in_status(self):
        """Verifies that health status includes confidentiality and safety boundary notices."""
        bridge = OVOSBridge(self.config)
        status = bridge.get_status()
        self.assertIn("safety_boundary", status)
        self.assertIn("confidentiality_notice", status)
        self.assertIn("best-effort", status["confidentiality_notice"].lower())

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
            bus_url="ws://127.0.0.1.attacker.com:8181/core",
            verified_local=True,
        )
        bridge = OVOSBridge(spoofed_cfg)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.ERROR)
        self.assertIn("not bound to a loopback address", resp.error.lower())

    def test_route_provenance_unknown_by_default(self):
        """Verifies route provenance defaults to UNKNOWN and fails closed under require_local=True."""
        unverified_cfg = OVOSConfig(
            enabled=True,
            bus_url="ws://127.0.0.1:8181/core",
            verified_local=False,
        )
        bridge = OVOSBridge(unverified_cfg)
        self.assertEqual(bridge.capabilities.default_route, ProcessingRoute.UNKNOWN)

        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello", require_local=True))
        self.assertEqual(resp.status, AssistantStatus.REJECTED_CLOUD_MODEL)
        self.assertIn("unknown", resp.error.lower())

    def test_is_mock_only_capability(self):
        """Verifies is_mock_only capability is True for assistant prompt submission."""
        bridge = OVOSBridge(self.config)
        self.assertTrue(bridge.capabilities.is_mock_only)

    def test_injected_live_like_client_factory_rejected(self):
        """Verifies that injecting a live or non-mock client factory fails closed and prevents dispatch."""
        class LiveLikeBusClient:
            def __init__(self):
                self.dispatched = False

            def send_and_wait(self, msg, timeout=5.0):
                self.dispatched = True
                return None

        live_client = LiveLikeBusClient()
        bridge = OVOSBridge(self.config, bus_client_factory=lambda: live_client)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello live bus"))
        self.assertEqual(resp.status, AssistantStatus.UNAVAILABLE)
        self.assertIn("live ovos bus dispatch rejected", resp.error.lower())
        self.assertFalse(live_client.dispatched, "Live-like client must NOT receive dispatch")

    def test_allow_live_skill_dispatch_without_verified_route_fails_closed(self):
        """Verifies enabling allow_live_skill_dispatch without verified_non_action_route fails closed."""
        cfg = OVOSConfig(
            enabled=True,
            verified_local=True,
            allow_live_skill_dispatch=True,
            verified_non_action_route=False
        )
        mock_client = MockBusClient()
        bridge = OVOSBridge(cfg, bus_client_factory=lambda: mock_client)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Hello"))
        self.assertEqual(resp.status, AssistantStatus.UNAVAILABLE)
    def test_spoofed_marker_client_rejected(self):
        """Verifies a live-like client spoofing is_mock=True or _is_cipher_mock=True is rejected fail-closed with 0 messages sent."""
        class SpoofedLiveClient:
            is_mock = True
            _is_cipher_mock = True
            is_mock_client = True

            def __init__(self):
                self.sent_messages = []
                self.dispatched = False

            def send_and_wait(self, msg, timeout=5.0):
                self.dispatched = True
                self.sent_messages.append(msg)
                return None

        spoofed = SpoofedLiveClient()
        bridge = OVOSBridge(self.config, bus_client_factory=lambda: spoofed)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Attempt bypass with spoofed marker"))

        self.assertEqual(resp.status, AssistantStatus.UNAVAILABLE)
        self.assertIn("live ovos bus dispatch rejected", resp.error.lower())
        self.assertIn("spoofing mock markers", resp.error.lower())
        self.assertFalse(spoofed.dispatched, "Live client must NOT receive dispatch")
        self.assertEqual(len(spoofed.sent_messages), 0, "Zero messages must be sent to spoofed client")

    def test_subclassed_spoofed_mock_rejected(self):
        """Verifies that subclassing MockBusClient cannot bypass exact type check."""
        class SubclassedMock(MockBusClient):
            def __init__(self):
                super().__init__()
                self.dispatched = False

            def send_and_wait(self, msg, timeout=5.0):
                self.dispatched = True
                return super().send_and_wait(msg, timeout)

        subclassed = SubclassedMock()
        bridge = OVOSBridge(self.config, bus_client_factory=lambda: subclassed)
        resp = bridge.submit_prompt(AssistantRequest(prompt="Attempt subclass bypass"))

        self.assertEqual(resp.status, AssistantStatus.UNAVAILABLE)
        self.assertFalse(subclassed.dispatched)
        self.assertEqual(len(subclassed.sent_messages), 0)

    def test_status_reports_operator_assertion(self):
        """Verifies get_status explicitly declares verified_local as operator assertion."""
        bridge = OVOSBridge(self.config)
        status = bridge.get_status()
        self.assertIn("verified_local_status", status)
        self.assertIn("operator_assertion", status["verified_local_status"])


if __name__ == "__main__":
    unittest.main()
