"""ENV-STALE-ABI: wrong-ABI modules and mutated-ABI packs on LittleFS.

The core's load + install gates must reject both artifact classes in both
directions (too old, too new) and fall back to English / skip the module, never
loading a mismatched artifact. The bad artifacts are produced and pushed by the
ABI test itself (test_abi_reject); this recipe just establishes the baseline that
the rest of the device is otherwise healthy.
"""
from common import provision


def _expect(env):
    f = []
    if not env.chainloader_alive:
        f.append("chainloader not alive after provisioning ENV-STALE-ABI")
    # The defining assertions (rejection) live in test_abi_reject, which builds the
    # mismatched artifacts itself; here we only require a healthy baseline so the
    # ABI hook can run.
    if env.modules_ready is False:
        f.append("partition/module scan did not complete")
    return f


RECIPE = provision.Recipe(
    name="ENV-STALE-ABI",
    retrogo=True,
    modules=["language", "installer"],
    langs=["it_IT"],
    settings={"fastboot": False, "lang": 0},
    manual_steps=[],
    expect=_expect,
)
