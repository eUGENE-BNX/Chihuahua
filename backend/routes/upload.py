from fastapi import APIRouter, Request, HTTPException, status
from fastapi.responses import JSONResponse, HTMLResponse
from typing import List, Optional
from pathlib import Path
import base64
import requests
from urllib.parse import urlparse, urlunparse
from starlette.requests import ClientDisconnect

from ..core.config import (
    UPLOAD_TOKEN,
    DEFAULT_AI_HOST,
    DEFAULT_AI_MODEL,
    DEFAULT_AI_PROMPT,
    DEFAULT_AI_NUM_CTX,
    DEFAULT_AI_NUM_PREDICT,
)
from ..core.storage import save_image
from ..core.db import get_device, update_config

router = APIRouter(tags=["upload"])


def _get_bearer(req: Request) -> Optional[str]:
    auth = req.headers.get("authorization") or req.headers.get("Authorization")
    if not auth:
        return None
    parts = auth.split()
    if len(parts) == 2 and parts[0].lower() == "bearer":
        return parts[1]
    return None


@router.get("/upload")
async def upload_info():
    html = """<!doctype html>
<meta charset=\"utf-8\">
<title>/upload endpoint</title>
<body style=\"font-family:sans-serif;margin:20px\">
  <h2>/upload endpoint</h2>
  <p>This endpoint expects <b>POST image/jpeg</b>. Quick test: <a href=\"/upload/form\">/upload/form</a></p>
  <pre style=\"background:#f6f6f6;padding:10px;border:1px solid #ddd;white-space:pre-wrap\">
curl -v -X POST http://127.0.0.1:8000/upload ^
  -H \"Authorization: Bearer %s\" ^
  -H \"Content-Type: image/jpeg\" ^
  --data-binary \"@C:\\path\\to\\test.jpg\"
  </pre>
</body>
""" % (UPLOAD_TOKEN,)
    return HTMLResponse(html)


@router.get("/upload/form")
async def upload_form():
    html = """<!doctype html>
<meta charset=\"utf-8\">
<title>Upload Test</title>
<body style=\"font-family:sans-serif;margin:20px\">
  <h2>Upload Test</h2>
  <p>Select a JPEG, enter token and click \"Send\".</p>
  <label>Upload Token</label><br>
  <input id=\"tok\" style=\"width:420px\" value=\"%s\"><br><br>
  <input type=\"file\" id=\"file\" accept=\"image/jpeg\"><br><br>
  <button id=\"btn\">Send</button>
  <pre id=\"out\" style=\"white-space:pre-wrap;background:#f6f6f6;padding:10px;border:1px solid #ddd;margin-top:12px\"></pre>

  <script>
    const $ = id => document.getElementById(id);
    $("btn").onclick = async () => {
      const f = $("file").files[0];
      const tok = $("tok").value.trim();
      if (!f) { $("out").textContent = "Select a JPEG first."; return; }
      try {
        const res = await fetch("/upload", {
          method: "POST",
          headers: {
            "Authorization": "Bearer " + tok,
            "Content-Type": f.type || "image/jpeg"
          },
          body: f
        });
        const text = await res.text();
        $("out").textContent = "HTTP " + res.status + "\\n" + text;
      } catch (e) {
        $("out").textContent = "Error: " + e.message;
      }
    };
  </script>
</body>
""" % (UPLOAD_TOKEN,)
    return HTMLResponse(html)


@router.post("/upload")
async def upload(req: Request):
    tok = _get_bearer(req)
    device_id = req.headers.get("X-Device-ID") or "UNKNOWN"
    row = get_device(device_id)

    ok = tok == UPLOAD_TOKEN
    if not ok:
        dev_tok = row["upload_token"] if row else None
        ok = tok is not None and dev_tok and tok == dev_tok

    if not ok:
        bearer = "none" if not tok else tok[:8] + "..."
        print(f"[UPLOAD-401] from {req.client.host} dev={device_id} bearer={bearer}")
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail=f"Unauthorized for device {device_id}")

    ctype = req.headers.get("content-type", "")
    if "image/jpeg" not in ctype:
        print(f"[UPLOAD-415] from {req.client.host} dev={device_id} ctype={ctype!r}")
        raise HTTPException(status_code=status.HTTP_415_UNSUPPORTED_MEDIA_TYPE, detail="Expecting image/jpeg")

    fname = req.headers.get("X-File-Name") or ""
    try:
        raw = await req.body()
    except ClientDisconnect:
        print(f"[UPLOAD-DISCONNECT] from {req.client.host} dev={device_id}")
        raise HTTPException(status_code=getattr(status, "HTTP_499_CLIENT_CLOSED_REQUEST", 400), detail="Client disconnected during upload")
    if not raw:
        print(f"[UPLOAD-400] from {req.client.host} dev={device_id} empty body")
        raise HTTPException(status_code=400, detail="Empty body")

    file_path, url_path, ts = save_image(device_id, raw, fname)

    image_urls = collect_last_images(device_id, url_path, row=row)
    analysis_text = run_ollama_analysis(row, file_path, url_path)

    patch = {
        "last_seen": ts,
        "last_img_url": url_path,
        "last_img_time": ts,
        "last_img_urls": image_urls,
    }
    if analysis_text is not None:
        patch["last_analysis"] = analysis_text
        patch["last_analysis_time"] = ts

    update_config(device_id, patch)

    print(f"[UPLOAD] {req.client.host} dev={device_id} size={len(raw)} saved={file_path}")
    return JSONResponse({"status": "ok", "url": url_path})


