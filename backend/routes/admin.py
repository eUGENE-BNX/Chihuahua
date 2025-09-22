from fastapi import APIRouter, HTTPException
from fastapi.responses import JSONResponse
import requests
from urllib.parse import urlparse, urlunparse
from pydantic import BaseModel, Field

from ..core.db import list_devices, get_device, update_config
from ..core.config import (
    DEFAULT_AI_HOST,
    DEFAULT_AI_MODEL,
    DEFAULT_AI_PROMPT,
    DEFAULT_AI_NUM_CTX,
    DEFAULT_AI_NUM_PREDICT,
)

router = APIRouter(prefix="/admin/api", tags=["admin"])


def _row_value(row, key, default=None):
    try:
        return row[key]
    except (KeyError, IndexError, TypeError):
        return default


def _str_or_default(value, default):
    if value is None:
        return default
    text = str(value).strip()
    return text if text else default


def _int_or_default(value, default):
    if value is None:
        return default
    if isinstance(value, str):
        value = value.strip()
        if not value:
            return default
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _bool_or_default(value, default):
    if value is None:
        return default
    if isinstance(value, str):
        lowered = value.strip().lower()
        if lowered in {"1", "true", "yes", "on"}:
            return True
        if lowered in {"0", "false", "no", "off"}:
            return False
    try:
        return bool(int(value))
    except (TypeError, ValueError):
        return default



def _build_ai_health_url(host: str) -> str | None:
    if not host:
        return None
    host = host.strip()
    if not host:
        return None
    if not host.startswith('http://') and not host.startswith('https://'):
        host = 'http://' + host
    parsed = urlparse(host)
    if not parsed.netloc:
        return None
    path = (parsed.path or '').rstrip('/')
    if path.endswith('/api'):
        health_path = f'{path}/tags'
    elif path.endswith('/api/generate'):
        health_path = path[: -len('/generate')] + '/tags'
    elif '/api/generate' in path:
        base = path.split('/api/generate')[0] + '/api'
        health_path = base + '/tags'
    else:
        health_path = f'{path}/api/tags' if path else '/api/tags'
    parsed = parsed._replace(path=health_path, params='', query='', fragment='')
    return urlunparse(parsed)

def _check_ai_status(host: str) -> bool:
    url = _build_ai_health_url(host)
    if not url:
        return False
    try:
        resp = requests.get(url, timeout=2)
    except Exception:
        return False
    return resp.ok
