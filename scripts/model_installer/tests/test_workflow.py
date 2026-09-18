import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts.model_installer.workflow import (
    atomic_publish,
    verify_exact_cache,
    verify_existing_cache_subset,
)


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

    def test_verified_staging_replaces_existing_destination(self):
        with tempfile.TemporaryDirectory() as root:
            staging = Path(root) / "staging"
            destination = Path(root) / "models"
            staging.mkdir()
            destination.mkdir()
            (staging / "model-lock.json").write_text("new", encoding="utf-8")
            (destination / "model-lock.json").write_text("old", encoding="utf-8")

            atomic_publish(
                staging, destination,
                lambda published: self.assertEqual(
                    (published / "model-lock.json").read_text(encoding="utf-8"),
                    "new",
                ),
                replace_existing=True,
            )

            self.assertFalse(staging.exists())
            self.assertEqual(
                (destination / "model-lock.json").read_text(encoding="utf-8"),
                "new",
            )
            self.assertFalse(destination.with_name("models.previous").exists())

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


class ExactCacheVerificationTests(unittest.TestCase):
    def test_verifies_authoritative_lock_and_reference_set(self):
        with tempfile.TemporaryDirectory() as root:
            cache = Path(root) / "cache"
            cache.mkdir()
            reference = Path(root) / "reference.json"
            reference.write_text(
                '{"models":[{"model_id":"model_expected"}]}',
                encoding="utf-8")
            lock = cache / "model-lock.json"
            lock.write_text(
                '{"models":[{"model_id":"model_expected"}]}',
                encoding="utf-8")

            with patch("scripts.model_installer.workflow.run_tool") as run_tool:
                verify_exact_cache(Path("models-tool"), reference, cache)

            self.assertEqual(run_tool.call_count, 2)
            run_tool.assert_any_call(
                Path("models-tool"), "verify", "--lock", lock,
                "--cache-dir", cache)
            run_tool.assert_any_call(
                Path("models-tool"), "verify", "--model-set", reference,
                "--cache-dir", cache)

    def test_requires_lock_and_reference_to_name_the_same_complete_set(self):
        with tempfile.TemporaryDirectory() as root:
            cache = Path(root) / "cache"
            cache.mkdir()
            reference = Path(root) / "reference.json"
            reference.write_text(
                '{"models":[{"model_id":"model_expected"}]}',
                encoding="utf-8")
            (cache / "model-lock.json").write_text(
                '{"models":[{"model_id":"model_expected"},'
                '{"model_id":"model_extra"}]}', encoding="utf-8")

            with self.assertRaisesRegex(RuntimeError, "exact reference model set"):
                verify_exact_cache(Path("unused"), reference, cache)

    def test_requires_authoritative_root_lock(self):
        with tempfile.TemporaryDirectory() as root:
            cache = Path(root) / "cache"
            cache.mkdir()
            reference = Path(root) / "reference.json"
            reference.write_text('{"models":[]}', encoding="utf-8")

            with self.assertRaisesRegex(RuntimeError, "missing authoritative"):
                verify_exact_cache(Path("unused"), reference, cache)

    def test_rejects_corrupt_authoritative_root_lock(self):
        with tempfile.TemporaryDirectory() as root:
            cache = Path(root) / "cache"
            cache.mkdir()
            reference = Path(root) / "reference.json"
            reference.write_text('{"models":[]}', encoding="utf-8")
            (cache / "model-lock.json").write_text("not JSON", encoding="utf-8")

            with self.assertRaisesRegex(RuntimeError, "could not parse"):
                verify_exact_cache(Path("unused"), reference, cache)

    def test_verified_cache_subset_matches_current_reference_identity(self):
        with tempfile.TemporaryDirectory() as root:
            cache = Path(root) / "cache"
            cache.mkdir()
            reference = Path(root) / "reference.json"
            reference.write_text(
                '{"models":[{"model_id":"model_existing",'
                '"model_bundle_id":"bundle_existing",'
                '"bundle_blake3":"blake3:existing"},'
                '{"model_id":"model_missing",'
                '"model_bundle_id":"bundle_missing",'
                '"bundle_blake3":"blake3:missing"}]}',
                encoding="utf-8")
            (cache / "model-lock.json").write_text(
                '{"models":[{"model_id":"model_existing",'
                '"model_bundle_id":"bundle_existing",'
                '"bundle_blake3":"blake3:existing"}]}',
                encoding="utf-8")

            with patch("scripts.model_installer.workflow.run_tool") as run_tool:
                model_ids = verify_existing_cache_subset(
                    Path("models-tool"), reference, cache)

            self.assertEqual(model_ids, {"model_existing"})
            run_tool.assert_called_once_with(
                Path("models-tool"), "verify", "--lock",
                cache / "model-lock.json", "--cache-dir", cache)

    def test_verified_cache_subset_rejects_stale_identity(self):
        with tempfile.TemporaryDirectory() as root:
            cache = Path(root) / "cache"
            cache.mkdir()
            reference = Path(root) / "reference.json"
            reference.write_text(
                '{"models":[{"model_id":"model_existing",'
                '"model_bundle_id":"bundle_current",'
                '"bundle_blake3":"blake3:current"},'
                '{"model_id":"model_missing",'
                '"model_bundle_id":"bundle_missing",'
                '"bundle_blake3":"blake3:missing"}]}',
                encoding="utf-8")
            (cache / "model-lock.json").write_text(
                '{"models":[{"model_id":"model_existing",'
                '"model_bundle_id":"bundle_old",'
                '"bundle_blake3":"blake3:old"}]}',
                encoding="utf-8")

            with self.assertRaisesRegex(RuntimeError, "not current"):
                verify_existing_cache_subset(
                    Path("unused"), reference, cache)


if __name__ == "__main__":
    unittest.main()
