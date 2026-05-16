"""Tests for config helpers."""

import pytest

from kublet_dev.config import load_env, resolve_app_dir, save_env


def test_load_env_empty(tmp_path):
    """Loading from a nonexistent file returns an empty dict."""
    env = load_env(tmp_path / ".env")
    assert env == {}


def test_load_save_env_roundtrip(tmp_path, monkeypatch):
    """Env values written can be read back."""
    path = tmp_path / ".env"
    # Suppress print by monkeypatching REPO_ROOT
    monkeypatch.setattr("kublet_dev.config.REPO_ROOT", tmp_path)
    save_env({"FOO": "bar", "BAZ": "qux"}, path)
    env = load_env(path)
    assert env["FOO"] == "bar"
    assert env["BAZ"] == "qux"


def test_load_env_ignores_comments(tmp_path):
    """Comments and blank lines are skipped."""
    path = tmp_path / ".env"
    path.write_text("# comment\n\nKEY=value\n")
    env = load_env(path)
    assert env == {"KEY": "value"}


def test_resolve_app_dir_by_name(tmp_path):
    """Resolves app by name from apps directory."""
    app_dir = tmp_path / "myapp"
    app_dir.mkdir()
    (app_dir / "platformio.ini").touch()

    result = resolve_app_dir("myapp", apps_dir=tmp_path)
    assert result == app_dir


def test_resolve_app_dir_missing(tmp_path):
    """Missing app exits with error."""
    with pytest.raises(SystemExit):
        resolve_app_dir("nonexistent", apps_dir=tmp_path)


def test_resolve_app_dir_cwd(tmp_path, monkeypatch):
    """Resolves from current directory when no name given."""
    (tmp_path / "platformio.ini").touch()
    monkeypatch.chdir(tmp_path)

    result = resolve_app_dir(None, apps_dir=tmp_path / "apps")
    assert result == tmp_path
