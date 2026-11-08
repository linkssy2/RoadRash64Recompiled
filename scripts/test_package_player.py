"""Offline package-layout, integrity and exclusion regressions; never starts a game."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

from package_player import package_player, rewrite_document, safe_name


def sha(data):
    return hashlib.sha256(data).hexdigest()


class PlayerPackageTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def baseline(self, extra=None, platform="windows"):
        files = {
            "README.md": b"[Notes](RELEASE_NOTES.md) and RELEASE_NOTES.md; `LICENSE`. See RELEASE_NOTES.md.\n",
            "RELEASE_NOTES.md": b"[Home](README.md) [Notice](licenses/NOTICE.txt)\n",
            "THIRD_PARTY_NOTICES.md": b"`tools/mk64-importer/licenses/COPYING.txt`\n",
            "LICENSE": b"Exact license bytes\r\n",
            "licenses/NOTICE.txt": b"Exact notice bytes\r\n",
            "tools/mk64-importer/licenses/COPYING.txt": b"Importer license\n",
            "BUILD_INFO.json": b'{"version":"fixture"}\n',
            "SHA256SUMS.txt": b"old identity\n",
        }
        if platform == "windows":
            files.update({"RoadRash64Recompiled.exe": b"MZ-game", "SDL3.dll": b"MZ-dll",
                          "portable.txt": b"portable\n", "assets/badge.png": b"badge-pixels",
                          "music/README.txt": b"Custom music\n",
                          "tools/mk64-importer/rr64-mk64-importer.exe": b"MZ-importer"})
        else:
            files.update({"RoadRash64Recompiled-x86_64.AppImage": b"\x7fELF-appimage",
                          "redistribution/sources.tar.xz": b"dependency-source"})
        files.update(extra or {})
        archive = self.root / "old.zip"
        with zipfile.ZipFile(archive, "x") as output:
            for name, data in files.items():
                output.writestr("Old/" + name, data)
        manifest = self.root / "manifest.json"
        manifest.write_text(json.dumps(dict(archive=str(archive), archiveSHA256=sha(archive.read_bytes()),
                                          files={name: sha(data) for name, data in files.items()})))
        return manifest, files

    def package(self, manifest, **kwargs):
        return package_player(manifest, self.root / "New", self.root / "New.zip", **kwargs)

    def test_windows_keeps_runtime_and_license_bytes_and_regenerates_identity(self):
        manifest, original = self.baseline()
        old_archive = (self.root / "old.zip").read_bytes()
        report = self.package(manifest)
        destination = self.root / "New"
        for name in ("RoadRash64Recompiled.exe", "SDL3.dll", "assets/badge.png", "portable.txt",
                     "music/README.txt", "tools/mk64-importer/rr64-mk64-importer.exe"):
            self.assertEqual((destination / name).read_bytes(), original[name])
        self.assertEqual((destination / "docs/LICENSE").read_bytes(), original["LICENSE"])
        self.assertEqual((destination / "docs/licenses/NOTICE.txt").read_bytes(), original["licenses/NOTICE.txt"])
        self.assertIn("[Notes](docs/RELEASE_NOTES.md)", (destination / "README.md").read_text())
        self.assertIn("and docs/RELEASE_NOTES.md", (destination / "README.md").read_text())
        self.assertIn("See docs/RELEASE_NOTES.md.", (destination / "README.md").read_text())
        self.assertIn("`docs/LICENSE`", (destination / "README.md").read_text())
        self.assertIn("[Home](../README.md)", (destination / "docs/RELEASE_NOTES.md").read_text())
        self.assertIn("[Notice](licenses/NOTICE.txt)", (destination / "docs/RELEASE_NOTES.md").read_text())
        self.assertIn("`../tools/", (destination / "docs/THIRD_PARTY_NOTICES.md").read_text())
        for line in (destination / "docs/SHA256SUMS.txt").read_text().splitlines():
            expected, name = line.split("  ", 1)
            self.assertEqual(sha((destination / name).read_bytes()), expected)
        self.assertEqual(old_archive, (self.root / "old.zip").read_bytes())
        self.assertTrue(report["runtimePathsUnchanged"])
        self.assertFalse(report["gameLaunched"])

    def test_linux_retains_appimage_permissions_and_dependency_sources(self):
        manifest, original = self.baseline(platform="linux")
        report = self.package(manifest)
        with zipfile.ZipFile(report["archive"]) as output:
            entry = output.getinfo("New/RoadRash64Recompiled-x86_64.AppImage")
            self.assertEqual(entry.external_attr >> 16, 0o100755)
            self.assertEqual(output.read(entry), original["RoadRash64Recompiled-x86_64.AppImage"])
            self.assertEqual(output.read("New/docs/redistribution/sources.tar.xz"), b"dependency-source")

    def test_replacement_requires_hash_and_explicit_runtime_allowlist(self):
        manifest, _ = self.baseline()
        replacement = self.root / "build.exe"
        replacement.write_bytes(b"MZ-new-game")
        with self.assertRaisesRegex(ValueError, "Replacement has changed"):
            self.package(manifest, replacements={"RoadRash64Recompiled.exe": {"path": str(replacement), "sha256": "wrong"}})
        with self.assertRaisesRegex(ValueError, "Not an allowed replacement"):
            self.package(manifest, replacements={"assets/badge.png": {"path": str(replacement), "sha256": sha(replacement.read_bytes())}})
        report = self.package(manifest, replacements={"RoadRash64Recompiled.exe": {"path": str(replacement), "sha256": sha(replacement.read_bytes())}})
        self.assertEqual(report["files"]["RoadRash64Recompiled.exe"], sha(replacement.read_bytes()))

    def test_altered_archive_and_existing_output_are_rejected(self):
        manifest, _ = self.baseline()
        self.package(manifest)
        with self.assertRaisesRegex(ValueError, "Output already exists"):
            self.package(manifest)
        with (self.root / "old.zip").open("ab") as stream:
            stream.write(b"tampered")
        with self.assertRaisesRegex(ValueError, "Source archive has changed"):
            self.package(manifest)

    def test_private_data_and_path_collisions_are_rejected_before_writing(self):
        for name in ("general.json", "controls.json.bak", "saves/player.sav", "race-packs/mk64/course.bin",
                     "music/song.flac", "runtime.log", "user.z64", "docs/LICENSE"):
            with self.subTest(name=name):
                manifest, _ = self.baseline({name: b"private or colliding"})
                with self.assertRaises(ValueError):
                    self.package(manifest)
                self.assertFalse((self.root / "New").exists())
                (self.root / "old.zip").unlink()

    def test_traversal_and_external_document_links(self):
        for name in ("../outside", "/root", "C:/absolute", "a\\b", "a//b", "a/./b"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                safe_name(name)
        names = {"README.md": "README.md", "LICENSE": "docs/LICENSE"}
        text = "[Upstream](https://example.test/LICENSE) [Source](BUILDING.md)"
        self.assertEqual(rewrite_document(text, "README.md", names), text)


if __name__ == "__main__":
    unittest.main()
