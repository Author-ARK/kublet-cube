"""Tests for the device registry."""

import pytest

from kublet_dev.devices import (
    load_devices,
    resolve_device_ip,
    save_device,
    save_devices,
)


def test_load_empty(tmp_path):
    """Loading from a nonexistent file returns an empty dict."""
    devices = load_devices(tmp_path / ".devices.yml")
    assert devices == {}


def test_save_and_load_roundtrip(tmp_path):
    """Devices written can be read back."""
    path = tmp_path / ".devices.yml"
    save_devices(
        {"kitchen": "192.168.1.50", "office": "192.168.1.51", "_default": "kitchen"},
        path,
    )
    devices = load_devices(path)
    assert devices["kitchen"] == "192.168.1.50"
    assert devices["office"] == "192.168.1.51"
    assert devices["_default"] == "kitchen"


def test_save_device_sets_default(tmp_path, monkeypatch):
    """Registering a device makes it the default."""
    path = tmp_path / ".devices.yml"
    # Suppress print output
    monkeypatch.setattr("kublet_dev.devices.REPO_ROOT", tmp_path)
    save_device("kitchen", "192.168.1.50", path)
    save_device("office", "192.168.1.51", path)
    devices = load_devices(path)
    assert devices["_default"] == "office"  # last registered = default
    assert devices["kitchen"] == "192.168.1.50"
    assert devices["office"] == "192.168.1.51"


def test_resolve_ip_override(tmp_path):
    """--ip flag wins over everything."""
    ip = resolve_device_ip(None, "10.0.0.1", tmp_path / ".devices.yml")
    assert ip == "10.0.0.1"


def test_resolve_by_name(tmp_path):
    """Lookup by device name."""
    path = tmp_path / ".devices.yml"
    save_devices({"kitchen": "192.168.1.50", "_default": "kitchen"}, path)
    ip = resolve_device_ip("kitchen", None, path)
    assert ip == "192.168.1.50"


def test_resolve_default(tmp_path):
    """Falls back to _default when no device name given."""
    path = tmp_path / ".devices.yml"
    save_devices({"kitchen": "192.168.1.50", "_default": "kitchen"}, path)
    ip = resolve_device_ip(None, None, path)
    assert ip == "192.168.1.50"


def test_resolve_unknown_device(tmp_path):
    """Unknown device name exits with error."""
    path = tmp_path / ".devices.yml"
    save_devices({"kitchen": "192.168.1.50", "_default": "kitchen"}, path)
    with pytest.raises(SystemExit):
        resolve_device_ip("nonexistent", None, path)


def test_resolve_no_default(tmp_path):
    """No default and no device name exits with error."""
    path = tmp_path / ".devices.yml"
    # Empty file
    with pytest.raises(SystemExit):
        resolve_device_ip(None, None, path)
