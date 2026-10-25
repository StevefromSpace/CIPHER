"""Unit and Security Tests for CIPHER Shared Assistant Contract."""

import io
import math
import unittest
import urllib.error
import urllib.request

from integrations.contract import (
    AssistantCapabilities,
    AssistantRequest,
    AssistantResponse,
    AssistantStatus,
    NoRedirectHandler,
    ProcessingRoute,
    ResponseTooLargeError,
    read_bounded,
    safe_urlopen,
    sanitize_sensitive_data,
    validate_assistant_request,
    validate_loopback_url,
)


class TestSharedContract(unittest.TestCase):
    """Test suite for central contract validation, loopback checks, and security handlers."""

    def setUp(self):
        self.local_caps = AssistantCapabilities(
            name="MockLocal",
            version="1.0.0",
            supports_text=True,
            default_route=ProcessingRoute.VERIFIED_LOCAL
        )
        self.cloud_caps = AssistantCapabilities(
            name="MockCloud",
            version="1.0.0",
            supports_text=True,
            default_route=ProcessingRoute.REMOTE_CLOUD
        )
        self.unknown_caps = AssistantCapabilities(
            name="MockUnknown",
            version="1.0.0",
            supports_text=True,
            default_route=ProcessingRoute.UNKNOWN
        )

    # -------------------------------------------------------------------------
    # 1. validate_assistant_request tests
    # -------------------------------------------------------------------------

    def test_valid_request(self):
        req = AssistantRequest(prompt="Hello CIPHER", timeout_seconds=30.0)
        valid, resp = validate_assistant_request(req, self.local_caps)
        self.assertTrue(valid)
        self.assertIsNone(resp)

    def test_invalid_request_type(self):
        valid, resp = validate_assistant_request("not a request", self.local_caps)
        self.assertFalse(valid)
        self.assertEqual(resp.status, AssistantStatus.INVALID_REQUEST)

    def test_blank_or_whitespace_prompt(self):
        for bad_prompt in ("", "   ", "\t\n\r  "):
            req = AssistantRequest(prompt=bad_prompt)
            valid, resp = validate_assistant_request(req, self.local_caps)
            self.assertFalse(valid)
            self.assertEqual(resp.status, AssistantStatus.INVALID_REQUEST)

    def test_timeout_bounds_and_finiteness(self):
        # Non-finite numbers
        for non_finite in (float("nan"), float("inf"), float("-inf")):
            req = AssistantRequest(prompt="Valid", timeout_seconds=non_finite)
            valid, resp = validate_assistant_request(req, self.local_caps)
            self.assertFalse(valid)
            self.assertEqual(resp.status, AssistantStatus.INVALID_REQUEST)

        # Boolean (bool is subclass of int in Python)
        req_bool = AssistantRequest(prompt="Valid", timeout_seconds=True)
        valid, resp = validate_assistant_request(req_bool, self.local_caps)
        self.assertFalse(valid)
        self.assertEqual(resp.status, AssistantStatus.INVALID_REQUEST)

        # Out-of-bounds numbers
        for bad_timeout in (-5.0, 0.0, 0.99, 300.01, 1000.0):
            req = AssistantRequest(prompt="Valid", timeout_seconds=bad_timeout)
            valid, resp = validate_assistant_request(req, self.local_caps)
            self.assertFalse(valid)
            self.assertEqual(resp.status, AssistantStatus.INVALID_REQUEST)

        # Boundary values [1.0, 300.0]
        for good_timeout in (1.0, 30.0, 300.0):
            req = AssistantRequest(prompt="Valid", timeout_seconds=good_timeout)
            valid, resp = validate_assistant_request(req, self.local_caps)
            self.assertTrue(valid)
            self.assertIsNone(resp)

    def test_tool_request_lockdown(self):
        req = AssistantRequest(prompt="Run tool", allow_tools=True)
        valid, resp = validate_assistant_request(req, self.local_caps)
        self.assertFalse(valid)
        self.assertEqual(resp.status, AssistantStatus.REJECTED_TOOLS_ENABLED)

    def test_require_local_route_enforcement(self):
        req_local = AssistantRequest(prompt="Local only", require_local=True)
        # Cloud caps fail
        valid, resp = validate_assistant_request(req_local, self.cloud_caps)
        self.assertFalse(valid)
        self.assertEqual(resp.status, AssistantStatus.REJECTED_CLOUD_MODEL)

        # Unknown caps fail (fails closed)
        valid_unk, resp_unk = validate_assistant_request(req_local, self.unknown_caps)
        self.assertFalse(valid_unk)
        self.assertEqual(resp_unk.status, AssistantStatus.REJECTED_CLOUD_MODEL)

        # require_local=False allows cloud
        req_cloud = AssistantRequest(prompt="Allow cloud", require_local=False)
        valid_ok, resp_ok = validate_assistant_request(req_cloud, self.cloud_caps)
        self.assertTrue(valid_ok)
        self.assertIsNone(resp_ok)

    # -------------------------------------------------------------------------
    # 2. validate_loopback_url tests
    # -------------------------------------------------------------------------

    def test_loopback_url_allowed_schemes(self):
        valid, _ = validate_loopback_url("http://127.0.0.1:8000", allowed_schemes=("http", "https"))
        self.assertTrue(valid)

        valid_ws, _ = validate_loopback_url("ws://127.0.0.1:18789", allowed_schemes=("ws", "wss"))
        self.assertTrue(valid_ws)

        # Disallowed scheme
        valid_ftp, err = validate_loopback_url("ftp://127.0.0.1:8000", allowed_schemes=("http", "https"))
        self.assertFalse(valid_ftp)
        self.assertIn("scheme 'ftp' is not permitted", err)

    def test_loopback_url_hostnames(self):
        for host in ("127.0.0.1", "localhost", "::1"):
            url = f"http://[{host}]:8000" if ":" in host else f"http://{host}:8000"
            valid, err = validate_loopback_url(url, allowed_schemes=("http",))
            self.assertTrue(valid, f"Failed for {url}: {err}")

        # Substring / spoofed loopback addresses must be rejected
        for bad_url in (
            "http://127.0.0.1.attacker.com:8000",
            "http://localhost.evil.org:8000",
            "http://192.168.1.100:8000",
            "http://10.0.0.5:1337",
            "http://example.com:8000",
        ):
            valid, err = validate_loopback_url(bad_url, allowed_schemes=("http",))
            self.assertFalse(valid)
            self.assertIn("not bound to a loopback address", err)

    def test_loopback_url_rejects_user_info(self):
        # Credentials embedded in URL must be rejected
        bad_urls = [
            "http://user:password@127.0.0.1:8000",
            "http://admin:secret@localhost:8000",
            "http://token@127.0.0.1:8000",
            "ws://user:pass@127.0.0.1:8181/core",
        ]
        for url in bad_urls:
            valid, err = validate_loopback_url(url, allowed_schemes=("http", "ws"))
            self.assertFalse(valid)
            self.assertIn("must not contain user credentials (user-info)", err)

    def test_loopback_url_port_validation(self):
        # Specific allowed ports
        valid, _ = validate_loopback_url("http://127.0.0.1:8000", allowed_schemes=("http",), allowed_ports=(8000, 8080))
        self.assertTrue(valid)

        valid_unallowed_port, err = validate_loopback_url("http://127.0.0.1:9999", allowed_schemes=("http",), allowed_ports=(8000, 8080))
        self.assertFalse(valid_unallowed_port)
        self.assertIn("port '9999' is not permitted", err)

        # Invalid port syntax
        valid_bad, err = validate_loopback_url("http://127.0.0.1:notaport", allowed_schemes=("http",))
        self.assertFalse(valid_bad)
        self.assertIn("invalid port", err.lower())

    # -------------------------------------------------------------------------
    # 3. NoRedirectHandler & safe_urlopen tests
    # -------------------------------------------------------------------------

    def test_no_redirect_handler_blocks_all_codes(self):
        handler = NoRedirectHandler()
        req = urllib.request.Request("http://127.0.0.1:8000")

        for code in (301, 302, 303, 307, 308):
            with self.assertRaises(urllib.error.HTTPError) as ctx:
                handler.redirect_request(req, None, code, "Redirect", {"Location": "http://evil.com"}, "http://evil.com")
            self.assertEqual(ctx.exception.code, code)
            self.assertIn("blocked for security", str(ctx.exception))
            try:
                ctx.exception.close()
            except Exception:
                pass

    # -------------------------------------------------------------------------
    # 4. sanitize_sensitive_data tests
    # -------------------------------------------------------------------------

    def test_sanitize_sensitive_data(self):
        raw = "Error connecting to http://admin:supersecret123@127.0.0.1:8000 with bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9 and x-api-key sk-1234567890abcdef"
        sanitized = sanitize_sensitive_data(raw)
        self.assertNotIn("supersecret123", sanitized)
        self.assertNotIn("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9", sanitized)
        self.assertNotIn("sk-1234567890abcdef", sanitized)
        self.assertIn("[REDACTED]", sanitized)

    # -------------------------------------------------------------------------
    # 5. read_bounded tests
    # -------------------------------------------------------------------------

    def test_read_bounded_exactly_at_limit(self):
        payload = b"X" * 100
        stream = io.BytesIO(payload)
        result = read_bounded(stream, max_bytes=100)
        self.assertEqual(result, payload)

    def test_read_bounded_under_limit(self):
        payload = b"X" * 42
        stream = io.BytesIO(payload)
        result = read_bounded(stream, max_bytes=100)
        self.assertEqual(result, payload)

    def test_read_bounded_one_byte_over_limit_aborts_and_closes(self):
        payload = b"X" * 101  # exactly 1 byte over 100

        class TrackingStream(io.BytesIO):
            def __init__(self, data):
                super().__init__(data)
                self.close_called = False
                self.bytes_read_total = 0

            def read(self, size=-1):
                chunk = super().read(size)
                self.bytes_read_total += len(chunk)
                return chunk

            def close(self):
                self.close_called = True
                super().close()

        tracker = TrackingStream(payload)
        with self.assertRaises(ResponseTooLargeError) as ctx:
            read_bounded(tracker, max_bytes=100, chunk_size=32)

        self.assertIn("exceeded limit of 100 bytes", str(ctx.exception))
        # Verify close/abort was invoked
        self.assertTrue(tracker.close_called)
        # Verify consumption stopped at limit + 1 byte (101 bytes) and didn't read arbitrarily
        self.assertEqual(tracker.bytes_read_total, 101)

    def test_read_bounded_massive_stream_stops_immediately(self):
        """Verifies that on a 10MB stream with a 1KB limit, only 1001 bytes are read."""
        large_payload = b"A" * (10 * 1024 * 1024)

        class CountingStream(io.BytesIO):
            def __init__(self, data):
                super().__init__(data)
                self.bytes_consumed = 0
                self.close_called = False

            def read(self, size=-1):
                chunk = super().read(size)
                self.bytes_consumed += len(chunk)
                return chunk

            def close(self):
                self.close_called = True
                super().close()

        counter = CountingStream(large_payload)
        with self.assertRaises(ResponseTooLargeError):
            read_bounded(counter, max_bytes=1000, chunk_size=256)

        self.assertTrue(counter.close_called)
        # Consumed at most 1001 bytes (limit + 1 detection byte), NOT the 10MB!
        self.assertEqual(counter.bytes_consumed, 1001)

    def test_read_bounded_chunked_incremental(self):
        payload = b"0123456789" * 10  # 100 bytes
        stream = io.BytesIO(payload)
        # Chunk size of 7 bytes -> requires 15 chunk reads
        result = read_bounded(stream, max_bytes=100, chunk_size=7)
        self.assertEqual(result, payload)

    def test_read_bounded_short_reads_and_eof(self):
        payload = b"short"
        stream = io.BytesIO(payload)
        result = read_bounded(stream, max_bytes=1000, chunk_size=64)
        self.assertEqual(result, payload)

    def test_read_bounded_stream_exception_propagates(self):
        class BrokenStream:
            def read(self, size=-1):
                raise OSError("Socket connection reset by peer")

        with self.assertRaises(OSError) as ctx:
            read_bounded(BrokenStream(), max_bytes=100)
        self.assertIn("Socket connection reset", str(ctx.exception))

    def test_read_bounded_negative_limit(self):
        with self.assertRaises(ValueError):
            read_bounded(io.BytesIO(b"data"), max_bytes=-1)

    def test_read_bounded_invalid_stream_type(self):
        with self.assertRaises(TypeError):
            read_bounded("not a stream", max_bytes=100)

    def test_read_bounded_error_response_body(self):
        """Verifies bounded reading works on urllib HTTPError fp streams."""
        error_content = b'{"error": "Unauthorized", "detail": "Invalid token"}'
        err = urllib.error.HTTPError(
            url="http://127.0.0.1:8000/api",
            code=401,
            msg="Unauthorized",
            hdrs={},
            fp=io.BytesIO(error_content)
        )
        try:
            body = read_bounded(err.fp, max_bytes=1024)
            self.assertEqual(body, error_content)
        finally:
            err.close()

        # Oversized error body should also be bounded
        huge_err_content = b"ERROR " * 1000
        err_huge = urllib.error.HTTPError(
            url="http://127.0.0.1:8000/api",
            code=500,
            msg="Internal Server Error",
            hdrs={},
            fp=io.BytesIO(huge_err_content)
        )
        try:
            with self.assertRaises(ResponseTooLargeError):
                read_bounded(err_huge.fp, max_bytes=100)
        finally:
            err_huge.close()


if __name__ == "__main__":
    unittest.main()
