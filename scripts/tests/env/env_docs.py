"""ENV-DOCS: the full documented golden setup.

Chainloader (reflashed) + Retro-Go + all PIE modules + every i18n pack + SD card
with content. The happy path every feature test can assume. The only recipe that
does a full chainloader reflash and pushes everything; all the cheaper recipes
toggle from a baseline.
"""
from common import provision
from . import all_lang_codes

# Resident + transient PIE modules that make up the documented feature set.
ALL_MODULES = ["theme", "language", "installer", "fatfs", "lfs_rw", "fileops", "mp3"]
# The modules whose presence we hard-assert (the rest are best-effort / optional).
KEY_MODULES = {"theme.bin", "language.bin"}


def _expect(env):
    f = []
    if not env.chainloader_alive:
        f.append("chainloader not alive after provisioning ENV-DOCS")
    if env.retrogo_present is False:
        f.append("Retro-Go not present (ENV-DOCS golden path requires it)")
    if env.lfs_listed:
        for m in KEY_MODULES:
            if m not in env.lfs_modules:
                f.append(f"key module {m} missing from LittleFS")
        if not env.lfs_langs:
            f.append("no i18n packs on LittleFS (expected the full set)")
    if env.theme_module_count == 0:
        f.append("theme module did not register any sprite theme")
    return f


RECIPE = provision.Recipe(
    name="ENV-DOCS",
    build_flags="REMOTE_INPUT=1",
    full_image=True,            # rebuild the whole golden image via make flash_all/flash_rg/...
    retrogo=True,
    modules=ALL_MODULES,
    langs=all_lang_codes(),
    settings={"fastboot": False, "lang": 0},
    manual_steps=[
        "Insert an SD card with test content: a playable /test.mp3, a few files "
        "and folders, and optionally an /i18n folder to exercise the installer.",
    ],
    expect=_expect,
)