def _device_row(row, include_ai_status: bool = False):
    stored_urls = _row_value(row, "last_img_urls")
    urls = [u for u in str(stored_urls).split("\n") if u] if stored_urls else []
    row_int = lambda key, default: _int_or_default(_row_value(row, key), default)
    row_bool = lambda key, default: _bool_or_default(_row_value(row, key), default)

    ai_host = _str_or_default(_row_value(row, "ai_host"), DEFAULT_AI_HOST)
    ai_model = _str_or_default(_row_value(row, "ai_model"), DEFAULT_AI_MODEL)
    ai_prompt = _str_or_default(_row_value(row, "ai_prompt"), DEFAULT_AI_PROMPT)
    ai_num_ctx = row_int("ai_num_ctx", 1024)
    if ai_num_ctx <= 0:
        ai_num_ctx = 1024
    ai_num_predict = row_int("ai_num_predict", 64)
    if ai_num_predict <= 0:
        ai_num_predict = 64

    brightness = max(-2, min(2, row_int("brightness", 0)))
    contrast = max(-2, min(2, row_int("contrast", 1)))
    saturation = max(-2, min(2, row_int("saturation", 1)))
    sharpness = max(-2, min(2, row_int("sharpness", 1)))
    wb_mode = max(0, min(4, row_int("wb_mode", 0)))
    gainceiling_val = row_int("gainceiling", 4)
    if gainceiling_val < 0:
        gainceiling_val = 0
    if gainceiling_val > 5:
        gainceiling_val = 5
    ae_level = max(-2, min(2, row_int("ae_level", 0)))
    special_effect = max(0, min(6, row_int("special_effect", 0)))

    auto_upload = row_bool("auto_upload", True)
    whitebal = row_bool("whitebal", True)
    hmirror = row_bool("hmirror", False)
    vflip = row_bool("vflip", False)
    awb_gain = row_bool("awb_gain", True)
    gain_ctrl = row_bool("gain_ctrl", True)
    exposure_ctrl = row_bool("exposure_ctrl", True)
    lens_corr = row_bool("lens_corr", True)
    raw_gma = row_bool("raw_gma", True)
    bpc = row_bool("bpc", True)
    wpc = row_bool("wpc", True)
    dcw = row_bool("dcw", True)
    colorbar = row_bool("colorbar", False)
    low_light = row_bool("low_light_boost", True)

    return {
        "deviceId": row["device_id"],
        "fw": row["fw"],
        "ip": row["ip"],
        "rssi": row["rssi"],
        "model": row["model"],
        "lastSeen": row["last_seen"],

        "framesize": row["framesize"],
        "jpegQuality": row_int("jpeg_quality", 15),
        "uploadIntervalSec": row_int("upload_interval_sec", 10),
        "autoUpload": auto_upload,
        "uploadUrl": row["upload_url"],
        "lastImgUrl": row["last_img_url"],
        "lastImgTime": row["last_img_time"],
        "lastImgUrls": urls,
        "lastAnalysis": _row_value(row, "last_analysis"),
        "lastAnalysisTime": _row_value(row, "last_analysis_time"),
        "aiHost": ai_host,
        "aiModel": ai_model,
        "aiPrompt": ai_prompt,
        "aiNumCtx": ai_num_ctx,
        "aiNumPredict": ai_num_predict,
        "whitebal": whitebal,
        "wbMode": wb_mode,
        "hmirror": hmirror,
        "vflip": vflip,
        "brightness": brightness,
        "contrast": contrast,
        "saturation": saturation,
        "sharpness": sharpness,
        "awbGain": awb_gain,
        "gainCtrl": gain_ctrl,
        "exposureCtrl": exposure_ctrl,
        "gainceiling": gainceiling_val,
        "aeLevel": ae_level,
        "lensCorr": lens_corr,
        "rawGma": raw_gma,
        "bpc": bpc,
        "wpc": wpc,
        "dcw": dcw,
        "colorbar": colorbar,
        "specialEffect": special_effect,
        "lowLightBoost": low_light,
        "aiReachable": _check_ai_status(ai_host) if include_ai_status else None,
    }


@router.get("/devices")
def devices():
    rows = list_devices()
    return JSONResponse([_device_row(r) for r in rows])


@router.get("/device/{device_id}")
def device(device_id: str):
    row = get_device(device_id)
    if not row:
        raise HTTPException(status_code=404, detail="Device not found")
    return _device_row(row, include_ai_status=True)

class UpdateConfigBody(BaseModel):
    framesize: str | None = Field(None, description="QQVGA,QVGA,CIF,VGA,SVGA,XGA,SXGA,UXGA")
    jpegQuality: int | None = Field(None, ge=5, le=63)
    uploadIntervalSec: int | None = Field(None, ge=1, le=3600)
    autoUpload: bool | None = None
    uploadUrl: str | None = None
    uploadToken: str | None = None
    whitebal: bool | None = None
    wbMode: int | None = Field(None, ge=0, le=4)
    hmirror: bool | None = None
    vflip: bool | None = None
    brightness: int | None = Field(None, ge=-2, le=2)
    contrast: int | None = Field(None, ge=-2, le=2)
    saturation: int | None = Field(None, ge=-2, le=2)
    sharpness: int | None = Field(None, ge=-2, le=2)
    awbGain: bool | None = None
    gainCtrl: bool | None = None
    exposureCtrl: bool | None = None
    gainceiling: int | None = Field(None, ge=0, le=5)
    aeLevel: int | None = Field(None, ge=-2, le=2)
    lensCorr: bool | None = None
    rawGma: bool | None = None
    bpc: bool | None = None
    wpc: bool | None = None
    dcw: bool | None = None
    colorbar: bool | None = None
    specialEffect: int | None = Field(None, ge=0, le=6)
    lowLightBoost: bool | None = None
    aiHost: str | None = None
    aiModel: str | None = None
    aiPrompt: str | None = None
    aiNumCtx: int | None = Field(None, ge=1)
    aiNumPredict: int | None = Field(None, ge=1)


