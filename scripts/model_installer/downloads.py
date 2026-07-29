import hashlib
import threading
import urllib.request
from collections import deque
from dataclasses import dataclass
from pathlib import Path


class DownloadFailure(RuntimeError):
    pass


@dataclass(frozen=True)
class Download:
    model_id: str
    model_label: str
    url: str
    destination: Path
    expected_bytes: int
    sha256: str
    headers: dict


class LocalFilesystem:
    def open_atomic(self, destination):
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_name(destination.name + ".part")
        return temporary, temporary.open("wb")

    def publish_atomic(self, temporary, destination):
        temporary.replace(destination)

    def discard(self, temporary):
        temporary.unlink(missing_ok=True)


class UrlTransport:
    # Bound stalled reads so cancellation from a peer failure cannot wait forever.
    timeout_seconds = 30

    def chunks(self, request, cancelled):
        http_request = urllib.request.Request(request.url, headers=request.headers)
        with urllib.request.urlopen(
                http_request, timeout=self.timeout_seconds) as response:
            while True:
                if cancelled.is_set():
                    raise DownloadFailure("cancelled after a peer failed")
                chunk = response.read(1024 * 1024)
                if not chunk:
                    return
                yield chunk


def run_downloads(downloads, parallel, progress, transport=None, filesystem=None):
    if parallel not in (1, 2):
        raise ValueError("parallel downloads must be 1 or 2")
    transport = transport or UrlTransport()
    filesystem = filesystem or LocalFilesystem()
    grouped = {}
    for item in downloads:
        grouped.setdefault(item.model_id, []).append(item)
    queue = deque(grouped.values())
    queue_lock = threading.Lock()
    progress_lock = threading.Lock()
    cancelled = threading.Event()
    first_error = []
    model_totals = {}
    model_current = {}
    aggregate_total = sum(item.expected_bytes for item in downloads)
    aggregate_current = 0
    for item in downloads:
        model_totals[item.model_id] = (
            model_totals.get(item.model_id, 0) + item.expected_bytes
        )
        model_current.setdefault(item.model_id, 0)

    def record_progress(item, amount):
        nonlocal aggregate_current
        with progress_lock:
            model_current[item.model_id] += amount
            aggregate_current += amount
            progress.emit(
                "stage_progress", "download", "Download",
                current=model_current[item.model_id],
                total=model_totals[item.model_id], unit="bytes",
                scope_id=item.model_id, scope_label=item.model_label,
            )
            progress.emit(
                "stage_progress", "bytes", "Bytes",
                current=aggregate_current, total=aggregate_total, unit="bytes",
                scope_id="aggregate-bytes",
            )

    def download_one(item):
        temporary, output = filesystem.open_atomic(item.destination)
        digest = hashlib.sha256()
        received = 0
        try:
            with output:
                for chunk in transport.chunks(item, cancelled):
                    received += len(chunk)
                    if received > item.expected_bytes:
                        raise DownloadFailure(
                            f"{item.model_id} exceeded catalog expected_bytes"
                        )
                    output.write(chunk)
                    digest.update(chunk)
                    record_progress(item, len(chunk))
            if received != item.expected_bytes:
                raise DownloadFailure(
                    f"{item.model_id} byte count mismatch: expected "
                    f"{item.expected_bytes}, actual {received}"
                )
            actual = digest.hexdigest()
            if actual != item.sha256:
                raise DownloadFailure(
                    f"{item.model_id} SHA-256 mismatch: expected "
                    f"{item.sha256}, actual {actual}"
                )
            filesystem.publish_atomic(temporary, item.destination)
        except BaseException:
            filesystem.discard(temporary)
            raise

    def worker():
        while not cancelled.is_set():
            with queue_lock:
                if cancelled.is_set() or not queue:
                    return
                model_downloads = queue.popleft()
            try:
                for item in model_downloads:
                    if cancelled.is_set():
                        return
                    download_one(item)
                model = model_downloads[0]
                progress.emit(
                    "stage_completed", "download", "Download",
                    current=model_totals[model.model_id],
                    total=model_totals[model.model_id], unit="bytes",
                    scope_id=model.model_id, scope_label=model.model_label,
                )
            except BaseException as error:
                with queue_lock:
                    if not first_error:
                        first_error.append(error)
                cancelled.set()
                return

    threads = [threading.Thread(target=worker) for _ in range(parallel)]
    try:
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
    except KeyboardInterrupt:
        cancelled.set()
        for thread in threads:
            if thread.ident is not None:
                thread.join()
        raise
    if first_error:
        raise first_error[0]

    progress.emit(
        "stage_completed", "bytes", "Bytes",
        current=aggregate_total, total=aggregate_total, unit="bytes",
        scope_id="aggregate-bytes",
    )

    return aggregate_current, aggregate_total
