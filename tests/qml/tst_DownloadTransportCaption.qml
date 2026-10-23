import QtQuick
import QtTest
import Basecamp.AppManager

// Where a download's bytes come from: the line under its name, and the
// "Finding peers…" its badge shows instead of "0 B" while Logos Storage is
// still looking for peers that have it.
TestCase {
    name: "DownloadTransportCaption"

    function test_logos_explains_it_may_be_slower() {
        compare(DownloadFormat.transportCaption("logos", false),
                "via Logos Storage · peer-to-peer, may be slower")
    }

    function test_github() {
        compare(DownloadFormat.transportCaption("http", false), "via GitHub")
    }

    function test_a_fallback_says_why_it_is_on_github() {
        compare(DownloadFormat.transportCaption("http", true),
                "via GitHub · Logos Storage didn't have it")
    }

    // A downloader that predates the transport: no caption, not a wrong one.
    function test_unknown_transport_has_no_caption() {
        compare(DownloadFormat.transportCaption("", false), "")
        compare(DownloadFormat.transportCaption(undefined, false), "")
    }

    function test_logos_without_a_byte_is_finding_peers() {
        verify(DownloadFormat.findingPeers("logos", 0))
        verify(!DownloadFormat.findingPeers("logos", 1024))
        verify(!DownloadFormat.findingPeers("http", 0))
        verify(!DownloadFormat.findingPeers("", 0))
    }
}
