from __future__ import annotations

import json
from pathlib import Path
from typing import Any

PROJECT_ROOT = Path(__file__).resolve().parents[1]


def sample(name: str) -> dict[str, Any]:
    return json.loads(
        (PROJECT_ROOT / "config" / name).read_text(encoding="utf-8")
    )
