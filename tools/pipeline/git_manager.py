"""Git Worktree and Safety Management for CIPHER Pipeline."""

import os
import shutil
import subprocess
from pathlib import Path
from typing import List, Optional, Tuple

from .config import REPO_ROOT, WORKTREE_BASE_DIR


class GitSafetyViolation(Exception):
    """Raised when an operation violates Git safety boundaries."""


class GitManager:
    def __init__(self, repo_root: Path = REPO_ROOT, worktree_base: Path = WORKTREE_BASE_DIR):
        self.repo_root = Path(repo_root)
        self.worktree_base = Path(worktree_base)
        self.worktree_base.mkdir(parents=True, exist_ok=True)

    def _run_git(self, args: List[str], cwd: Optional[Path] = None) -> Tuple[int, str, str]:
        working_dir = cwd or self.repo_root
        cmd = ["git"] + args
        proc = subprocess.run(
            cmd,
            cwd=str(working_dir),
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=30
        )
        stdout_str = (proc.stdout or "").strip()
        stderr_str = (proc.stderr or "").strip()
        return proc.returncode, stdout_str, stderr_str

    def get_current_head(self) -> str:
        code, out, err = self._run_git(["rev-parse", "HEAD"])
        if code != 0:
            raise RuntimeError(f"Failed to get HEAD commit: {err}")
        return out

    def is_working_tree_clean(self) -> bool:
        code, out, _ = self._run_git(["status", "--porcelain"])
        return code == 0 and len(out.strip()) == 0

    def create_worktree(self, job_id: str) -> Tuple[Path, str]:
        """Creates an isolated git worktree on a dedicated branch."""
        branch_name = f"pipeline/job-{job_id[:8]}"
        worktree_path = self.worktree_base / f"job-{job_id[:8]}"

        # Remove stale worktree path if it already exists
        if worktree_path.exists():
            self.remove_worktree(worktree_path, branch_name)

        # git worktree add -b <branch> <path> HEAD
        code, out, err = self._run_git(["worktree", "add", "-b", branch_name, str(worktree_path), "HEAD"])
        if code != 0:
            # Fallback if branch already exists: checkout existing branch
            if "already exists" in err:
                code, out, err = self._run_git(["worktree", "add", str(worktree_path), branch_name])
            if code != 0:
                raise RuntimeError(f"Failed to create worktree: {err or out}")

        return worktree_path, branch_name

    def remove_worktree(self, worktree_path: Path, branch_name: Optional[str] = None) -> bool:
        """Removes the worktree and optionally deletes the job branch."""
        import tempfile
        path = Path(worktree_path)
        resolved = path.resolve()
        resolved_base = WORKTREE_BASE_DIR.resolve()
        resolved_temp = Path(tempfile.gettempdir()).resolve()

        # Confine removal strictly to authorized worktree or temp directories
        if not (resolved.is_relative_to(resolved_base) or resolved.is_relative_to(resolved_temp)):
            raise GitSafetyViolation(f"Refusing to remove worktree outside authorized base directory: {path}")

        if path.exists():
            self._run_git(["worktree", "remove", "--force", str(path)])
            if path.exists():
                try:
                    shutil.rmtree(path, ignore_errors=True)
                except Exception:
                    pass

        self._run_git(["worktree", "prune"])

        if branch_name:
            self._run_git(["branch", "-D", branch_name])

        return not path.exists()

    def get_worktree_diff(self, worktree_path: Path, base_commit: Optional[str] = None) -> Tuple[str, List[str]]:
        """Returns the full diff and a list of modified files inside the worktree against base commit."""
        # Use intent-to-add so new files appear in git diff
        self._run_git(["add", "-N", "."], cwd=worktree_path)

        base_ref = base_commit
        if not base_ref:
            code_mb, mb_out, _ = self._run_git(["merge-base", "main", "HEAD"], cwd=worktree_path)
            if code_mb == 0 and mb_out:
                base_ref = mb_out
            else:
                base_ref = "HEAD"

        code, out, _ = self._run_git(["diff", "--binary", base_ref], cwd=worktree_path)
        if code != 0:
            code, out, _ = self._run_git(["diff", "--binary", "HEAD"], cwd=worktree_path)

        # Get list of all changed files relative to base_ref
        code_files, files_out, _ = self._run_git(["diff", "--name-only", base_ref], cwd=worktree_path)
        modified_files = set()
        if code_files == 0 and files_out:
            modified_files.update(line.strip() for line in files_out.splitlines() if line.strip())

        code2, untracked, _ = self._run_git(["status", "--porcelain=v1", "-uall"], cwd=worktree_path)
        if code2 == 0 and untracked:
            for line in untracked.splitlines():
                parts = line.strip().split(maxsplit=1)
                if len(parts) == 2:
                    filepath = parts[1]
                    if "->" in filepath:
                        for item in filepath.split("->"):
                            modified_files.add(item.strip())
                    else:
                        modified_files.add(filepath)

        return out, sorted(list(modified_files))

    def validate_safety(self, worktree_path: Path, modified_files: List[str]) -> None:
        """Safety check to ensure critical repository boundaries and paths are preserved."""
        restricted_prefixes = [".git/", ".github/"]
        resolved_wt = Path(worktree_path).resolve()

        for f in modified_files:
            cleaned = f.strip().strip('"').strip("'")
            norm = cleaned.replace("\\", "/")
            parts = norm.split("/")
            if ".." in parts or norm.startswith("/") or (len(norm) > 1 and norm[1] == ":"):
                raise GitSafetyViolation(f"Path escape attempt '{f}' outside worktree is prohibited.")

            try:
                resolved_file = (Path(worktree_path) / cleaned).resolve()
                if not resolved_file.is_relative_to(resolved_wt):
                    raise GitSafetyViolation(f"Path '{f}' escapes worktree boundary '{worktree_path}'.")
            except Exception:
                raise GitSafetyViolation(f"Invalid path structure for '{f}'.")

            norm_lower = norm.lower()
            for prefix in restricted_prefixes:
                if norm_lower.startswith(prefix.lower()):
                    raise GitSafetyViolation(f"Modification to restricted path '{f}' is prohibited.")
            if norm_lower == "cipher.conf" or norm_lower.endswith("/cipher.conf"):
                raise GitSafetyViolation(f"Modification to '{f}' is prohibited.")

        if (Path(worktree_path) / "cipher.conf").exists():
            raise GitSafetyViolation("Creation of forbidden file 'cipher.conf' is prohibited.")


    def compute_candidate_fingerprint(self, worktree_path: Path, base_commit: Optional[str] = None) -> str:
        """Computes a reproducible SHA-256 fingerprint of the complete candidate content.

        Guarantees candidate identity incorporates all relevant content:
        - Base commit reference
        - Staged index status and staged diff (including binary data)
        - Working-tree status (--porcelain=v1 -uall) and working diff (--binary)
        - Actual disk bytes for every candidate file (staged, unstaged, untracked, binary)
        - Explicit detection of additions, deletions, and renames
        """
        import hashlib
        self._run_git(["add", "-N", "."], cwd=worktree_path)
        diff_text, modified_files = self.get_worktree_diff(worktree_path, base_commit=base_commit)
        _, status_out, _ = self._run_git(["status", "--porcelain=v1", "-uall"], cwd=worktree_path)
        base_ref = base_commit or "HEAD"

        # Staged diff specifically captures index-level modifications
        _, cached_diff, _ = self._run_git(["diff", "--binary", "--cached", base_ref], cwd=worktree_path)
        # Staged index blob objects
        _, stage_out, _ = self._run_git(["ls-files", "--stage"], cwd=worktree_path)

        h = hashlib.sha256()
        h.update(base_ref.encode("utf-8"))
        h.update(b"\n---STATUS---\n")
        h.update(status_out.encode("utf-8"))
        h.update(b"\n---STAGE_INDEX---\n")
        h.update(stage_out.encode("utf-8"))
        h.update(b"\n---FILES---\n")
        h.update("\n".join(sorted(modified_files)).encode("utf-8"))
        h.update(b"\n---DIFF---\n")
        h.update(diff_text.encode("utf-8"))
        h.update(b"\n---CACHED_DIFF---\n")
        h.update(cached_diff.encode("utf-8"))

        # Incorporate actual file bytes directly
        h.update(b"\n---CONTENT_BYTES---\n")
        for rel_path in sorted(modified_files):
            file_path = worktree_path / rel_path
            h.update(rel_path.encode("utf-8"))
            h.update(b":")
            if file_path.is_file():
                try:
                    content_bytes = file_path.read_bytes()
                    file_sha = hashlib.sha256(content_bytes).hexdigest()
                    h.update(file_sha.encode("utf-8"))
                except Exception as e:
                    h.update(f"READ_ERROR:{str(e)}".encode("utf-8"))
            elif not file_path.exists():
                h.update(b"DELETED")
            else:
                h.update(b"SPECIAL_OR_DIR")
            h.update(b"\n")

        return h.hexdigest()
