import io
import _thread
import threading
import time
import unittest
from pathlib import Path

from scripts.model_installer.downloads import Download, DownloadFailure, run_downloads


class FakeProgress:
    def __init__(self):
        self.events = []

    def emit(self, *args, **kwargs):
        self.events.append((args, kwargs))


class FakeWriter(io.BytesIO):
    def __exit__(self, exc_type, exc, traceback):
        return False

    def close(self):
        pass


class FakeFilesystem:
    def __init__(self):
        self.temporary = {}
        self.published = {}

    def open_atomic(self, destination):
        temporary = str(destination) + ".part"
        writer = FakeWriter()
        self.temporary[temporary] = writer
        return temporary, writer

    def publish_atomic(self, temporary, destination):
        self.published[str(destination)] = self.temporary.pop(temporary).getvalue()

    def discard(self, temporary):
        self.temporary.pop(temporary, None)


class FakeTransport:
    def __init__(self, payloads, failing_model=None):
        self.payloads = payloads
        self.failing_model = failing_model
        self.active = 0
        self.maximum_active = 0
        self.started_models = []
        self.lock = threading.Lock()

    def chunks(self, request, cancelled):
        with self.lock:
            self.active += 1
            self.maximum_active = max(self.maximum_active, self.active)
            self.started_models.append(request.model_id)
        try:
            if request.model_id == self.failing_model:
                raise DownloadFailure("fake failure")
            time.sleep(0.02)
            yield self.payloads[request.url]
        finally:
            with self.lock:
                self.active -= 1


class InterruptingTransport:
    def __init__(self):
        self.cancelled_before_return = False

    def chunks(self, request, cancelled):
        _thread.interrupt_main()
        while not cancelled.wait(0.01):
            pass
        self.cancelled_before_return = True
        return
        yield


def item(model_id, url, payload):
    import hashlib
    return Download(
        model_id=model_id,
        model_label=model_id,
        url=url,
        destination=Path("/") / model_id / "model.bin",
        expected_bytes=len(payload),
        sha256=hashlib.sha256(payload).hexdigest(),
        headers={},
    )


class DownloadTests(unittest.TestCase):
    def test_two_workers_never_exceed_two_model_jobs(self):
        payloads = {f"u{i}": bytes([i]) * 8 for i in range(4)}
        downloads = [item(f"model_{i}", f"u{i}", payloads[f"u{i}"])
                     for i in range(4)]
        transport = FakeTransport(payloads)
        filesystem = FakeFilesystem()
        progress = FakeProgress()

        current, total = run_downloads(
            downloads, 2, progress, transport, filesystem)

        self.assertEqual(current, total)
        self.assertEqual(transport.maximum_active, 2)
        self.assertEqual(len(filesystem.published), 4)
        byte_events = [event for event in progress.events
                       if event[0][1] == "bytes"]
        self.assertEqual(byte_events[-1][1]["current"], total)

    def test_first_failure_cancels_unscheduled_models_and_partial_file(self):
        payloads = {f"u{i}": bytes([i]) * 8 for i in range(4)}
        downloads = [item(f"model_{i}", f"u{i}", payloads[f"u{i}"])
                     for i in range(4)]
        transport = FakeTransport(payloads, failing_model="model_0")
        filesystem = FakeFilesystem()

        with self.assertRaisesRegex(DownloadFailure, "fake failure"):
            run_downloads(downloads, 2, FakeProgress(), transport, filesystem)

        self.assertNotIn("model_2", transport.started_models)
        self.assertNotIn("model_3", transport.started_models)
        self.assertFalse(filesystem.temporary)

    def test_catalog_byte_count_is_enforced_without_content_length(self):
        payload = b"too-long"
        download = item("model_one", "u", b"short")
        transport = FakeTransport({"u": payload})
        filesystem = FakeFilesystem()

        with self.assertRaisesRegex(DownloadFailure, "expected_bytes"):
            run_downloads([download], 1, FakeProgress(), transport, filesystem)

        self.assertFalse(filesystem.published)
        self.assertFalse(filesystem.temporary)

    def test_parallel_range_is_closed(self):
        with self.assertRaisesRegex(ValueError, "must be 1 or 2"):
            run_downloads([], 3, FakeProgress())

    def test_keyboard_interrupt_cancels_and_joins_workers_before_return(self):
        payload = b"payload"
        downloads = [item("model_0", "u0", payload),
                     item("model_1", "u1", payload)]
        transport = InterruptingTransport()
        filesystem = FakeFilesystem()

        with self.assertRaises(KeyboardInterrupt):
            run_downloads(downloads, 1, FakeProgress(), transport, filesystem)

        self.assertTrue(transport.cancelled_before_return)
        self.assertFalse(filesystem.temporary)
        self.assertFalse(filesystem.published)


if __name__ == "__main__":
    unittest.main()
