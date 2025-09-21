from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse
from pydantic import BaseModel
import time

from ..core.config import (
    BACKEND_TOKEN,
    DEFAULT_AI_HOST,
    DEFAULT_AI_MODEL,
    DEFAULT_AI_PROMPT,
    DEFAULT_AI_NUM_CTX,
    DEFAULT_AI_NUM_PREDICT,
    UPLOAD_TOKEN,
)
from ..core.db import upsert_device, get_device, update_config
from ..core.auth import require_bearer


def _clamp(value: int, low: int, high: int) -> int:
    if value < low:
        return low
    if value > high:
        return high
    return value


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

router = APIRouter(prefix="/api", tags=["device"])

class RegisterBody(BaseModel):
    deviceId: str
    uniqueId: str | None = None
    fw: str | None = None
    ip: str | None = None
    rssi: int | None = None
    model: str | None = None
    chipModel: str | None = None
    chipRev: int | None = None
    cores: int | None = None
    psram: bool | None = None
    flashSize: int | None = None
    sdk: str | None = None

@router.post("/register")
async def register(req: Request, body: RegisterBody):
    require_bearer(req, BACKEND_TOKEN)
    device_id = body.deviceId.strip()
    base_url = str(req.base_url).rstrip("/")

    info = {
        "device_id": device_id,
        "fw": body.fw or "",
        "ip": body.ip or req.client.host,
        "rssi": body.rssi or 0,
        "model": body.model or "ESP32-CAM",
        "last_seen": int(time.time()),
        "upload_url": f"{base_url}/upload",
        "upload_token": "",
    }
    upsert_device(info)
    # İsteğe bağlı: burada body.uniqueId ve diğer tanıtıcılar loglanabilir
    return JSONResponse({"status":"ok"})

@router.get("/config")
async def get_config(req: Request, deviceId: str, rev: int = 0):
    require_bearer(req, BACKEND_TOKEN)
    row = get_device(deviceId)
    base_url = str(req.base_url).rstrip("/")

    if not row:
        upsert_device({
            "device_id": deviceId, "fw": "", "ip": req.client.host, "rssi": 0,
            "model": "ESP32-CAM", "last_seen": int(time.time()),
            "upload_url": f"{base_url}/upload", "upload_token": "",
        })
        row = get_device(deviceId)

    upload_url = row["upload_url"] or f"{base_url}/upload"
    upload_token = row["upload_token"] or UPLOAD_TOKEN

    def _clean_str(value, default):
        if value is None:
            return default
        text = str(value).strip()
        return text if text else default

    def _clean_int(value, default):
        if value is None:
            return default
        try:
            ivalue = int(value)
        except (TypeError, ValueError):
            return default
        return ivalue if ivalue > 0 else default

    ai_host = _clean_str(row["ai_host"], DEFAULT_AI_HOST)
    ai_model = _clean_str(row["ai_model"], DEFAULT_AI_MODEL)
    ai_prompt = _clean_str(row["ai_prompt"], DEFAULT_AI_PROMPT)
    ai_num_ctx = _clean_int(row["ai_num_ctx"], DEFAULT_AI_NUM_CTX)
    ai_num_predict = _clean_int(row["ai_num_predict"], DEFAULT_AI_NUM_PREDICT)

    update_config(
        deviceId,
        {
            "last_seen": int(time.time()),
            "upload_url": upload_url,
            "upload_token": upload_token,
        },
    )

    row_int = lambda key, default: _int_or_default(_row_value(row, key), default)
    row_bool = lambda key, default: _bool_or_default(_row_value(row, key), default)

    brightness = _clamp(row_int("brightness", 0), -2, 2)
    contrast = _clamp(row_int("contrast", 1), -2, 2)
    saturation = _clamp(row_int("saturation", 1), -2, 2)
    sharpness = _clamp(row_int("sharpness", 1), -2, 2)
    awb_gain = row_bool("awb_gain", True)
    gain_ctrl = row_bool("gain_ctrl", True)
    exposure_ctrl = row_bool("exposure_ctrl", True)
    gainceiling_val = row_int("gainceiling", 4)
    if gainceiling_val < 0:
        gainceiling_val = 0
    if gainceiling_val > 5:
        gainceiling_val = 5
    ae_level = _clamp(row_int("ae_level", 0), -2, 2)
    lens_corr = row_bool("lens_corr", True)
    raw_gma = row_bool("raw_gma", True)
    bpc = row_bool("bpc", True)
    wpc = row_bool("wpc", True)
    dcw = row_bool("dcw", True)
    colorbar = row_bool("colorbar", False)
    special_effect = _clamp(row_int("special_effect", 0), 0, 6)
    low_light = row_bool("low_light_boost", True)
    whitebal_val = row_bool("whitebal", True)
    wb_mode_val = _clamp(row_int("wb_mode", 0), 0, 4)
    hmirror_val = row_bool("hmirror", False)
    vflip_val = row_bool("vflip", False)
    auto_upload = row_bool("auto_upload", True)

    data = {
        "rev": row["config_rev"] or 1,
        "framesize": row["framesize"] or "VGA",
        "jpegQuality": row_int("jpeg_quality", 15),
        "uploadIntervalSec": row_int("upload_interval_sec", 10),
        "uploadUrl": upload_url,
        "uploadToken": upload_token,
        "autoUpload": auto_upload,
        "whitebal": whitebal_val,
        "wbMode": wb_mode_val,
        "hmirror": hmirror_val,
        "vflip": vflip_val,
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
        "aiHost": ai_host,
        "aiModel": ai_model,
        "aiPrompt": ai_prompt,
        "aiNumCtx": ai_num_ctx,
        "aiNumPredict": ai_num_predict,
    }
    return JSONResponse(data)























