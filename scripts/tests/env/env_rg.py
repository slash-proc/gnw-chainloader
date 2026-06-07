"""ENV-RG: chainloader + Retro-Go + minimal extflash filesystems.

No OFW in Bank 2, no SD, no modules/packs. Exercises the Retro-Go round trip and
fast-boot path with the simplest valid launcher target present.
"""
from common import provision


def _expect(env):
    f = []
    if not env.chainloader_alive:
        f.append("chainloader not alive after provisioning ENV-RG")
    if env.retrogo_present is False:
        f.append("Retro-Go not present (ENV-RG requires a valid Retro-Go)")
    if env.ofw_in_bank2:
        f.append(f"Bank 2 OFW present ({env.ofw_in_bank2}); ENV-RG expects none")
    return f


RECIPE = provision.Recipe(
    name="ENV-RG",
    retrogo=True,
    bank2="erase",
    modules=[],
    langs=[],
    settings={"fastboot": False, "lang": 0},
    manual_steps=["Remove the SD card (ENV-RG expects no card present)."],
    expect=_expect,
)
