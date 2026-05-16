"""Tests for build helpers."""

from unittest.mock import MagicMock, patch

from kublet_dev.build import ota_send, write_user_setup
from kublet_dev.config import USER_SETUP_H


def test_write_user_setup(tmp_path):
    """User_Setup.h is written with 40 MHz SPI config."""
    app_dir = tmp_path / "myapp"
    tft_dir = app_dir / ".pio/libdeps/esp32dev/TFT_eSPI"
    tft_dir.mkdir(parents=True)

    write_user_setup(app_dir)

    setup = (tft_dir / "User_Setup.h").read_text()
    assert setup == USER_SETUP_H
    assert "SPI_FREQUENCY 40000000" in setup
    assert "ST7789_DRIVER" in setup


def test_ota_send_builds_multipart(tmp_path):
    """OTA send constructs correct multipart body."""
    firmware = tmp_path / "firmware.bin"
    firmware.write_bytes(b"\x00" * 100)

    mock_conn = MagicMock()
    mock_resp = MagicMock()
    mock_resp.read.return_value = b"OK"
    mock_conn.getresponse.return_value = mock_resp
    # sock needed for settimeout call
    mock_conn.sock = MagicMock()

    with patch("kublet_dev.build.http.client.HTTPConnection", return_value=mock_conn), \
         patch("kublet_dev.build._verify_reboot"):
        ota_send("192.168.1.50", firmware, "test-app")

        # Verify connection target
        mock_conn.putrequest.assert_called_once_with("POST", "/update")

        # Verify multipart headers were set
        headers = {call[0][0]: call[0][1] for call in mock_conn.putheader.call_args_list}
        assert "Content-Type" in headers
        assert "multipart/form-data" in headers["Content-Type"]
        assert "Content-Length" in headers
        assert "X-Firmware-MD5" in headers

        # Verify firmware data was sent (header_part + firmware chunks + footer)
        sent_data = b"".join(call[0][0] for call in mock_conn.send.call_args_list)
        assert b'name="filedata"' in sent_data
        assert b"firmware.bin" in sent_data
        assert b"\x00" * 100 in sent_data
