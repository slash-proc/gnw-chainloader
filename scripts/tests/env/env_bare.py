"""ENV-BARE: the chainloader is the sole thing on the device.

No Retro-Go, no OFW in Bank 2, extflash partition head erased, no SD, bare
LittleFS. The hardest test of the stability invariant: the menu must still be
reachable and navigable with nothing else installed.
"""
from common import provision


def _expect(env):
    f = []
    if not env.chainloader_alive:
        f.append("chainloader not alive after provisioning ENV-BARE")
    if env.retrogo_present:
        f.append("Retro-Go still present (ENV-BARE expects it absent)")
    if env.ofw_in_bank2:
        f.append(f"Bank 2 OFW still present ({env.ofw_in_bank2}); expected empty")
    if env.lfs_listed and env.lfs_modules:
        f.append(f"LittleFS modules present: {sorted(env.lfs_modules)}; expected none")
    if env.lfs_listed and env.lfs_langs:
        f.append(f"LittleFS langs present: {sorted(env.lfs_langs)}; expected none")
    return f


RECIPE = provision.Recipe(
    name="ENV-BARE",
    retrogo=False,
    bank2="erase",
    extflash="erase",
    modules=[],
    langs=[],
    settings={"fastboot": False, "lang": 0, "mario_slot": 0, "zelda_slot": 0},
    manual_steps=["Remove the SD card (ENV-BARE expects no card present)."],
    expect=_expect,
)
