import os
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
DATA_DIR = PROJECT_ROOT / "data"
UPLOAD_DIR = PROJECT_ROOT / "uploads"
FRONTEND_DIR = PROJECT_ROOT / "frontend"

DATA_DIR.mkdir(parents=True, exist_ok=True)
UPLOAD_DIR.mkdir(parents=True, exist_ok=True)

DB_PATH = DATA_DIR / "devices.sqlite3"

# Varsayilan tokenlar (gerekirse degistir)
BACKEND_TOKEN = os.getenv("BACKEND_TOKEN", "1234567890")
UPLOAD_TOKEN = os.getenv("UPLOAD_TOKEN", "0987654321")

def _env_int(name: str, default: int) -> int:
    value = os.getenv(name)
    if value is None:
        return default
    value = value.strip()
    if not value:
        return default
    try:
        return int(value)
    except ValueError:
        return default

DEFAULT_AI_HOST = os.getenv("DEFAULT_AI_HOST", "http://192.168.1.90:11434")
DEFAULT_AI_MODEL = os.getenv("DEFAULT_AI_MODEL", "gemma3:12b")
DEFAULT_AI_PROMPT = os.getenv("DEFAULT_AI_PROMPT", "Bu resimde ne goruyorsun, kisaca tanimla? {path}")
DEFAULT_AI_NUM_CTX = _env_int("DEFAULT_AI_NUM_CTX", 1024)
DEFAULT_AI_NUM_PREDICT = _env_int("DEFAULT_AI_NUM_PREDICT", 64)
