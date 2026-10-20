"""Independent verification engine for CIPHER test suites."""

import re
import subprocess
from pathlib import Path
from typing import Any, Dict, List, Optional

from .config import (
    REPO_ROOT,
    TEST_TARGETS,
    TEST_TIMEOUT_SECONDS,
    BUILD_TIMEOUT_SECONDS,
    CMAKE_BIN,
    NINJA_BIN,
    VCVARS64_BAT
)


class Verifier:
    def __init__(self, target_dir: Optional[Path] = None):
        self.target_dir = Path(target_dir) if target_dir else REPO_ROOT

    def _detect_code_changes(self, worktree_path: Path, git_diff: str) -> bool:
        """Determines if C++ source, headers, or build configurations were modified."""
        code_patterns = [
            r"\.(cpp|cxx|cc|c|hpp|hxx|h)$",
            r"CMakeLists\.txt",
            r"\.cmake$"
        ]
        # Check git diff headers
        for line in git_diff.splitlines():
            if line.startswith("diff --git"):
                for pat in code_patterns:
                    if re.search(pat, line, re.IGNORECASE):
                        return True
        return False

    def build_worktree(self, worktree_path: Path) -> Dict[str, Any]:
        """Configures and builds the isolated worktree using CMake and Ninja."""
        cmake_exe = str(CMAKE_BIN) if CMAKE_BIN.exists() else "cmake"
        ninja_exe = str(NINJA_BIN) if NINJA_BIN.exists() else "ninja"
        build_dir = worktree_path / "build"
        build_dir.mkdir(parents=True, exist_ok=True)

        # 1. CMake Configure
        if VCVARS64_BAT.exists():
            conf_cmd = f'call "{VCVARS64_BAT}" && "{cmake_exe}" -B "{build_dir}" -S "{worktree_path}" -G Ninja -DCMAKE_MAKE_PROGRAM="{ninja_exe}"'
            use_shell = True
        else:
            conf_cmd = [cmake_exe, "-B", str(build_dir), "-S", str(worktree_path), "-G", "Ninja"]
            if NINJA_BIN.exists():
                conf_cmd.append(f"-DCMAKE_MAKE_PROGRAM={ninja_exe}")
            use_shell = False

        try:
            conf_proc = subprocess.run(
                conf_cmd,
                cwd=str(worktree_path),
                shell=use_shell,
                capture_output=True,
                text=True,
                timeout=BUILD_TIMEOUT_SECONDS
            )
            if conf_proc.returncode != 0:
                err_msg = conf_proc.stderr.strip() or conf_proc.stdout.strip()
                return {
                    "success": False,
                    "error": f"CMake configure failed with code {conf_proc.returncode}:\n{err_msg[:400]}"
                }
        except subprocess.TimeoutExpired:
            return {"success": False, "error": f"CMake configure timed out after {BUILD_TIMEOUT_SECONDS}s."}
        except Exception as e:
            return {"success": False, "error": f"CMake configure execution error: {str(e)}"}

        # 2. CMake Build
        if VCVARS64_BAT.exists():
            build_cmd = f'call "{VCVARS64_BAT}" && "{cmake_exe}" --build "{build_dir}"'
            use_shell = True
        else:
            build_cmd = [cmake_exe, "--build", str(build_dir)]
            use_shell = False

        try:
            build_proc = subprocess.run(
                build_cmd,
                cwd=str(worktree_path),
                shell=use_shell,
                capture_output=True,
                text=True,
                timeout=BUILD_TIMEOUT_SECONDS
            )
            if build_proc.returncode != 0:
                err_msg = build_proc.stderr.strip() or build_proc.stdout.strip()
                return {
                    "success": False,
                    "error": f"CMake build failed with code {build_proc.returncode}:\n{err_msg[:400]}"
                }
        except subprocess.TimeoutExpired:
            return {"success": False, "error": f"CMake build timed out after {BUILD_TIMEOUT_SECONDS}s."}
        except Exception as e:
            return {"success": False, "error": f"CMake build execution error: {str(e)}"}

        return {"success": True, "build_dir": build_dir}

    def run_tests(self, git_diff: str = "") -> Dict[str, Any]:
        """Executes test verification.

        If code changes were made, compiles the worktree in an isolated build dir.
        If non-code changes (e.g. docs), verifies against baseline binaries.
        """
        is_code_change = self._detect_code_changes(self.target_dir, git_diff)

        results: Dict[str, Any] = {
            "all_passed": True,
            "is_code_change": is_code_change,
            "built_from_worktree": False,
            "target_results": {},
            "total_assertions_checked": 0,
            "error_details": []
        }

        if is_code_change:
            # Code was modified: MUST compile the worktree
            build_res = self.build_worktree(self.target_dir)
            if not build_res["success"]:
                results["all_passed"] = False
                results["build_passed"] = False
                results["error_details"].append(build_res["error"])
                results["summary"] = f"Worktree build FAILED: {build_res['error']}"
                return results

            results["built_from_worktree"] = True
            build_dir = self.target_dir / "build"
        else:
            # Documentation / non-code change: verify with existing baseline build
            build_dir = self.target_dir / "build"
            if not build_dir.exists():
                build_dir = REPO_ROOT / "build"

        for target in TEST_TARGETS:
            binary_name = target.name
            binary_path = build_dir / binary_name

            if not binary_path.exists():
                results["all_passed"] = False
                err = f"Binary not found: {binary_path} (stale or uncompiled binary)"
                results["target_results"][binary_name] = {"passed": False, "error": err}
                results["error_details"].append(err)
                continue

            try:
                proc = subprocess.run(
                    [str(binary_path)],
                    cwd=str(self.target_dir),
                    capture_output=True,
                    text=True,
                    timeout=TEST_TIMEOUT_SECONDS
                )
                passed = (proc.returncode == 0)
                if not passed:
                    results["all_passed"] = False
                    results["error_details"].append(f"{binary_name} exited with code {proc.returncode}:\n{proc.stderr}")

                # Count assertions passed from output lines
                pass_count = proc.stdout.count("PASS:")
                results["total_assertions_checked"] += pass_count

                results["target_results"][binary_name] = {
                    "passed": passed,
                    "returncode": proc.returncode,
                    "pass_count": pass_count,
                    "stdout_snippet": proc.stdout[-500:] if len(proc.stdout) > 500 else proc.stdout
                }
            except subprocess.TimeoutExpired:
                results["all_passed"] = False
                err = f"{binary_name} timed out after {TEST_TIMEOUT_SECONDS}s"
                results["target_results"][binary_name] = {"passed": False, "error": err}
                results["error_details"].append(err)
            except Exception as e:
                results["all_passed"] = False
                err = f"{binary_name} execution error: {str(e)}"
                results["target_results"][binary_name] = {"passed": False, "error": err}
                results["error_details"].append(err)

        target_count = len(TEST_TARGETS)
        source_label = "isolated worktree build" if results["built_from_worktree"] else "baseline binaries"
        results["summary"] = (
            f"All {target_count} targets passed ({results['total_assertions_checked']} assertions verified via {source_label})"
            if results["all_passed"]
            else f"Verification FAILED: {', '.join(results['error_details'])}"
        )
        return results

    def run_baseline_tests(self) -> Dict[str, Any]:
        """Runs the verified baseline test suite directly against REPO_ROOT/build."""
        return Verifier(REPO_ROOT).run_tests(git_diff="")