def _row_value(row, key, default=None):
    if row is None:
        return default
    try:
        return row[key]
    except (KeyError, IndexError, TypeError):
        return default


def collect_last_images(device_id: str, new_url: str, limit: int = 20, row=None) -> str:
    existing: List[str] = []
    if row is None and device_id:
        try:
            row = get_device(device_id)
        except Exception:
            row = None
    if row is not None:
        stored = _row_value(row, "last_img_urls")
        if stored:
            existing = [u for u in str(stored).split("\n") if u]
    existing.insert(0, new_url)
    return "\n".join(existing[:limit])


def run_ollama_analysis(row, file_path: str, url_path: str) -> Optional[str]:
    host = str((_row_value(row, "ai_host") or DEFAULT_AI_HOST or "")).strip()
    model = str((_row_value(row, "ai_model") or DEFAULT_AI_MODEL or "")).strip()
    prompt_template = _row_value(row, "ai_prompt") or DEFAULT_AI_PROMPT
    if not host or not model or not prompt_template:
        return None

    parsed = urlparse(host.strip())
    path = (parsed.path or "").rstrip("/")

    if path.endswith("/api/generate"):
        final_path = path
    elif path.endswith("/api"):
        final_path = f"{path}/generate"
    elif path.endswith("/generate") and "/api" in path:
        final_path = path
    elif "/api/generate" in path:
        final_path = path
    else:
        final_path = f"{path}/api/generate" if path else "/api/generate"

    parsed = parsed._replace(path=final_path, params="", query="", fragment="")
    endpoint = urlunparse(parsed)

    def _normalize_int(value, fallback):
        if value is None:
            return fallback
        if isinstance(value, str):
            value = value.strip()
            if not value:
                return fallback
        try:
            ivalue = int(value)
        except (TypeError, ValueError):
            return fallback
        return ivalue if ivalue > 0 else fallback

    num_ctx = _normalize_int(_row_value(row, "ai_num_ctx"), DEFAULT_AI_NUM_CTX)
    num_predict = _normalize_int(_row_value(row, "ai_num_predict"), DEFAULT_AI_NUM_PREDICT)

    prompt = str(prompt_template)
    prompt = prompt.replace("{url}", url_path)
    prompt = prompt.replace("{path}", file_path)
    prompt = prompt.replace("{filename}", Path(file_path).name)

    payload = {
        "model": model,
        "prompt": prompt,
        "stream": False,
        "options": {}
    }

    try:
        image_bytes = Path(file_path).read_bytes()
        image_b64 = base64.b64encode(image_bytes).decode("ascii")
        if image_b64:
            payload["images"] = [image_b64]
    except Exception as exc:
        print(f"[OLLAMA] failed to encode image for device={_row_value(row, 'device_id')}: {exc}")

    if num_ctx:
        payload["options"]["num_ctx"] = int(num_ctx)
    if num_predict:
        payload["options"]["num_predict"] = int(num_predict)

    try:
        response = requests.post(endpoint, json=payload, timeout=20)
    except Exception as exc:
        print(f"[OLLAMA] request error for device={_row_value(row, 'device_id')}: {exc}")
        return None

    if response.status_code != 200:
        print(f"[OLLAMA] HTTP {response.status_code} for device={_row_value(row, 'device_id')} endpoint={endpoint} body={response.text[:200]}")
        return None

    try:
        data = response.json()
    except ValueError:
        print(f"[OLLAMA] invalid JSON for device={_row_value(row, 'device_id')}")
        return None

    text = data.get("response") or data.get("output") or data.get("text")
    if isinstance(text, list):
        text = " ".join(str(part) for part in text if part)
    if text:
        return str(text).strip()
    return None


