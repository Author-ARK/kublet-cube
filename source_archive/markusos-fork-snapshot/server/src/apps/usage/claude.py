"""Claude Code usage endpoint."""

import json
import os
import subprocess
from pathlib import Path

_DIR = Path(__file__).parent


def get_usage_data(log, cached, **_kwargs) -> dict:
    """Fetch Claude Code usage by running fetch_claude_usage.sh."""

    def _fetch():
        script = _DIR / "fetch_claude_usage.sh"
        try:
            result = subprocess.run(
                [str(script)],
                capture_output=True,
                text=True,
                timeout=30,
                env={**os.environ, "CLAUDECODE": ""},
            )
            data = json.loads(result.stdout.strip())
            log(
                f"usage: session={data['session']['percent']}% weekly={data['weekly']['percent']}%"
            )
            return data
        except (subprocess.TimeoutExpired, json.JSONDecodeError, KeyError) as e:
            log(f"usage fetch error: {e}")
            return {"session": {"percent": 0}, "weekly": {"percent": 0}}

    return cached("usage", 300, _fetch)
