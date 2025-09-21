from fastapi import Request, HTTPException, status

def _get_bearer(req: Request) -> str | None:
    auth = req.headers.get("authorization") or req.headers.get("Authorization")
    if not auth:
        return None
    parts = auth.split()
    if len(parts) == 2 and parts[0].lower() == "bearer":
        return parts[1]
    return None

def require_bearer(req: Request, expected: str):
    tok = _get_bearer(req)
    if tok != expected:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="Unauthorized")
