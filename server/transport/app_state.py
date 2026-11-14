"""实时 aiohttp 应用使用的带类型状态。"""

from dataclasses import dataclass, field

from aiohttp import ClientSession, web


@dataclass
class VoiceConnections:
    active: int = 0
    sockets: set = field(default_factory=set)


CONNECTIONS = web.AppKey("voice_connections", VoiceConnections)
PROVIDER_HTTP = web.AppKey("provider_http", ClientSession)
