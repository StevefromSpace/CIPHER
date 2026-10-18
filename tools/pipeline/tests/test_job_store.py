"""Unit tests for the pipeline JobStore."""

import shutil
import tempfile
import unittest
from pathlib import Path

from tools.pipeline.job_store import JobRecord, JobStatus, JobStore


class TestJobStore(unittest.TestCase):
    def setUp(self):
        self.test_dir = Path(tempfile.mkdtemp())
        self.job_store = JobStore(store_dir=self.test_dir)

    def tearDown(self):
        shutil.rmtree(self.test_dir, ignore_errors=True)

    def test_create_and_get_job(self):
        job = self.job_store.create_job("Test task description", task_type="docs")
        self.assertIsNotNone(job.job_id)
        self.assertEqual(job.task_description, "Test task description")
        self.assertEqual(job.task_type, "docs")
        self.assertEqual(job.status, JobStatus.PENDING)
        self.assertEqual(len(job.events), 1)

        fetched = self.job_store.get_job(job.job_id)
        self.assertIsNotNone(fetched)
        self.assertEqual(fetched.job_id, job.job_id)
        self.assertEqual(fetched.status, JobStatus.PENDING)

    def test_update_status(self):
        job = self.job_store.create_job("Status change test")
        updated = self.job_store.update_status(job.job_id, JobStatus.IMPLEMENTING, "Starting work")
        self.assertEqual(updated.status, JobStatus.IMPLEMENTING)
        self.assertEqual(len(updated.events), 2)

        completed = self.job_store.update_status(job.job_id, JobStatus.READY_FOR_APPROVAL, "Completed")
        self.assertEqual(completed.status, JobStatus.READY_FOR_APPROVAL)
        self.assertIsNotNone(completed.completed_at)

    def test_list_jobs(self):
        self.job_store.create_job("Task 1")
        self.job_store.create_job("Task 2")
        jobs = self.job_store.list_jobs(limit=10)
        self.assertEqual(len(jobs), 2)

    def test_historical_codex_reviews_migration(self):
        legacy_data = {
            "job_id": "legacy-uuid-1234",
            "task_description": "Legacy task",
            "status": "READY_FOR_APPROVAL",
            "codex_reviews": [
                {
                    "passed": True,
                    "blocking": False,
                    "findings": [{"severity": "WARNING", "file": "src/legacy.cpp"}]
                }
            ]
        }
        record = JobRecord.from_dict(legacy_data)
        # Verify migration to generic reviews field
        self.assertEqual(len(record.reviews), 1)
        self.assertTrue(record.reviews[0]["passed"])
        # Verify backward-compatibility property access
        self.assertEqual(len(record.codex_reviews), 1)
        self.assertEqual(record.codex_reviews, record.reviews)

        # Verify serialization uses generic 'reviews' field
        serialized = record.to_dict()
        self.assertIn("reviews", serialized)
        self.assertNotIn("codex_reviews", serialized)

    def test_candidate_fingerprint_persistence(self):
        job = self.job_store.create_job("Fingerprint test")
        job.candidate_fingerprint = "abc123sha256hash"
        self.job_store.save_job(job)

        fetched = self.job_store.get_job(job.job_id)
        self.assertIsNotNone(fetched)
        self.assertEqual(fetched.candidate_fingerprint, "abc123sha256hash")


if __name__ == "__main__":
    unittest.main()
