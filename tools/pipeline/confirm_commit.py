"""Owner-controlled local confirmation mechanism for CIPHER Pipeline commits.

Keeps commit authorization strictly outside the agent workflow.
Requires interactive owner confirmation or explicit --confirm flag on the local terminal.
Validates candidate fingerprint integrity before recording approval.
Strictly separates commit approval from remote push approval.
"""

import argparse
import sys
from pathlib import Path
from typing import Optional

from .job_store import JobStatus, JobStore
from .orchestrator import PipelineOrchestrator


def confirm_pipeline_commit(
    job_id: str,
    commit_message: Optional[str] = None,
    auto_confirm: bool = False,
    job_store: Optional[JobStore] = None
) -> bool:
    """Owner confirmation entrypoint for a verified candidate commit."""
    store = job_store or JobStore()
    orchestrator = PipelineOrchestrator(job_store=store)

    job = store.get_job(job_id)
    if not job:
        print(f"Error: Job '{job_id}' not found.", file=sys.stderr)
        return False

    if job.status != JobStatus.READY_FOR_APPROVAL:
        print(
            f"Error: Job '{job_id}' is in status '{job.status.value}', not READY_FOR_APPROVAL.",
            file=sys.stderr
        )
        return False

    print("=================================================================")
    print(f"CIPHER Pipeline — Local Commit Confirmation (Owner Gate)")
    print("=================================================================")
    print(f"Job ID:               {job.job_id}")
    print(f"Task:                 {job.task_description}")
    print(f"Branch:               {job.branch_name}")
    print(f"Candidate Hash:       {job.candidate_fingerprint}")
    print(f"Repair Attempts:      {job.repair_attempts}")
    print(f"Review Status:        {'Passed' if job.reviews and job.reviews[-1].get('passed') else 'None'}")
    print(f"Verification:         {'Passed' if job.verification_results and job.verification_results.get('all_passed') else 'None'}")
    print(f"Diff Summary:         {len(job.git_diff.splitlines()) if job.git_diff else 0} diff lines")
    print("-----------------------------------------------------------------")

    import os
    if auto_confirm:
        if os.environ.get("CIPHER_TEST_MODE") != "1":
            print("Error: --confirm flag is strictly restricted to test mode (CIPHER_TEST_MODE=1).", file=sys.stderr)
            return False
    else:
        if not sys.stdin.isatty() and os.environ.get("CIPHER_TEST_MODE") != "1":
            print("Error: Interactive owner confirmation requires an active TTY terminal.", file=sys.stderr)
            return False
        try:
            prompt = input("Do you authorize this local commit? [y/N]: ").strip().lower()
            if prompt not in ("y", "yes"):
                print("Commit authorization aborted by owner.")
                return False
        except (KeyboardInterrupt, EOFError):
            print("\nCommit authorization cancelled.")
            return False

    # Execute commit approval with candidate integrity check
    try:
        updated_job = orchestrator.approve_commit(job_id, commit_message=commit_message)
        print(f"[SUCCESS] Candidate integrity verified. Local commit approved for job '{job_id}'.")
        print(f"[NOTICE] Status transitioned to: {updated_job.status.value}")
        print("[NOTICE] Push to remote is a SEPARATE action and was NOT performed.")
        return True
    except Exception as e:
        print(f"[FAILED] Commit approval rejected: {e}", file=sys.stderr)
        return False


def main():
    parser = argparse.ArgumentParser(
        description="Owner-controlled local commit approval CLI for CIPHER Pipeline."
    )
    parser.add_argument("job_id", help="UUID of the job to approve")
    parser.add_argument("-m", "--message", help="Commit message", default=None)
    parser.add_argument(
        "--confirm",
        action="store_true",
        help="Explicitly confirm without interactive prompt (for headless/automated tests)"
    )

    args = parser.parse_args()
    success = confirm_pipeline_commit(
        job_id=args.job_id,
        commit_message=args.message,
        auto_confirm=args.confirm
    )
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
