"""ENV-NO-EXTFLASH: external OSPI flash content erased.

We cannot unsolder the chip, so we simulate "no extflash" by erasing the
partition head to 0xFF: the OSPI probe still detects the chip size, but no
filesystems validate, so the partition scan finds nothing. The invariant must
hold — the menu stays reachable and the OSPI-absent path degrades gracefully.
True chip-absent is a documented manual case.
"""
from common import provision


def _expect(env):
    f = []
    if not env.chainloader_alive:
        f.append("chainloader not alive after provisioning ENV-NO-EXTFLASH")
    # Chip size may still report (detection is content-independent); what must
    # hold is that no LittleFS content is found and the menu is up.
    if env.lfs_listed and (env.lfs_modules or env.lfs_langs):
        f.append("LittleFS content still present after erasing the partition head")
    return f


RECIPE = provision.Recipe(
    name="ENV-NO-EXTFLASH",
    extflash="erase",
    modules=None,          # leave whatever is on internal flash; only ext is wiped
    settings={"fastboot": False, "lang": 0},
    manual_steps=["Remove the SD card to isolate the external-flash-absent path."],
    expect=_expect,
)
