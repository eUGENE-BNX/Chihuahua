# backend/core/db.py
import sqlite3
from typing import Optional, Dict, Any, List
from .config import DB_PATH

def get_conn():
    conn = sqlite3.connect(DB_PATH, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    return conn

def _has_col(cur, table: str, col: str) -> bool:
    cur.execute(f"PRAGMA table_info({table})")
    return any(r[1] == col for r in cur.fetchall())

def init_db():
    conn = get_conn()
    cur = conn.cursor()
    cur.execute("""
    CREATE TABLE IF NOT EXISTS devices (
        device_id TEXT PRIMARY KEY,
        fw TEXT,
        ip TEXT,
        rssi INTEGER,
        model TEXT,
        last_seen INTEGER,
        config_rev INTEGER DEFAULT 1,

        framesize TEXT DEFAULT 'VGA',
        jpeg_quality INTEGER DEFAULT 15,
        upload_interval_sec INTEGER DEFAULT 10,
        auto_upload INTEGER DEFAULT 1,

        upload_url TEXT,
        upload_token TEXT
    );
    """)
    # Yeni sütunlar
    cols = [
        ("whitebal", "INTEGER DEFAULT 1"),
        ("wb_mode", "INTEGER DEFAULT 0"),
        ("hmirror", "INTEGER DEFAULT 0"),
        ("vflip", "INTEGER DEFAULT 0"),
        ("brightness", "INTEGER DEFAULT 0"),
        ("contrast", "INTEGER DEFAULT 0"),
        ("saturation", "INTEGER DEFAULT 0"),
        ("sharpness", "INTEGER DEFAULT 0"),
        ("awb_gain", "INTEGER DEFAULT 1"),
        ("gain_ctrl", "INTEGER DEFAULT 1"),
        ("exposure_ctrl", "INTEGER DEFAULT 1"),
        ("gainceiling", "INTEGER DEFAULT 4"),
        ("ae_level", "INTEGER DEFAULT 0"),
        ("lens_corr", "INTEGER DEFAULT 1"),
        ("raw_gma", "INTEGER DEFAULT 1"),
        ("bpc", "INTEGER DEFAULT 1"),
        ("wpc", "INTEGER DEFAULT 1"),
        ("dcw", "INTEGER DEFAULT 1"),
        ("colorbar", "INTEGER DEFAULT 0"),
        ("special_effect", "INTEGER DEFAULT 0"),
        ("low_light_boost", "INTEGER DEFAULT 1"),
        # >>> son resim alanları
        ("last_img_url", "TEXT"),
        ("last_img_time", "INTEGER"),
        ("last_img_urls", "TEXT"),
        ("last_analysis", "TEXT"),
        ("last_analysis_time", "INTEGER"),
        ("ai_host", "TEXT"),
        ("ai_model", "TEXT"),
        ("ai_prompt", "TEXT"),
        ("ai_num_ctx", "INTEGER"),
        ("ai_num_predict", "INTEGER"),
    ]
    for name, decl in cols:
        if not _has_col(cur, "devices", name):
            cur.execute(f"ALTER TABLE devices ADD COLUMN {name} {decl}")

    conn.commit()
    conn.close()

def upsert_device(info: Dict[str, Any]):
    conn = get_conn()
    cur = conn.cursor()
    cur.execute("""
        INSERT INTO devices(device_id, fw, ip, rssi, model, last_seen, upload_url, upload_token)
        VALUES (:device_id, :fw, :ip, :rssi, :model, :last_seen, :upload_url, :upload_token)
        ON CONFLICT(device_id) DO UPDATE SET
            fw=excluded.fw,
            ip=excluded.ip,
            rssi=excluded.rssi,
            model=excluded.model,
            last_seen=excluded.last_seen,
            upload_url=COALESCE(devices.upload_url, excluded.upload_url),
            upload_token=COALESCE(devices.upload_token, excluded.upload_token);
    """, info)
    conn.commit()
    conn.close()

def get_device(device_id: str) -> Optional[sqlite3.Row]:
    conn = get_conn()
    cur = conn.cursor()
    cur.execute("SELECT * FROM devices WHERE device_id=?", (device_id,))
    row = cur.fetchone()
    conn.close()
    return row

def list_devices() -> List[sqlite3.Row]:
    conn = get_conn()
    cur = conn.cursor()
    cur.execute("""
        SELECT device_id, fw, ip, rssi, model, last_seen, config_rev,
               framesize, jpeg_quality, upload_interval_sec, auto_upload,
               upload_url,
               whitebal, wb_mode, hmirror, vflip, brightness, contrast, saturation,
               sharpness, awb_gain, gain_ctrl, exposure_ctrl, gainceiling, ae_level,
               lens_corr, raw_gma, bpc, wpc, dcw, colorbar, special_effect, low_light_boost,
               last_img_url, last_img_time,
               last_img_urls, last_analysis, last_analysis_time,
               ai_host, ai_model, ai_prompt, ai_num_ctx, ai_num_predict
        FROM devices
        ORDER BY (last_seen IS NULL) ASC, last_seen DESC
    """)
    rows = cur.fetchall()
    conn.close()
    return rows

def update_config(device_id: str, cfg: Dict[str, Any]):
    conn = get_conn()
    cur = conn.cursor()
    sets, params = [], {"device_id": device_id}
    for k, v in cfg.items():
        sets.append(f"{k} = :{k}")
        params[k] = v
    if sets:
        sql = f"UPDATE devices SET {', '.join(sets)} WHERE device_id = :device_id"
        cur.execute(sql, params)
        conn.commit()
    conn.close()

def bump_rev(device_id: str) -> int:
    conn = get_conn()
    cur = conn.cursor()
    cur.execute("UPDATE devices SET config_rev = COALESCE(config_rev,1) + 1 WHERE device_id=?", (device_id,))
    cur.execute("SELECT config_rev FROM devices WHERE device_id=?", (device_id,))
    rev = cur.fetchone()[0]
    conn.commit()
    conn.close()
    return rev


