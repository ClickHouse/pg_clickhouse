"""Pytest configuration: add this directory to ``sys.path`` so the test file
can ``import mock_clickhouse`` without an installed package."""

from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
