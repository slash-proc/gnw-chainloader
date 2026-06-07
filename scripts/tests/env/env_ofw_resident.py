"""ENV-OFW-RESIDENT: an OFW actually resident in Bank 2.

Bank 2 holds Mario (the default; a sibling recipe could pin Zelda). Exercises the
no-flash-needed boot path, the bank swap, and the in-game escape. Bank-2 flashing
is done through the LAUNCH menu (warm-switch) rather than a host flash, so apply()
logs guidance instead of auto-flashing.
"""
from common import provision


def _expect(env):
    f = []
    if not env.chainloader_alive:
        f.append("chainloader not alive after provisioning ENV-OFW-RESIDENT")
    if env.bank_swapped:
        f.append("device booted into the OFW (banks swapped); expected the menu")
    if env.ofw_in_bank2 not in ("MARIO", "ZELDA"):
        f.append(f"no recognized OFW resident in Bank 2 (got {env.ofw_in_bank2}); "
                 f"flash one via the LAUNCH menu first")
    return f


RECIPE = provision.Recipe(
    name="ENV-OFW-RESIDENT",
    retrogo=True,
    bank2="mario",
    modules=["theme", "language"],
    settings={"fastboot": False, "lang": 0},
    manual_steps=[
        "Ensure Mario (or Zelda) is resident in Bank 2: select it once in the "
        "LAUNCH menu so it flashes from the extflash backup, then escape with "
        "LEFT+GAME back to the chainloader.",
    ],
    expect=_expect,
)
