import QtQuick
import QtTest
import Basecamp.AppManager

// The notice above the apps: why downloads may not work right now. Offline is
// Basecamp's offline toast, not this; Logos Storage being down only matters where it
// is the only source, or where each download's source is shown (the Package
// Manager).
TestCase {
    name: "DownloadNotice"

    function notice(online, source, storage, showsFallback) {
        return DownloadFormat.downloadNotice(online, source, storage, showsFallback)
    }

    // Basecamp's offline toast covers it, and Storage failing is a symptom.
    function test_offline_says_nothing_here() {
        for (const source of ["any", "logos", "http"])
            compare(notice(false, source, "failed", true), null, source)
    }

    function test_github_only_never_mentions_storage() {
        compare(notice(true, "http", "failed", true), null)
        compare(notice(true, "http", "off", true), null)
    }

    function test_storage_only_and_down_says_why_and_where_to_fix_it() {
        const n = notice(true, "logos", "failed", false)
        compare(n.severity, "error")
        compare(n.title, "Logos Storage isn't running")
        verify(n.message.indexOf("Settings → Repositories") >= 0)
    }

    function test_storage_only_while_starting() {
        compare(notice(true, "logos", "starting", false).title, "Starting Logos Storage…")
        compare(notice(true, "logos", "waiting", false).title, "Starting Logos Storage…")
    }

    // GitHub covers for it: news only where each download's source is shown.
    function test_both_sources_with_storage_down() {
        compare(notice(true, "any", "failed", false), null)
        const n = notice(true, "any", "failed", true)
        compare(n.severity, "info")
        compare(n.title, "Logos Storage isn't running")
    }

    function test_nothing_to_say_when_storage_runs() {
        compare(notice(true, "logos", "running", true), null)
        compare(notice(true, "any", "running", true), null)
    }

    // Before the downloader has said anything: no guess.
    function test_unknown_storage_state_says_nothing() {
        compare(notice(true, "logos", "", true), null)
    }
}
