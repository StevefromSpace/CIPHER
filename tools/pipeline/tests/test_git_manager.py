"""Unit tests for GitManager and safety validation."""

import shutil
import tempfile
import unittest
from pathlib import Path

from tools.pipeline.git_manager import GitManager, GitSafetyViolation


class TestGitManager(unittest.TestCase):
    def setUp(self):
        self.temp_dir = Path(tempfile.mkdtemp())
        self.git_mgr = GitManager(worktree_base=self.temp_dir)

    def tearDown(self):
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def test_git_head_and_clean_status(self):
        head = self.git_mgr.get_current_head()
        self.assertTrue(len(head) == 40)

    def test_safety_validation(self):
        # Valid safe file modifications
        safe_files = ["src/cipher_core.cpp", "docs/ROADMAP.md", "tests/test_config.cpp"]
        self.git_mgr.validate_safety(self.temp_dir, safe_files)

        # Prohibited modifications: .git internal files
        with self.assertRaises(GitSafetyViolation):
            self.git_mgr.validate_safety(self.temp_dir, [".git/config"])

        # Prohibited modifications: cipher.conf
        with self.assertRaises(GitSafetyViolation):
            self.git_mgr.validate_safety(self.temp_dir, ["cipher.conf"])

        # Prohibited modifications: .github
        with self.assertRaises(GitSafetyViolation):
            self.git_mgr.validate_safety(self.temp_dir, [".github/workflows/ci.yml"])

        # Prohibited modifications: path traversal escape attempts
        with self.assertRaises(GitSafetyViolation):
            self.git_mgr.validate_safety(self.temp_dir, ["../outside.txt"])

        with self.assertRaises(GitSafetyViolation):
            self.git_mgr.validate_safety(self.temp_dir, ["docs/../../escape.txt"])

        with self.assertRaises(GitSafetyViolation):
            self.git_mgr.validate_safety(self.temp_dir, ["/etc/passwd"])


    def test_untracked_file_content_changes_alter_candidate_fingerprint(self):
        """Untracked file addition and subsequent content mutations alter the candidate fingerprint."""
        worktree_path, branch_name = self.git_mgr.create_worktree("untracktest")
        try:
            base_commit = self.git_mgr.get_current_head()
            fp_clean = self.git_mgr.compute_candidate_fingerprint(worktree_path, base_commit=base_commit)

            # 1. Create untracked file
            untracked_file = worktree_path / "docs" / "scratch_untracked.txt"
            untracked_file.parent.mkdir(parents=True, exist_ok=True)
            untracked_file.write_text("initial untracked content", encoding="utf-8")

            fp_untracked = self.git_mgr.compute_candidate_fingerprint(worktree_path, base_commit=base_commit)
            self.assertNotEqual(fp_clean, fp_untracked, "Untracked file addition must change fingerprint")

            # 2. Modify untracked file content
            untracked_file.write_text("mutated untracked content", encoding="utf-8")
            fp_mutated = self.git_mgr.compute_candidate_fingerprint(worktree_path, base_commit=base_commit)
            self.assertNotEqual(fp_untracked, fp_mutated, "Untracked file content change must change fingerprint")

        finally:
            self.git_mgr.remove_worktree(worktree_path, branch_name)

    def test_staged_file_content_change_alters_fingerprint(self):
        """Changing the contents of a staged file changes the fingerprint even when path and status remain unchanged."""
        worktree_path, branch_name = self.git_mgr.create_worktree("stagedtest")
        try:
            base_commit = self.git_mgr.get_current_head()

            # 1. Stage a change to an existing tracked file
            tracked_file = worktree_path / "README.md"
            original_content = tracked_file.read_text(encoding="utf-8")
            tracked_file.write_text(original_content + "\n<!-- Staged content v1 -->\n", encoding="utf-8")
            self.git_mgr._run_git(["add", "README.md"], cwd=worktree_path)

            _, status_1, _ = self.git_mgr._run_git(["status", "--porcelain=v1"], cwd=worktree_path)
            self.assertTrue(status_1.startswith("M  README.md"))
            fp_staged_1 = self.git_mgr.compute_candidate_fingerprint(worktree_path, base_commit=base_commit)

            # 2. Mutate the content and re-stage. Path is still README.md, status is still 'M  README.md'
            tracked_file.write_text(original_content + "\n<!-- Staged content v2 altered bytes -->\n", encoding="utf-8")
            self.git_mgr._run_git(["add", "README.md"], cwd=worktree_path)

            _, status_2, _ = self.git_mgr._run_git(["status", "--porcelain=v1"], cwd=worktree_path)
            self.assertEqual(status_1.split()[0], status_2.split()[0], "Staged status code must remain identical")
            fp_staged_2 = self.git_mgr.compute_candidate_fingerprint(worktree_path, base_commit=base_commit)

            self.assertNotEqual(
                fp_staged_1,
                fp_staged_2,
                "Changing contents of a staged file must alter the candidate fingerprint"
            )
        finally:
            self.git_mgr.remove_worktree(worktree_path, branch_name)

    def test_binary_file_content_change_alters_fingerprint(self):
        """Mutating bytes of a binary file alters the candidate fingerprint."""
        worktree_path, branch_name = self.git_mgr.create_worktree("binarytest")
        try:
            base_commit = self.git_mgr.get_current_head()
            bin_file = worktree_path / "docs" / "test_asset.bin"
            bin_file.parent.mkdir(parents=True, exist_ok=True)

            bin_file.write_bytes(b"\x00\x01\x02\x03\xFF\xFE\xFD")
            fp_bin_1 = self.git_mgr.compute_candidate_fingerprint(worktree_path, base_commit=base_commit)

            # Mutate a single byte
            bin_file.write_bytes(b"\x00\x01\x02\x03\xFF\xFE\x00")
            fp_bin_2 = self.git_mgr.compute_candidate_fingerprint(worktree_path, base_commit=base_commit)

            self.assertNotEqual(fp_bin_1, fp_bin_2, "Binary byte change must alter fingerprint")
        finally:
            self.git_mgr.remove_worktree(worktree_path, branch_name)

    def test_file_additions_deletions_and_renames_cannot_escape_detection(self):
        """File additions, deletions, and renames must all alter the candidate fingerprint."""
        worktree_path, branch_name = self.git_mgr.create_worktree("lifecycletest")
        try:
            base_commit = self.git_mgr.get_current_head()
            fp_baseline = self.git_mgr.compute_candidate_fingerprint(worktree_path, base_commit=base_commit)

            # 1. Addition
            new_file = worktree_path / "docs" / "new_doc.md"
            new_file.write_text("# New Document\n", encoding="utf-8")
            fp_addition = self.git_mgr.compute_candidate_fingerprint(worktree_path, base_commit=base_commit)
            self.assertNotEqual(fp_baseline, fp_addition, "Addition must change fingerprint")

            # Stage the addition
            self.git_mgr._run_git(["add", "docs/new_doc.md"], cwd=worktree_path)
            fp_addition_staged = self.git_mgr.compute_candidate_fingerprint(worktree_path, base_commit=base_commit)

            # 2. Rename via git mv
            self.git_mgr._run_git(["mv", "docs/new_doc.md", "docs/renamed_doc.md"], cwd=worktree_path)
            fp_renamed = self.git_mgr.compute_candidate_fingerprint(worktree_path, base_commit=base_commit)
            self.assertNotEqual(fp_addition_staged, fp_renamed, "Rename must change fingerprint")

            # 3. Deletion of an existing tracked file
            tracked_file = worktree_path / "CMakeLists.txt"
            tracked_file.unlink()
            fp_deleted = self.git_mgr.compute_candidate_fingerprint(worktree_path, base_commit=base_commit)
            self.assertNotEqual(fp_renamed, fp_deleted, "Deletion must change fingerprint")
        finally:
            self.git_mgr.remove_worktree(worktree_path, branch_name)

    def test_candidate_fingerprint_is_stable_when_candidate_unchanged(self):
        """Candidate fingerprint is deterministic and stable across multiple evaluations."""
        worktree_path, branch_name = self.git_mgr.create_worktree("stabilitytest")
        try:
            base_commit = self.git_mgr.get_current_head()

            # Introduce staged, unstaged, untracked, and binary changes
            untracked = worktree_path / "untracked.txt"
            untracked.write_text("untracked text", encoding="utf-8")

            binary_file = worktree_path / "data.bin"
            binary_file.write_bytes(b"\xCA\xFE\xBA\xBE")

            staged_file = worktree_path / "README.md"
            staged_file.write_text("staged edit", encoding="utf-8")
            self.git_mgr._run_git(["add", "README.md"], cwd=worktree_path)

            fp1 = self.git_mgr.compute_candidate_fingerprint(worktree_path, base_commit=base_commit)
            fp2 = self.git_mgr.compute_candidate_fingerprint(worktree_path, base_commit=base_commit)
            fp3 = self.git_mgr.compute_candidate_fingerprint(worktree_path, base_commit=base_commit)

            self.assertEqual(fp1, fp2)
            self.assertEqual(fp2, fp3)
            self.assertTrue(len(fp1) == 64, "Fingerprint must be a valid 64-char hex SHA-256")
        finally:
            self.git_mgr.remove_worktree(worktree_path, branch_name)


if __name__ == "__main__":
    unittest.main()
