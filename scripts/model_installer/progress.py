import json
import threading


class JsonProgress:
    def __init__(self, stream):
        self._stream = stream
        self._lock = threading.Lock()

    def emit(self, kind, stage, label, *, current=None, total=None, unit=None,
             scope_id=None, scope_label=None, message=None):
        event = {"kind": kind, "stage": stage, "stage_label": label}
        for key, value in (
            ("current", current), ("total", total), ("unit", unit),
            ("scope_id", scope_id), ("scope_label", scope_label),
            ("message", message),
        ):
            if value is not None and value != "":
                event[key] = value
        if current is not None and total:
            event["fraction"] = current / total
        with self._lock:
            self._stream.write(json.dumps(event, separators=(",", ":")) + "\n")
            self._stream.flush()
