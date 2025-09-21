# backend/app.py
from fastapi import FastAPI
from fastapi.responses import RedirectResponse
from fastapi.staticfiles import StaticFiles

from .core.db import init_db
from .core.config import FRONTEND_DIR, UPLOAD_DIR
from .routes.device import router as device_router
from .routes.upload import router as upload_router
from .routes.admin import router as admin_router

def create_app():
    init_db()
    app = FastAPI(title="HomeDog Backend", version="1.0.0")

    # API router'ları
    app.include_router(device_router)
    app.include_router(upload_router)
    app.include_router(admin_router)

    # Statik dosyalar
    app.mount("/admin", StaticFiles(directory=str(FRONTEND_DIR), html=True), name="admin")
    app.mount("/uploads", StaticFiles(directory=str(UPLOAD_DIR), html=False), name="uploads")

    @app.get("/")
    def root():
        return RedirectResponse(url="/admin/")

    return app

# <— Uvicorn backend.app:app ararken bunu bulmalı
app = create_app()
