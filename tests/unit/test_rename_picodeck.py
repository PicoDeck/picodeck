"""Unit tests for tools/rename_picodeck.py. Run: python3 -m pytest tests/unit/test_rename_picodeck.py -v"""
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import rename_picodeck as rp  # noqa: E402

CASES = [
    ("--- PicOS booting ---", "--- PicoDeck booting ---"),
    ("void picos_main(const PicoCalcAPI *api)", "void picodeck_main(const PicoCalcAPI *api)"),
    ("export PICOS_SIM_BINARY=x/picos_simulator", "export PICODECK_SIM_BINARY=x/picodeck_simulator"),
    ("class PicosSimulator:", "class PicodeckSimulator:"),
    ('luaL_newmetatable(L, "PicOSSprite")', 'luaL_newmetatable(L, "PicoDeckSprite")'),
    ('"id": "com.picos.snake"', '"id": "net.picodeck.snake"'),
    ('local CACHE_DIR = "/data/com.picos.store"', 'local CACHE_DIR = "/data/net.picodeck.store"'),
    ('local CATALOG_HOST = "picos.jeffory.dev"', 'local CATALOG_HOST = "store.picodeck.net"'),
    ("https://github.com/jeffory/PicOS/wiki/Lua-SDK-Reference", "https://picodeck.net/docs/lua-sdk-reference/"),
    ("see https://github.com/jeffory/PicOS/wiki for more", "see https://picodeck.net/docs/ for more"),
    ("github.com/jeffory/PicOS/wiki/Home", "https://picodeck.net/docs/"),
    ('local GH_REPO = "jeffory/picOS"', 'local GH_REPO = "PicoDeck/picodeck"'),
    ("url = https://github.com/jeffory/PicOS.git", "url = https://github.com/PicoDeck/picodeck.git"),
    ("https://github.com/jeffory/PicOS/releases", "https://github.com/PicoDeck/picodeck/releases"),
    ('"repo": "jeffory/picos-apps",', '"repo": "PicoDeck/picodeck",'),
    ("jeffory/picos-snake and jeffory/picos-store", "PicoDeck/snake and PicoDeck/store"),
    ("git@github.com:jeffory/PicOS-Rally.git", "git@github.com:PicoDeck/rally.git"),
    ("url = https://github.com/jeffory/RP2350-GBC.git", "url = https://github.com/PicoDeck/RP2350-GBC.git"),
    ("\tbranch = picos", "\tbranch = picodeck"),
    ("build/picocalc_os.uf2", "build/picodeck.uf2"),
    ('"name"%s*:%s*"picocalc_os%.bin"', '"name"%s*:%s*"picodeck%.bin"'),
    ("env.PICOS_STORE_KV.put(k, v)", "env.STORE_KV.put(k, v)"),
    ('os.environ.get("PICOS_CDOGS_DIR", "~/Projects/picos-cdogs")',
     'os.environ.get("PICODECK_CDOGS_DIR", "~/Projects/PicoDeck/cdogs")'),
    ("/home/keith/Projects/PicOS/apps", "/home/keith/Projects/PicoDeck/picodeck/apps"),
    ("cd ~/Projects/PicOS-Store", "cd ~/Projects/PicoDeck/store"),
    ("topic:picos-app is:public", "topic:picodeck-app is:public"),
    ('"mcp__picos__keypress"', '"mcp__picodeck__keypress"'),
    ("usb-Raspberry_Pi_PicOS_Device_<serial>-if00", "usb-Raspberry_Pi_PicoDeck_Device_<serial>-if00"),
    ('static const char product[16] = "PicOS_MSC";', 'static const char product[16] = "PicoDeck_MSC";'),
    ("-include third_party/miniz_picos.h", "-include third_party/miniz_picodeck.h"),
    ("// PicOS patch: retry ARP", "// PicoDeck patch: retry ARP"),
]

UNCHANGED = [
    "#define MG_ARCH MG_ARCH_PICOSDK",
    "#if MG_OTA == MG_OTA_PICOSDK",
    "static bool mg_picosdk_write(void *addr, const void *buf, size_t len)",
    "s_mg_flash_picosdk.secsz",
    "picocalc.display.clear()",
    "const PicoCalcAPI *api",
    '"id": "com.id.doom"',
    '"author": "jeffory"',
    "https://github.com/raspberrypi/pico-sdk.git",
]


@pytest.mark.parametrize("old,new", CASES)
def test_rename_text(old, new):
    assert rp.rename_text(old) == new


@pytest.mark.parametrize("text", UNCHANGED)
def test_leaves_non_brand_text_alone(text):
    assert rp.rename_text(text) == text
    assert rp.leftovers(text) == []


@pytest.mark.parametrize("old,new", CASES)
def test_rename_is_idempotent(old, new):
    assert rp.rename_text(new) == new


@pytest.mark.parametrize("old,new", CASES)
def test_output_has_no_leftovers(old, new):
    assert rp.leftovers(new) == []


def test_leftovers_reports_line_numbers():
    assert rp.leftovers("ok\nPicOS here\nMG_ARCH_PICOSDK\n") == [(2, "PicOS here")]


