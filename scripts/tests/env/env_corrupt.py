"""ENV-CORRUPT: deliberately corrupted regions.

An invalid Retro-Go reset vector and an erased Bank 2, to prove the boot path
survives garbage in the secondary slots: the menu must remain reachable, LAUNCH
greys out the unbootable targets, and nothing hangs or HardFaults. The single
non-negotiable invariant under adversarial flash state.
"""
from common import provision


def _expect(env):
    f = []
    if not env.chainloader_alive:
        f.append("chainloader not alive after provisioning ENV-CORRUPT "
                 "(the invariant FAILED — the menu must survive corruption)")
    if env.retrogo_present:
        f.append("Retro-Go still validates (ENV-CORRUPT expects an invalid vector)")
    return f


RECIPE = provision.Recipe(
    name="ENV-CORRUPT",
    retrogo=False,        # write a zero (invalid) reset-vector pair at RETROGO_BASE
    bank2="erase",        # invalidate the OFW image
    settings={"fastboot": False, "lang": 0},
    manual_steps=[],
    expect=_expect,
)
