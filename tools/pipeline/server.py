"""MCP Server exposing the CIPHER pipeline tools to Antigravity / Gemini."""

import json
from pathlib import Path
from typing import Optional

from mcp.server.mcpserver import MCPServer

from .job_store import JobStore
from .orchestrator import PipelineOrchestrator

# Initialize server and orchestrator
mcp = MCPServer(name="cipher-pipeline")
job_store = JobStore()
orchestrator = PipelineOrchestrator(job_store=job_store)


@mcp.tool()
def submit_pipeline_task(
    task_description: str,
    task_type: str = "general",
    use_mock: bool = False,
    run_async: bool = False
) -> str:
    """Submit a development task to the CIPHER multi-agent pipeline.

    Flow: Claude Agent A (implementation) -> Claude Agent B (independent review) -> Verifier (independent tests).

    Args:
        task_description: The goal or specification of the task.
        task_type: Type of task, e.g. 'docs', 'bugfix', 'refactor'.
        use_mock: If True, uses mock adapters (for testing pipeline state transitions without model tokens).
                  Restricted to test mode (CIPHER_PIPELINE_ALLOW_MOCK=1).
        run_async: If True, returns immediately with the job_id while the task runs in the background.
    """
    import os
    if use_mock and os.environ.get("CIPHER_PIPELINE_ALLOW_MOCK") != "1":
        return json.dumps({
            "success": False,
            "error": "use_mock is disallowed in production. It is strictly restricted to test environments (CIPHER_PIPELINE_ALLOW_MOCK=1)."
        }, indent=2)

    try:
        job = orchestrator.submit_task(
            task_description=task_description,
            task_type=task_type,
            use_mock=use_mock,
            run_async=run_async
        )
        return json.dumps({
            "success": True,
            "job_id": job.job_id,
            "status": job.status.value,
            "branch_name": job.branch_name,
            "message": f"Pipeline task submitted. Current status: {job.status.value}"
        }, indent=2)
    except Exception as e:
        return json.dumps({
            "success": False,
            "error": str(e)
        }, indent=2)


@mcp.tool()
def get_pipeline_status(job_id: str) -> str:
    """Get the current execution status and progress of a pipeline job.

    Args:
        job_id: The UUID of the job returned by submit_pipeline_task.
    """
    job = job_store.get_job(job_id)
    if not job:
        return json.dumps({"error": f"Job '{job_id}' not found."}, indent=2)

    return json.dumps({
        "job_id": job.job_id,
        "status": job.status.value,
        "task_description": job.task_description,
        "repair_attempts": job.repair_attempts,
        "branch_name": job.branch_name,
        "candidate_fingerprint": job.candidate_fingerprint,
        "created_at": job.created_at,
        "updated_at": job.updated_at,
        "completed_at": job.completed_at,
        "recent_event": job.events[-1] if job.events else None
    }, indent=2)


@mcp.tool()
def get_pipeline_result(job_id: str) -> str:
    """Retrieve full results, reviews, test reports, and Git diff for a completed pipeline job.

    Args:
        job_id: The UUID of the job.
    """
    job = job_store.get_job(job_id)
    if not job:
        return json.dumps({"error": f"Job '{job_id}' not found."}, indent=2)

    return json.dumps({
        "job_id": job.job_id,
        "status": job.status.value,
        "task_description": job.task_description,
        "branch_name": job.branch_name,
        "worktree_path": job.worktree_path,
        "repair_attempts": job.repair_attempts,
        "candidate_fingerprint": job.candidate_fingerprint,
        "verification_results": job.verification_results,
        "reviews": job.reviews,
        "git_diff_summary": f"{len(job.git_diff.splitlines())} lines changed" if job.git_diff else "No diff",
        "git_diff": job.git_diff,
        "events": job.events
    }, indent=2)


@mcp.tool()
def cancel_pipeline_task(job_id: str) -> str:
    """Cancel a running pipeline job and remove its temporary worktree.

    Args:
        job_id: The UUID of the job to cancel.
    """
    job = orchestrator.cancel_job(job_id)
    if not job:
        return json.dumps({"error": f"Job '{job_id}' not found."}, indent=2)

    return json.dumps({
        "job_id": job.job_id,
        "status": job.status.value,
        "message": "Job cancelled and worktree cleaned up successfully."
    }, indent=2)


def main():
    mcp.run(transport="stdio")


if __name__ == "__main__":
    main()