def git(root, *args):
    return subprocess.run(["git", "-C", str(root), *args], check=True,
                          capture_output=True, text=True).stdout


@pytest.fixture
def repo(tmp_path):
    git(tmp_path, "init", "-q")
    git(tmp_path, "config", "user.email", "t@example.com")
    git(tmp_path, "config", "user.name", "t")
    (tmp_path / "cmake").mkdir()
    (tmp_path / "cmake/picos_lua.cmake").write_text("set(PICOS_LUA_SOURCES x)\n")
    (tmp_path / "src.c").write_text("int picos_main(void);\n#define A MG_ARCH_PICOSDK\n")
    (tmp_path / "logo.png").write_bytes(b"\x89PNG\r\n\x1a\n\0\0PicOS")
    (tmp_path / "tests/fuzz/corpus").mkdir(parents=True)
    (tmp_path / "tests/fuzz/corpus/app.json").write_text('{"id":"com.picos.hello"}')
    (tmp_path / "latin1.txt").write_bytes("caf\xe9 PicOS\n".encode("latin-1"))
    git(tmp_path, "add", "-A")
    git(tmp_path, "commit", "-qm", "init")
    return tmp_path


def test_apply_rewrites_contents_and_paths(repo):
    assert rp.main(["--root", str(repo)]) == 0
    assert not (repo / "cmake/picos_lua.cmake").exists()
    assert (repo / "cmake/picodeck_lua.cmake").read_text() == "set(PICODECK_LUA_SOURCES x)\n"
    assert (repo / "src.c").read_text() == "int picodeck_main(void);\n#define A MG_ARCH_PICOSDK\n"


def test_non_utf8_text_round_trips(repo):
    rp.main(["--root", str(repo)])
    assert (repo / "latin1.txt").read_bytes() == "caf\xe9 PicoDeck\n".encode("latin-1")


def test_apply_skips_binaries_and_fuzz_corpus(repo):
    rp.main(["--root", str(repo)])
    assert (repo / "logo.png").read_bytes() == b"\x89PNG\r\n\x1a\n\0\0PicOS"
    assert (repo / "tests/fuzz/corpus/app.json").read_text() == '{"id":"com.picos.hello"}'


def test_apply_skips_submodules(repo):
    sha = git(repo, "rev-parse", "HEAD").strip()
    git(repo, "update-index", "--add", "--cacheinfo", f"160000,{sha},vendor/picos_sub")
    git(repo, "commit", "-qm", "submodule")
    rp.main(["--root", str(repo)])
    assert "vendor/picos_sub" in git(repo, "ls-files", "-s", "vendor")


def test_rename_is_staged_as_a_git_move(repo):
    rp.main(["--root", str(repo)])
    assert "cmake/picos_lua.cmake -> cmake/picodeck_lua.cmake" in git(repo, "status", "--porcelain")


def test_second_run_changes_nothing(repo):
    rp.main(["--root", str(repo)])
    git(repo, "add", "-A")
    git(repo, "commit", "-qm", "sweep")
    rp.main(["--root", str(repo)])
    assert git(repo, "status", "--porcelain") == ""


def test_check_fails_before_and_passes_after(repo, capsys):
    assert rp.main(["--root", str(repo), "--check"]) == 1
    out = capsys.readouterr().out
    assert "src.c:1:" in out
    assert "cmake/picos_lua.cmake: old name in the path" in out
    rp.main(["--root", str(repo)])
    assert rp.main(["--root", str(repo), "--check"]) == 0


def test_check_honours_extra_excludes(repo):
    rp.main(["--root", str(repo)])
    (repo / "docs/critic").mkdir(parents=True)
    (repo / "docs/critic/report.md").write_text("PicOS\n")
    git(repo, "add", "-A")
    assert rp.main(["--root", str(repo), "--check"]) == 1
    assert rp.main(["--root", str(repo), "--check", "--exclude", "docs/critic/"]) == 0


def test_refuses_to_overwrite_an_existing_target(repo):
    (repo / "cmake/picodeck_lua.cmake").write_text("x\n")
    git(repo, "add", "-A")
    git(repo, "commit", "-qm", "clash")
    with pytest.raises(SystemExit, match="already exists"):
        rp.main(["--root", str(repo)])


def test_check_flags_old_ids_compiled_into_binaries(repo, capsys):
    # A native app bakes its /data path into the ELF; a text sweep cannot
    # rewrite it, so --check must say the binary needs a rebuild.
    rp.main(["--root", str(repo)])
    (repo / "main.elf").write_bytes(b"\x7fELF\0\0\0/data/com.picos.c64/roms\0")
    git(repo, "add", "-A")
    assert rp.main(["--root", str(repo), "--check"]) == 1
    assert "main.elf: binary contains an old id or host" in capsys.readouterr().out


def test_check_ignores_brand_text_inside_binaries(repo):
    # Binaries may say "PicOS" in comments/strings that nothing depends on;
    # only ids and hosts (which break at run time) are flagged.
    rp.main(["--root", str(repo)])
    assert rp.main(["--root", str(repo), "--check"]) == 0   # logo.png holds "PicOS"
