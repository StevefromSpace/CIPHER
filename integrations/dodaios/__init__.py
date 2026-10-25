"""CIPHER — Dodaios Assistant Integration.

Provides optional integration adapter for Dodaios (formerly Personal Jarvis).
"""

from .bridge import DodaiosBridge
from .config import DodaiosConfig

__all__ = ["DodaiosBridge", "DodaiosConfig"]
