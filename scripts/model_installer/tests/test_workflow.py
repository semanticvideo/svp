import tempfile
import unittest
from pathlib import Path

from scripts.model_installer.workflow import atomic_publish


class AtomicPublishTests(unittest.TestCase):
    def test_verified_staging_becomes_destination(self):
        with tempfile.TemporaryDirectory() as root:
            staging = Path(root) / "staging"
            destination = Path(root) / "models"
            staging.mkdir()
            (staging / "model-lock.json").write_text("ready", encoding="utf-8")

            atomic_publish(
                staging, destination,
                lambda published: self.assertEqual(
                    (published / "model-lock.json").read_text(encoding="utf-8"),
                    "ready",
                ),
            )

            self.assertFalse(staging.exists())
            self.assertTrue(destination.is_dir())

    def test_failed_final_verification_removes_published_staging(self):
        with tempfile.TemporaryDirectory() as root:
            staging = Path(root) / "staging"
            destination = Path(root) / "models"
            staging.mkdir()

            def reject(_published):
                raise RuntimeError("verification failed")

            with self.assertRaisesRegex(RuntimeError, "verification failed"):
                atomic_publish(staging, destination, reject)

            self.assertFalse(destination.exists())

    def test_existing_destination_is_never_replaced(self):
        with tempfile.TemporaryDirectory() as root:
            staging = Path(root) / "staging"
            destination = Path(root) / "models"
            staging.mkdir()
            destination.mkdir()
            existing = destination / "user-file"
            existing.write_text("keep", encoding="utf-8")

            with self.assertRaisesRegex(RuntimeError, "already exists"):
                atomic_publish(staging, destination, lambda _published: None)

            self.assertEqual(existing.read_text(encoding="utf-8"), "keep")
            self.assertTrue(staging.is_dir())


if __name__ == "__main__":
    unittest.main()
