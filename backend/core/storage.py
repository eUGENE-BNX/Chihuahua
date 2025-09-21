from .config import UPLOAD_DIR
from pathlib import Path
import time
import re

SAFE = re.compile(r"[^A-Za-z0-9_\-\.]")

def safe_name(name: str) -> str:
    return SAFE.sub("_", name)

def save_image(device_id: str, raw: bytes, suggested: str | None) -> tuple[str, str, int]:
    ts = int(time.time())
    d = UPLOAD_DIR / device_id
    d.mkdir(parents=True, exist_ok=True)
    fname = suggested or f"{device_id}_{ts}.jpg"
    fname = safe_name(fname)
    if not fname.lower().endswith(".jpg"):
        fname += ".jpg"
    path = d / fname
    path.write_bytes(raw)
    # StaticFiles serves uploads under /uploads
    url_path = f"/uploads/{device_id}/{fname}"
    return str(path), url_path, ts
