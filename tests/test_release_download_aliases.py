import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "aliases", Path(__file__).parents[1] / "scripts/release-download-aliases.py"
)
aliases = importlib.util.module_from_spec(spec)
spec.loader.exec_module(aliases)


def release_fixture(version="0.3.1", commit="aeb819"):
    return {
        "draft": False,
        "prerelease": False,
        "assets": [
            {"name": name.replace("Desktop-", f"Desktop-v{version}-{commit}-"), "state": "uploaded"}
            for name in aliases.ALIASES
        ],
    }


class ReleaseDownloadAliasesTests(unittest.TestCase):
    def test_selects_all_four_platforms_across_versions(self):
        for version, commit in [("0.3.1", "aeb819"), ("0.4.0", "123abc")]:
            release = release_fixture(version, commit)
            self.assertEqual(set(aliases.select_sources(release)), set(aliases.ALIASES))

    def test_existing_aliases_are_not_selected_as_sources(self):
        release = release_fixture()
        expected = aliases.select_sources(release)
        release["assets"] += [{"name": name, "state": "uploaded"} for name in aliases.ALIASES]
        self.assertEqual(aliases.select_sources(release), expected)

    def test_rejects_missing_unfinished_and_ambiguous_assets(self):
        release = release_fixture()
        release["assets"][0]["state"] = "open"
        with self.assertRaises(FileNotFoundError):
            aliases.select_sources(release)
        release = release_fixture()
        release["assets"].append(release["assets"][0])
        with self.assertRaises(ValueError):
            aliases.select_sources(release)

    def test_allows_draft_full_releases_before_publication(self):
        release = release_fixture()
        release["draft"] = True
        self.assertEqual(len(aliases.select_sources(release)), 4)

    def test_rejects_prereleases(self):
        release = release_fixture()
        release["prerelease"] = True
        with self.assertRaises(ValueError):
            aliases.select_sources(release)

    def test_uploads_identical_bytes_and_reruns_without_replacing_assets(self):
        release = release_fixture()
        sources = aliases.select_sources(release)
        uploaded = {}

        def fake_gh(*args):
            if args[:2] == ("release", "download"):
                name = args[args.index("--pattern") + 1]
                directory = Path(args[args.index("--dir") + 1])
                directory.joinpath(name).write_bytes(uploaded.get(name, b"original binary"))
            elif args[:2] == ("release", "upload"):
                for filename in args[5:]:
                    path = Path(filename)
                    uploaded[path.name] = path.read_bytes()
            return ""

        with patch.object(aliases, "gh", side_effect=fake_gh) as command:
            aliases.publish_aliases("owner/repo", "0.3.1", release, sources)
            self.assertEqual(uploaded, {name: b"original binary" for name in aliases.ALIASES})
            release["assets"] += [{"name": name} for name in uploaded]
            command.reset_mock()
            aliases.publish_aliases("owner/repo", "0.3.1", release, sources)
            self.assertFalse(any(call.args[:2] == ("release", "upload") for call in command.call_args_list))
            uploaded[aliases.ALIASES[0]] = b"different binary"
            with self.assertRaises(ValueError):
                aliases.publish_aliases("owner/repo", "0.3.1", release, sources)

    def test_waits_for_in_progress_release_uploads(self):
        incomplete = release_fixture()
        incomplete["assets"] = []
        complete = release_fixture()
        with patch.object(aliases, "load_release", side_effect=[incomplete, complete]), patch.object(aliases.time, "sleep"):
            release, sources = aliases.wait_for_sources("owner/repo", "0.3.1", 60)
        self.assertEqual(release, complete)
        self.assertEqual(len(sources), 4)

    def test_rejects_immutable_releases(self):
        release = release_fixture()
        release["immutable"] = True
        with self.assertRaises(ValueError):
            aliases.publish_aliases("owner/repo", "0.3.1", release, aliases.select_sources(release))


if __name__ == "__main__":
    unittest.main()