@router.post("/device/{device_id}/config")
def update_device_config(device_id: str, body: UpdateConfigBody):
    row = get_device(device_id)
    if not row:
        raise HTTPException(status_code=404, detail="Device not found")

    patch = {}
    if body.framesize:
        patch["framesize"] = body.framesize
    if body.jpegQuality is not None:
        patch["jpeg_quality"] = int(body.jpegQuality)
    if body.uploadIntervalSec is not None:
        patch["upload_interval_sec"] = int(body.uploadIntervalSec)
    if body.autoUpload is not None:
        patch["auto_upload"] = 1 if body.autoUpload else 0
    if body.uploadUrl is not None:
        patch["upload_url"] = body.uploadUrl
    if body.uploadToken:
        patch["upload_token"] = body.uploadToken
    if body.whitebal is not None:
        patch["whitebal"] = 1 if body.whitebal else 0
    if body.wbMode is not None:
        patch["wb_mode"] = int(body.wbMode)
    if body.hmirror is not None:
        patch["hmirror"] = 1 if body.hmirror else 0
    if body.vflip is not None:
        patch["vflip"] = 1 if body.vflip else 0
    if body.brightness is not None:
        patch["brightness"] = int(body.brightness)
    if body.contrast is not None:
        patch["contrast"] = int(body.contrast)
    if body.saturation is not None:
        patch["saturation"] = int(body.saturation)
    if body.sharpness is not None:
        patch["sharpness"] = int(body.sharpness)
    if body.awbGain is not None:
        patch["awb_gain"] = 1 if body.awbGain else 0
    if body.gainCtrl is not None:
        patch["gain_ctrl"] = 1 if body.gainCtrl else 0
    if body.exposureCtrl is not None:
        patch["exposure_ctrl"] = 1 if body.exposureCtrl else 0
    if body.gainceiling is not None:
        patch["gainceiling"] = max(0, min(5, int(body.gainceiling)))
    if body.aeLevel is not None:
        patch["ae_level"] = int(body.aeLevel)
    if body.lensCorr is not None:
        patch["lens_corr"] = 1 if body.lensCorr else 0
    if body.rawGma is not None:
        patch["raw_gma"] = 1 if body.rawGma else 0
    if body.bpc is not None:
        patch["bpc"] = 1 if body.bpc else 0
    if body.wpc is not None:
        patch["wpc"] = 1 if body.wpc else 0
    if body.dcw is not None:
        patch["dcw"] = 1 if body.dcw else 0
    if body.colorbar is not None:
        patch["colorbar"] = 1 if body.colorbar else 0
    if body.specialEffect is not None:
        patch["special_effect"] = max(0, min(6, int(body.specialEffect)))
    if body.lowLightBoost is not None:
        patch["low_light_boost"] = 1 if body.lowLightBoost else 0
    if body.aiHost is not None:
        patch["ai_host"] = body.aiHost
    if body.aiModel is not None:
        patch["ai_model"] = body.aiModel
    if body.aiPrompt is not None:
        patch["ai_prompt"] = body.aiPrompt
    if body.aiNumCtx is not None:
        patch["ai_num_ctx"] = int(body.aiNumCtx)
    if body.aiNumPredict is not None:
        patch["ai_num_predict"] = int(body.aiNumPredict)

    if patch:
        update_config(device_id, patch)
    return {"status": "ok"}













