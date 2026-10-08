"""What the tests share: the definitions to run against."""

import tempfile
import unittest
from pathlib import Path

from mapconv.definitions import DEFAULT_DIRECTORY, Definitions
from mapconv.tests import fixture_definitions

_fixture = None


def fixture():
    """The made-up definitions (tests/fixture_definitions.py)."""
    global _fixture
    if _fixture is None:
        _fixture = Definitions.load(fixture_definitions.write(tempfile.mkdtemp(prefix='mapconv-defs-')))
    return _fixture


def invader():
    """Invader's definitions, if they have been fetched (python3 -m mapconv
    fetch-definitions); the tests that need them are skipped otherwise."""
    if not (Path(DEFAULT_DIRECTORY) / 'fourcc.hpp').exists():
        raise unittest.SkipTest(f'no Invader definitions in {DEFAULT_DIRECTORY}')
    return Definitions.load(DEFAULT_DIRECTORY)


SOURCE_ROOT = Path(__file__).resolve().parents[4]
