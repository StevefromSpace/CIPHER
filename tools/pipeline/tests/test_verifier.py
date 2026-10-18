"""Unit tests for Verifier: stale binaries, build failures, and isolated builds."""

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools.pipeline.verifier import Verifier


class TestVerifier(unittest.TestCase):
    def setUp(self):
        self.temp_dir = Path(tempfile.mkdtemp())
        self.verifier = Verifier(target_dir=self.temp_dir)

    def test_detect_code_changes(self):
        self.assertFalse(self.verifier._detect_code_changes(self.temp_dir, "diff --git a/docs/README.md b/docs/README.md"))
        self.assertFalse(self.verifier._detect_code_changes(self.temp_dir, "diff --git a/.gitignore b/.gitignore"))

        self.assertTrue(self.verifier._detect_code_changes(self.temp_dir, "diff --git a/src/cipher_core.cpp b/src/cipher_core.cpp"))
        self.assertTrue(self.verifier._detect_code_changes(self.temp_dir, "diff --git a/include/cipher/cipher_core.hpp b/include/cipher/cipher_core.hpp"))
        self.assertTrue(self.verifier._detect_code_changes(self.temp_dir, "diff --git a/CMakeLists.txt b/CMakeLists.txt"))

    @patch.object(Verifier, "build_worktree")
    def test_build_failure_reported(self, mock_build):
        mock_build.return_value = {"success": False, "error": "fatal error: include/cipher/version.hpp not found"}

        code_diff = "diff --git a/src/cipher_core.cpp b/src/cipher_core.cpp\n+int x = 1;"
        results = self.verifier.run_tests(git_diff=code_diff)

        self.assertFalse(results["all_passed"])
        self.assertFalse(results.get("build_passed", True))
        self.assertIn("Worktree build FAILED", results["summary"])
        self.assertIn("fatal error", results["error_details"][0])

    @patch.object(Verifier, "build_worktree")
    def test_stale_or_missing_binary(self, mock_build):
        mock_build.return_value = {"success": True, "build_dir": self.temp_dir / "build"}

        code_diff = "diff --git a/src/cipher_core.cpp b/src/cipher_core.cpp\n+int x = 1;"
        results = self.verifier.run_tests(git_diff=code_diff)

        self.assertFalse(results["all_passed"])
        self.assertTrue(results["built_from_worktree"])
        self.assertIn("stale or uncompiled binary", results["error_details"][0])

    def test_docs_only_uses_baseline_without_compiling(self):
        doc_diff = "diff --git a/docs/GUIDE.md b/docs/GUIDE.md\n+# Guide"
        with patch.object(self.verifier, "build_worktree") as mock_build:
            results = self.verifier.run_tests(git_diff=doc_diff)
            mock_build.assert_not_called()
            self.assertFalse(results["is_code_change"])
            self.assertFalse(results["built_from_worktree"])


if __name__ == "__main__":
    unittest.main()
