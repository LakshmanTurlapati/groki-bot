# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: MIT
from contextlib import asynccontextmanager
from unittest.mock import AsyncMock

from aiohttp import web
from aiohttp.test_utils import TestClient, TestServer
import pytest

from stackchan_mcp.gateway import Gateway


@asynccontextmanager
async def say_client(monkeypatch):
    monkeypatch.setenv("STACKCHAN_USB_DISABLE", "1")
    gateway = Gateway()
    gateway._gemini_say = AsyncMock(return_value={"ok": True})
    app = web.Application()
    app.router.add_post("/say", gateway._handle_say)
    async with TestClient(TestServer(app)) as client:
        yield client, gateway._gemini_say


@pytest.mark.asyncio
async def test_say_accepts_cli_without_origin_and_same_origin_browser(monkeypatch):
    async with say_client(monkeypatch) as (client, speak):
        origin = str(client.make_url("/")).rstrip("/")
        response = await client.post("/say", json={"text": "hello"})
        assert response.status == 200
        response = await client.post("/say", json={"text": "again"},
                                     headers={"Origin": origin, "Sec-Fetch-Site": "same-origin"})
        assert response.status == 200
        assert speak.await_count == 2


@pytest.mark.asyncio
@pytest.mark.parametrize("headers", [
    {"Origin": "https://attacker.example", "Sec-Fetch-Site": "cross-site"},
    {"Origin": "https://attacker.example"},
    {"Origin": "null"},
    {"Origin": "http://127.0.0.1:9"},
    {"Sec-Fetch-Site": "same-site"},
    {"Sec-Fetch-Site": "cross-site"},
    {"Host": "attacker.example", "Origin": "http://attacker.example", "Sec-Fetch-Site": "same-origin"},
    {"Host": "attacker.example"},
    {"Host": "user@127.0.0.1"},
])
async def test_say_rejects_cross_origin_simple_posts_and_dns_rebinding(monkeypatch, headers):
    async with say_client(monkeypatch) as (client, speak):
        response = await client.post("/say", data='{"text":"not authorized"}',
                                     headers={"Content-Type": "text/plain", **headers})
        assert response.status == 403
        speak.assert_not_called()
