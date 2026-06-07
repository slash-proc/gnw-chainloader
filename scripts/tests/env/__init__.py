"""Named environment fixtures for the QA suite.

Each `env_*.py` module exports a single declarative ``RECIPE`` (a
``common.provision.Recipe``) with an ``expect`` closure that verifies the device
reached the intended state. This package collects them into ``ENVIRONMENTS``,
the registry the suite runner consumes (`run_suite.py --matrix ENV-...`).

Recipes are DATA, not code: they say *what* the environment is (Retro-Go present?,
which modules/langs, which settings, Bank-2 contents, extflash); `provision.apply`
turns that into the cheap toggles + targeted flashes that compose it, reserving
full-image flashes for ENV-DOCS.
"""
from __future__ import annotations

import sys
from pathlib import Path

# scripts/tests/env/__init__.py -> parents[2] is scripts/, so `from common import ...`
# resolves whether this package is imported by the runner or directly.
_SCRIPTS = Path(__file__).resolve().parents[2]
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

_REPO = Path(__file__).resolve().parents[3]


def all_lang_codes() -> list:
    """Every real language code, scanned from i18n/lang/<code>/ (never a literal —
    the count grows as languages are added)."""
    root = _REPO / "i18n" / "lang"
    if not root.is_dir():
        return []
    return sorted(p.name for p in root.iterdir()
                  if p.is_dir() and not p.name.startswith("_"))


# Import the fixtures AFTER all_lang_codes is defined (env_docs uses it).
from . import (env_bare, env_rg, env_docs, env_ofw_resident,    # noqa: E402
               env_no_extflash, env_stale_abi, env_corrupt)

_MODULES = [env_bare, env_rg, env_docs, env_ofw_resident,
            env_no_extflash, env_stale_abi, env_corrupt]

ENVIRONMENTS = {m.RECIPE.name: m.RECIPE for m in _MODULES}


def get(name: str):
    """Return the Recipe for `name`, or raise KeyError with the known set."""
    try:
        return ENVIRONMENTS[name]
    except KeyError:
        raise KeyError(f"unknown environment {name!r}; known: {sorted(ENVIRONMENTS)}")


def names() -> list:
    return sorted(ENVIRONMENTS)
