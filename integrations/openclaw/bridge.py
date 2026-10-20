"""CIPHER — OpenClaw Assistant Bridge Adapter.

Implements the request/response bridge via the supported OpenClaw CLI interface,
enforcing local-first isolation, configurable timeouts, cloud rejection, tool execution lockdown,
and error containment.
"""

from datetime import datetime
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import threading
import time
from typing import Any, Dict, List, Optional, Tuple

from integrations.contract import (
    AssistantCapabilities,
    AssistantRequest,
    AssistantResponse,
    AssistantStatus,
    BaseAssistantBackend,
    ProcessingRoute,
    ResponseTooLargeError,
    read_bounded,
    sanitize_sensitive_data,
    validate_assistant_request,
)
from .config import OpenClawConfig


class OpenClawBridge(BaseAssistantBackend):
    """Optional bridge connecting CIPHER to a local OpenClaw assistant."""

    def __init__(self, config: Optional[OpenClawConfig] = None):
        self.config = config or OpenClawConfig()
        self._resolved_cli_path: Optional[Path] = None

    @property
    def capabilities(self) -> AssistantCapabilities:
        """Declares OpenClaw bridge capabilities and provenance constraints.

        Route provenance:
        OpenClaw runs as an external Node.js agent. Model name prefixes like 'ollama/'
        do not constitute cryptographic proof of local inference. Route remains UNKNOWN
        unless config.verified_local is explicitly enabled after in-process verification.
        """
        route = ProcessingRoute.VERIFIED_LOCAL if self.config.verified_local else ProcessingRoute.UNKNOWN
        return AssistantCapabilities(
            name="OpenClaw",
            version="1.0.0",
            supports_text=True,
            supports_streaming=False,
            supports_sessions=True,
            supports_voice_events=False,
            supports_tools=False,  # Locked down in CIPHER
            default_route=route,
            is_mock_only=False,
        )

    def _resolve_cli(self) -> Optional[Path]:
        """Locates the openclaw executable path safely."""
        if self.config.cli_path and self.config.cli_path.exists():
            return self.config.cli_path

        found = shutil.which("openclaw")
        if found:
            return Path(found)

        # Common Windows npm global / local paths
        if os.name == "nt":
            appdata = os.environ.get("APPDATA")
            if appdata:
                npm_cmd = Path(appdata) / "npm" / "openclaw.cmd"
                if npm_cmd.exists():
                    return npm_cmd

        return None

    def _check_cli_version(self, cli: Path) -> Tuple[bool, Optional[str]]:
        """Probes and verifies the OpenClaw CLI version against supported upstream versions."""
        if not cli or not cli.exists():
            return False, "OpenClaw binary is not installed."
        try:
            res = subprocess.run(
                [str(cli), "--version"],
                capture_output=True,
                text=True,
                timeout=5.0,
                shell=False
            )
            if res.returncode != 0:
                err_msg = res.stderr.strip() or res.stdout.strip() or f"exit code {res.returncode}"
                return False, f"OpenClaw --version probe failed: {err_msg}"
            version_str = res.stdout.strip()
            ok, msg = self.config.is_cli_version_supported(version_str)
            if not ok:
                return False, msg
            return True, version_str
        except Exception as ex:
            return False, f"Exception while probing OpenClaw version: {ex}"

    def is_available(self) -> bool:
        """Checks if OpenClaw CLI is installed and runnable with a supported upstream version."""
        if not self.config.enabled:
            return False

        cli = self._resolve_cli()
        if not cli:
            return False

        ok, _ = self._check_cli_version(cli)
        return ok

    def get_status(self) -> Dict[str, Any]:
        """Returns sanitized status and health information."""
        cli = self._resolve_cli()
        cli_compatible = False
        cli_version = None
        if cli:
            try:
                ver_ok, ver_val = self._check_cli_version(cli)
                cli_compatible = ver_ok
                cli_version = ver_val if ver_ok else None
            except Exception:
                cli_compatible = False

        # Mark effective runtime enforcement as "unverified" if compatible CLI is unavailable,
        # or live execution / verified local has not been established.
        effective_enforcement = (
            "verified"
            if (cli and cli_compatible and self.config.enable_live_execution and self.config.verified_local)
            else "unverified"
        )

        return {
            "name": "OpenClaw",
            "enabled": self.config.enabled,
            "installed": cli is not None,
            "cli_path": str(cli) if cli else None,
            "cli_version": cli_version,
            "cli_compatible": cli_compatible,
            "gateway_url": self.config.gateway_url,
            "default_model": self.config.default_model,
            "strict_local": self.config.strict_local,
            "tool_profile": self.config.tool_profile,
            "timeout_seconds": self.config.timeout_seconds,
            "route": self.capabilities.default_route.value,
            "verified_local_status": "operator_assertion (loopback URL compliance per contract, unverified hardware airgap)",
            "enable_live_execution": self.config.enable_live_execution,
            "execution_mode": self.config.execution_mode,
            "live_enforcement": effective_enforcement,
        }

    def _sanitize_error(self, message: str) -> str:
        """Strips potential credentials, tokens, or private paths from error output."""
        return sanitize_sensitive_data(message)

    def _execute_bounded_process(
        self,
        cmd: List[str],
        env: Dict[str, str],
        timeout: float,
        max_bytes: int
    ) -> Tuple[int, bytes, bytes, Optional[str]]:
        """Executes a subprocess with bounded stream collection and immediate overflow termination.

        Enforces output byte limits during collection by reading in chunks with read_bounded.
        If stdout or stderr exceeds max_bytes, the subprocess is terminated and killed
        immediately from the reader thread, and the main thread breaks instantly without
        waiting for the timeout.
        """
        proc = subprocess.Popen(
            cmd,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
            shell=False
        )

        stdout_chunks: List[bytes] = []
        stderr_chunks: List[bytes] = []
        stream_errors: List[Exception] = []
        overflow_event = threading.Event()
        error_event = threading.Event()

        def _kill_proc_immediately():
            try:
                proc.terminate()
            except Exception:
                pass
            time.sleep(0.02)
            if proc.poll() is None:
                try:
                    proc.kill()
                except Exception:
                    pass

        def _reader(stream, storage, is_stdout: bool):
            try:
                storage.append(read_bounded(stream, max_bytes=max_bytes))
            except ResponseTooLargeError as ex:
                stream_errors.append(ex)
                overflow_event.set()
                _kill_proc_immediately()
            except (BrokenPipeError, ConnectionResetError):
                pass
            except Exception as ex:
                stream_errors.append(ex)
                error_event.set()
                _kill_proc_immediately()

        t_out = threading.Thread(target=_reader, args=(proc.stdout, stdout_chunks, True))
        t_err = threading.Thread(target=_reader, args=(proc.stderr, stderr_chunks, False))
        t_out.daemon = True
        t_err.daemon = True
        t_out.start()
        t_err.start()

        start_t = time.perf_counter()
        timed_out = False

        try:
            # Responsive polling loop: react immediately to overflow or error events
            while True:
                if overflow_event.is_set():
                    _kill_proc_immediately()
                    break
                if error_event.is_set():
                    _kill_proc_immediately()
                    break

                if proc.poll() is not None:
                    break

                elapsed = time.perf_counter() - start_t
                if elapsed >= timeout:
                    timed_out = True
                    _kill_proc_immediately()
                    break

                time.sleep(0.01)

            try:
                proc.wait(timeout=0.5)
            except Exception:
                _kill_proc_immediately()
                try:
                    proc.wait(timeout=0.5)
                except Exception:
                    pass

            t_out.join(timeout=0.5)
            t_err.join(timeout=0.5)

            if timed_out:
                return -1, b"", b"", f"OpenClaw execution timed out after {timeout:.1f}s."

            if overflow_event.is_set():
                return -1, b"", b"", f"OpenClaw output exceeded maximum permitted byte limit ({max_bytes} bytes)."

            if error_event.is_set() or stream_errors:
                err = stream_errors[0] if stream_errors else "Stream read failed"
                return -1, b"", b"", f"I/O error reading process streams: {err}"

            out_b = stdout_chunks[0] if stdout_chunks else b""
            err_b = stderr_chunks[0] if stderr_chunks else b""
            return proc.returncode, out_b, err_b, None
        finally:
            _kill_proc_immediately()
            try:
                if proc.stdout:
                    proc.stdout.close()
            except Exception:
                pass
            try:
                if proc.stderr:
                    proc.stderr.close()
            except Exception:
                pass

    def _verify_empty_tool_inventory(self, cli: Path, env: Optional[Dict[str, str]] = None) -> Tuple[bool, Optional[str]]:
        """Verifies that the effective tool inventory is confirmed empty before prompt submission.

        In OpenClaw, client-side config cannot be assumed to restrict an already-running
        Gateway daemon. Furthermore, embedded execution must verifiably resolve to an
        empty tool inventory. If the CLI, supported version, or runtime cannot prove zero tools, fail closed.

        Guarantees:
        1. Verifies that the installed CLI exists and is runnable.
        2. Verifies CLI version against supported upstream CLI versions.
        3. Executes probe command `openclaw tools list --json`.
        4. Parses JSON output strictly:
           - If output is not valid JSON, reject as malformed schema.
           - If output is a list: must be empty (`len(tools_data) == 0`). Non-empty list rejected.
           - If output is a dict: must have 'tools' key, and 'tools' MUST be a list, and `len(tools_data["tools"]) == 0`.
             A missing 'tools' property is strictly rejected as unexpected/malformed schema (NOT treated as empty inventory).
           - Any other data type (string, int, bool, None) is rejected as unexpected schema.
        5. If probe fails or cannot be verified, fail closed.
        """
        # If no compatible binary is installed, tool inventory cannot be proven
        if not cli or not cli.exists():
            return False, "OpenClaw binary is not installed; effective tool inventory cannot be verified."

        # Verify upstream CLI version compatibility first
        ver_ok, ver_err = self._check_cli_version(cli)
        if not ver_ok:
            return False, f"Incompatible or unverified OpenClaw CLI version: {ver_err}"

        # Probe the tool inventory via CLI
        try:
            probe_cmd = [str(cli), "tools", "list", "--json"]
            probe_env = env or os.environ.copy()
            ret, stdout_b, stderr_b, err_msg = self._execute_bounded_process(
                cmd=probe_cmd,
                env=probe_env,
                timeout=5.0,
                max_bytes=65536
            )
            if ret != 0 or err_msg:
                detail = err_msg or stderr_b.decode("utf-8", errors="replace")[:200]
                return False, f"OpenClaw tools list probe failed (code {ret}): {detail}"

            raw_text = stdout_b.decode("utf-8", errors="replace").strip()
            if not raw_text:
                return False, "OpenClaw tools list returned empty output; expected JSON schema."

            try:
                tools_data = json.loads(raw_text)
            except (json.JSONDecodeError, ValueError) as ex:
                return False, f"Malformed response schema from OpenClaw tools list (invalid JSON): {ex}"

            # Strict response schema verification:
            # 1. Output is a list
            if isinstance(tools_data, list):
                if len(tools_data) == 0:
                    return True, None
                return False, f"Effective tool inventory is not empty: found {len(tools_data)} registered tools: {tools_data}."

            # 2. Output is a dict with 'tools' list
            if isinstance(tools_data, dict):
                # Reject if missing "tools" key -- do NOT treat missing "tools" as empty inventory!
                if "tools" not in tools_data:
                    return False, (
                        "Unexpected response schema from OpenClaw tools list: missing 'tools' property. "
                        f"Response keys: {list(tools_data.keys())}."
                    )
                tools_list = tools_data["tools"]
                if not isinstance(tools_list, list):
                    return False, (
                        f"Malformed response schema from OpenClaw tools list: 'tools' property is not a list "
                        f"(found type '{type(tools_list).__name__}')."
                    )
                if len(tools_list) == 0:
                    return True, None
                return False, f"Effective tool inventory is not empty: found {len(tools_list)} tools in 'tools' property: {tools_list}."

            return False, (
                f"Unexpected response schema from OpenClaw tools list: expected JSON object or array, "
                f"got {type(tools_data).__name__}."
            )
        except Exception as ex:
            return False, f"Exception while probing OpenClaw tool inventory: {ex}"

    def submit_prompt(self, request: AssistantRequest) -> AssistantResponse:
        """Submits a single text prompt to OpenClaw and returns the structured response.

        Guarantees:
        - Central policy validation (blank prompts, timeout bounds, tool lockdown, local route).
        - Rejects immediately if integration is disabled.
        - Fails closed if live execution is not enabled under CIPHER safety policy.
        - Verifies tool policy restricts tool execution (profile == 'minimal', deny == ['*']).
        - Verifies that effective tool inventory is empty before prompt submission.
        - Rejects cloud models before executing any external process.
        - Enforces strict process timeouts and immediate overflow kill.
        - Parses responses into a standardized, decoupled AssistantResponse.
        """
        start_time = time.perf_counter()

        # 1. Gate: Check if integration is enabled
        if not self.config.enabled:
            return AssistantResponse(
                status=AssistantStatus.DISABLED,
                error="OpenClaw integration is disabled in CIPHER configuration (CIPHER_OPENCLAW_ENABLED=0).",
                route=self.capabilities.default_route,
            )

        # 2. Central policy validation gate
        is_valid, validation_resp = validate_assistant_request(request, self.capabilities)
        if not is_valid and validation_resp:
            return validation_resp

        # 3. Gate: Loopback network check
        is_loopback, loopback_err = self.config.validate_gateway_url()
        if not is_loopback:
            return AssistantResponse(
                status=AssistantStatus.ERROR,
                error=f"Security violation: {loopback_err}",
                route=self.capabilities.default_route,
            )

        # 4. Gate: Tool policy verification
        is_tool_safe, tool_err = self.config.is_tool_policy_safe()
        if not is_tool_safe:
            return AssistantResponse(
                status=AssistantStatus.REJECTED_TOOLS_ENABLED,
                error=f"Tool Policy Rejection: {tool_err}",
                route=self.capabilities.default_route,
            )

        # 5. Gate: Model selection and local-first verification
        model = request.model_override or self.config.default_model
        is_local, rejection_reason = self.config.is_model_local(model)
        if not is_local:
            return AssistantResponse(
                status=AssistantStatus.REJECTED_CLOUD_MODEL,
                model=model,
                route=ProcessingRoute.REMOTE_CLOUD,
                error=f"Local-First Policy Rejection: {rejection_reason}",
            )

        # 6. Gate: Live execution enablement under safety policy
        if not self.config.enable_live_execution:
            duration_ms = (time.perf_counter() - start_time) * 1000.0
            return AssistantResponse(
                status=AssistantStatus.UNAVAILABLE,
                model=model,
                route=self.capabilities.default_route,
                duration_ms=duration_ms,
                error=(
                    "OpenClaw live execution is disabled under CIPHER safety policy (enable_live_execution=False). "
                    "Gateway-mediated execution cannot guarantee client-side tool policy enforcement on the gateway, "
                    "and embedded execution mode with empty tool inventory is unverified."
                ),
            )

        # 7. Gate: CLI executable presence
        cli = self._resolve_cli()
        if not cli:
            return AssistantResponse(
                status=AssistantStatus.UNAVAILABLE,
                model=model,
                route=self.capabilities.default_route,
                error=(
                    "OpenClaw CLI executable not found on PATH or configured location. "
                    "Live enforcement is unverified without an installed, compatible OpenClaw binary."
                ),
            )


        # 8. Configure isolated environment and build strict upstream configuration
        env = os.environ.copy()
        temp_dir_obj = None
        try:
            if self.config.enforce_isolated_config:
                temp_dir_obj = tempfile.TemporaryDirectory(prefix="cipher_openclaw_")
                temp_dir = Path(temp_dir_obj.name)
                config_file = temp_dir / "openclaw.json"
                state_dir = temp_dir / "state"
                state_dir.mkdir(parents=True, exist_ok=True)

                # Upstream strict schema: tools object only (no invalid gateway.url key)
                isolated_conf = {
                    "tools": {
                        "profile": self.config.tool_profile,
                        "deny": list(self.config.tool_deny_patterns)
                    }
                }
                with open(config_file, "w", encoding="utf-8") as f:
                    json.dump(isolated_conf, f)

                env["OPENCLAW_CONFIG_PATH"] = str(config_file)
                env["OPENCLAW_STATE_DIR"] = str(state_dir)
                env["OPENCLAW_GATEWAY_URL"] = self.config.gateway_url
                env["OPENCLAW_PROFILE"] = self.config.tool_profile

            # 9. Gate: Effective tool inventory verification
            is_empty_inv, inv_err = self._verify_empty_tool_inventory(cli, env)
            if not is_empty_inv:
                return AssistantResponse(
                    status=AssistantStatus.UNAVAILABLE,
                    model=model,
                    route=self.capabilities.default_route,
                    error=f"Tool Isolation Blocker: {inv_err}",
                )

            # 10. Build CLI command arguments (literal array, no shell execution)
            timeout = request.timeout_seconds if request.timeout_seconds is not None else self.config.timeout_seconds
            if self.config.execution_mode == "agent-exec":
                cmd: List[str] = [
                    str(cli),
                    "agent",
                    "exec",
                    "--message", request.prompt,
                    "--model", model,
                    "--timeout", str(int(timeout)),
                    "--profile", self.config.tool_profile,
                    "--json"
                ]
            else:
                cmd = [
                    str(cli),
                    "agent",
                    "--message", request.prompt,
                    "--model", model,
                    "--timeout", str(int(timeout)),
                    "--profile", self.config.tool_profile,
                    "--gateway", self.config.gateway_url,
                    "--json"
                ]

            if request.session_id:
                cmd.extend(["--session-id", request.session_id])

            retcode, stdout_b, stderr_b, err_msg = self._execute_bounded_process(
                cmd=cmd,
                env=env,
                timeout=timeout,
                max_bytes=self.config.max_output_bytes,
            )

            duration_ms = (time.perf_counter() - start_time) * 1000.0

            if err_msg:
                if "timed out" in err_msg.lower():
                    status = AssistantStatus.TIMEOUT
                else:
                    status = AssistantStatus.ERROR
                return AssistantResponse(
                    status=status,
                    model=model,
                    route=self.capabilities.default_route,
                    duration_ms=duration_ms,
                    error=err_msg,
                )

            if retcode != 0:
                raw_err = self._sanitize_error(
                    stderr_b.decode("utf-8", errors="replace") or stdout_b.decode("utf-8", errors="replace")
                )
                lower_err = raw_err.lower()

                # Classify connection vs authentication vs general error
                if "econnrefused" in lower_err or "connect" in lower_err or "offline" in lower_err or "unreachable" in lower_err:
                    status = AssistantStatus.CONNECTION_ERROR
                elif "auth" in lower_err or "unauthorized" in lower_err or "forbidden" in lower_err:
                    status = AssistantStatus.AUTH_ERROR
                else:
                    status = AssistantStatus.ERROR

                return AssistantResponse(
                    status=status,
                    model=model,
                    route=self.capabilities.default_route,
                    duration_ms=duration_ms,
                    error=f"OpenClaw execution failed (exit code {retcode}): {raw_err[:300]}",
                    raw_output=raw_err
                )

            # 9. Parse JSON output
            raw_out = stdout_b.decode("utf-8", errors="replace").strip()
            if not raw_out:
                return AssistantResponse(
                    status=AssistantStatus.ERROR,
                    model=model,
                    route=self.capabilities.default_route,
                    duration_ms=duration_ms,
                    error="OpenClaw returned empty output.",
                    raw_output=raw_out
                )

            try:
                # Find JSON block in output if wrapped in prose
                data = None
                json_match = re.search(r'(\{.*\})', raw_out, re.DOTALL)
                if json_match:
                    data = json.loads(json_match.group(1))
                else:
                    data = json.loads(raw_out)

                reply_text = ""
                if isinstance(data, dict):
                    if "reply" in data and isinstance(data["reply"], str):
                        reply_text = data["reply"]
                    elif "text" in data and isinstance(data["text"], str):
                        reply_text = data["text"]
                    elif "result" in data and isinstance(data["result"], dict) and "text" in data["result"]:
                        reply_text = data["result"]["text"]
                    elif "payloads" in data and isinstance(data["payloads"], list) and len(data["payloads"]) > 0:
                        first = data["payloads"][0]
                        if isinstance(first, dict) and "text" in first:
                            reply_text = first["text"]
                    elif "content" in data and isinstance(data["content"], str):
                        reply_text = data["content"]
                    elif "status" in data and data.get("status") in ("error", "failed"):
                        return AssistantResponse(
                            status=AssistantStatus.ERROR,
                            model=model,
                            route=self.capabilities.default_route,
                            duration_ms=duration_ms,
                            error=f"OpenClaw run reported failure: {data.get('error', 'unknown error')}",
                            raw_output=raw_out
                        )

                if not reply_text:
                    return AssistantResponse(
                        status=AssistantStatus.ERROR,
                        model=model,
                        route=self.capabilities.default_route,
                        duration_ms=duration_ms,
                        error="Malformed response from OpenClaw: missing assistant reply text field.",
                        raw_output=raw_out[:300]
                    )

                return AssistantResponse(
                    status=AssistantStatus.OK,
                    text=reply_text,
                    model=model,
                    route=self.capabilities.default_route,
                    duration_ms=duration_ms,
                    raw_output=raw_out
                )

            except (json.JSONDecodeError, ValueError) as je:
                return AssistantResponse(
                    status=AssistantStatus.ERROR,
                    model=model,
                    route=self.capabilities.default_route,
                    duration_ms=duration_ms,
                    error=f"Malformed response from OpenClaw (invalid JSON): {str(je)}",
                    raw_output=raw_out[:300]
                )

        except Exception as e:
            duration_ms = (time.perf_counter() - start_time) * 1000.0
            return AssistantResponse(
                status=AssistantStatus.ERROR,
                model=model,
                route=self.capabilities.default_route,
                duration_ms=duration_ms,
                error=f"Unexpected error executing OpenClaw subprocess: {self._sanitize_error(str(e))}"
            )
        finally:
            if temp_dir_obj is not None:
                try:
                    temp_dir_obj.cleanup()
                except Exception:
                    pass
