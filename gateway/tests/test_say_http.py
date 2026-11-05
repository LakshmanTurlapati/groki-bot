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


@asynccontextmanager
async def end_client(monkeypatch, proxy):
    from types import SimpleNamespace

    monkeypatch.setenv("STACKCHAN_USB_DISABLE", "1")
    gateway = Gateway()
    gateway.esp32 = SimpleNamespace(connection=SimpleNamespace(cloud_proxy=proxy) if proxy else None)
    app = web.Application()
    app.router.add_post("/conversation/end", gateway._handle_conversation_end)
    async with TestClient(TestServer(app)) as client:
        yield client


@pytest.mark.asyncio
async def test_conversation_end_closes_the_live_window(monkeypatch):
    from types import SimpleNamespace

    proxy = SimpleNamespace(end_conversation=AsyncMock(return_value=True))
    async with end_client(monkeypatch, proxy) as client:
        response = await client.post("/conversation/end")
        assert response.status == 200
        assert await response.json() == {"ok": True, "was_listening": True}
        proxy.end_conversation.assert_awaited_once()


@pytest.mark.asyncio
async def test_conversation_end_never_opens_a_session(monkeypatch):
    async with end_client(monkeypatch, None) as client:
        response = await client.post("/conversation/end")
        assert response.status == 409
        assert (await response.json())["ok"] is False


@pytest.mark.asyncio
@pytest.mark.parametrize("headers", [
    {"Origin": "https://attacker.example", "Sec-Fetch-Site": "cross-site"},
    {"Host": "attacker.example"},
])
async def test_conversation_end_rejects_cross_origin_requests(monkeypatch, headers):
    from types import SimpleNamespace

    proxy = SimpleNamespace(end_conversation=AsyncMock(return_value=True))
    async with end_client(monkeypatch, proxy) as client:
        response = await client.post("/conversation/end", headers=headers)
        assert response.status == 403
        proxy.end_conversation.assert_not_awaited()
